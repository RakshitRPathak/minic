// irvm.hpp - an interpreter for the SSA IR.
//
// It exists to keep the IR honest: `minic --ir` runs a program through
// AST -> SSA -> (optimizer) -> this interpreter, and the test suite checks that
// the output is identical to the bytecode VM's for every test program, with and
// without optimization. A register-based back end can later replace it.
//
// Phis are the one construct with no direct machine equivalent. On entry to a
// block all of its phis are evaluated together, each taking the operand that
// belongs to the block we arrived from. "Together" matters: one phi may read
// the old value of another (a swap in a loop), so the new values are computed
// first and written afterwards.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

#include "ir.hpp"
#include "vm.hpp"

namespace minic {
namespace ir {

inline VMStats runIr(const Module& m, std::ostream& out, uint64_t maxSteps) {
    struct Frame {
        const Function* fn;
        std::vector<int64_t> values;
        std::vector<int64_t> args;
        int block = 0;
        size_t ip = 0;
        int resultDst = -1;  // where the caller wants the return value
    };
    std::vector<int64_t> globals(m.globals.size(), 0);
    std::vector<Frame> frames;
    VMStats stats;

    auto fail = [&](const std::string& msg) -> void {
        const std::string where = frames.empty() ? "main" : frames.back().fn->name;
        throw RuntimeError(msg + " (in " + where + ")");
    };
    auto push = [&](const Function& f, std::vector<int64_t> args, int resultDst) {
        Frame fr;
        fr.fn = &f;
        fr.values.assign(static_cast<size_t>(f.nvalues), 0);
        fr.args = std::move(args);
        fr.resultDst = resultDst;
        frames.push_back(std::move(fr));
    };
    // Move to `target`, evaluating its phis for the edge from the current block.
    auto enter = [&](Frame& fr, int target) {
        const Block& b = fr.fn->blocks[static_cast<size_t>(target)];
        std::vector<int64_t> incoming(b.phis.size(), 0);
        for (size_t i = 0; i < b.phis.size(); ++i) {
            const Inst& phi = b.phis[i];
            bool found = false;
            for (size_t k = 0; k < phi.args.size(); ++k)
                if (phi.phiPreds[k] == fr.block) {
                    incoming[i] = fr.values[static_cast<size_t>(phi.args[k])];
                    found = true;
                    break;
                }
            if (!found) fail("internal error: phi has no operand for the incoming edge");
        }
        for (size_t i = 0; i < b.phis.size(); ++i)
            fr.values[static_cast<size_t>(b.phis[i].dst)] = incoming[i];
        stats.steps += b.phis.size();
        fr.block = target;
        fr.ip = 0;
    };

    push(m.main, {}, -1);
    for (;;) {
        Frame& fr = frames.back();
        const Block& b = fr.fn->blocks[static_cast<size_t>(fr.block)];
        if (fr.ip >= b.insts.size()) fail("internal error: block has no terminator");
        if (++stats.steps > maxSteps)
            fail("step limit of " + std::to_string(maxSteps) +
                 " exceeded (infinite loop? raise it with --max-steps)");

        const Inst& in = b.insts[fr.ip++];
        auto val = [&](size_t i) { return fr.values[static_cast<size_t>(in.args[i])]; };
        auto set = [&](int64_t v) { fr.values[static_cast<size_t>(in.dst)] = v; };
        switch (in.op) {
            case IOp::Const: set(in.imm); break;
            case IOp::Param: set(fr.args.at(static_cast<size_t>(in.imm))); break;
            case IOp::Neg: set(wrapNeg(val(0))); break;
            case IOp::Bin: {
                int64_t r = 0;
                if (!evalBinary(in.bin, val(0), val(1), r)) fail("division by zero");
                set(r);
                break;
            }
            case IOp::Phi: fail("internal error: phi outside the phi list"); break;
            case IOp::LoadGlobal: set(globals.at(static_cast<size_t>(in.imm))); break;
            case IOp::StoreGlobal: globals.at(static_cast<size_t>(in.imm)) = val(0); break;
            case IOp::Print: out << val(0) << '\n'; break;
            case IOp::Jmp: enter(fr, in.t0); break;
            case IOp::Br: enter(fr, val(0) != 0 ? in.t0 : in.t1); break;
            case IOp::Call: {
                const Function& callee = m.funcs.at(static_cast<size_t>(in.imm));
                if (frames.size() > kMaxCallDepth)
                    fail("call depth limit of " + std::to_string(kMaxCallDepth) + " exceeded in '" +
                         callee.name + "' (infinite recursion?)");
                std::vector<int64_t> args;
                args.reserve(in.args.size());
                for (size_t i = 0; i < in.args.size(); ++i) args.push_back(val(i));
                push(callee, std::move(args), in.dst);  // invalidates `fr`
                ++stats.calls;
                if (frames.size() - 1 > stats.maxDepth) stats.maxDepth = frames.size() - 1;
                break;
            }
            case IOp::Ret: {
                int64_t v = val(0);
                int dst = fr.resultDst;
                frames.pop_back();
                if (frames.empty()) return stats;  // main returned: the program is done
                if (dst >= 0) frames.back().values[static_cast<size_t>(dst)] = v;
                break;
            }
        }
    }
}

}  // namespace ir
}  // namespace minic
