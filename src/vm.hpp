// vm.hpp - Stage 5: the virtual machine that executes bytecode.
//
// A classic fetch-decode-execute loop: read the instruction at `pc`, switch on
// its opcode, update the stack / variables / pc, repeat until HALT.
//
// Function calls use a second stack of frames. CALL moves the arguments from
// the operand stack into a fresh block of frame slots and remembers where to
// come back to; RET drops that block and resumes the caller. Recursion depth is
// capped so a runaway recursion is reported instead of exhausting memory.
#pragma once

#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bytecode.hpp"

namespace minic {

struct RuntimeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct VMStats {
    uint64_t steps = 0;
    uint64_t calls = 0;
    size_t maxDepth = 0;
};

constexpr size_t kMaxCallDepth = 10000;

inline VMStats runVM(const BytecodeProgram& prog, std::ostream& out, uint64_t maxSteps) {
    std::vector<int64_t> stack;
    stack.reserve(64);
    std::vector<int64_t> vars(prog.vars.size(), 0);  // uninitialised slots read as 0
    struct Frame {
        size_t returnPc;
        size_t callerBase;
    };
    std::vector<Frame> frames;
    std::vector<int64_t> locals;  // frame slots of every active call, innermost last
    size_t base = 0;              // where the current frame's slots start in `locals`
    VMStats stats;
    size_t pc = 0;

    auto fail = [&](const std::string& msg) -> void {
        throw RuntimeError(msg + " (at instruction " + std::to_string(pc) + ")");
    };
    auto pop = [&]() -> int64_t {
        if (stack.empty()) fail("stack underflow");
        int64_t v = stack.back();
        stack.pop_back();
        return v;
    };

    for (;;) {
        if (pc >= prog.code.size()) fail("program counter ran off the end of the program");
        if (++stats.steps > maxSteps)
            fail("step limit of " + std::to_string(maxSteps) +
                 " exceeded (infinite loop? raise it with --max-steps)");

        const Instr& in = prog.code[pc];
        switch (in.op) {
            case Op::Push:
                stack.push_back(in.arg);
                ++pc;
                break;
            case Op::Load:
                stack.push_back(vars.at(static_cast<size_t>(in.arg)));
                ++pc;
                break;
            case Op::Store:
                vars.at(static_cast<size_t>(in.arg)) = pop();
                ++pc;
                break;
            case Op::Neg:
                stack.push_back(wrapNeg(pop()));
                ++pc;
                break;
            case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
            case Op::Lt: case Op::Gt: case Op::Le: case Op::Ge: case Op::Eq: case Op::Ne: {
                int64_t b = pop();  // right operand was pushed last
                int64_t a = pop();
                int64_t r = 0;
                if (!evalBinary(in.op, a, b, r)) fail("division by zero");
                stack.push_back(r);
                ++pc;
                break;
            }
            case Op::Jmp:
                pc = static_cast<size_t>(in.arg);
                break;
            case Op::JmpIfFalse:
                pc = (pop() == 0) ? static_cast<size_t>(in.arg) : pc + 1;
                break;
            case Op::Print:
                out << pop() << '\n';
                ++pc;
                break;
            case Op::LoadLocal:
                stack.push_back(locals.at(base + static_cast<size_t>(in.arg)));
                ++pc;
                break;
            case Op::StoreLocal:
                locals.at(base + static_cast<size_t>(in.arg)) = pop();
                ++pc;
                break;
            case Op::Pop:
                pop();
                ++pc;
                break;
            case Op::Call: {
                const FuncInfo& f = prog.funcs.at(static_cast<size_t>(in.arg));
                if (frames.size() >= kMaxCallDepth)
                    fail("call depth limit of " + std::to_string(kMaxCallDepth) +
                         " exceeded in '" + f.name + "' (infinite recursion?)");
                const size_t newBase = locals.size();
                locals.resize(newBase + f.locals.size(), 0);  // uninitialised slots read as 0
                for (int i = f.nparams - 1; i >= 0; --i)      // last argument is on top
                    locals[newBase + static_cast<size_t>(i)] = pop();
                frames.push_back(Frame{pc + 1, base});
                base = newBase;
                pc = f.entry;
                ++stats.calls;
                if (frames.size() > stats.maxDepth) stats.maxDepth = frames.size();
                break;
            }
            case Op::Ret: {
                if (frames.empty()) fail("return outside of a function call");
                int64_t value = pop();
                locals.resize(base);
                pc = frames.back().returnPc;
                base = frames.back().callerBase;
                frames.pop_back();
                stack.push_back(value);
                break;
            }
            case Op::Halt:
                return stats;
        }
    }
}

}  // namespace minic
