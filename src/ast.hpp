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
enum class ExprKind { Number, Var, Unary, Binary };

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

struct VarExpr : Expr {
    std::string name;
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

// ----------------------------------------------------------------- statements
enum class StmtKind { Let, Assign, Print, If, While, Block };

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
    LetStmt(std::string n, ExprPtr e, int l)
        : Stmt(StmtKind::Let, l), name(std::move(n)), init(std::move(e)) {}
};

struct AssignStmt : Stmt {
    std::string name;
    ExprPtr value;
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
    }
    return "";
}

}  // namespace minic
