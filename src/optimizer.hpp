// optimizer.hpp - Stage 3: AST-level optimizations.
//
//   1. Constant folding:  2 * 3 + x      ->  6 + x
//   2. Dead-branch elimination:  if (0) {...} else {...}  ->  the else block
//   3. Dead-loop elimination:    while (0) {...}          ->  nothing
//   4. Unreachable code:  statements after a `return` in the same block
//
// Calls are never folded or removed: a function may print or change a global.
//
// Folding never evaluates a division by zero: `1 / 0` is left alone so the
// program still fails at run time exactly as the unoptimized program does.
// Constant folding uses the same evalBinary() as the VM, so results agree.
#pragma once

#include <memory>
#include <utility>

#include "ast.hpp"
#include "bytecode.hpp"

namespace minic {

struct OptStats {
    int foldedExprs = 0;
    int deadBranches = 0;
    int deadLoops = 0;
    int unreachableStmts = 0;
};

class Optimizer {
public:
    explicit Optimizer(OptStats& stats) : stats_(stats) {}

    void run(Program& prog) { optimizeChildren(prog); }

private:
    OptStats& stats_;

    static bool isNumber(const ExprPtr& e) { return e->kind == ExprKind::Number; }
    static int64_t numberValue(const ExprPtr& e) {
        return static_cast<const NumberExpr&>(*e).value;
    }

    ExprPtr fold(ExprPtr e) {
        switch (e->kind) {
            case ExprKind::Number:
            case ExprKind::Var:
                return e;
            case ExprKind::Unary: {
                auto& u = static_cast<UnaryExpr&>(*e);
                u.operand = fold(std::move(u.operand));
                if (isNumber(u.operand)) {
                    ++stats_.foldedExprs;
                    return std::make_unique<NumberExpr>(wrapNeg(numberValue(u.operand)), u.line);
                }
                return e;
            }
            case ExprKind::Binary: {
                auto& b = static_cast<BinaryExpr&>(*e);
                b.lhs = fold(std::move(b.lhs));
                b.rhs = fold(std::move(b.rhs));
                if (isNumber(b.lhs) && isNumber(b.rhs)) {
                    int64_t result = 0;
                    if (evalBinary(binaryOpFor(b.op), numberValue(b.lhs), numberValue(b.rhs),
                                   result)) {
                        ++stats_.foldedExprs;
                        return std::make_unique<NumberExpr>(result, b.line);
                    }
                    // division by zero: leave it for the VM to report
                }
                return e;
            }
            case ExprKind::Call: {
                auto& c = static_cast<CallExpr&>(*e);
                for (auto& a : c.args) a = fold(std::move(a));
                return e;
            }
        }
        return e;
    }

    StmtPtr emptyBlock(int line) { return std::make_unique<BlockStmt>(line); }

    static bool isEmptyBlock(const StmtPtr& s) {
        return s->kind == StmtKind::Block && static_cast<const BlockStmt&>(*s).stmts.empty();
    }

    void optimizeChildren(BlockStmt& blk) {
        std::vector<StmtPtr> kept;
        kept.reserve(blk.stmts.size());
        for (size_t i = 0; i < blk.stmts.size(); ++i) {
            StmtPtr o = optimize(std::move(blk.stmts[i]));
            if (isEmptyBlock(o)) continue;
            const bool isReturn = o->kind == StmtKind::Return;
            kept.push_back(std::move(o));
            if (isReturn) {  // nothing after a return in the same block can run
                stats_.unreachableStmts += static_cast<int>(blk.stmts.size() - i - 1);
                break;
            }
        }
        blk.stmts = std::move(kept);
    }

    StmtPtr optimize(StmtPtr s) {
        switch (s->kind) {
            case StmtKind::Let: {
                auto& n = static_cast<LetStmt&>(*s);
                n.init = fold(std::move(n.init));
                return s;
            }
            case StmtKind::Assign: {
                auto& n = static_cast<AssignStmt&>(*s);
                n.value = fold(std::move(n.value));
                return s;
            }
            case StmtKind::Print: {
                auto& n = static_cast<PrintStmt&>(*s);
                n.value = fold(std::move(n.value));
                return s;
            }
            case StmtKind::Block:
                optimizeChildren(static_cast<BlockStmt&>(*s));
                return s;
            case StmtKind::If: {
                auto& n = static_cast<IfStmt&>(*s);
                n.cond = fold(std::move(n.cond));
                n.thenBranch = optimize(std::move(n.thenBranch));
                if (n.elseBranch) n.elseBranch = optimize(std::move(n.elseBranch));
                if (isNumber(n.cond)) {
                    ++stats_.deadBranches;
                    if (numberValue(n.cond) != 0) return std::move(n.thenBranch);
                    if (n.elseBranch) return std::move(n.elseBranch);
                    return emptyBlock(n.line);
                }
                return s;
            }
            case StmtKind::While: {
                auto& n = static_cast<WhileStmt&>(*s);
                n.cond = fold(std::move(n.cond));
                n.body = optimize(std::move(n.body));
                if (isNumber(n.cond) && numberValue(n.cond) == 0) {
                    ++stats_.deadLoops;
                    return emptyBlock(n.line);
                }
                return s;
            }
            case StmtKind::Fn:
                optimizeChildren(*static_cast<FnDecl&>(*s).body);
                return s;
            case StmtKind::Return: {
                auto& n = static_cast<ReturnStmt&>(*s);
                if (n.value) n.value = fold(std::move(n.value));
                return s;
            }
            case StmtKind::ExprStmt: {
                auto& n = static_cast<ExprStmt&>(*s);
                n.expr = fold(std::move(n.expr));
                return s;
            }
        }
        return s;
    }
};

}  // namespace minic
