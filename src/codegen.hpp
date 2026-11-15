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
//
// Layout: the top-level statements come first and end in HALT; function bodies
// follow. A call pushes its arguments left to right and emits CALL <index>; the
// VM looks the entry address up in the function table, so calls need no patching.
// Every function body ends in `PUSH 0; RET`, so falling off the end returns 0.
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
        for (const auto& s : prog.stmts)
            if (s->kind == StmtKind::Fn) function(static_cast<const FnDecl&>(*s));
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

    void function(const FnDecl& f) {
        // Sema numbered the functions in source order, which is the order we emit them in.
        FuncInfo info;
        info.name = f.name;
        info.entry = here();
        info.nparams = static_cast<int>(f.params.size());
        info.locals = f.localNames;
        out_.funcs.push_back(std::move(info));
        stmt(*f.body);
        emit(Op::Push, 0);
        emit(Op::Ret);
    }

    void expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Number:
                emit(Op::Push, static_cast<const NumberExpr&>(e).value);
                break;
            case ExprKind::Var: {
                const auto& v = static_cast<const VarExpr&>(e);
                emit(v.local ? Op::LoadLocal : Op::Load, v.slot);
                break;
            }
            case ExprKind::Call: {
                const auto& c = static_cast<const CallExpr&>(e);
                for (const auto& a : c.args) expr(*a);
                emit(Op::Call, c.fnIndex);
                break;
            }
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
                emit(n.local ? Op::StoreLocal : Op::Store, n.slot);
                break;
            }
            case StmtKind::Assign: {
                const auto& n = static_cast<const AssignStmt&>(s);
                expr(*n.value);
                emit(n.local ? Op::StoreLocal : Op::Store, n.slot);
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
            case StmtKind::Fn:
                break;  // emitted after the top-level code, see generate()
            case StmtKind::Return: {
                const auto& n = static_cast<const ReturnStmt&>(s);
                if (n.value) expr(*n.value);
                else emit(Op::Push, 0);
                emit(Op::Ret);
                break;
            }
            case StmtKind::ExprStmt:
                expr(*static_cast<const ExprStmt&>(s).expr);
                emit(Op::Pop);
                break;
        }
    }
};

}  // namespace minic
