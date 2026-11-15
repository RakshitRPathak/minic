// sema.hpp - semantic analysis: check that every variable and function is
// declared before use, check call arity, and resolve every variable reference
// to a numbered slot (written back into the AST).
//
// This runs BEFORE the optimizer on purpose. The optimizer may delete code
// (e.g. `if (0) { let a = 1; }`), and deleting a declaration must never change
// whether the rest of the program is accepted. By resolving names on the
// original program, optimization cannot change program meaning.
//
// Scoping is deliberately flat, like early C or classic BASIC: there is one
// global scope, and one scope per function (its parameters and every `let` in
// its body). Blocks do not introduce new scopes. Inside a function a name
// resolves to the function's own variable if one has been declared, otherwise
// to a global declared earlier in the file. Functions live in their own
// namespace and are collected up front, so they can be called before their
// definition and can be mutually recursive.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ast.hpp"

namespace minic {

struct FuncSym {
    std::string name;
    int nparams = 0;
};

struct SymbolTable {
    std::unordered_map<std::string, int> slot;  // global variables
    std::vector<std::string> names;             // global slot -> name
    std::unordered_map<std::string, int> fnIndex;
    std::vector<FuncSym> funcs;
};

class Sema {
public:
    SymbolTable analyze(Program& prog) {
        syms_ = SymbolTable{};
        fn_ = nullptr;
        // Pass 1: collect function signatures.
        for (const auto& s : prog.stmts) {
            if (s->kind != StmtKind::Fn) continue;
            auto& f = static_cast<FnDecl&>(*s);
            if (syms_.fnIndex.count(f.name))
                throw CompileError(f.line, "function '" + f.name + "' is already defined");
            f.index = static_cast<int>(syms_.funcs.size());
            syms_.fnIndex[f.name] = f.index;
            syms_.funcs.push_back(FuncSym{f.name, static_cast<int>(f.params.size())});
        }
        // Pass 2: check everything in source order.
        for (auto& s : prog.stmts) stmt(*s);
        return std::move(syms_);
    }

private:
    SymbolTable syms_;
    FnDecl* fn_ = nullptr;  // function being analyzed, or null at the top level
    std::unordered_map<std::string, int> locals_;

    void declare(const std::string& name, int line, int& slot, bool& local) {
        if (fn_) {
            if (locals_.count(name))
                throw CompileError(line, "variable '" + name + "' is already declared");
            slot = static_cast<int>(fn_->localNames.size());
            local = true;
            locals_[name] = slot;
            fn_->localNames.push_back(name);
            return;
        }
        if (syms_.slot.count(name))
            throw CompileError(line, "variable '" + name + "' is already declared");
        slot = static_cast<int>(syms_.names.size());
        local = false;
        syms_.slot[name] = slot;
        syms_.names.push_back(name);
    }

    void resolve(const std::string& name, int line, const char* what, int& slot, bool& local) {
        if (fn_) {
            auto it = locals_.find(name);
            if (it != locals_.end()) { slot = it->second; local = true; return; }
        }
        auto it = syms_.slot.find(name);
        if (it == syms_.slot.end())
            throw CompileError(line, std::string(what) + " variable '" + name + "'");
        slot = it->second;
        local = false;
    }

    void expr(Expr& e) {
        switch (e.kind) {
            case ExprKind::Number: break;
            case ExprKind::Var: {
                auto& v = static_cast<VarExpr&>(e);
                resolve(v.name, v.line, "undefined", v.slot, v.local);
                break;
            }
            case ExprKind::Unary:
                expr(*static_cast<UnaryExpr&>(e).operand);
                break;
            case ExprKind::Binary: {
                auto& b = static_cast<BinaryExpr&>(e);
                expr(*b.lhs);
                expr(*b.rhs);
                break;
            }
            case ExprKind::Call: {
                auto& c = static_cast<CallExpr&>(e);
                auto it = syms_.fnIndex.find(c.name);
                if (it == syms_.fnIndex.end())
                    throw CompileError(c.line, "undefined function '" + c.name + "'");
                const FuncSym& f = syms_.funcs[static_cast<size_t>(it->second)];
                if (static_cast<int>(c.args.size()) != f.nparams)
                    throw CompileError(c.line, "function '" + c.name + "' expects " +
                                                   std::to_string(f.nparams) + " argument(s), got " +
                                                   std::to_string(c.args.size()));
                c.fnIndex = it->second;
                for (auto& a : c.args) expr(*a);
                break;
            }
        }
    }

    void stmt(Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: {
                auto& n = static_cast<LetStmt&>(s);
                expr(*n.init);  // check the initializer first: `let x = x;` is an error
                declare(n.name, n.line, n.slot, n.local);
                break;
            }
            case StmtKind::Assign: {
                auto& n = static_cast<AssignStmt&>(s);
                resolve(n.name, n.line, "assignment to undeclared", n.slot, n.local);
                expr(*n.value);
                break;
            }
            case StmtKind::Print:
                expr(*static_cast<PrintStmt&>(s).value);
                break;
            case StmtKind::Block:
                for (auto& c : static_cast<BlockStmt&>(s).stmts) stmt(*c);
                break;
            case StmtKind::If: {
                auto& n = static_cast<IfStmt&>(s);
                expr(*n.cond);
                stmt(*n.thenBranch);
                if (n.elseBranch) stmt(*n.elseBranch);
                break;
            }
            case StmtKind::While: {
                auto& n = static_cast<WhileStmt&>(s);
                expr(*n.cond);
                stmt(*n.body);
                break;
            }
            case StmtKind::Fn: {
                auto& f = static_cast<FnDecl&>(s);
                fn_ = &f;
                locals_.clear();
                f.localNames.clear();
                for (const auto& p : f.params) {
                    if (locals_.count(p))
                        throw CompileError(f.line, "duplicate parameter '" + p + "' in function '" +
                                                       f.name + "'");
                    locals_[p] = static_cast<int>(f.localNames.size());
                    f.localNames.push_back(p);
                }
                stmt(*f.body);
                fn_ = nullptr;
                locals_.clear();
                break;
            }
            case StmtKind::Return: {
                auto& n = static_cast<ReturnStmt&>(s);
                if (!fn_) throw CompileError(n.line, "'return' outside of a function");
                if (n.value) expr(*n.value);
                break;
            }
            case StmtKind::ExprStmt:
                expr(*static_cast<ExprStmt&>(s).expr);
                break;
        }
    }
};

}  // namespace minic
