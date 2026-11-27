// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file spice_expr.hpp
 * @brief SPICE expressions: parser, constant folding, flat tape, evaluation over Dual<N>.
 *
 * Grammar (lowest precedence first): `?:` (right associative), `||`, `&&`,
 * `== != < <= > >=`, `+ -`, `* /`, unary `- + !`, `**`/`^` (left associative like
 * ngspice-46: 2**3**2 = 64; `-x**2` = -(x**2)), primary.
 * Primaries: numbers with SPICE suffixes (f p n u m k meg g t mil), parameter names,
 * function calls, `v(a)`, `v(a,b)`, `i(name)`, `time`, `pi`, `temper` (27 degC).
 * `{...}` and `'...'` count as parentheses. Names inside `v(...)` and `i(...)` are always
 * node or device names (`v(pi,n)` reads node `pi`).
 *
 * Semantics follow ngspice B sources (checked against values measured with ngspice-46,
 * see tests/spice_expr_test.cpp): `pow(x,y)`, `x**y`, `x^y` = |x|^y; `pwr(x,y)` = sign(x)|x|^y
 * (with CompileOptions::psa: |x|^y); `u(x)` = 0 / 0.5 / 1 for x < 0 / = 0 / > 0; comparisons
 * and logic give 0 or 1; `?:` picks the branch by value != 0. Non-smooth selectors pass on
 * the derivative of the chosen branch. `limit(x,lo,hi)` clamps (as ngspice with psa; plain
 * ngspice-46 returned 0 for limit(3,0,2)). fold() keeps pwr() with a negative constant base
 * so that compile() applies the psa choice.
 *
 * Use: parse_expression() -> resolve() (parameters to constants, node/device renaming)
 * -> fold() -> compile() -> evaluate<T>(tape, v) for T = double, long double, Dual<N>.
 */
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <mspice/sim/dual.hpp>

namespace mspice::io {

/** @brief Syntax or semantic error in an expression; what() names the cause. */
class ExprError : public std::runtime_error {
  public:
    /** @brief Error with message @p msg. */
    explicit ExprError(const std::string &msg) : std::runtime_error(msg) {}
};

/** @brief Node of the expression syntax tree. */
struct ExprNode {
    /** @brief Kind of node. */
    enum class Kind {
        Number,  ///< constant `value`
        Name,    ///< bare identifier `name` (parameter, `time`, `pi`, `temper` before resolve)
        Time,    ///< simulation time
        Voltage, ///< v(name) or v(name, name2)
        Current, ///< i(name): current of a voltage source / inductor / B-V source
        Unary,   ///< op in {"-", "+", "!"}, args[0]
        Binary,  ///< op in {"+","-","*","/","**","==","!=","<","<=",">",">=","&&","||"}
        Ternary, ///< args = {cond, then, else}
        Call     ///< function `name` with args
    };
    Kind kind = Kind::Number;
    double value = 0.0; ///< Number: value
    std::string name;   ///< Name, Voltage (node), Current (device), Call (function)
    std::string name2;  ///< Voltage: second node of v(a,b), empty otherwise
    std::string op;     ///< Unary/Binary operator text
    std::vector<std::shared_ptr<ExprNode>> args; ///< operands / call arguments
};
/** @brief Shared, immutable-by-convention expression tree. */
using ExprPtr = std::shared_ptr<ExprNode>;

/** @brief Parse a SPICE number with suffix (`2n`, `1meg`, `10uF`, `.5`, `1e-3`).
 *  @param text token text (lower or upper case)
 *  @return value, or nullopt when @p text is not entirely a number (trailing letters
 *          after the scale suffix are ignored, as in SPICE)
 */
std::optional<double> parse_spice_number(std::string_view text);

/** @brief Parse an expression.
 *  @param text expression text; outer `{}` or `''` are allowed
 *  @return syntax tree
 *  @throws ExprError on syntax errors (message names the position)
 */
ExprPtr parse_expression(std::string_view text);

/** @brief Functions known to the evaluator (lower case). */
const std::vector<std::string> &known_functions();

/** @brief Name resolution for resolve(). Empty callbacks keep names unchanged / unknown. */
struct Resolver {
    /** Value of a parameter, nullopt when unknown. */
    std::function<std::optional<double>(const std::string &)> param;
    /** Flattened node name for a local node name (v(...)). */
    std::function<std::string(const std::string &)> node;
    /** Flattened device name for a local device name (i(...)). */
    std::function<std::string(const std::string &)> device;
    /** True: v(), i() and time are errors (parameter expressions). */
    bool constant_only = false;
};

/** @brief Replace parameters by constants, `pi`/`temper` by numbers, `time` by Kind::Time,
 *  and rename nodes and devices.
 *  @param e tree from parse_expression()
 *  @param r resolver
 *  @return new tree (the input is not modified)
 *  @throws ExprError for unknown names, unknown functions, wrong argument counts and,
 *          with Resolver::constant_only, for v(), i() and time
 */
ExprPtr resolve(const ExprPtr &e, const Resolver &r);

/** @brief Constant folding: every subtree without Voltage/Current/Time becomes a Number.
 *  @param e resolved tree
 *  @return folded tree
 */
ExprPtr fold(const ExprPtr &e);

/** @brief Value of a constant tree (after resolve()).
 *  @param e tree without v(), i(), time and bare names
 *  @return value
 *  @throws ExprError when the tree is not constant
 */
double evaluate_constant(const ExprPtr &e);

/** @brief Parse, resolve parameters with @p param and evaluate to a number.
 *  @param text expression (`{...}`, `'...'` or plain)
 *  @param param parameter lookup
 *  @return value
 *  @throws ExprError on any error
 */
double evaluate_parameter_expression(
    std::string_view text, const std::function<std::optional<double>(const std::string &)> &param);

/** @brief Human-readable form of a tree (fully parenthesised, for reports and tests). */
std::string to_string(const ExprPtr &e);

/** @brief Opcodes of the flat tape (postfix, fixed stack). */
enum class OpCode : std::uint8_t {
    Const,   ///< push constants[arg]
    Control, ///< push v[arg]
    Load,    ///< push register arg (common subexpression)
    Store,   ///< copy top of stack into register arg (no pop)
    Neg,
    Not,
    Add,
    Sub,
    Mul,
    Div,
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Ne,
    And,
    Or,
    Select, ///< c a b -> c != 0 ? a : b
    Pow,    ///< |a|^b
    PowC,   ///< |a|^constants[arg]
    Pwr,    ///< sign(a)|a|^b
    PwrC,   ///< sign(a)|a|^constants[arg]
    Sqr,    ///< a*a (|a|^2, replaces PowC with exponent 2: std::pow is slow)
    Min,
    Max,
    Abs,
    Sqrt,
    Exp,
    Ln,
    Log10,
    Tanh,
    Sin,
    Cos,
    Tan,
    Atan,
    Sinh,
    Cosh,
    Floor,
    Ceil,
    Step, ///< ngspice u()
    Sgn,
    Uramp,
    Limit ///< x lo hi -> clamp
};

/** @brief One tape instruction. */
struct Instr {
    OpCode op;
    std::uint32_t arg = 0;
};

/** @brief Control quantity read by a tape. */
struct ControlRef {
    /** @brief Kind of control. */
    enum class Kind { Node, Current, Time };
    Kind kind = Kind::Node;
    std::string name; ///< node name or device name; empty for Time
    /** @brief Equality (used to deduplicate). */
    bool operator==(const ControlRef &o) const { return kind == o.kind && name == o.name; }
};

/** @brief Largest stack depth / register count of a tape (stack arrays, no heap per call). */
inline constexpr int tape_max_stack = 64;
/** @brief Largest register count of a tape. */
inline constexpr int tape_max_registers = 64;

/** @brief Compiled expression: postfix code, constant pool, controls, time breakpoints. */
struct Tape {
    std::vector<Instr> code;
    std::vector<double> constants;
    std::vector<ControlRef> controls; ///< unique, in order of first appearance
    std::vector<double> breakpoints;  ///< c of u(time-c), time>c, ... (sorted, unique)
    int max_stack = 0;
    int registers = 0;
    /** @brief Control names in the form Circuit expects for nodes and time (`#T`);
     *  currents appear as `i(<device>)` and must be mapped to `#I<n>` by the caller. */
    std::vector<std::string> control_names() const;
};

/** @brief Options of compile(). */
struct CompileOptions {
    /** ngspice option ngbehavior=psa: pwr(x,y) = |x|^y
     *  like pow(); default ngspice: pwr(x,y) = sign(x)|x|^y. */
    bool psa = false;
};

/** @brief Compile a resolved tree to a tape (folds constants and merges common
 *  subexpressions first).
 *  @param e tree after resolve()
 *  @param opt compatibility options
 *  @return tape
 *  @throws ExprError when stack or register limits are exceeded
 */
Tape compile(const ExprPtr &e, const CompileOptions &opt = {});

namespace detail {
/** @brief Uninitialised slot (union member is constructed on first assignment). */
template <class T> union Slot {
    /** @brief Leaves the member unconstructed. */
    Slot() {}
    T x;
};
/** @brief Fixed array of uninitialised slots, indexed like an array of T. */
template <class T, int Cap> struct Slots {
    std::array<Slot<T>, Cap> a;
    /** @brief Element i (must have been assigned before it is read). */
    T &operator[](int i) { return a[i].x; }
};

/** @brief Value of a floating-point number (identity). */
template <class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0> T val(const T &x) {
    return x;
}
/** @brief Value of a dual number. */
template <int N> double val(const sim::Dual<N> &x) { return x.v; }

/** @brief |x|^y for floating-point types. */
template <class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
T powabs(const T &x, const T &y) {
    using std::abs;
    using std::pow;
    return pow(abs(x), y);
}
/** @brief |x|^y for dual numbers; d/dy term is 0 at x = 0 (no log(0)). */
template <int N> sim::Dual<N> powabs(const sim::Dual<N> &x, const sim::Dual<N> &y) {
    const double ax = std::abs(x.v);
    const double f = std::pow(ax, y.v);
    const double sx = x.v < 0 ? -1.0 : 1.0;
    const double dx = ax > 0 ? y.v * f / ax * sx : (y.v == 1.0 ? sx : 0.0);
    const double dy = ax > 0 ? f * std::log(ax) : 0.0;
    sim::Dual<N> r(f);
    for (int i = 0; i < N; ++i)
        r.d[i] = dx * x.d[i] + dy * y.d[i];
    return r;
}
/** @brief |x|^c with constant exponent for floating-point types. */
template <class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
T powabsc(const T &x, double c) {
    using std::abs;
    using std::pow;
    return pow(abs(x), T(c));
}
/** @brief |x|^c with constant exponent for dual numbers. */
template <int N> sim::Dual<N> powabsc(const sim::Dual<N> &x, double c) {
    const double ax = std::abs(x.v);
    const double f = std::pow(ax, c);
    const double sx = x.v < 0 ? -1.0 : 1.0;
    const double df = ax > 0 ? c * f / ax * sx : (c == 1.0 ? sx : 0.0);
    return sim::chain(x, f, df);
}
/** @brief Unit step u(x) for floating-point types (0.5 at 0). */
template <class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0> T ustep(const T &x) {
    return x > 0 ? T(1) : (x < 0 ? T(0) : T(0.5));
}
/** @brief Unit step u(x) for dual numbers. */
template <int N> sim::Dual<N> ustep(const sim::Dual<N> &x) { return sim::ustep(x, 0.5); }
} // namespace detail

/** @brief Evaluate a tape.
 *  @tparam V indexable controls: `const double*`, `const long double*`, `const Dual<N>*`,
 *          `std::vector<double>`; v[k] is the value of tape.controls[k]
 *  @param t compiled tape
 *  @param v control values
 *  @return value of the element type of @p v
 */
template <class V> auto evaluate(const Tape &t, const V &v) {
    using T = std::remove_cv_t<std::remove_reference_t<decltype(v[0])>>;
    using detail::val;
    using std::abs;
    using std::atan;
    using std::ceil;
    using std::cos;
    using std::cosh;
    using std::exp;
    using std::floor;
    using std::log;
    using std::max;
    using std::min;
    using std::sin;
    using std::sinh;
    using std::sqrt;
    using std::tanh;
    // Uninitialised slots: Dual<N> zeroes its gradient on default construction, which
    // would cost 2 x 64 x (N+1) stores per call. A trivial assignment into a union member
    // starts its lifetime (C++20 [class.union]), so no slot is read before it is written.
    detail::Slots<T, tape_max_stack> s;
    detail::Slots<T, tape_max_registers> reg;
    int sp = -1;
    for (const Instr &in : t.code) {
        switch (in.op) {
        case OpCode::Const:
            s[++sp] = T(t.constants[in.arg]);
            break;
        case OpCode::Control:
            s[++sp] = T(v[in.arg]);
            break;
        case OpCode::Load:
            s[++sp] = reg[in.arg];
            break;
        case OpCode::Store:
            reg[in.arg] = s[sp];
            break;
        case OpCode::Neg:
            s[sp] = -s[sp];
            break;
        case OpCode::Not:
            s[sp] = T(val(s[sp]) == 0 ? 1.0 : 0.0);
            break;
#define MSPICE_BIN(OPC, EXPR)                                                                     \
    case OpCode::OPC: {                                                                            \
        --sp;                                                                                      \
        const T &a = s[sp];                                                                        \
        const T &b = s[sp + 1];                                                                    \
        s[sp] = EXPR;                                                                              \
        break;                                                                                     \
    }
            MSPICE_BIN(Add, a + b)
            MSPICE_BIN(Sub, a - b)
            MSPICE_BIN(Mul, a * b)
            MSPICE_BIN(Div, a / b)
            MSPICE_BIN(Lt, T(val(a) < val(b) ? 1.0 : 0.0))
            MSPICE_BIN(Le, T(val(a) <= val(b) ? 1.0 : 0.0))
            MSPICE_BIN(Gt, T(val(a) > val(b) ? 1.0 : 0.0))
            MSPICE_BIN(Ge, T(val(a) >= val(b) ? 1.0 : 0.0))
            MSPICE_BIN(Eq, T(val(a) == val(b) ? 1.0 : 0.0))
            MSPICE_BIN(Ne, T(val(a) != val(b) ? 1.0 : 0.0))
            MSPICE_BIN(And, T(val(a) != 0 && val(b) != 0 ? 1.0 : 0.0))
            MSPICE_BIN(Or, T(val(a) != 0 || val(b) != 0 ? 1.0 : 0.0))
            MSPICE_BIN(Pow, detail::powabs(a, b))
            MSPICE_BIN(Pwr, val(a) < 0 ? -detail::powabs(a, b) : detail::powabs(a, b))
            MSPICE_BIN(Min, val(b) < val(a) ? b : a)
            MSPICE_BIN(Max, val(a) < val(b) ? b : a)
#undef MSPICE_BIN
        case OpCode::Select:
            sp -= 2;
            s[sp] = val(s[sp]) != 0 ? s[sp + 1] : s[sp + 2];
            break;
        case OpCode::PowC:
            s[sp] = detail::powabsc(s[sp], t.constants[in.arg]);
            break;
        case OpCode::PwrC:
            s[sp] = val(s[sp]) < 0 ? -detail::powabsc(s[sp], t.constants[in.arg])
                                   : detail::powabsc(s[sp], t.constants[in.arg]);
            break;
        case OpCode::Sqr:
            s[sp] = s[sp] * s[sp];
            break;
        case OpCode::Abs:
            s[sp] = val(s[sp]) < 0 ? -s[sp] : s[sp];
            break;
        case OpCode::Sqrt:
            s[sp] = sqrt(s[sp]);
            break;
        case OpCode::Exp:
            s[sp] = exp(s[sp]);
            break;
        case OpCode::Ln:
            s[sp] = log(s[sp]);
            break;
        case OpCode::Log10:
            s[sp] = log(s[sp]) / T(2.302585092994045684017991454684364208L);
            break;
        case OpCode::Tanh:
            s[sp] = tanh(s[sp]);
            break;
        case OpCode::Sin:
            s[sp] = sin(s[sp]);
            break;
        case OpCode::Cos:
            s[sp] = cos(s[sp]);
            break;
        case OpCode::Tan:
            s[sp] = sin(s[sp]) / cos(s[sp]);
            break;
        case OpCode::Atan:
            s[sp] = atan(s[sp]);
            break;
        case OpCode::Sinh:
            s[sp] = sinh(s[sp]);
            break;
        case OpCode::Cosh:
            s[sp] = cosh(s[sp]);
            break;
        case OpCode::Floor:
            s[sp] = floor(s[sp]);
            break;
        case OpCode::Ceil:
            s[sp] = ceil(s[sp]);
            break;
        case OpCode::Step:
            s[sp] = detail::ustep(s[sp]);
            break;
        case OpCode::Sgn:
            s[sp] = T(val(s[sp]) > 0 ? 1.0 : (val(s[sp]) < 0 ? -1.0 : 0.0));
            break;
        case OpCode::Uramp:
            s[sp] = val(s[sp]) > 0 ? s[sp] : T(0.0);
            break;
        case OpCode::Limit:
            sp -= 2; // x lo hi
            if (val(s[sp]) < val(s[sp + 1]))
                s[sp] = s[sp + 1];
            else if (val(s[sp + 2]) < val(s[sp]))
                s[sp] = s[sp + 2];
            break;
        }
    }
    return s[0];
}

} // namespace mspice::io
