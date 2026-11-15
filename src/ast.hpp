// ast.hpp - Stage 2: the Abstract Syntax Tree.
//
// The parser turns the token list into a tree that mirrors the program's
// structure. Nodes own their children through std::unique_ptr, so the whole
// tree is freed automatically and can never be copied by accident.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lexer.hpp"

namespace minic {

// ---------------------------------------------------------------- expressions
enum class ExprKind { Number, Var, Unary, Binary, Call };

struct Expr {
    ExprKind kind;
    int line;
    virtual ~Expr() = default;

protected:
    Expr(ExprKind k, int l) : kind(k), line(l) {}
};
using ExprPtr = std::unique_ptr<Expr>;

struct NumberExpr : Expr {
    int64_t value;
    NumberExpr(int64_t v, int l) : Expr(ExprKind::Number, l), value(v) {}
};

// Sema resolves every variable reference to a numbered slot: either a global
// slot or a slot in the enclosing function's frame (`local`).
struct VarExpr : Expr {
    std::string name;
    int slot = -1;
    bool local = false;
    VarExpr(std::string n, int l) : Expr(ExprKind::Var, l), name(std::move(n)) {}
};

struct UnaryExpr : Expr {  // only unary minus exists
    ExprPtr operand;
    UnaryExpr(ExprPtr e, int l) : Expr(ExprKind::Unary, l), operand(std::move(e)) {}
};

struct BinaryExpr : Expr {
    Tok op;
    ExprPtr lhs, rhs;
    BinaryExpr(Tok o, ExprPtr a, ExprPtr b, int l)
        : Expr(ExprKind::Binary, l), op(o), lhs(std::move(a)), rhs(std::move(b)) {}
};

struct CallExpr : Expr {
    std::string name;
    std::vector<ExprPtr> args;
    int fnIndex = -1;  // filled in by sema
    CallExpr(std::string n, std::vector<ExprPtr> a, int l)
        : Expr(ExprKind::Call, l), name(std::move(n)), args(std::move(a)) {}
};

// ----------------------------------------------------------------- statements
enum class StmtKind { Let, Assign, Print, If, While, Block, Fn, Return, ExprStmt };

struct Stmt {
    StmtKind kind;
    int line;
    virtual ~Stmt() = default;

protected:
    Stmt(StmtKind k, int l) : kind(k), line(l) {}
};
using StmtPtr = std::unique_ptr<Stmt>;

struct LetStmt : Stmt {
    std::string name;
    ExprPtr init;
    int slot = -1;       // filled in by sema, like VarExpr
    bool local = false;
    LetStmt(std::string n, ExprPtr e, int l)
        : Stmt(StmtKind::Let, l), name(std::move(n)), init(std::move(e)) {}
};

struct AssignStmt : Stmt {
    std::string name;
    ExprPtr value;
    int slot = -1;
    bool local = false;
    AssignStmt(std::string n, ExprPtr e, int l)
        : Stmt(StmtKind::Assign, l), name(std::move(n)), value(std::move(e)) {}
};

struct PrintStmt : Stmt {
    ExprPtr value;
    PrintStmt(ExprPtr e, int l) : Stmt(StmtKind::Print, l), value(std::move(e)) {}
};

struct BlockStmt : Stmt {
    std::vector<StmtPtr> stmts;
    explicit BlockStmt(int l) : Stmt(StmtKind::Block, l) {}
};

struct IfStmt : Stmt {
    ExprPtr cond;
    StmtPtr thenBranch;
    StmtPtr elseBranch;  // may be null
    IfStmt(ExprPtr c, StmtPtr t, StmtPtr e, int l)
        : Stmt(StmtKind::If, l), cond(std::move(c)), thenBranch(std::move(t)),
          elseBranch(std::move(e)) {}
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    StmtPtr body;
    WhileStmt(ExprPtr c, StmtPtr b, int l)
        : Stmt(StmtKind::While, l), cond(std::move(c)), body(std::move(b)) {}
};

// `fn name(a, b) { ... }`. Only allowed at the top level of a program.
struct FnDecl : Stmt {
    std::string name;
    std::vector<std::string> params;
    std::unique_ptr<BlockStmt> body;
    int index = -1;                       // position in the function table (sema)
    std::vector<std::string> localNames;  // frame slot -> name; parameters come first (sema)
    FnDecl(std::string n, std::vector<std::string> p, std::unique_ptr<BlockStmt> b, int l)
        : Stmt(StmtKind::Fn, l), name(std::move(n)), params(std::move(p)), body(std::move(b)) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value;  // may be null: `return;` returns 0
    ReturnStmt(ExprPtr e, int l) : Stmt(StmtKind::Return, l), value(std::move(e)) {}
};

// A call used as a statement: `f(1, 2);`. The result is discarded.
struct ExprStmt : Stmt {
    ExprPtr expr;
    ExprStmt(ExprPtr e, int l) : Stmt(StmtKind::ExprStmt, l), expr(std::move(e)) {}
};

// A whole program is just a top-level block.
using Program = BlockStmt;

// ------------------------------------------------------------------ AST dump
inline const char* opText(Tok t) {
    switch (t) {
        case Tok::Plus: return "+";
        case Tok::Minus: return "-";
        case Tok::Star: return "*";
        case Tok::Slash: return "/";
        case Tok::Lt: return "<";
        case Tok::Gt: return ">";
        case Tok::Le: return "<=";
        case Tok::Ge: return ">=";
        case Tok::EqEq: return "==";
        case Tok::Ne: return "!=";
        default: return "?";
    }
}

inline std::string dumpExpr(const Expr& e) {
    switch (e.kind) {
        case ExprKind::Number:
            return std::to_string(static_cast<const NumberExpr&>(e).value);
        case ExprKind::Var:
            return static_cast<const VarExpr&>(e).name;
        case ExprKind::Unary:
            return "(neg " + dumpExpr(*static_cast<const UnaryExpr&>(e).operand) + ")";
        case ExprKind::Binary: {
            const auto& b = static_cast<const BinaryExpr&>(e);
            return std::string("(") + opText(b.op) + " " + dumpExpr(*b.lhs) + " " +
                   dumpExpr(*b.rhs) + ")";
        }
        case ExprKind::Call: {
            const auto& c = static_cast<const CallExpr&>(e);
            std::string out = "(call " + c.name;
            for (const auto& a : c.args) out += " " + dumpExpr(*a);
            return out + ")";
        }
    }
    return "?";
}

inline std::string dumpStmt(const Stmt& s, int depth = 0) {
    std::string pad(static_cast<size_t>(depth) * 2, ' ');
    switch (s.kind) {
        case StmtKind::Let: {
            const auto& n = static_cast<const LetStmt&>(s);
            return pad + "(let " + n.name + " " + dumpExpr(*n.init) + ")\n";
        }
        case StmtKind::Assign: {
            const auto& n = static_cast<const AssignStmt&>(s);
            return pad + "(set " + n.name + " " + dumpExpr(*n.value) + ")\n";
        }
        case StmtKind::Print:
            return pad + "(print " + dumpExpr(*static_cast<const PrintStmt&>(s).value) + ")\n";
        case StmtKind::Block: {
            std::string out = pad + "(block\n";
            for (const auto& c : static_cast<const BlockStmt&>(s).stmts)
                out += dumpStmt(*c, depth + 1);
            return out + pad + ")\n";
        }
        case StmtKind::If: {
            const auto& n = static_cast<const IfStmt&>(s);
            std::string out = pad + "(if " + dumpExpr(*n.cond) + "\n" +
                              dumpStmt(*n.thenBranch, depth + 1);
            if (n.elseBranch) out += dumpStmt(*n.elseBranch, depth + 1);
            return out + pad + ")\n";
        }
        case StmtKind::While: {
            const auto& n = static_cast<const WhileStmt&>(s);
            return pad + "(while " + dumpExpr(*n.cond) + "\n" + dumpStmt(*n.body, depth + 1) +
                   pad + ")\n";
        }
        case StmtKind::Fn: {
            const auto& n = static_cast<const FnDecl&>(s);
            std::string out = pad + "(fn " + n.name + " (";
            for (size_t i = 0; i < n.params.size(); ++i) out += (i ? " " : "") + n.params[i];
            return out + ")\n" + dumpStmt(*n.body, depth + 1) + pad + ")\n";
        }
        case StmtKind::Return: {
            const auto& n = static_cast<const ReturnStmt&>(s);
            return pad + "(return" + (n.value ? " " + dumpExpr(*n.value) : std::string()) + ")\n";
        }
        case StmtKind::ExprStmt:
            return pad + "(expr " + dumpExpr(*static_cast<const ExprStmt&>(s).expr) + ")\n";
    }
    return "";
}

}  // namespace minic
