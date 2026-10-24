// parser.hpp - Stage 2: recursive descent parser (tokens -> AST).
//
// Grammar (lowest precedence at the top, one function per level):
//
//   program    := statement* END
//   statement  := "let" IDENT "=" expr ";"
//               | IDENT "=" expr ";"
//               | "print" expr ";"
//               | "if" "(" expr ")" statement ( "else" statement )?
//               | "while" "(" expr ")" statement
//               | "{" statement* "}"
//   expr       := equality
//   equality   := comparison ( ("==" | "!=") comparison )*
//   comparison := term ( ("<" | ">" | "<=" | ">=") term )*
//   term       := factor ( ("+" | "-") factor )*
//   factor     := unary ( ("*" | "/") unary )*
//   unary      := "-" unary | primary
//   primary    := NUMBER | IDENT | "(" expr ")"
//
// Why this gives the right precedence: `term` calls `factor` for each operand,
// so for 2 + 3 * 4 the `3 * 4` is grouped by `factor` *before* `term` ever sees
// the '+'. Operators that bind tighter live deeper in the call chain.
#pragma once

#include <utility>
#include <vector>

#include "ast.hpp"
#include "lexer.hpp"

namespace minic {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

    std::unique_ptr<Program> parseProgram() {
        auto prog = std::make_unique<Program>(1);
        while (peek().type != Tok::End) prog->stmts.push_back(statement());
        return prog;
    }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;

    // ------------------------------------------------------------ token helpers
    const Token& peek(size_t ahead = 0) const {
        size_t i = pos_ + ahead;
        return i < toks_.size() ? toks_[i] : toks_.back();  // back() is always End
    }
    const Token& advance() {
        const Token& t = peek();
        if (t.type != Tok::End) ++pos_;
        return t;
    }
    bool match(Tok t) {
        if (peek().type == t) { advance(); return true; }
        return false;
    }
    const Token& expect(Tok t, const char* context) {
        if (peek().type != t) {
            throw CompileError(peek().line, std::string("expected ") + tokName(t) + " " +
                                                context + ", found " + tokName(peek().type));
        }
        return advance();
    }

    // --------------------------------------------------------------- statements
    StmtPtr statement() {
        const Token& t = peek();
        switch (t.type) {
            case Tok::Let:    return letStatement();
            case Tok::Print:  return printStatement();
            case Tok::If:     return ifStatement();
            case Tok::While:  return whileStatement();
            case Tok::LBrace: return block();
            case Tok::Ident:
                if (peek(1).type == Tok::Assign) return assignStatement();
                break;
            default: break;
        }
        throw CompileError(t.line, std::string("unexpected ") + tokName(t.type) +
                                       " at start of statement");
    }

    StmtPtr letStatement() {
        int line = advance().line;  // 'let'
        std::string name = expect(Tok::Ident, "after 'let'").text;
        expect(Tok::Assign, "after variable name");
        ExprPtr init = expression();
        expect(Tok::Semi, "after expression");
        return std::make_unique<LetStmt>(std::move(name), std::move(init), line);
    }

    StmtPtr assignStatement() {
        const Token& id = advance();
        std::string name = id.text;
        int line = id.line;
        expect(Tok::Assign, "in assignment");
        ExprPtr value = expression();
        expect(Tok::Semi, "after expression");
        return std::make_unique<AssignStmt>(std::move(name), std::move(value), line);
    }

    StmtPtr printStatement() {
        int line = advance().line;  // 'print'
        ExprPtr value = expression();
        expect(Tok::Semi, "after expression");
        return std::make_unique<PrintStmt>(std::move(value), line);
    }

    StmtPtr ifStatement() {
        int line = advance().line;  // 'if'
        expect(Tok::LParen, "after 'if'");
        ExprPtr cond = expression();
        expect(Tok::RParen, "after condition");
        StmtPtr thenBranch = statement();
        StmtPtr elseBranch;
        if (match(Tok::Else)) elseBranch = statement();
        return std::make_unique<IfStmt>(std::move(cond), std::move(thenBranch),
                                        std::move(elseBranch), line);
    }

    StmtPtr whileStatement() {
        int line = advance().line;  // 'while'
        expect(Tok::LParen, "after 'while'");
        ExprPtr cond = expression();
        expect(Tok::RParen, "after condition");
        StmtPtr body = statement();
        return std::make_unique<WhileStmt>(std::move(cond), std::move(body), line);
    }

    StmtPtr block() {
        int line = expect(Tok::LBrace, "to start block").line;
        auto blk = std::make_unique<BlockStmt>(line);
        while (peek().type != Tok::RBrace) {
            if (peek().type == Tok::End)
                throw CompileError(peek().line, "unterminated block: missing '}'");
            blk->stmts.push_back(statement());
        }
        advance();  // '}'
        return blk;
    }

    // -------------------------------------------------------------- expressions
    ExprPtr expression() { return equality(); }

    ExprPtr binaryLevel(ExprPtr (Parser::*next)(), std::initializer_list<Tok> ops) {
        ExprPtr left = (this->*next)();
        for (;;) {
            Tok found = Tok::End;
            for (Tok op : ops)
                if (peek().type == op) { found = op; break; }
            if (found == Tok::End) return left;
            int line = advance().line;
            ExprPtr right = (this->*next)();
            left = std::make_unique<BinaryExpr>(found, std::move(left), std::move(right), line);
        }
    }

    ExprPtr equality()   { return binaryLevel(&Parser::comparison, {Tok::EqEq, Tok::Ne}); }
    ExprPtr comparison() { return binaryLevel(&Parser::term, {Tok::Lt, Tok::Gt, Tok::Le, Tok::Ge}); }
    ExprPtr term()       { return binaryLevel(&Parser::factor, {Tok::Plus, Tok::Minus}); }
    ExprPtr factor()     { return binaryLevel(&Parser::unary, {Tok::Star, Tok::Slash}); }

    ExprPtr unary() {
        if (peek().type == Tok::Minus) {
            int line = advance().line;
            return std::make_unique<UnaryExpr>(unary(), line);
        }
        return primary();
    }

    ExprPtr primary() {
        const Token& t = peek();
        switch (t.type) {
            case Tok::Number: {
                advance();
                return std::make_unique<NumberExpr>(t.value, t.line);
            }
            case Tok::Ident: {
                advance();
                return std::make_unique<VarExpr>(t.text, t.line);
            }
            case Tok::LParen: {
                advance();
                ExprPtr e = expression();
                expect(Tok::RParen, "to close '('");
                return e;
            }
            default:
                throw CompileError(t.line, std::string("expected expression, found ") +
                                               tokName(t.type));
        }
    }
};

}  // namespace minic
