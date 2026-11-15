// bytecode.hpp - the instruction set shared by the code generator, the
// disassembler, the optimizer and the virtual machine.
//
// minic targets a *stack machine*: instructions pop operands from a stack and
// push results back. (PTX, NVIDIA's GPU virtual ISA, is register based instead:
// it has unlimited virtual registers that `ptxas` later maps onto real ones.
// A stack VM is simply easier to generate code for, which is why we use it.)
#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "lexer.hpp"

namespace minic {

enum class Op : uint8_t {
    Push,        // push <arg>
    Load,        // push global variable slot <arg>
    Store,       // pop into global variable slot <arg>
    LoadLocal,   // push slot <arg> of the current call frame
    StoreLocal,  // pop into slot <arg> of the current call frame
    Add, Sub, Mul, Div,
    Neg,
    Lt, Gt, Le, Ge, Eq, Ne,   // push 1 or 0
    Jmp,         // pc = <arg>
    JmpIfFalse,  // pop; if it was 0, pc = <arg>
    Print,       // pop and print
    Pop,         // discard the top of the stack (a call used as a statement)
    Call,        // call function <arg> (index into the function table); arguments are on the stack
    Ret,         // pop the return value, drop the frame, push the value for the caller
    Halt
};

struct Instr {
    Op op;
    int64_t arg = 0;
};

// One entry per function. CALL refers to a function by its index here, so the
// code generator never has to backpatch a call to a function it has not emitted yet.
struct FuncInfo {
    std::string name;
    size_t entry = 0;                 // index of the first instruction
    int nparams = 0;
    std::vector<std::string> locals;  // frame slot -> name; parameters come first
};

struct BytecodeProgram {
    std::vector<Instr> code;
    std::vector<std::string> vars;  // slot index -> variable name (for the disassembler)
    std::vector<FuncInfo> funcs;
};

inline const char* opName(Op op) {
    switch (op) {
        case Op::Push: return "PUSH";
        case Op::Load: return "LOAD";
        case Op::Store: return "STORE";
        case Op::Add: return "ADD";
        case Op::Sub: return "SUB";
        case Op::Mul: return "MUL";
        case Op::Div: return "DIV";
        case Op::Neg: return "NEG";
        case Op::Lt: return "LT";
        case Op::Gt: return "GT";
        case Op::Le: return "LE";
        case Op::Ge: return "GE";
        case Op::Eq: return "EQ";
        case Op::Ne: return "NE";
        case Op::Jmp: return "JMP";
        case Op::JmpIfFalse: return "JMP_IF_FALSE";
        case Op::Print: return "PRINT";
        case Op::LoadLocal: return "LOAD_LOCAL";
        case Op::StoreLocal: return "STORE_LOCAL";
        case Op::Pop: return "POP";
        case Op::Call: return "CALL";
        case Op::Ret: return "RET";
        case Op::Halt: return "HALT";
    }
    return "?";
}

inline bool hasOperand(Op op) {
    return op == Op::Push || op == Op::Load || op == Op::Store || op == Op::Jmp ||
           op == Op::JmpIfFalse || op == Op::LoadLocal || op == Op::StoreLocal || op == Op::Call;
}

// Map a binary-operator token to its opcode.
inline Op binaryOpFor(Tok t) {
    switch (t) {
        case Tok::Plus: return Op::Add;
        case Tok::Minus: return Op::Sub;
        case Tok::Star: return Op::Mul;
        case Tok::Slash: return Op::Div;
        case Tok::Lt: return Op::Lt;
        case Tok::Gt: return Op::Gt;
        case Tok::Le: return Op::Le;
        case Tok::Ge: return Op::Ge;
        case Tok::EqEq: return Op::Eq;
        case Tok::Ne: return Op::Ne;
        default: return Op::Halt;  // unreachable for valid ASTs
    }
}

// ---------------------------------------------------------------------------
// Arithmetic semantics. The optimizer (constant folding) and the VM BOTH call
// these, so folding `2 * 3` at compile time can never disagree with computing
// it at run time. Integer arithmetic wraps around (two's complement) instead of
// invoking signed-overflow undefined behaviour.
// ---------------------------------------------------------------------------
inline int64_t wrapNeg(int64_t a) {
    return static_cast<int64_t>(uint64_t{0} - static_cast<uint64_t>(a));
}

// Returns false on division by zero (the caller decides how to react).
inline bool evalBinary(Op op, int64_t a, int64_t b, int64_t& out) {
    const uint64_t ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
    switch (op) {
        case Op::Add: out = static_cast<int64_t>(ua + ub); return true;
        case Op::Sub: out = static_cast<int64_t>(ua - ub); return true;
        case Op::Mul: out = static_cast<int64_t>(ua * ub); return true;
        case Op::Div:
            if (b == 0) return false;
            out = (a == INT64_MIN && b == -1) ? INT64_MIN : a / b;
            return true;
        case Op::Lt: out = a < b; return true;
        case Op::Gt: out = a > b; return true;
        case Op::Le: out = a <= b; return true;
        case Op::Ge: out = a >= b; return true;
        case Op::Eq: out = a == b; return true;
        case Op::Ne: out = a != b; return true;
        default: return false;
    }
}

inline std::string disassemble(const BytecodeProgram& p) {
    std::ostringstream os;
    const FuncInfo* cur = nullptr;  // function whose body we are inside, if any
    for (size_t i = 0; i < p.code.size(); ++i) {
        const Instr& in = p.code[i];
        for (const FuncInfo& f : p.funcs)
            if (f.entry == i) {
                cur = &f;
                os << "\n" << f.name << ":    ; " << f.nparams << " parameter(s), " << f.locals.size()
                   << " frame slot(s)\n";
            }
        os << (i < 10 ? "  " : i < 100 ? " " : "") << i << "  " << opName(in.op);
        if (hasOperand(in.op)) {
            os << ' ' << in.arg;
            if ((in.op == Op::Load || in.op == Op::Store) && in.arg >= 0 &&
                static_cast<size_t>(in.arg) < p.vars.size())
                os << "    ; " << p.vars[static_cast<size_t>(in.arg)];
            if ((in.op == Op::LoadLocal || in.op == Op::StoreLocal) && cur && in.arg >= 0 &&
                static_cast<size_t>(in.arg) < cur->locals.size())
                os << "    ; " << cur->locals[static_cast<size_t>(in.arg)];
            if (in.op == Op::Call && in.arg >= 0 && static_cast<size_t>(in.arg) < p.funcs.size())
                os << "    ; " << p.funcs[static_cast<size_t>(in.arg)].name;
        }
        os << '\n';
    }
    return os.str();
}

}  // namespace minic
