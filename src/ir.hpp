// ir.hpp - a three-address intermediate representation in SSA form.
//
// The bytecode in bytecode.hpp is a *stack* code: operands are implicit, which
// makes it easy to generate and hard to optimize. This file defines the other
// classic shape, the one optimizers work on:
//
//   * every instruction names its operands and its result:  v7 = add v3, v5
//   * code is grouped into BASIC BLOCKS (straight-line runs ending in one jump,
//     branch or return), and the blocks form a control-flow graph (CFG)
//   * it is in SSA form (static single assignment): each value `vN` is assigned
//     exactly once. Where two paths meet and a variable could hold either of two
//     values, a PHI instruction picks the one that belongs to the edge taken:
//
//         b3:  v9 = phi [b1: v4], [b2: v6]
//
// SSA is what makes the optimizations in iropt.hpp short: "is this value a
// constant?" and "is this the same computation as that one?" become questions
// about a single definition instead of about every assignment to a variable.
//
// IrBuilder lowers the AST straight into SSA with the algorithm of Braun et
// al., "Simple and Efficient Construction of Static Single Assignment Form"
// (2013): variables are looked up backwards through the CFG on demand, and a
// phi is created only where a lookup reaches a join. No dominance frontiers
// are needed.
//
// What becomes an SSA value and what stays in memory:
//   * parameters and `let` variables of a function are SSA values
//   * a global that some function reads or writes stays in memory
//     (load_global / store_global), because a call may change it
//   * a global that no function touches is only visible to the top-level code,
//     so it is promoted to an SSA value of `main`
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast.hpp"
#include "bytecode.hpp"

namespace minic {
namespace ir {

enum class IOp {
    Const,        // dst = imm
    Param,        // dst = argument number imm
    Bin,          // dst = args[0] <bin> args[1]
    Neg,          // dst = -args[0]
    Phi,          // dst = args[i] when control came from block phiPreds[i]
    LoadGlobal,   // dst = global slot imm
    StoreGlobal,  // global slot imm = args[0]
    Call,         // dst = function imm (args...)
    Print,        // print args[0]
    Jmp,          // goto t0
    Br,           // if args[0] != 0 goto t0 else goto t1
    Ret           // return args[0]
};

struct Inst {
    IOp op = IOp::Const;
    int dst = -1;               // value defined, or -1
    Op bin = Op::Halt;          // which operator, for IOp::Bin (reuses the bytecode opcodes)
    std::vector<int> args;      // operand values
    std::vector<int> phiPreds;  // for Phi: the predecessor block each operand comes from
    int64_t imm = 0;
    int t0 = -1, t1 = -1;       // branch targets
};

struct Block {
    std::vector<Inst> phis;   // all phis come first and are evaluated together
    std::vector<Inst> insts;  // the last one is the terminator (Jmp, Br or Ret)
    std::vector<int> preds;
    bool removed = false;

    bool terminated() const {
        if (insts.empty()) return false;
        IOp o = insts.back().op;
        return o == IOp::Jmp || o == IOp::Br || o == IOp::Ret;
    }
    std::vector<int> succs() const {
        if (!terminated()) return {};
        const Inst& t = insts.back();
        if (t.op == IOp::Jmp) return {t.t0};
        if (t.op == IOp::Br) return {t.t0, t.t1};
        return {};
    }
};

struct Function {
    std::string name;
    int nparams = 0;
    std::vector<Block> blocks;  // block 0 is the entry
    int nvalues = 0;
};

struct Module {
    std::vector<Function> funcs;       // user functions, in function-table order
    Function main;                     // the top-level code
    std::vector<std::string> globals;  // slot -> name
};

inline bool isPure(const Inst& in) {
    return in.op == IOp::Const || in.op == IOp::Param || in.op == IOp::Bin || in.op == IOp::Neg ||
           in.op == IOp::Phi || in.op == IOp::LoadGlobal;
}

// ------------------------------------------------------------------- printer
inline const char* binName(Op op) {
    switch (op) {
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Mul: return "mul";
        case Op::Div: return "div";
        case Op::Lt: return "lt";
        case Op::Gt: return "gt";
        case Op::Le: return "le";
        case Op::Ge: return "ge";
        case Op::Eq: return "eq";
        case Op::Ne: return "ne";
        default: return "?";
    }
}

inline void dumpInst(std::ostream& os, const Inst& in, const Module& m) {
    os << "  ";
    if (in.dst >= 0) os << 'v' << in.dst << " = ";
    auto arg = [&](size_t i) { return "v" + std::to_string(in.args[i]); };
    auto global = [&]() {
        size_t s = static_cast<size_t>(in.imm);
        return s < m.globals.size() ? m.globals[s] : "?";
    };
    switch (in.op) {
        case IOp::Const: os << "const " << in.imm; break;
        case IOp::Param: os << "param " << in.imm; break;
        case IOp::Bin: os << binName(in.bin) << ' ' << arg(0) << ", " << arg(1); break;
        case IOp::Neg: os << "neg " << arg(0); break;
        case IOp::Phi:
            os << "phi";
            for (size_t i = 0; i < in.args.size(); ++i)
                os << (i ? ", " : " ") << "[b" << in.phiPreds[i] << ": " << arg(i) << ']';
            break;
        case IOp::LoadGlobal: os << "load_global " << global(); break;
        case IOp::StoreGlobal: os << "store_global " << global() << ", " << arg(0); break;
        case IOp::Call: {
            size_t f = static_cast<size_t>(in.imm);
            os << "call " << (f < m.funcs.size() ? m.funcs[f].name : "?") << '(';
            for (size_t i = 0; i < in.args.size(); ++i) os << (i ? ", " : "") << arg(i);
            os << ')';
            break;
        }
        case IOp::Print: os << "print " << arg(0); break;
        case IOp::Jmp: os << "jmp b" << in.t0; break;
        case IOp::Br: os << "br " << arg(0) << ", b" << in.t0 << ", b" << in.t1; break;
        case IOp::Ret: os << "ret " << arg(0); break;
    }
    os << '\n';
}

inline void dumpFunction(std::ostream& os, const Function& f, const Module& m) {
    os << f.name << ':';
    if (f.name != "main") os << "    ; " << f.nparams << " parameter(s)";
    os << '\n';
    for (size_t b = 0; b < f.blocks.size(); ++b) {
        const Block& blk = f.blocks[b];
        if (blk.removed) continue;
        os << " b" << b << ':';
        if (!blk.preds.empty()) {
            os << "    ; from";
            for (int p : blk.preds) os << " b" << p;
        }
        os << '\n';
        for (const Inst& in : blk.phis) dumpInst(os, in, m);
        for (const Inst& in : blk.insts) dumpInst(os, in, m);
    }
}

inline std::string dump(const Module& m) {
    std::ostringstream os;
    dumpFunction(os, m.main, m);
    for (const Function& f : m.funcs) {
        os << '\n';
        dumpFunction(os, f, m);
    }
    return os.str();
}

inline size_t countInsts(const Function& f) {
    size_t n = 0;
    for (const Block& b : f.blocks)
        if (!b.removed) n += b.phis.size() + b.insts.size();
    return n;
}
inline size_t countInsts(const Module& m) {
    size_t n = countInsts(m.main);
    for (const Function& f : m.funcs) n += countInsts(f);
    return n;
}

// ------------------------------------------------------------------- builder
class IrBuilder {
public:
    Module build(const Program& prog, const std::vector<std::string>& globalNames) {
        Module m;
        m.globals = globalNames;

        // A global stays in memory if any function body mentions it.
        memoryGlobals_.clear();
        for (const auto& s : prog.stmts)
            if (s->kind == StmtKind::Fn) scanStmt(*static_cast<const FnDecl&>(*s).body);

        for (const auto& s : prog.stmts) {
            if (s->kind != StmtKind::Fn) continue;
            const auto& f = static_cast<const FnDecl&>(*s);
            begin(f.name, static_cast<int>(f.params.size()), false);
            for (size_t i = 0; i < f.localNames.size(); ++i) {
                Inst in;
                if (i < f.params.size()) { in.op = IOp::Param; in.imm = static_cast<int64_t>(i); }
                else { in.op = IOp::Const; in.imm = 0; }  // an unset variable reads as 0
                writeVar(static_cast<int>(i), cur_, emit(std::move(in)));
            }
            stmt(*f.body);
            finish();
            m.funcs.push_back(std::move(fn_));
        }

        begin("main", 0, true);
        for (size_t g = 0; g < globalNames.size(); ++g) {
            if (memoryGlobals_.count(static_cast<int>(g))) continue;
            Inst in;
            in.op = IOp::Const;
            in.imm = 0;
            writeVar(static_cast<int>(g), cur_, emit(std::move(in)));
        }
        for (const auto& s : prog.stmts)
            if (s->kind != StmtKind::Fn) stmt(*s);
        finish();
        m.main = std::move(fn_);
        return m;
    }

private:
    Function fn_;
    int cur_ = 0;
    bool inMain_ = false;
    std::unordered_set<int> memoryGlobals_;
    // Braun et al. state, per block.
    std::vector<std::unordered_map<int, int>> currentDef_;   // variable -> value
    std::vector<std::unordered_map<int, int>> incomplete_;   // variable -> phi awaiting operands
    std::vector<bool> sealed_;                               // all predecessors known?

    // ---- which globals do functions touch?
    void scanExpr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Number: break;
            case ExprKind::Var: {
                const auto& v = static_cast<const VarExpr&>(e);
                if (!v.local) memoryGlobals_.insert(v.slot);
                break;
            }
            case ExprKind::Unary: scanExpr(*static_cast<const UnaryExpr&>(e).operand); break;
            case ExprKind::Binary: {
                const auto& b = static_cast<const BinaryExpr&>(e);
                scanExpr(*b.lhs);
                scanExpr(*b.rhs);
                break;
            }
            case ExprKind::Call:
                for (const auto& a : static_cast<const CallExpr&>(e).args) scanExpr(*a);
                break;
        }
    }
    void scanStmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: scanExpr(*static_cast<const LetStmt&>(s).init); break;
            case StmtKind::Assign: {
                const auto& n = static_cast<const AssignStmt&>(s);
                if (!n.local) memoryGlobals_.insert(n.slot);
                scanExpr(*n.value);
                break;
            }
            case StmtKind::Print: scanExpr(*static_cast<const PrintStmt&>(s).value); break;
            case StmtKind::Block:
                for (const auto& c : static_cast<const BlockStmt&>(s).stmts) scanStmt(*c);
                break;
            case StmtKind::If: {
                const auto& n = static_cast<const IfStmt&>(s);
                scanExpr(*n.cond);
                scanStmt(*n.thenBranch);
                if (n.elseBranch) scanStmt(*n.elseBranch);
                break;
            }
            case StmtKind::While: {
                const auto& n = static_cast<const WhileStmt&>(s);
                scanExpr(*n.cond);
                scanStmt(*n.body);
                break;
            }
            case StmtKind::Fn: break;
            case StmtKind::Return: {
                const auto& n = static_cast<const ReturnStmt&>(s);
                if (n.value) scanExpr(*n.value);
                break;
            }
            case StmtKind::ExprStmt: scanExpr(*static_cast<const ExprStmt&>(s).expr); break;
        }
    }

    // ---- blocks and instructions
    void begin(const std::string& name, int nparams, bool isMain) {
        fn_ = Function{};
        fn_.name = name;
        fn_.nparams = nparams;
        inMain_ = isMain;
        currentDef_.clear();
        incomplete_.clear();
        sealed_.clear();
        cur_ = newBlock();
        sealed_[static_cast<size_t>(cur_)] = true;  // the entry has no predecessors
    }

    void finish() {
        if (!block(cur_).terminated()) ret(constant(0));
    }

    int newBlock() {
        fn_.blocks.emplace_back();
        currentDef_.emplace_back();
        incomplete_.emplace_back();
        sealed_.push_back(false);
        return static_cast<int>(fn_.blocks.size()) - 1;
    }
    Block& block(int b) { return fn_.blocks[static_cast<size_t>(b)]; }

    // Appends to the current block. Code after a `return` is unreachable, so it
    // goes into a fresh block with no predecessors, which the optimizer deletes.
    int emit(Inst in) {
        if (block(cur_).terminated()) {
            cur_ = newBlock();
            sealed_[static_cast<size_t>(cur_)] = true;
        }
        bool defines = in.op != IOp::StoreGlobal && in.op != IOp::Print && in.op != IOp::Jmp &&
                       in.op != IOp::Br && in.op != IOp::Ret;
        if (defines) in.dst = fn_.nvalues++;
        int dst = in.dst;
        block(cur_).insts.push_back(std::move(in));
        return dst;
    }

    int constant(int64_t v) {
        Inst in;
        in.op = IOp::Const;
        in.imm = v;
        return emit(std::move(in));
    }
    void jmp(int target) {
        if (block(cur_).terminated()) return;
        Inst in;
        in.op = IOp::Jmp;
        in.t0 = target;
        block(target).preds.push_back(cur_);
        block(cur_).insts.push_back(std::move(in));
    }
    void br(int cond, int ifTrue, int ifFalse) {
        Inst in;
        in.op = IOp::Br;
        in.args = {cond};
        in.t0 = ifTrue;
        in.t1 = ifFalse;
        emit(std::move(in));  // may have moved to a fresh block; record preds afterwards
        block(ifTrue).preds.push_back(cur_);
        block(ifFalse).preds.push_back(cur_);
    }
    void ret(int value) {
        Inst in;
        in.op = IOp::Ret;
        in.args = {value};
        emit(std::move(in));
    }

    // ---- SSA construction (Braun et al.)
    void writeVar(int var, int b, int value) { currentDef_[static_cast<size_t>(b)][var] = value; }

    int readVar(int var, int b) {
        auto& defs = currentDef_[static_cast<size_t>(b)];
        auto it = defs.find(var);
        if (it != defs.end()) return it->second;

        int value;
        if (!sealed_[static_cast<size_t>(b)]) {
            // A loop header whose back edge is not known yet: leave an empty phi
            // and fill it in when the block is sealed.
            value = newPhi(b);
            incomplete_[static_cast<size_t>(b)][var] = value;
        } else if (block(b).preds.empty()) {
            // Unreachable code (after a return). Any value will do.
            Inst in;
            in.op = IOp::Const;
            in.imm = 0;
            in.dst = fn_.nvalues++;
            value = in.dst;
            block(b).insts.insert(block(b).insts.begin(), std::move(in));
        } else if (block(b).preds.size() == 1) {
            value = readVar(var, block(b).preds[0]);  // no join here, so no phi
        } else {
            value = newPhi(b);
            writeVar(var, b, value);  // break cycles before looking at the predecessors
            addPhiOperands(var, b, value);
        }
        writeVar(var, b, value);
        return value;
    }

    int newPhi(int b) {
        Inst phi;
        phi.op = IOp::Phi;
        phi.dst = fn_.nvalues++;
        int dst = phi.dst;
        block(b).phis.push_back(std::move(phi));
        return dst;
    }

    void addPhiOperands(int var, int b, int phiValue) {
        std::vector<int> preds = block(b).preds;  // copy: readVar may add blocks
        for (int p : preds) {
            int v = readVar(var, p);
            for (Inst& phi : block(b).phis)
                if (phi.dst == phiValue) {
                    phi.args.push_back(v);
                    phi.phiPreds.push_back(p);
                }
        }
    }

    void seal(int b) {
        auto pending = incomplete_[static_cast<size_t>(b)];
        for (const auto& kv : pending) addPhiOperands(kv.first, b, kv.second);
        incomplete_[static_cast<size_t>(b)].clear();
        sealed_[static_cast<size_t>(b)] = true;
    }

    // ---- variables: SSA value or memory?
    bool isSsaVar(bool local, int slot) const {
        if (local) return true;
        return inMain_ && !memoryGlobals_.count(slot);
    }

    int load(bool local, int slot) {
        if (isSsaVar(local, slot)) {
            if (block(cur_).terminated()) constant(0);  // moves us into a fresh dead block
            return readVar(slot, cur_);
        }
        Inst in;
        in.op = IOp::LoadGlobal;
        in.imm = slot;
        return emit(std::move(in));
    }

    void store(bool local, int slot, int value) {
        if (isSsaVar(local, slot)) {
            if (block(cur_).terminated()) constant(0);
            writeVar(slot, cur_, value);
            return;
        }
        Inst in;
        in.op = IOp::StoreGlobal;
        in.imm = slot;
        in.args = {value};
        emit(std::move(in));
    }

    // ---- lowering
    int expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Number:
                return constant(static_cast<const NumberExpr&>(e).value);
            case ExprKind::Var: {
                const auto& v = static_cast<const VarExpr&>(e);
                return load(v.local, v.slot);
            }
            case ExprKind::Unary: {
                Inst in;
                in.op = IOp::Neg;
                in.args = {expr(*static_cast<const UnaryExpr&>(e).operand)};
                return emit(std::move(in));
            }
            case ExprKind::Binary: {
                const auto& b = static_cast<const BinaryExpr&>(e);
                Inst in;
                in.op = IOp::Bin;
                in.bin = binaryOpFor(b.op);
                int lhs = expr(*b.lhs);  // left operand first, as in the bytecode back end
                int rhs = expr(*b.rhs);
                in.args = {lhs, rhs};
                return emit(std::move(in));
            }
            case ExprKind::Call: {
                const auto& c = static_cast<const CallExpr&>(e);
                Inst in;
                in.op = IOp::Call;
                in.imm = c.fnIndex;
                for (const auto& a : c.args) in.args.push_back(expr(*a));
                return emit(std::move(in));
            }
        }
        return -1;
    }

    void stmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                const auto& n = static_cast<const LetStmt&>(s);
                store(n.local, n.slot, expr(*n.init));
                break;
            }
            case StmtKind::Assign: {
                const auto& n = static_cast<const AssignStmt&>(s);
                store(n.local, n.slot, expr(*n.value));
                break;
            }
            case StmtKind::Print: {
                Inst in;
                in.op = IOp::Print;
                in.args = {expr(*static_cast<const PrintStmt&>(s).value)};
                emit(std::move(in));
                break;
            }
            case StmtKind::Block:
                for (const auto& c : static_cast<const BlockStmt&>(s).stmts) stmt(*c);
                break;
            case StmtKind::If: {
                const auto& n = static_cast<const IfStmt&>(s);
                int cond = expr(*n.cond);
                int thenB = newBlock();
                int elseB = n.elseBranch ? newBlock() : -1;
                int join = newBlock();
                br(cond, thenB, n.elseBranch ? elseB : join);
                seal(thenB);
                cur_ = thenB;
                stmt(*n.thenBranch);
                jmp(join);
                if (n.elseBranch) {
                    seal(elseB);
                    cur_ = elseB;
                    stmt(*n.elseBranch);
                    jmp(join);
                }
                seal(join);
                cur_ = join;
                break;
            }
            case StmtKind::While: {
                const auto& n = static_cast<const WhileStmt&>(s);
                if (block(cur_).terminated()) constant(0);  // unreachable loop: start a dead block
                int header = newBlock();
                int body = newBlock();
                int exit = newBlock();
                jmp(header);
                cur_ = header;  // not sealed yet: the back edge comes later
                int cond = expr(*n.cond);
                br(cond, body, exit);
                seal(body);
                cur_ = body;
                stmt(*n.body);
                jmp(header);
                seal(header);
                seal(exit);
                cur_ = exit;
                break;
            }
            case StmtKind::Fn:
                break;
            case StmtKind::Return: {
                const auto& n = static_cast<const ReturnStmt&>(s);
                ret(n.value ? expr(*n.value) : constant(0));
                break;
            }
            case StmtKind::ExprStmt:
                expr(*static_cast<const ExprStmt&>(s).expr);
                break;
        }
    }
};

}  // namespace ir
}  // namespace minic
