// sema.hpp - semantic analysis: check that every variable is declared before
// use and assign each variable a numbered slot.
//
// This runs BEFORE the optimizer on purpose. The optimizer may delete code
// (e.g. `if (0) { let a = 1; }`), and deleting a declaration must never change
// whether the rest of the program is accepted. By building the symbol table
// from the original program, optimization cannot change program meaning.
//
// Scoping is deliberately flat (one scope for the whole program), like early C
// or classic BASIC: `let` declares a variable once, blocks do not introduce new
// scopes. It keeps the compiler small; per-block scopes are a good extension.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hpp"

namespace minic {

struct SymbolTable {
    std::unordered_map<std::string, int> slot;
    std::vector<std::string> names;  // slot -> name
};

class Sema {
public:
    SymbolTable analyze(const Program& prog) {
        syms_ = SymbolTable{};
        stmt(prog);
        return std::move(syms_);
    }

private:
    SymbolTable syms_;

    void declare(const std::string& name, int line) {
        if (syms_.slot.count(name))
            throw CompileError(line, "variable '" + name + "' is already declared");
        syms_.slot[name] = static_cast<int>(syms_.names.size());
        syms_.names.push_back(name);
    }

    void requireDeclared(const std::string& name, int line, const char* what) {
        if (!syms_.slot.count(name))
            throw CompileError(line, std::string(what) + " variable '" + name + "'");
    }

    void expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Number: break;
            case ExprKind::Var: {
                const auto& v = static_cast<const VarExpr&>(e);
                requireDeclared(v.name, v.line, "undefined");
                break;
            }
            case ExprKind::Unary:
                expr(*static_cast<const UnaryExpr&>(e).operand);
                break;
            case ExprKind::Binary: {
                const auto& b = static_cast<const BinaryExpr&>(e);
                expr(*b.lhs);
                expr(*b.rhs);
                break;
            }
        }
    }

    void stmt(const Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                const auto& n = static_cast<const LetStmt&>(s);
                expr(*n.init);  // check the initializer first: `let x = x;` is an error
                declare(n.name, n.line);
                break;
            }
            case StmtKind::Assign: {
                const auto& n = static_cast<const AssignStmt&>(s);
                requireDeclared(n.name, n.line, "assignment to undeclared");
                expr(*n.value);
                break;
            }
            case StmtKind::Print:
                expr(*static_cast<const PrintStmt&>(s).value);
                break;
            case StmtKind::Block:
                for (const auto& c : static_cast<const BlockStmt&>(s).stmts) stmt(*c);
                break;
            case StmtKind::If: {
                const auto& n = static_cast<const IfStmt&>(s);
                expr(*n.cond);
                stmt(*n.thenBranch);
                if (n.elseBranch) stmt(*n.elseBranch);
                break;
            }
            case StmtKind::While: {
                const auto& n = static_cast<const WhileStmt&>(s);
                expr(*n.cond);
                stmt(*n.body);
                break;
            }
        }
    }
};

}  // namespace minic
