// lexer.hpp - Stage 1: turn raw source text into a flat list of tokens.
//
// A lexer is a small hand-written finite-state scanner: look at the current
// character, decide which kind of token starts here, consume it, repeat.
#pragma once

#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace minic {

// Every error the compiler reports carries a line number.
struct CompileError : std::runtime_error {
    int line;
    CompileError(int line_no, const std::string& msg)
        : std::runtime_error("Line " + std::to_string(line_no) + ": " + msg), line(line_no) {}
};

enum class Tok {
    Number, Ident,
    Let, Print, If, Else, While,
    Plus, Minus, Star, Slash,
    Lt, Gt, Le, Ge, EqEq, Ne,
    Assign,
    LParen, RParen, LBrace, RBrace, Semi,
    End
};

struct Token {
    Tok type = Tok::End;
    std::string text;
    int64_t value = 0;  // only meaningful for Tok::Number
    int line = 1;
};

inline const char* tokName(Tok t) {
    switch (t) {
        case Tok::Number: return "number";
        case Tok::Ident:  return "identifier";
        case Tok::Let:    return "'let'";
        case Tok::Print:  return "'print'";
        case Tok::If:     return "'if'";
        case Tok::Else:   return "'else'";
        case Tok::While:  return "'while'";
        case Tok::Plus:   return "'+'";
        case Tok::Minus:  return "'-'";
        case Tok::Star:   return "'*'";
        case Tok::Slash:  return "'/'";
        case Tok::Lt:     return "'<'";
        case Tok::Gt:     return "'>'";
        case Tok::Le:     return "'<='";
        case Tok::Ge:     return "'>='";
        case Tok::EqEq:   return "'=='";
        case Tok::Ne:     return "'!='";
        case Tok::Assign: return "'='";
        case Tok::LParen: return "'('";
        case Tok::RParen: return "')'";
        case Tok::LBrace: return "'{'";
        case Tok::RBrace: return "'}'";
        case Tok::Semi:   return "';'";
        case Tok::End:    return "end of input";
    }
    return "?";
}

class Lexer {
public:
    explicit Lexer(std::string src) : src_(std::move(src)) {}

    std::vector<Token> tokenize() {
        std::vector<Token> out;
        for (;;) {
            skipWhitespaceAndComments();
            if (pos_ >= src_.size()) break;
            out.push_back(next());
        }
        Token end;
        end.type = Tok::End;
        end.text = "<end>";
        end.line = line_;
        out.push_back(end);
        return out;
    }

private:
    std::string src_;
    size_t pos_ = 0;
    int line_ = 1;

    char peek(size_t ahead = 0) const {
        return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
    }

    void skipWhitespaceAndComments() {
        while (pos_ < src_.size()) {
            char c = peek();
            if (c == '\n') { ++line_; ++pos_; }
            else if (std::isspace(static_cast<unsigned char>(c))) { ++pos_; }
            else if (c == '/' && peek(1) == '/') {
                while (pos_ < src_.size() && peek() != '\n') ++pos_;
            } else break;
        }
    }

    Token make(Tok t, std::string text) const {
        Token tok;
        tok.type = t;
        tok.text = std::move(text);
        tok.line = line_;
        return tok;
    }

    Token next() {
        char c = peek();

        if (std::isdigit(static_cast<unsigned char>(c))) return number();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return identifier();

        ++pos_;
        switch (c) {
            case '+': return make(Tok::Plus, "+");
            case '-': return make(Tok::Minus, "-");
            case '*': return make(Tok::Star, "*");
            case '/': return make(Tok::Slash, "/");
            case '(': return make(Tok::LParen, "(");
            case ')': return make(Tok::RParen, ")");
            case '{': return make(Tok::LBrace, "{");
            case '}': return make(Tok::RBrace, "}");
            case ';': return make(Tok::Semi, ";");
            // Two-character operators: peek one more character to decide.
            case '<':
                if (peek() == '=') { ++pos_; return make(Tok::Le, "<="); }
                return make(Tok::Lt, "<");
            case '>':
                if (peek() == '=') { ++pos_; return make(Tok::Ge, ">="); }
                return make(Tok::Gt, ">");
            case '=':
                if (peek() == '=') { ++pos_; return make(Tok::EqEq, "=="); }
                return make(Tok::Assign, "=");
            case '!':
                if (peek() == '=') { ++pos_; return make(Tok::Ne, "!="); }
                throw CompileError(line_, "unexpected character '!' (did you mean '!='?)");
            default:
                throw CompileError(line_, std::string("unexpected character '") + c + "'");
        }
    }

    Token number() {
        size_t start = pos_;
        uint64_t v = 0;
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            unsigned digit = static_cast<unsigned>(peek() - '0');
            // Reject literals that do not fit in int64_t.
            if (v > (static_cast<uint64_t>(INT64_MAX) - digit) / 10)
                throw CompileError(line_, "integer literal too large");
            v = v * 10 + digit;
            ++pos_;
        }
        Token t = make(Tok::Number, src_.substr(start, pos_ - start));
        t.value = static_cast<int64_t>(v);
        return t;
    }

    Token identifier() {
        size_t start = pos_;
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') ++pos_;
        std::string word = src_.substr(start, pos_ - start);
        Tok type = Tok::Ident;
        if (word == "let") type = Tok::Let;
        else if (word == "print") type = Tok::Print;
        else if (word == "if") type = Tok::If;
        else if (word == "else") type = Tok::Else;
        else if (word == "while") type = Tok::While;
        return make(type, word);
    }
};

}  // namespace minic
