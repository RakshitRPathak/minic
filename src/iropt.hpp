// iropt.hpp - optimizations on the SSA IR.
//
//   1. Constant propagation and folding, with algebraic identities
//        v2 = add v0, v1   (v0 = 2, v1 = 3)   ->  v2 = const 5
//        v3 = mul v2, v9   (v9 = 1)           ->  uses of v3 become v2
//      Because a variable is now a value, a constant flows through assignments
//      and across blocks, which the AST folder in optimizer.hpp cannot do.
//   2. Branch folding and unreachable-block removal
//        br v4, b1, b2     (v4 = const 0)     ->  jmp b2, and b1 goes if nothing else reaches it
//   3. Phi simplification
//        v7 = phi [b1: v3], [b2: v3]          ->  uses of v7 become v3
//   4. Global value numbering (common subexpression elimination)
//        Two instructions that compute the same operator on the same values
//        give the same result, so the second is replaced by the first whenever
//        the first DOMINATES it (runs on every path that reaches it). The pass
//        walks the dominator tree with a scoped table.
//   5. Dead code elimination
//        Anything whose result is never used and that has no effect is removed.
//   6. Block merging
//        A block that only one block jumps to, and that is that block's only
//        successor, is appended to it. Folded branches leave such chains behind.
//
// What is never removed: calls, prints, stores to globals, and a division whose
// divisor is not a known non-zero constant (it may be the division by zero the
// program is supposed to fail with).
//
// Folding uses the same evalBinary() as the VM, so results cannot disagree.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include "ir.hpp"

namespace minic {
namespace ir {

struct IrOptStats {
    int constantsFolded = 0;
    int identities = 0;
    int branchesFolded = 0;
    int blocksRemoved = 0;
    int blocksMerged = 0;
    int phisRemoved = 0;
    int cseHits = 0;
    int deadInsts = 0;
};

// Immediate dominators by the iterative algorithm of Cooper, Harvey and Kennedy
// ("A Simple, Fast Dominance Algorithm"). idom[entry] == entry; -1 = unreachable.
inline std::vector<int> computeIdom(const Function& f, std::vector<int>* rpoOut = nullptr) {
    const int n = static_cast<int>(f.blocks.size());
    std::vector<int> order;  // postorder
    std::vector<char> seen(static_cast<size_t>(n), 0);
    std::vector<std::pair<int, size_t>> stack;
    stack.push_back({0, 0});
    seen[0] = 1;
    while (!stack.empty()) {
        int b = stack.back().first;
        std::vector<int> ss = f.blocks[static_cast<size_t>(b)].succs();
        if (stack.back().second < ss.size()) {
            int s = ss[stack.back().second++];
            if (!seen[static_cast<size_t>(s)]) {
                seen[static_cast<size_t>(s)] = 1;
                stack.push_back({s, 0});
            }
        } else {
            order.push_back(b);
            stack.pop_back();
        }
    }
    std::vector<int> rpo(order.rbegin(), order.rend());
    std::vector<int> rpoIndex(static_cast<size_t>(n), -1);
    for (size_t i = 0; i < rpo.size(); ++i) rpoIndex[static_cast<size_t>(rpo[i])] = static_cast<int>(i);

    std::vector<int> idom(static_cast<size_t>(n), -1);
    idom[0] = 0;
    auto intersect = [&](int a, int b) {
        while (a != b) {
            while (rpoIndex[static_cast<size_t>(a)] > rpoIndex[static_cast<size_t>(b)])
                a = idom[static_cast<size_t>(a)];
            while (rpoIndex[static_cast<size_t>(b)] > rpoIndex[static_cast<size_t>(a)])
                b = idom[static_cast<size_t>(b)];
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (int b : rpo) {
            if (b == 0) continue;
            int best = -1;
            for (int p : f.blocks[static_cast<size_t>(b)].preds) {
                if (rpoIndex[static_cast<size_t>(p)] < 0 || idom[static_cast<size_t>(p)] < 0) continue;
                best = best < 0 ? p : intersect(p, best);
            }
            if (best >= 0 && idom[static_cast<size_t>(b)] != best) {
                idom[static_cast<size_t>(b)] = best;
                changed = true;
            }
        }
    }
    if (rpoOut) *rpoOut = rpo;
    return idom;
}

class IrOptimizer {
public:
    explicit IrOptimizer(IrOptStats& stats) : stats_(stats) {}

    void run(Module& m) {
        for (Function& f : m.funcs) runFunction(f);
        runFunction(m.main);
    }

    void runFunction(Function& f) {
        f_ = &f;
        repl_.assign(static_cast<size_t>(f.nvalues), -1);
        for (int round = 0; round < 50; ++round) {
            bool changed = false;
            changed |= removeUnreachable();
            changed |= simplifyPhis();
            changed |= foldConstants();
            changed |= foldBranches();
            changed |= valueNumber();
            changed |= eliminateDeadCode();
            changed |= mergeBlocks();
            if (!changed) break;
        }
        compact();
    }

private:
    IrOptStats& stats_;
    Function* f_ = nullptr;
    std::vector<int> repl_;  // value -> the value that replaces it (union-find style)

    int find(int v) {
        while (repl_[static_cast<size_t>(v)] >= 0) {
            int next = repl_[static_cast<size_t>(v)];
            if (repl_[static_cast<size_t>(next)] >= 0) repl_[static_cast<size_t>(v)] = repl_[static_cast<size_t>(next)];
            v = next;
        }
        return v;
    }
    void replace(int from, int to) {
        to = find(to);
        if (from != to) repl_[static_cast<size_t>(from)] = to;
    }

    // Rewrite every operand through the replacement table.
    void applyReplacements() {
        for (Block& b : f_->blocks) {
            if (b.removed) continue;
            for (Inst& in : b.phis)
                for (int& a : in.args) a = find(a);
            for (Inst& in : b.insts)
                for (int& a : in.args) a = find(a);
        }
    }

    // Drop the edge pred -> b: forget the predecessor and its phi operands.
    void removeEdge(int pred, int b) {
        Block& blk = f_->blocks[static_cast<size_t>(b)];
        blk.preds.erase(std::remove(blk.preds.begin(), blk.preds.end(), pred), blk.preds.end());
        for (Inst& phi : blk.phis)
            for (size_t i = phi.args.size(); i-- > 0;)
                if (phi.phiPreds[i] == pred) {
                    phi.args.erase(phi.args.begin() + static_cast<long>(i));
                    phi.phiPreds.erase(phi.phiPreds.begin() + static_cast<long>(i));
                }
    }

    bool removeUnreachable() {
        const size_t n = f_->blocks.size();
        std::vector<char> reach(n, 0);
        std::vector<int> work{0};
        reach[0] = 1;
        while (!work.empty()) {
            int b = work.back();
            work.pop_back();
            for (int s : f_->blocks[static_cast<size_t>(b)].succs())
                if (!reach[static_cast<size_t>(s)]) {
                    reach[static_cast<size_t>(s)] = 1;
                    work.push_back(s);
                }
        }
        bool changed = false;
        for (size_t b = 0; b < n; ++b) {
            Block& blk = f_->blocks[b];
            if (blk.removed || reach[b]) continue;
            for (int s : blk.succs())
                if (reach[static_cast<size_t>(s)]) removeEdge(static_cast<int>(b), s);
            blk.removed = true;
            blk.phis.clear();
            blk.insts.clear();
            blk.preds.clear();
            ++stats_.blocksRemoved;
            changed = true;
        }
        return changed;
    }

    // A phi whose operands are all the same value (ignoring itself) is that value.
    bool simplifyPhis() {
        bool any = false;
        for (bool changed = true; changed;) {
            changed = false;
            for (Block& b : f_->blocks) {
                if (b.removed) continue;
                for (size_t i = b.phis.size(); i-- > 0;) {
                    Inst& phi = b.phis[i];
                    int same = -1;
                    bool trivial = true;
                    for (int a : phi.args) {
                        a = find(a);
                        if (a == phi.dst || a == same) continue;
                        if (same >= 0) { trivial = false; break; }
                        same = a;
                    }
                    if (!trivial || same < 0) continue;
                    replace(phi.dst, same);
                    b.phis.erase(b.phis.begin() + static_cast<long>(i));
                    ++stats_.phisRemoved;
                    changed = any = true;
                }
            }
        }
        if (any) applyReplacements();
        return any;
    }

    // value -> constant, for every `const` instruction currently in the function
    std::vector<std::pair<bool, int64_t>> constants() {
        std::vector<std::pair<bool, int64_t>> c(static_cast<size_t>(f_->nvalues), {false, 0});
        for (const Block& b : f_->blocks)
            for (const Inst& in : b.insts)
                if (in.op == IOp::Const) c[static_cast<size_t>(in.dst)] = {true, in.imm};
        return c;
    }

    bool foldConstants() {
        bool any = false;
        auto c = constants();
        auto isConst = [&](int v, int64_t k) {
            return c[static_cast<size_t>(v)].first && c[static_cast<size_t>(v)].second == k;
        };
        std::vector<int> rpo;
        computeIdom(*f_, &rpo);  // reverse postorder: definitions before uses (except around loops)
        for (bool changed = true; changed;) {
            changed = false;
            for (int bi : rpo) {
                Block& b = f_->blocks[static_cast<size_t>(bi)];
                for (size_t i = 0; i < b.insts.size(); ++i) {
                    Inst& in = b.insts[i];
                    if (in.op != IOp::Bin && in.op != IOp::Neg) continue;
                    for (int& a : in.args) a = find(a);
                    auto toConst = [&](int64_t v) {
                        in.op = IOp::Const;
                        in.imm = v;
                        in.args.clear();
                        c[static_cast<size_t>(in.dst)] = {true, v};
                        changed = any = true;
                    };
                    auto toValue = [&](int v) {
                        replace(in.dst, v);
                        b.insts.erase(b.insts.begin() + static_cast<long>(i));
                        --i;
                        ++stats_.identities;
                        changed = any = true;
                    };
                    if (in.op == IOp::Neg) {
                        if (c[static_cast<size_t>(in.args[0])].first) {
                            ++stats_.constantsFolded;
                            toConst(wrapNeg(c[static_cast<size_t>(in.args[0])].second));
                        }
                        continue;
                    }
                    const int x = in.args[0], y = in.args[1];
                    const auto cx = c[static_cast<size_t>(x)], cy = c[static_cast<size_t>(y)];
                    if (cx.first && cy.first) {
                        int64_t r = 0;
                        if (evalBinary(in.bin, cx.second, cy.second, r)) {  // false: division by zero
                            ++stats_.constantsFolded;
                            toConst(r);
                        }
                        continue;
                    }
                    // Identities. None of these can hide a division by zero.
                    switch (in.bin) {
                        case Op::Add:
                            if (isConst(y, 0)) toValue(x);
                            else if (isConst(x, 0)) toValue(y);
                            break;
                        case Op::Sub:
                            if (isConst(y, 0)) toValue(x);
                            else if (x == y) { ++stats_.identities; toConst(0); }
                            break;
                        case Op::Mul:
                            if (isConst(y, 1)) toValue(x);
                            else if (isConst(x, 1)) toValue(y);
                            else if (isConst(x, 0) || isConst(y, 0)) { ++stats_.identities; toConst(0); }
                            break;
                        case Op::Div:
                            if (isConst(y, 1)) toValue(x);
                            break;
                        case Op::Eq: case Op::Le: case Op::Ge:
                            if (x == y) { ++stats_.identities; toConst(1); }
                            break;
                        case Op::Ne: case Op::Lt: case Op::Gt:
                            if (x == y) { ++stats_.identities; toConst(0); }
                            break;
                        default: break;
                    }
                }
            }
        }
        if (any) applyReplacements();
        return any;
    }

    bool foldBranches() {
        bool any = false;
        auto c = constants();
        for (size_t bi = 0; bi < f_->blocks.size(); ++bi) {
            Block& b = f_->blocks[bi];
            if (b.removed || b.insts.empty() || b.insts.back().op != IOp::Br) continue;
            Inst& t = b.insts.back();
            int cond = find(t.args[0]);
            if (!c[static_cast<size_t>(cond)].first) continue;
            bool taken = c[static_cast<size_t>(cond)].second != 0;
            int keep = taken ? t.t0 : t.t1;
            int drop = taken ? t.t1 : t.t0;
            t.op = IOp::Jmp;
            t.args.clear();
            t.t0 = keep;
            t.t1 = -1;
            if (drop != keep) removeEdge(static_cast<int>(bi), drop);
            ++stats_.branchesFolded;
            any = true;
        }
        return any;
    }

    bool valueNumber() {
        std::vector<int> rpo;
        std::vector<int> idom = computeIdom(*f_, &rpo);
        const size_t n = f_->blocks.size();
        std::vector<std::vector<int>> children(n);
        for (int b : rpo)
            if (b != 0) children[static_cast<size_t>(idom[static_cast<size_t>(b)])].push_back(b);

        using Key = std::tuple<int, int, int64_t, int, int>;  // op, operator, imm, arg0, arg1
        std::map<Key, int> table;
        bool any = false;

        // Depth-first over the dominator tree. Entries added in a block are
        // visible in the blocks it dominates and removed again on the way out.
        struct Visit { int block; size_t child; std::vector<Key> added; };
        std::vector<Visit> stack;
        auto enter = [&](int bi) {
            Visit v{bi, 0, {}};
            Block& b = f_->blocks[static_cast<size_t>(bi)];
            for (Inst& phi : b.phis)
                for (int& a : phi.args) a = find(a);
            for (size_t i = 0; i < b.insts.size(); ++i) {
                Inst& in = b.insts[i];
                for (int& a : in.args) a = find(a);
                if (in.op != IOp::Const && in.op != IOp::Bin && in.op != IOp::Neg) continue;
                int a0 = in.args.size() > 0 ? in.args[0] : -1;
                int a1 = in.args.size() > 1 ? in.args[1] : -1;
                bool commutative = in.op == IOp::Bin && (in.bin == Op::Add || in.bin == Op::Mul ||
                                                         in.bin == Op::Eq || in.bin == Op::Ne);
                if (commutative && a0 > a1) std::swap(a0, a1);
                Key key{static_cast<int>(in.op), static_cast<int>(in.bin), in.imm, a0, a1};
                auto it = table.find(key);
                if (it != table.end()) {
                    replace(in.dst, it->second);
                    b.insts.erase(b.insts.begin() + static_cast<long>(i));
                    --i;
                    ++stats_.cseHits;
                    any = true;
                } else {
                    table.emplace(key, in.dst);
                    v.added.push_back(key);
                }
            }
            stack.push_back(std::move(v));
        };
        enter(0);
        while (!stack.empty()) {
            Visit& top = stack.back();
            const auto& kids = children[static_cast<size_t>(top.block)];
            if (top.child < kids.size()) {
                enter(kids[top.child++]);
            } else {
                for (const Key& k : top.added) table.erase(k);
                stack.pop_back();
            }
        }
        if (any) applyReplacements();
        return any;
    }

    bool eliminateDeadCode() {
        auto c = constants();
        std::vector<char> live(static_cast<size_t>(f_->nvalues), 0);
        std::vector<const Inst*> def(static_cast<size_t>(f_->nvalues), nullptr);
        std::vector<int> work;
        auto use = [&](const Inst& in) {
            for (int a : in.args)
                if (!live[static_cast<size_t>(a)]) {
                    live[static_cast<size_t>(a)] = 1;
                    work.push_back(a);
                }
        };
        auto mayTrap = [&](const Inst& in) {
            if (in.op != IOp::Bin || in.bin != Op::Div) return false;
            auto d = c[static_cast<size_t>(in.args[1])];
            return !(d.first && d.second != 0);
        };
        for (const Block& b : f_->blocks) {
            for (const Inst& in : b.phis) def[static_cast<size_t>(in.dst)] = &in;
            for (const Inst& in : b.insts) {
                if (in.dst >= 0) def[static_cast<size_t>(in.dst)] = &in;
                if (!isPure(in) || mayTrap(in)) {  // roots: effects, terminators, possible traps
                    if (in.dst >= 0) live[static_cast<size_t>(in.dst)] = 1;
                    use(in);
                }
            }
        }
        while (!work.empty()) {
            int v = work.back();
            work.pop_back();
            if (def[static_cast<size_t>(v)]) use(*def[static_cast<size_t>(v)]);
        }
        bool any = false;
        auto dead = [&](const Inst& in) {
            return in.dst >= 0 && !live[static_cast<size_t>(in.dst)] && isPure(in) && !mayTrap(in);
        };
        for (Block& b : f_->blocks) {
            size_t before = b.phis.size() + b.insts.size();
            b.phis.erase(std::remove_if(b.phis.begin(), b.phis.end(), dead), b.phis.end());
            b.insts.erase(std::remove_if(b.insts.begin(), b.insts.end(), dead), b.insts.end());
            size_t removed = before - b.phis.size() - b.insts.size();
            stats_.deadInsts += static_cast<int>(removed);
            any |= removed > 0;
        }
        return any;
    }

    bool mergeBlocks() {
        bool any = false;
        for (size_t bi = 0; bi < f_->blocks.size(); ++bi) {
            for (;;) {
                Block& b = f_->blocks[bi];
                if (b.removed || b.insts.empty() || b.insts.back().op != IOp::Jmp) break;
                const int si = b.insts.back().t0;
                Block& s = f_->blocks[static_cast<size_t>(si)];
                // The entry block must stay first, and a block with phis still has a real join.
                if (si == 0 || static_cast<size_t>(si) == bi || s.preds.size() != 1 || !s.phis.empty()) break;
                b.insts.pop_back();
                for (Inst& in : s.insts) b.insts.push_back(std::move(in));
                for (int succ : b.succs()) {  // the successors now hang off b
                    Block& t = f_->blocks[static_cast<size_t>(succ)];
                    for (int& p : t.preds)
                        if (p == si) p = static_cast<int>(bi);
                    for (Inst& phi : t.phis)
                        for (int& p : phi.phiPreds)
                            if (p == si) p = static_cast<int>(bi);
                }
                s.insts.clear();
                s.preds.clear();
                s.removed = true;
                ++stats_.blocksMerged;
                any = true;
            }
        }
        return any;
    }

    // Renumber blocks and values densely so the dump reads b0, b1, ... / v0, v1, ...
    void compact() {
        applyReplacements();
        std::vector<int> blockMap(f_->blocks.size(), -1);
        std::vector<Block> kept;
        for (size_t b = 0; b < f_->blocks.size(); ++b)
            if (!f_->blocks[b].removed) {
                blockMap[b] = static_cast<int>(kept.size());
                kept.push_back(std::move(f_->blocks[b]));
            }
        std::vector<int> valueMap(static_cast<size_t>(f_->nvalues), -1);
        int next = 0;
        for (Block& b : kept) {
            for (Inst& in : b.phis) valueMap[static_cast<size_t>(in.dst)] = next++;
            for (Inst& in : b.insts)
                if (in.dst >= 0) valueMap[static_cast<size_t>(in.dst)] = next++;
        }
        auto fix = [&](Inst& in) {
            if (in.dst >= 0) in.dst = valueMap[static_cast<size_t>(in.dst)];
            for (int& a : in.args) a = valueMap[static_cast<size_t>(a)];
            for (int& p : in.phiPreds) p = blockMap[static_cast<size_t>(p)];
            if (in.t0 >= 0) in.t0 = blockMap[static_cast<size_t>(in.t0)];
            if (in.t1 >= 0) in.t1 = blockMap[static_cast<size_t>(in.t1)];
        };
        for (Block& b : kept) {
            for (int& p : b.preds) p = blockMap[static_cast<size_t>(p)];
            for (Inst& in : b.phis) fix(in);
            for (Inst& in : b.insts) fix(in);
        }
        f_->blocks = std::move(kept);
        f_->nvalues = next;
    }
};

}  // namespace ir
}  // namespace minic
