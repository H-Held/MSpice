// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file spice_expr.cpp
 * @brief Expression parser, resolution, constant folding and tape compiler.
 */
#include <mspice/io/spice_expr.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <unordered_map>

namespace mspice::io {

namespace {

/** @brief Lower-case copy of @p s (ASCII). */
std::string lower(std::string_view s) {
    std::string r(s);
    for (char &c : r)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

/** @brief Trim ASCII whitespace on both sides. */
std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return std::string(s.substr(a, b - a));
}

/** @brief Scale factor of a SPICE suffix (letters after the mantissa); 1 when none. */
double suffix_scale(std::string_view suf) {
    if (suf.empty())
        return 1.0;
    const std::string s = lower(suf);
    if (s.rfind("meg", 0) == 0)
        return 1e6;
    if (s.rfind("mil", 0) == 0)
        return 25.4e-6;
    switch (s[0]) {
    case 't':
        return 1e12;
    case 'g':
        return 1e9;
    case 'k':
        return 1e3;
    case 'm':
        return 1e-3;
    case 'u':
        return 1e-6;
    case 'n':
        return 1e-9;
    case 'p':
        return 1e-12;
    case 'f':
        return 1e-15;
    default:
        return 1.0; // other letters are units (V, A, Ohm, F, H, s) and ignored
    }
}

/** @brief Decimal exponent of a SPICE suffix; nullopt for `mil` and units without scale. */
std::optional<int> suffix_exponent(std::string_view suf) {
    if (suf.empty())
        return 0;
    const std::string s = lower(suf);
    if (s.rfind("meg", 0) == 0)
        return 6;
    if (s.rfind("mil", 0) == 0)
        return std::nullopt;
    switch (s[0]) {
    case 't':
        return 12;
    case 'g':
        return 9;
    case 'k':
        return 3;
    case 'm':
        return -3;
    case 'u':
        return -6;
    case 'n':
        return -9;
    case 'p':
        return -12;
    case 'f':
        return -15;
    default:
        return 0;
    }
}

/** @brief Value of mantissa text @p mant with suffix @p suf; the suffix is applied as a
 *  decimal exponent before rounding, so 5u == 5e-6 exactly. */
double scaled_value(std::string_view mant, std::string_view suf) {
    const auto ex = suffix_exponent(suf);
    if (!ex)
        return std::stod(std::string(mant)) * suffix_scale(suf);
    if (*ex == 0)
        return std::stod(std::string(mant));
    std::string m(mant);
    int e = *ex;
    const std::size_t epos = m.find_first_of("eE");
    if (epos != std::string::npos) {
        e += std::stoi(m.substr(epos + 1));
        m.erase(epos);
    }
    return std::stod(m + "e" + std::to_string(e));
}

/** @brief Length of the mantissa (digits, point, exponent) at the start of @p s; 0 if none. */
std::size_t mantissa_length(std::string_view s) {
    std::size_t i = 0;
    bool digits = false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
        digits = true;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            ++i;
            digits = true;
        }
    }
    if (!digits)
        return 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < s.size() && (s[j] == '+' || s[j] == '-'))
            ++j;
        if (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) {
            while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
                ++j;
            i = j;
        }
    }
    return i;
}

/** @brief Function table entry: name, arity (-1 = see below), opcode. */
struct FuncInfo {
    const char *name;
    int arity;
    OpCode op;
};

/** @brief All supported functions (ngspice B-source names). */
const FuncInfo kFuncs[] = {
    {"min", 2, OpCode::Min},   {"max", 2, OpCode::Max},     {"abs", 1, OpCode::Abs},
    {"sqrt", 1, OpCode::Sqrt}, {"exp", 1, OpCode::Exp},     {"ln", 1, OpCode::Ln},
    {"log", 1, OpCode::Ln},    {"log10", 1, OpCode::Log10}, {"tanh", 1, OpCode::Tanh},
    {"pow", 2, OpCode::Pow},   {"pwr", 2, OpCode::Pwr},     {"u", 1, OpCode::Step},
    {"sin", 1, OpCode::Sin},   {"cos", 1, OpCode::Cos},     {"tan", 1, OpCode::Tan},
    {"atan", 1, OpCode::Atan}, {"sinh", 1, OpCode::Sinh},   {"cosh", 1, OpCode::Cosh},
    {"sgn", 1, OpCode::Sgn},   {"uramp", 1, OpCode::Uramp}, {"floor", 1, OpCode::Floor},
    {"ceil", 1, OpCode::Ceil}, {"limit", 3, OpCode::Limit},
};

/** @brief Look up a function by name; nullptr when unknown. */
const FuncInfo *find_func(const std::string &name) {
    for (const FuncInfo &f : kFuncs)
        if (name == f.name)
            return &f;
    return nullptr;
}

/** @brief Token kinds of the expression lexer. */
enum class Tok { Num, Ident, Op, LParen, RParen, Comma, Question, Colon, End };

/** @brief One expression token. */
struct Token {
    Tok kind;
    std::string text;
    double value = 0.0;
    std::size_t pos = 0;
};

/** @brief Recursive-descent parser over a pre-lexed token list. */
class Parser {
  public:
    /** @brief Lex @p text into tokens. */
    explicit Parser(std::string_view text) : src_(text) { lex(); }

    /** @brief Parse the whole input as one expression. */
    ExprPtr parse() {
        ExprPtr e = ternary();
        if (peek().kind != Tok::End)
            fail("unexpected character '" + peek().text + "'");
        return e;
    }

  private:
    std::string_view src_;
    std::vector<Token> toks_;
    std::size_t at_ = 0;

    /** @brief Throw an ExprError with position information. */
    [[noreturn]] void fail(const std::string &msg) const {
        const std::size_t pos = at_ < toks_.size() ? toks_[at_].pos : src_.size();
        throw ExprError(msg + " at position " + std::to_string(pos) + " in '" + std::string(src_) +
                        "'");
    }

    /** @brief Split the source into tokens; v(...)/i(...) arguments become raw names. */
    void lex() {
        const std::string_view s = src_;
        std::size_t i = 0;
        bool quote_open = false;
        while (i < s.size()) {
            const char c = s[i];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++i;
                continue;
            }
            Token t{Tok::Op, std::string(1, c), 0.0, i};
            if (std::isdigit(static_cast<unsigned char>(c)) ||
                (c == '.' && i + 1 < s.size() &&
                 std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
                const std::size_t m = mantissa_length(s.substr(i));
                std::size_t j = i + m;
                while (j < s.size() && std::isalpha(static_cast<unsigned char>(s[j])))
                    ++j;
                t.kind = Tok::Num;
                t.text = std::string(s.substr(i, j - i));
                t.value = scaled_value(s.substr(i, m), s.substr(i + m, j - i - m));
                toks_.push_back(t);
                i = j;
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
                std::size_t j = i;
                while (j < s.size() &&
                       (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_'))
                    ++j;
                t.kind = Tok::Ident;
                t.text = lower(s.substr(i, j - i));
                std::size_t k = j;
                while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k])))
                    ++k;
                if ((t.text == "v" || t.text == "i") && k < s.size() && s[k] == '(') {
                    // Probe: raw names up to the closing parenthesis.
                    const std::size_t close = s.find(')', k);
                    if (close == std::string_view::npos) {
                        at_ = toks_.size();
                        toks_.push_back(t);
                        fail("missing ')' after " + t.text + "(");
                    }
                    t.text += "(" + lower(s.substr(k + 1, close - k - 1)) + ")";
                    toks_.push_back(t);
                    i = close + 1;
                    continue;
                }
                toks_.push_back(t);
                i = j;
                continue;
            }
            const std::string two = i + 1 < s.size() ? std::string(s.substr(i, 2)) : std::string();
            if (two == "**" || two == "==" || two == "!=" || two == "<=" || two == ">=" ||
                two == "&&" || two == "||") {
                t.text = two;
                toks_.push_back(t);
                i += 2;
                continue;
            }
            switch (c) {
            case '(':
            case '{':
                t.kind = Tok::LParen;
                break;
            case ')':
            case '}':
                t.kind = Tok::RParen;
                break;
            case '\'':
                t.kind = quote_open ? Tok::RParen : Tok::LParen;
                quote_open = !quote_open;
                break;
            case ',':
                t.kind = Tok::Comma;
                break;
            case '?':
                t.kind = Tok::Question;
                break;
            case ':':
                t.kind = Tok::Colon;
                break;
            case '+':
            case '-':
            case '*':
            case '/':
            case '^':
            case '<':
            case '>':
            case '!':
                t.kind = Tok::Op;
                break;
            default:
                at_ = toks_.size();
                toks_.push_back(t);
                fail("unknown character '" + std::string(1, c) + "'");
            }
            toks_.push_back(t);
            ++i;
        }
        toks_.push_back(Token{Tok::End, "<end>", 0.0, s.size()});
    }

    /** @brief Current token. */
    const Token &peek() const { return toks_[at_]; }
    /** @brief True and consume when the current token is operator @p op. */
    bool accept_op(const char *op) {
        if (peek().kind == Tok::Op && peek().text == op) {
            ++at_;
            return true;
        }
        return false;
    }
    /** @brief Consume a token of kind @p k or fail with @p what. */
    void expect(Tok k, const char *what) {
        if (peek().kind != k)
            fail(std::string("expected ") + what + ", found '" + peek().text + "'");
        ++at_;
    }

    /** @brief Make a binary node. */
    static ExprPtr bin(const std::string &op, ExprPtr a, ExprPtr b) {
        auto n = std::make_shared<ExprNode>();
        n->kind = ExprNode::Kind::Binary;
        n->op = op;
        n->args = {std::move(a), std::move(b)};
        return n;
    }

    /** @brief ternary := or ['?' ternary ':' ternary] */
    ExprPtr ternary() {
        ExprPtr c = logic_or();
        if (peek().kind == Tok::Question) {
            ++at_;
            ExprPtr a = ternary();
            expect(Tok::Colon, "':'");
            ExprPtr b = ternary();
            auto n = std::make_shared<ExprNode>();
            n->kind = ExprNode::Kind::Ternary;
            n->args = {c, a, b};
            return n;
        }
        return c;
    }
    /** @brief or := and {'||' and} */
    ExprPtr logic_or() {
        ExprPtr a = logic_and();
        while (accept_op("||"))
            a = bin("||", a, logic_and());
        return a;
    }
    /** @brief and := cmp {'&&' cmp} */
    ExprPtr logic_and() {
        ExprPtr a = compare();
        while (accept_op("&&"))
            a = bin("&&", a, compare());
        return a;
    }
    /** @brief cmp := add {relop add} */
    ExprPtr compare() {
        ExprPtr a = additive();
        for (;;) {
            bool hit = false;
            for (const char *op : {"==", "!=", "<=", ">=", "<", ">"}) {
                if (accept_op(op)) {
                    a = bin(op, a, additive());
                    hit = true;
                    break;
                }
            }
            if (!hit)
                return a;
        }
    }
    /** @brief add := mul {(+|-) mul} */
    ExprPtr additive() {
        ExprPtr a = multiplicative();
        for (;;) {
            if (accept_op("+"))
                a = bin("+", a, multiplicative());
            else if (accept_op("-"))
                a = bin("-", a, multiplicative());
            else
                return a;
        }
    }
    /** @brief mul := unary {(*|/) unary} */
    ExprPtr multiplicative() {
        ExprPtr a = unary();
        for (;;) {
            if (accept_op("*"))
                a = bin("*", a, unary());
            else if (accept_op("/"))
                a = bin("/", a, unary());
            else
                return a;
        }
    }
    /** @brief unary := (-|+|!) unary | power */
    ExprPtr unary() {
        for (const char *op : {"-", "+", "!"}) {
            if (accept_op(op)) {
                auto n = std::make_shared<ExprNode>();
                n->kind = ExprNode::Kind::Unary;
                n->op = op;
                n->args = {unary()};
                return n;
            }
        }
        return power();
    }
    /** @brief power := primary {(**|^) operand}, left associative as in ngspice-46
     *  (2**3**2 = 64, as measured with ngspice-46). */
    ExprPtr power() {
        ExprPtr a = primary();
        while (accept_op("**") || accept_op("^"))
            a = bin("**", a, power_operand());
        return a;
    }
    /** @brief operand := (-|+) operand | primary (sign of an exponent, e.g. x**-2). */
    ExprPtr power_operand() {
        for (const char *op : {"-", "+"}) {
            if (accept_op(op)) {
                auto n = std::make_shared<ExprNode>();
                n->kind = ExprNode::Kind::Unary;
                n->op = op;
                n->args = {power_operand()};
                return n;
            }
        }
        return primary();
    }
    /** @brief primary := number | probe | ident ['(' args ')'] | '(' ternary ')' */
    ExprPtr primary() {
        const Token t = peek();
        auto n = std::make_shared<ExprNode>();
        switch (t.kind) {
        case Tok::Num:
            ++at_;
            n->kind = ExprNode::Kind::Number;
            n->value = t.value;
            return n;
        case Tok::LParen: {
            ++at_;
            ExprPtr e = ternary();
            expect(Tok::RParen, "')'");
            return e;
        }
        case Tok::Ident: {
            ++at_;
            if ((t.text[0] == 'v' || t.text[0] == 'i') && t.text.size() > 1 && t.text[1] == '(') {
                const std::string inner = t.text.substr(2, t.text.size() - 3);
                const std::size_t comma = inner.find(',');
                if (t.text[0] == 'i') {
                    if (comma != std::string::npos)
                        fail("i() with two names is not supported");
                    n->kind = ExprNode::Kind::Current;
                    n->name = trim(inner);
                } else {
                    n->kind = ExprNode::Kind::Voltage;
                    n->name = trim(inner.substr(0, comma));
                    if (comma != std::string::npos) {
                        n->name2 = trim(inner.substr(comma + 1));
                        if (n->name2.find(',') != std::string::npos)
                            fail("v() with more than two nodes");
                    }
                }
                if (n->name.empty() || (comma != std::string::npos && n->name2.empty()))
                    fail("empty name in " + t.text);
                return n;
            }
            if (peek().kind == Tok::LParen) {
                ++at_;
                n->kind = ExprNode::Kind::Call;
                n->name = t.text;
                if (peek().kind != Tok::RParen) {
                    n->args.push_back(ternary());
                    while (peek().kind == Tok::Comma) {
                        ++at_;
                        n->args.push_back(ternary());
                    }
                }
                expect(Tok::RParen, "')'");
                return n;
            }
            n->kind = ExprNode::Kind::Name;
            n->name = t.text;
            return n;
        }
        default:
            fail("operand expected, found '" + t.text + "'");
        }
    }
};

/** @brief Shallow copy of a node with new arguments. */
ExprPtr with_args(const ExprPtr &e, std::vector<ExprPtr> args) {
    auto n = std::make_shared<ExprNode>(*e);
    n->args = std::move(args);
    return n;
}

/** @brief Number node. */
ExprPtr number(double v) {
    auto n = std::make_shared<ExprNode>();
    n->kind = ExprNode::Kind::Number;
    n->value = v;
    return n;
}

/** @brief Opcode of a unary/binary operator. */
OpCode op_of(const ExprNode &e) {
    if (e.kind == ExprNode::Kind::Unary)
        return e.op == "-" ? OpCode::Neg : OpCode::Not; // unary '+' is dropped earlier
    static const std::map<std::string, OpCode> m = {
        {"+", OpCode::Add},  {"-", OpCode::Sub}, {"*", OpCode::Mul}, {"/", OpCode::Div},
        {"**", OpCode::Pow}, {"==", OpCode::Eq}, {"!=", OpCode::Ne}, {"<", OpCode::Lt},
        {"<=", OpCode::Le},  {">", OpCode::Gt},  {">=", OpCode::Ge}, {"&&", OpCode::And},
        {"||", OpCode::Or}};
    return m.at(e.op);
}

/** @brief Evaluate one operation on constant arguments with the tape semantics. */
double apply_constant(OpCode op, const std::vector<double> &a) {
    Tape t;
    for (std::size_t k = 0; k < a.size(); ++k) {
        t.constants.push_back(a[k]);
        t.code.push_back({OpCode::Const, static_cast<std::uint32_t>(k)});
    }
    t.code.push_back({op, 0});
    const double *none = nullptr;
    return evaluate(t, none);
}

/** @brief Affine form a*time + b of @p e, nullopt when not affine in time. */
std::optional<std::pair<double, double>> affine_time(const ExprPtr &e) {
    using K = ExprNode::Kind;
    switch (e->kind) {
    case K::Number:
        return std::make_pair(0.0, e->value);
    case K::Time:
        return std::make_pair(1.0, 0.0);
    case K::Unary: {
        auto x = affine_time(e->args[0]);
        if (!x)
            return std::nullopt;
        return e->op == "-" ? std::make_pair(-x->first, -x->second) : *x;
    }
    case K::Binary: {
        auto x = affine_time(e->args[0]);
        auto y = affine_time(e->args[1]);
        if (!x || !y)
            return std::nullopt;
        if (e->op == "+")
            return std::make_pair(x->first + y->first, x->second + y->second);
        if (e->op == "-")
            return std::make_pair(x->first - y->first, x->second - y->second);
        if (e->op == "*" && x->first == 0)
            return std::make_pair(x->second * y->first, x->second * y->second);
        if (e->op == "*" && y->first == 0)
            return std::make_pair(y->second * x->first, y->second * x->second);
        if (e->op == "/" && y->first == 0 && y->second != 0)
            return std::make_pair(x->first / y->second, x->second / y->second);
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

/** @brief Tape compiler with common-subexpression registers. */
class Compiler {
  public:
    /** @brief Compile @p root into @p tape. */
    Compiler(const ExprPtr &root, Tape &tape, const CompileOptions &opt) : t_(tape), opt_(opt) {
        count(root);
        emit(root);
        std::sort(t_.breakpoints.begin(), t_.breakpoints.end());
        t_.breakpoints.erase(std::unique(t_.breakpoints.begin(), t_.breakpoints.end()),
                             t_.breakpoints.end());
    }

  private:
    Tape &t_;
    const CompileOptions &opt_;
    std::unordered_map<const ExprNode *, std::string> key_;
    std::map<std::string, int> uses_;
    std::map<std::string, int> reg_;
    int depth_ = 0;

    /** @brief Canonical text of a subtree (memoised). */
    const std::string &key(const ExprPtr &e) {
        auto it = key_.find(e.get());
        if (it != key_.end())
            return it->second;
        return key_[e.get()] = to_string(e);
    }
    /** @brief Count occurrences of each non-leaf subtree. */
    void count(const ExprPtr &e) {
        if (e->args.empty())
            return;
        if (++uses_[key(e)] > 1)
            return; // children were counted with the first occurrence
        for (const ExprPtr &a : e->args)
            count(a);
    }
    /** @brief Append an instruction and track the stack depth. */
    void push(OpCode op, std::uint32_t arg, int delta) {
        t_.code.push_back({op, arg});
        depth_ += delta;
        t_.max_stack = std::max(t_.max_stack, depth_);
        if (t_.max_stack > tape_max_stack)
            throw ExprError("expression nested too deeply (stack > " +
                            std::to_string(tape_max_stack) + ")");
    }
    /** @brief Index of a constant in the pool (reused when equal). */
    std::uint32_t constant(double v) {
        for (std::size_t k = 0; k < t_.constants.size(); ++k)
            if (t_.constants[k] == v && std::signbit(t_.constants[k]) == std::signbit(v))
                return static_cast<std::uint32_t>(k);
        t_.constants.push_back(v);
        return static_cast<std::uint32_t>(t_.constants.size() - 1);
    }
    /** @brief Index of a control (deduplicated, order of first appearance). */
    std::uint32_t control(ControlRef c) {
        for (std::size_t k = 0; k < t_.controls.size(); ++k)
            if (t_.controls[k] == c)
                return static_cast<std::uint32_t>(k);
        t_.controls.push_back(std::move(c));
        return static_cast<std::uint32_t>(t_.controls.size() - 1);
    }
    /** @brief Record a breakpoint when @p e (a step argument or a difference) is affine in time. */
    void breakpoint_from(const ExprPtr &e) {
        const auto a = affine_time(e);
        if (a && a->first != 0.0)
            t_.breakpoints.push_back(-a->second / a->first);
    }
    /** @brief Power with constant exponent @p c on the top of stack; |x|^2 becomes Sqr
     *  (same value and derivative, std::pow costs about 20 ns). */
    void pow_const(OpCode op, double c) {
        if (op == OpCode::PowC && c == 2.0)
            push(OpCode::Sqr, 0, 0);
        else
            push(op, constant(c), 0);
    }
    /** @brief Emit code for @p e (post-order), reusing registers for repeated subtrees. */
    void emit(const ExprPtr &e) {
        using K = ExprNode::Kind;
        if (!e->args.empty()) {
            const std::string &k = key(e);
            auto r = reg_.find(k);
            if (r != reg_.end()) {
                push(OpCode::Load, static_cast<std::uint32_t>(r->second), +1);
                return;
            }
        }
        switch (e->kind) {
        case K::Number:
            push(OpCode::Const, constant(e->value), +1);
            return;
        case K::Time:
            push(OpCode::Control, control({ControlRef::Kind::Time, ""}), +1);
            return;
        case K::Voltage:
            push(OpCode::Control, control({ControlRef::Kind::Node, e->name}), +1);
            if (!e->name2.empty()) {
                push(OpCode::Control, control({ControlRef::Kind::Node, e->name2}), +1);
                push(OpCode::Sub, 0, -1);
            }
            return;
        case K::Current:
            push(OpCode::Control, control({ControlRef::Kind::Current, e->name}), +1);
            return;
        case K::Name:
            throw ExprError("unresolved name '" + e->name + "'");
        case K::Unary:
            emit(e->args[0]);
            if (e->op != "+")
                push(op_of(*e), 0, 0);
            break;
        case K::Binary: {
            const OpCode op = op_of(*e);
            if (op == OpCode::Lt || op == OpCode::Le || op == OpCode::Gt || op == OpCode::Ge) {
                auto diff = std::make_shared<ExprNode>();
                diff->kind = K::Binary;
                diff->op = "-";
                diff->args = e->args;
                breakpoint_from(diff);
            }
            if (op == OpCode::Pow && e->args[1]->kind == K::Number) {
                emit(e->args[0]);
                pow_const(OpCode::PowC, e->args[1]->value);
                break;
            }
            emit(e->args[0]);
            emit(e->args[1]);
            push(op, 0, -1);
            break;
        }
        case K::Ternary:
            emit(e->args[0]);
            emit(e->args[1]);
            emit(e->args[2]);
            push(OpCode::Select, 0, -2);
            break;
        case K::Call: {
            const FuncInfo *f = find_func(e->name);
            if (!f)
                throw ExprError("unknown function '" + e->name + "'");
            if (f->op == OpCode::Step || f->op == OpCode::Uramp)
                breakpoint_from(e->args[0]);
            // ngbehavior=psa: pwr() loses its sign and equals pow() (as ngspice-46).
            const OpCode fop = (f->op == OpCode::Pwr && opt_.psa) ? OpCode::Pow : f->op;
            if ((fop == OpCode::Pow || fop == OpCode::Pwr) && e->args[1]->kind == K::Number) {
                emit(e->args[0]);
                pow_const(fop == OpCode::Pow ? OpCode::PowC : OpCode::PwrC, e->args[1]->value);
                break;
            }
            for (const ExprPtr &a : e->args)
                emit(a);
            push(fop, 0, 1 - static_cast<int>(e->args.size()));
            break;
        }
        }
        const std::string &k = key(e);
        if (uses_[k] > 1) {
            const int r = static_cast<int>(reg_.size());
            if (r >= tape_max_registers)
                return; // out of registers: recompute later occurrences
            reg_[k] = r;
            t_.registers = r + 1;
            push(OpCode::Store, static_cast<std::uint32_t>(r), 0);
        }
    }
};

/** @brief Format a double with full precision. */
std::string fmt(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

} // namespace

std::optional<double> parse_spice_number(std::string_view text) {
    std::string_view s = text;
    double sign = 1.0;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        sign = s[0] == '-' ? -1.0 : 1.0;
        s.remove_prefix(1);
    }
    const std::size_t m = mantissa_length(s);
    if (m == 0)
        return std::nullopt;
    for (std::size_t k = m; k < s.size(); ++k)
        if (!std::isalpha(static_cast<unsigned char>(s[k])))
            return std::nullopt;
    return sign * scaled_value(s.substr(0, m), s.substr(m));
}

ExprPtr parse_expression(std::string_view text) {
    const std::string t = trim(text);
    if (t.empty())
        throw ExprError("empty expression");
    return Parser(t).parse();
}

const std::vector<std::string> &known_functions() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> r;
        for (const FuncInfo &f : kFuncs)
            r.emplace_back(f.name);
        return r;
    }();
    return names;
}

ExprPtr resolve(const ExprPtr &e, const Resolver &r) {
    using K = ExprNode::Kind;
    switch (e->kind) {
    case K::Number:
        return e;
    case K::Time:
        if (r.constant_only)
            throw ExprError("'time' in a parameter expression");
        return e;
    case K::Name: {
        if (r.param) {
            if (auto v = r.param(e->name))
                return number(*v);
        }
        if (e->name == "time") {
            if (r.constant_only)
                throw ExprError("'time' in a parameter expression");
            auto n = std::make_shared<ExprNode>();
            n->kind = K::Time;
            return n;
        }
        if (e->name == "pi")
            return number(3.14159265358979323846);
        if (e->name == "temper")
            return number(27.0);
        throw ExprError("unknown name '" + e->name + "'");
    }
    case K::Voltage: {
        if (r.constant_only)
            throw ExprError("v(" + e->name + ") in a parameter expression");
        auto n = std::make_shared<ExprNode>(*e);
        if (r.node) {
            n->name = r.node(e->name);
            if (!e->name2.empty())
                n->name2 = r.node(e->name2);
        }
        return n;
    }
    case K::Current: {
        if (r.constant_only)
            throw ExprError("i(" + e->name + ") in a parameter expression");
        auto n = std::make_shared<ExprNode>(*e);
        if (r.device)
            n->name = r.device(e->name);
        return n;
    }
    case K::Call: {
        const FuncInfo *f = find_func(e->name);
        if (!f)
            throw ExprError("unknown function '" + e->name + "'");
        if (static_cast<int>(e->args.size()) != f->arity)
            throw ExprError("function '" + e->name + "' expects " + std::to_string(f->arity) +
                            " argument(s), got " + std::to_string(e->args.size()));
        [[fallthrough]];
    }
    default: {
        std::vector<ExprPtr> args;
        args.reserve(e->args.size());
        for (const ExprPtr &a : e->args)
            args.push_back(resolve(a, r));
        return with_args(e, std::move(args));
    }
    }
}

ExprPtr fold(const ExprPtr &e) {
    using K = ExprNode::Kind;
    if (e->args.empty())
        return e;
    std::vector<ExprPtr> args;
    args.reserve(e->args.size());
    bool all_const = true;
    for (const ExprPtr &a : e->args) {
        args.push_back(fold(a));
        all_const = all_const && args.back()->kind == K::Number;
    }
    if (e->kind == K::Ternary && args[0]->kind == K::Number)
        return args[0]->value != 0 ? args[1] : args[2];
    if (e->kind == K::Unary && e->op == "+")
        return args[0];
    // pwr() with a negative base depends on CompileOptions::psa: leave it to compile().
    const bool pwr_negative =
        e->kind == K::Call && e->name == "pwr" && all_const && args[0]->value < 0;
    if (!all_const || pwr_negative)
        return with_args(e, std::move(args));
    std::vector<double> v;
    for (const ExprPtr &a : args)
        v.push_back(a->value);
    OpCode op;
    if (e->kind == K::Call) {
        const FuncInfo *f = find_func(e->name);
        if (!f)
            throw ExprError("unknown function '" + e->name + "'");
        op = f->op;
    } else if (e->kind == K::Ternary) {
        op = OpCode::Select;
    } else {
        op = op_of(*e);
    }
    return number(apply_constant(op, v));
}

double evaluate_constant(const ExprPtr &e) {
    const ExprPtr f = fold(e);
    if (f->kind == ExprNode::Kind::Number)
        return f->value;
    const Tape t = compile(f); // e.g. pwr() with negative base, left unfolded
    if (!t.controls.empty())
        throw ExprError("expression is not constant: " + to_string(e));
    const double *none = nullptr;
    return evaluate(t, none);
}

double evaluate_parameter_expression(
    std::string_view text, const std::function<std::optional<double>(const std::string &)> &param) {
    if (auto v = parse_spice_number(trim(text)))
        return *v;
    Resolver r;
    r.param = param;
    r.constant_only = true;
    return evaluate_constant(resolve(parse_expression(text), r));
}

std::string to_string(const ExprPtr &e) {
    using K = ExprNode::Kind;
    switch (e->kind) {
    case K::Number:
        return fmt(e->value);
    case K::Name:
        return e->name;
    case K::Time:
        return "time";
    case K::Voltage:
        return "v(" + e->name + (e->name2.empty() ? "" : "," + e->name2) + ")";
    case K::Current:
        return "i(" + e->name + ")";
    case K::Unary:
        return "(" + e->op + to_string(e->args[0]) + ")";
    case K::Binary:
        return "(" + to_string(e->args[0]) + " " + e->op + " " + to_string(e->args[1]) + ")";
    case K::Ternary:
        return "(" + to_string(e->args[0]) + " ? " + to_string(e->args[1]) + " : " +
               to_string(e->args[2]) + ")";
    case K::Call: {
        std::string s = e->name + "(";
        for (std::size_t k = 0; k < e->args.size(); ++k)
            s += (k ? ", " : "") + to_string(e->args[k]);
        return s + ")";
    }
    }
    return {};
}

std::vector<std::string> Tape::control_names() const {
    std::vector<std::string> r;
    r.reserve(controls.size());
    for (const ControlRef &c : controls) {
        switch (c.kind) {
        case ControlRef::Kind::Node:
            r.push_back(c.name);
            break;
        case ControlRef::Kind::Time:
            r.emplace_back("#T");
            break;
        case ControlRef::Kind::Current:
            r.push_back("i(" + c.name + ")");
            break;
        }
    }
    return r;
}

Tape compile(const ExprPtr &e, const CompileOptions &opt) {
    Tape t;
    Compiler c(fold(e), t, opt);
    return t;
}

} // namespace mspice::io
