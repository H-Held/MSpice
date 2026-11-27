// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors

#include "expr.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "util.hpp"

namespace libcheck {

const std::vector<FunctionInfo>& builtin_functions() {
    static const std::vector<FunctionInfo> f = {
        {"abs", 1, 1},   {"sqrt", 1, 1},  {"exp", 1, 1},   {"ln", 1, 1},     {"log", 1, 1},    {"log10", 1, 1},
        {"sin", 1, 1},   {"cos", 1, 1},   {"tan", 1, 1},   {"asin", 1, 1},   {"acos", 1, 1},   {"atan", 1, 1},
        {"sinh", 1, 1},  {"cosh", 1, 1},  {"tanh", 1, 1},  {"floor", 1, 1},  {"ceil", 1, 1},   {"round", 1, 1},
        {"sgn", 1, 1},   {"u", 1, 1},     {"uramp", 1, 1}, {"atan2", 2, 2},  {"pow", 2, 2},    {"pwr", 2, 2},
        {"min", 2, -1},  {"max", 2, -1},  {"limit", 3, 3}, {"if", 3, 3},     {"V", 1, 2},      {"I", 1, 1},
        {"rising", 1, 1}, {"falling", 1, 1}, {"in_state", 1, 1},
    };
    return f;
}

const FunctionInfo* find_function(std::string_view name) {
    std::string n(name);
    if (n == "v") n = "V";
    if (n == "i") n = "I";
    for (const auto& f : builtin_functions())
        if (n == f.name) return &f;
    return nullptr;
}

const std::vector<std::string>& builtin_names() {
    static const std::vector<std::string> n = {"time", "temp", "pi", "time_in_state"};
    return n;
}

bool is_reserved_name(std::string_view name) {
    for (const auto& b : builtin_names())
        if (b == name) return true;
    if (name == "v" || name == "i") return true;
    return find_function(name) != nullptr;
}

namespace {

struct SyntaxError {
    int pos;
    std::string msg;
};

enum class Tok { Number, Ident, Op, LParen, RParen, Comma, End };

struct Token {
    Tok kind;
    std::string text;
    int pos;
};

bool is_ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

class Lexer {
public:
    Lexer(std::string_view s, ExprResult& r) : s_(s), r_(r) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        std::size_t i = 0;
        while (true) {
            while (i < s_.size() && std::isspace(static_cast<unsigned char>(s_[i]))) ++i;
            if (i >= s_.size()) break;
            const char c = s_[i];
            const int pos = static_cast<int>(i);
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && i + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i + 1])))) {
                out.push_back(number(i));
                continue;
            }
            if (is_ident_start(c)) {
                std::size_t j = i;
                while (j < s_.size() && is_ident_char(s_[j])) ++j;
                out.push_back({Tok::Ident, std::string(s_.substr(i, j - i)), pos});
                i = j;
                continue;
            }
            if (c == '(') { out.push_back({Tok::LParen, "(", pos}); ++i; continue; }
            if (c == ')') { out.push_back({Tok::RParen, ")", pos}); ++i; continue; }
            if (c == ',') { out.push_back({Tok::Comma, ",", pos}); ++i; continue; }
            static const char* two[] = {"**", "==", "!=", "<=", ">=", "&&", "||"};
            bool matched = false;
            for (const char* t : two) {
                if (s_.substr(i, 2) == t) {
                    out.push_back({Tok::Op, t, pos});
                    i += 2;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
            if (std::string_view("+-*/^<>!?:").find(c) != std::string_view::npos) {
                out.push_back({Tok::Op, std::string(1, c), pos});
                ++i;
                continue;
            }
            if (c == '=')
                throw SyntaxError{pos, "single `=` is not an operator; use `==` to compare"};
            if (c == '{' || c == '}')
                throw SyntaxError{pos, "braces are only used around a whole value, not inside an expression"};
            throw SyntaxError{pos, std::string("unexpected character `") + c + "`"};
        }
        out.push_back({Tok::End, "", static_cast<int>(s_.size())});
        return out;
    }

private:
    Token number(std::size_t& i) {
        const std::size_t start = i;
        while (i < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i]))) ++i;
        if (i < s_.size() && s_[i] == '.') {
            ++i;
            while (i < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i]))) ++i;
        }
        if (i < s_.size() && (s_[i] == 'e' || s_[i] == 'E')) {
            std::size_t j = i + 1;
            if (j < s_.size() && (s_[j] == '+' || s_[j] == '-')) ++j;
            if (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) {
                while (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) ++j;
                i = j;
            }
        }
        const std::size_t suffix_start = i;
        while (i < s_.size() && std::isalpha(static_cast<unsigned char>(s_[i]))) ++i;
        const std::string suffix(s_.substr(suffix_start, i - suffix_start));
        if (!suffix.empty()) {
            const std::string low = to_lower(suffix);
            static const char* ok[] = {"f", "p", "n", "u", "m", "k", "meg", "g", "t"};
            bool good = std::find_if(std::begin(ok), std::end(ok), [&](const char* x) { return low == x; }) !=
                        std::end(ok);
            if (!good) {
                throw SyntaxError{static_cast<int>(suffix_start),
                                  "`" + std::string(s_.substr(start, i - start)) +
                                      "`: letters after a number must be one SPICE suffix (f p n u m k meg g t); "
                                      "units are not allowed in expressions"};
            }
            if (suffix == "M") r_.mega_warnings.push_back(static_cast<int>(suffix_start));
        }
        if (i < s_.size() && (s_[i] == '_' || std::isdigit(static_cast<unsigned char>(s_[i]))))
            throw SyntaxError{static_cast<int>(i), "malformed number"};
        return {Tok::Number, std::string(s_.substr(start, i - start)), static_cast<int>(start)};
    }

    std::string_view s_;
    ExprResult& r_;
};

class Parser {
public:
    Parser(std::vector<Token> toks, ExprResult& r) : t_(std::move(toks)), r_(r) {}

    void parse_all() {
        ternary();
        if (peek().kind != Tok::End) throw SyntaxError{peek().pos, "unexpected `" + peek().text + "`"};
    }

private:
    const Token& peek() const { return t_[p_]; }
    Token next() { return t_[p_++]; }
    bool is_op(const char* op) const { return peek().kind == Tok::Op && peek().text == op; }

    void expect(Tok k, const char* what) {
        if (peek().kind != k) {
            const std::string found = peek().kind == Tok::End ? "end of expression" : "`" + peek().text + "`";
            throw SyntaxError{peek().pos, std::string("expected ") + what + ", found " + found};
        }
        ++p_;
    }

    void ternary() {
        logic_or();
        if (is_op("?")) {
            next();
            ternary();
            if (!is_op(":")) {
                const std::string found = peek().kind == Tok::End ? "end of expression" : "`" + peek().text + "`";
                throw SyntaxError{peek().pos, "expected `:` of `c ? a : b`, found " + found};
            }
            next();
            ternary();
        }
    }
    void logic_or() {
        logic_and();
        while (is_op("||")) { next(); logic_and(); }
    }
    void logic_and() {
        equality();
        while (is_op("&&")) { next(); equality(); }
    }
    void equality() {
        relational();
        while (is_op("==") || is_op("!=")) { next(); relational(); }
    }
    void relational() {
        additive();
        while (is_op("<") || is_op("<=") || is_op(">") || is_op(">=")) { next(); additive(); }
    }
    void additive() {
        multiplicative();
        while (is_op("+") || is_op("-")) { next(); multiplicative(); }
    }
    void multiplicative() {
        unary();
        while (is_op("*") || is_op("/")) { next(); unary(); }
    }
    void unary() {
        if (is_op("-") || is_op("+") || is_op("!")) {
            next();
            unary();
            return;
        }
        power();
    }
    void power() {
        primary();
        if (is_op("^") || is_op("**")) {
            next();
            unary();  // right associative, allows 2^-1
        }
    }
    void primary() {
        const Token& tk = peek();
        switch (tk.kind) {
            case Tok::Number: next(); return;
            case Tok::LParen:
                next();
                ternary();
                expect(Tok::RParen, "`)`");
                return;
            case Tok::Ident: {
                Token id = next();
                if (peek().kind == Tok::LParen) {
                    call(id);
                } else {
                    r_.uses.push_back({ExprUse::Kind::Name, id.text, id.pos, 0});
                }
                return;
            }
            case Tok::End: throw SyntaxError{tk.pos, "expression ends too early"};
            default: throw SyntaxError{tk.pos, "unexpected `" + tk.text + "`"};
        }
    }

    // Argument of V(), I(), rising(), ...: a single node or name token.
    Token name_arg(const char* what) {
        const Token& tk = peek();
        if (tk.kind == Tok::Ident) return next();
        if (tk.kind == Tok::Number) {
            const bool digits_only =
                std::all_of(tk.text.begin(), tk.text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
            if (digits_only) return next();
        }
        throw SyntaxError{tk.pos, std::string("expected ") + what};
    }

    void call(const Token& id) {
        next();  // (
        const std::string& n = id.text;
        const bool is_v = (n == "V" || n == "v"), is_i = (n == "I" || n == "i");
        if (is_v || is_i || n == "rising" || n == "falling" || n == "in_state") {
            int argc = 0;
            ExprUse::Kind k = is_v ? ExprUse::Kind::Node
                            : is_i ? ExprUse::Kind::Current
                            : n == "in_state" ? ExprUse::Kind::StateArg
                                              : ExprUse::Kind::EdgeArg;
            const char* what = is_v ? "a node name" : is_i ? "a pin or element name"
                             : n == "in_state" ? "a state name" : "an input name";
            if (peek().kind != Tok::RParen) {
                while (true) {
                    Token a = name_arg(what);
                    r_.uses.push_back({k, a.text, a.pos, 0});
                    ++argc;
                    if (peek().kind == Tok::Comma) { next(); continue; }
                    break;
                }
            }
            expect(Tok::RParen, "`)`");
            r_.uses.push_back({ExprUse::Kind::Call, n, id.pos, argc});
            return;
        }
        int argc = 0;
        if (peek().kind != Tok::RParen) {
            while (true) {
                ternary();
                ++argc;
                if (peek().kind == Tok::Comma) { next(); continue; }
                break;
            }
        }
        expect(Tok::RParen, "`,` or `)`");
        r_.uses.push_back({ExprUse::Kind::Call, n, id.pos, argc});
    }

    std::vector<Token> t_;
    std::size_t p_ = 0;
    ExprResult& r_;
};

}  // namespace

ExprResult parse_expression(std::string_view text) {
    ExprResult r;
    try {
        if (trim(text).empty()) throw SyntaxError{0, "empty expression"};
        Lexer lx(text, r);
        Parser p(lx.run(), r);
        p.parse_all();
    } catch (const SyntaxError& e) {
        r.ok = false;
        r.err_pos = e.pos;
        r.error = e.msg;
    }
    return r;
}

ExprResult parse_assignment(std::string_view text, std::string& target, int& target_pos) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    std::size_t j = i;
    while (j < text.size() && is_ident_char(text[j])) ++j;
    std::size_t k = j;
    while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
    if (j == i || !is_ident_start(text[i]) || k >= text.size() || text[k] != '=' ||
        (k + 1 < text.size() && text[k + 1] == '=')) {
        ExprResult r;
        r.ok = false;
        r.err_pos = static_cast<int>(i);
        r.error = "an action must have the form `variable = expression`";
        return r;
    }
    target = std::string(text.substr(i, j - i));
    target_pos = static_cast<int>(i);
    ExprResult r = parse_expression(text.substr(k + 1));
    const int shift = static_cast<int>(k + 1);
    if (!r.ok) r.err_pos += shift;
    for (auto& u : r.uses) u.pos += shift;
    for (auto& m : r.mega_warnings) m += shift;
    return r;
}

}  // namespace libcheck
