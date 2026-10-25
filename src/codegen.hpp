// codegen.hpp - Stage 4: walk the AST and emit bytecode.
//
// The interesting part is BACKPATCHING. When we emit the jump at the top of an
// `if`, we do not yet know where the else-branch will start, because we have not
// generated the then-branch yet. So we emit the jump with a placeholder target,
// remember its index, and fill the real address in once it is known:
//
//     JMP_IF_FALSE ?      <- emitted first, target unknown
//     ...then-branch...
//     JMP ?               <- skips else, target unknown
//     ...else-branch...   <- first JMP_IF_FALSE gets patched to point here
//     (end)               <- second JMP gets patched to point here
//
// Assemblers and linkers solve the same problem with labels and relocations.
#pragma once

#include "ast.hpp"
#include "bytecode.hpp"
#include "sema.hpp"

namespace minic {

class CodeGen {
public:
    explicit CodeGen(const SymbolTable& syms) : syms_(syms) {}

    BytecodeProgram generate(const Program& prog) {
        out_ = BytecodeProgram{};
        out_.vars = syms_.names;
        stmt(prog);
        emit(Op::Halt);
        return std::move(out_);
    }

private:
    const SymbolTable& syms_;
    BytecodeProgram out_;

    size_t here() const { return out_.code.size(); }

    size_t emit(Op op, int64_t arg = 0) {
        out_.code.push_back(Instr{op, arg});
        return out_.code.size() - 1;
    }

    void patch(size_t at, size_t target) { out_.code[at].arg = static_cast<int64_t>(target); }

    int64_t slotOf(const std::string& name) const { return syms_.slot.at(name); }

    void expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Number:
                emit(Op::Push, static_cast<const NumberExpr&>(e).value);
                break;
            case ExprKind::Var:
                emit(Op::Load, slotOf(static_cast<const VarExpr&>(e).name));
                break;
            case ExprKind::Unary:
                expr(*static_cast<const UnaryExpr&>(e).operand);
                emit(Op::Neg);
                break;
            case ExprKind::Binary: {
                const auto& b = static_cast<const BinaryExpr&>(e);
                expr(*b.lhs);  // left operand first, then right: a stack machine
                expr(*b.rhs);  // evaluates in postfix order
                emit(binaryOpFor(b.op));
                break;
            }
        }
    }

    void stmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                const auto& n = static_cast<const LetStmt&>(s);
                expr(*n.init);
                emit(Op::Store, slotOf(n.name));
                break;
            }
            case StmtKind::Assign: {
                const auto& n = static_cast<const AssignStmt&>(s);
                expr(*n.value);
                emit(Op::Store, slotOf(n.name));
                break;
            }
            case StmtKind::Print:
                expr(*static_cast<const PrintStmt&>(s).value);
                emit(Op::Print);
                break;
            case StmtKind::Block:
                for (const auto& c : static_cast<const BlockStmt&>(s).stmts) stmt(*c);
                break;
            case StmtKind::If: {
                const auto& n = static_cast<const IfStmt&>(s);
                expr(*n.cond);
                size_t jumpToElse = emit(Op::JmpIfFalse, -1);
                stmt(*n.thenBranch);
                if (n.elseBranch) {
                    size_t jumpToEnd = emit(Op::Jmp, -1);
                    patch(jumpToElse, here());
                    stmt(*n.elseBranch);
                    patch(jumpToEnd, here());
                } else {
                    patch(jumpToElse, here());
                }
                break;
            }
            case StmtKind::While: {
                const auto& n = static_cast<const WhileStmt&>(s);
                size_t loopStart = here();
                expr(*n.cond);
                size_t exitJump = emit(Op::JmpIfFalse, -1);
                stmt(*n.body);
                emit(Op::Jmp, static_cast<int64_t>(loopStart));
                patch(exitJump, here());
                break;
            }
        }
    }
};

}  // namespace minic
