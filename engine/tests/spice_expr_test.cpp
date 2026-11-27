// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file spice_expr_test.cpp
 * @brief SPICE expression parser, folding, tape, Dual<N> evaluation, breakpoints.
 *
 * Modes (first argument):
 *  - `parse`: grammar, precedence, numbers with suffix, errors.
 *  - `semantics`: pow, pwr, **, ^, u, ?:, logic against values measured with ngspice-46
 *    (netlist with B sources at `op`, both default and `ngbehavior=psa`).
 *  - `expressions <file>`: every expression of tests/spice/expressions.txt parses and compiles;
 *    double vs long double reference (rel. 1e-12) and Dual<N> gradient vs central finite
 *    differences in long double (rel. 1e-6) at random points.
 *  - `breakpoints`: breakpoints from u(time-c), time > c, affine forms.
 *  - `cse`: common subexpressions go to registers, value unchanged.
 *  - `circuit`: a tape lambda satisfies sim::is_dual_behaviour_v and differentiate().
 *  - `bench`: tape vs hand-written lambda for three typical expressions, both called through
 *    sim::differentiate() like the engine does (prints ns per evaluation and the ratio; no
 *    pass/fail limit).
 */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <mspice/io/spice_expr.hpp>
#include <mspice/sim/circuit.hpp>

using namespace mspice::io;
using mspice::sim::Dual;

namespace {

int failures = 0;
int checks = 0;

/** @brief Record a failed check with a message. */
void fail(const std::string &msg) {
    ++failures;
    std::printf("FAIL %s\n", msg.c_str());
}

/** @brief Check |a-b| <= tol*max(1,|b|). */
void near(const std::string &what, double a, double b, double tol = 1e-12) {
    ++checks;
    if (!(std::abs(a - b) <= tol * std::max(1.0, std::abs(b))))
        fail(what + ": " + std::to_string(a) + " != " + std::to_string(b));
}

/** @brief Compile @p text with constants only and return its value. */
double value_of(const std::string &text, const CompileOptions &opt = {}) {
    Resolver r;
    const Tape t = compile(resolve(parse_expression(text), r), opt);
    const double *none = nullptr;
    return evaluate(t, none);
}

/** @brief Evaluate @p text with v(a) = a, v(z) = 0, v(p) = 3 (as in the reference netlist). */
double value_at(const std::string &text, double a, const CompileOptions &opt = {}) {
    const Tape t = compile(resolve(parse_expression(text), Resolver{}), opt);
    std::vector<double> v;
    for (const ControlRef &c : t.controls)
        v.push_back(c.name == "a" ? a : (c.name == "p" ? 3.0 : 0.0));
    return evaluate(t, v);
}

/** @brief True when parsing/resolving @p text throws an ExprError containing @p part. */
bool throws(const std::string &text, const std::string &part) {
    try {
        Resolver r;
        r.param = [](const std::string &n) -> std::optional<double> {
            if (n == "known")
                return 1.0;
            return std::nullopt;
        };
        resolve(parse_expression(text), r);
    } catch (const ExprError &e) {
        return std::string(e.what()).find(part) != std::string::npos;
    }
    return false;
}

/** @brief Mode parse. */
void test_parse() {
    near("suffix n", *parse_spice_number("2n"), 2e-9);
    near("suffix meg", *parse_spice_number("1meg"), 1e6);
    near("suffix m", *parse_spice_number("5m"), 5e-3);
    near("suffix mil", *parse_spice_number("1mil"), 25.4e-6);
    near("unit F", *parse_spice_number("10uF"), 10e-6);
    near("exp", *parse_spice_number("1.5e-3"), 1.5e-3);
    near("dot", *parse_spice_number(".5"), 0.5);
    near("neg", *parse_spice_number("-2k"), -2000);
    ++checks;
    if (parse_spice_number("abc") || parse_spice_number("1k2") || parse_spice_number(""))
        fail("parse_spice_number accepts non-numbers");
    near("prec mul", value_of("1+2*3"), 7);
    near("prec unary pow", value_of("-2**2"), -4);
    near("pow left assoc", value_of("2**3**2"), 64);
    near("caret", value_of("2^3"), 8);
    near("pow neg exp", value_of("2**-1"), 0.5);
    near("braces", value_of("{1/5m}"), 200);
    near("quotes", value_of("'2*(3+4)'"), 14);
    near("ternary nested", value_of("0 ? 1 : 0 ? 2 : 3"), 3);
    near("cmp chain", value_of("1 < 2 == 1"), 1);
    near("and or", value_of("0 || 1 && 0"), 0);
    near("not", value_of("!0 + !5"), 1);
    near("pi", value_of("2*pi"), 2 * 3.14159265358979323846);
    near("temper", value_of("temper"), 27);
    {
        Resolver r;
        r.param = [](const std::string &n) -> std::optional<double> {
            if (n == "fsw")
                return 400e3;
            return std::nullopt;
        };
        near("param suffix", evaluate_constant(resolve(parse_expression("{1/fsw-2n}"), r)),
             1 / 400e3 - 2e-9);
    }
    // v() takes raw node names: pi is a node here, 0 is ground.
    {
        const ExprPtr e = parse_expression("v(pi,n)*v(0)");
        ++checks;
        if (to_string(e) != "(v(pi,n) * v(0))")
            fail("to_string probe: " + to_string(e));
    }
    ++checks;
    if (!throws("foo(1)", "unknown function 'foo'"))
        fail("unknown function not reported");
    ++checks;
    if (!throws("min(1)", "expects 2"))
        fail("arity not checked");
    ++checks;
    if (!throws("unknownparam*2", "unknown name 'unknownparam'"))
        fail("unknown name not reported");
    ++checks;
    if (!throws("1 +", "operand expected"))
        fail("syntax error not reported");
    ++checks;
    if (!throws("(1+2", "expected ')'"))
        fail("missing paren not reported");
    ++checks;
    if (!throws("1 # 2", "unknown character"))
        fail("bad char not reported");
    {
        Resolver r;
        r.constant_only = true;
        ++checks;
        try {
            resolve(parse_expression("v(a)+1"), r);
            fail("v() accepted in parameter expression");
        } catch (const ExprError &) {
        }
    }
}

/** @brief Mode semantics: values measured with ngspice-46 (v(a)=-2, v(z)=0, v(p)=3). */
void test_semantics() {
    CompileOptions psa;
    psa.psa = true;
    struct Ref {
        const char *expr;
        double ngspice;     ///< default ngbehavior; NaN = op aborted (derivative NaN)
        double ngspice_psa; ///< ngbehavior=psa
    };
    const double nan = std::nan("");
    const Ref refs[] = {
        {"pow(v(a),2)", 4, 4},
        {"pow(v(a),0.5)", nan, 1.414214},
        {"pwr(v(a),0.5)", nan, 1.414214},
        {"v(a)**3", 8, 8},
        {"v(a)**0.5", nan, 1.414214},
        {"v(a)^3", 8, 8},
        {"pwr(v(a),3)", -8, 8},
        {"pwr(v(a),2)", -4, 4},
        {"u(v(a))", 0, 0},
        {"u(v(z))", 0.5, 0.5},
        {"u(v(p))", 1, 1},
        {"v(a)>-3 ? 7 : 9", 7, 7},
        {"v(z) ? 7 : 9", 9, 9},
        {"v(a) ? 7 : 9", 7, 7},
        {"-v(p)**2", -9, -9},
        {"pow(v(p),v(a))", 0.1111111, 0.1111111},
        {"v(p)**v(a)", 0.1111111, 0.1111111},
        {"2**3**2", 64, 64},
        {"uramp(v(a))", 0, 0},
        {"sgn(v(a))", -1, -1},
        {"v(a)<0 && v(p)>2 || 0", 1, 1},
        {"!v(z)", 1, 1},
        {"pow(v(z),0.5)", nan, 0},
    };
    for (const Ref &r : refs) {
        const double ours = value_at(r.expr, -2.0);
        const double ours_psa = value_at(r.expr, -2.0, psa);
        // ngspice prints 7 digits. Where default ngspice aborted the operating point (NaN
        // derivative of a negative or zero base), there is no reference value.
        if (!std::isnan(r.ngspice))
            near(std::string("ngspice ") + r.expr, ours, r.ngspice, 1e-6);
        near(std::string("ngspice psa ") + r.expr, ours_psa, r.ngspice_psa, 1e-6);
    }
    // pwr with a negative constant base is not folded, so psa still applies.
    near("pwr const default", value_of("pwr(-2,3)"), -8);
    near("pwr const psa", value_of("pwr(-2,3)", psa), 8);
    near("limit clamp", value_of("limit(3,0,2)"), 2);
}

/** @brief Deterministic parameter value for a name (0.5 .. 2.0). */
double param_value(const std::string &n) {
    unsigned h = 2166136261u;
    for (char c : n)
        h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return 0.5 + (h % 1000) / 666.0;
}

/** @brief Evaluate the tape on Dual<N> and fill value and gradient. */
template <int N>
void dual_eval(const Tape &t, const std::vector<double> &x, double &val, std::vector<double> &g) {
    std::array<Dual<N>, N> v;
    for (int i = 0; i < N; ++i)
        v[i] = mspice::sim::make_variable<N>(x[i], i);
    const Dual<N> r = evaluate(t, v.data());
    val = r.v;
    g.assign(r.d.begin(), r.d.end());
}

/** @brief Dispatch dual_eval for n = 1..8; false when n is out of range. */
bool dual_eval_n(const Tape &t, const std::vector<double> &x, double &val, std::vector<double> &g) {
    switch (x.size()) {
    case 1:
        dual_eval<1>(t, x, val, g);
        return true;
    case 2:
        dual_eval<2>(t, x, val, g);
        return true;
    case 3:
        dual_eval<3>(t, x, val, g);
        return true;
    case 4:
        dual_eval<4>(t, x, val, g);
        return true;
    case 5:
        dual_eval<5>(t, x, val, g);
        return true;
    case 6:
        dual_eval<6>(t, x, val, g);
        return true;
    case 7:
        dual_eval<7>(t, x, val, g);
        return true;
    case 8:
        dual_eval<8>(t, x, val, g);
        return true;
    default:
        return false;
    }
}

/** @brief Long-double evaluation at x with component k shifted by h. */
long double eval_ld(const Tape &t, std::vector<long double> x, int k, long double h) {
    if (k >= 0)
        x[k] += h;
    return evaluate(t, x.data());
}

/** @brief Mode expressions. */
void test_file(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
        fail("cannot open " + path);
        return;
    }
    std::string line;
    int exprs = 0, points = 0, grads = 0, nonsmooth = 0, maxctl = 0;
    std::mt19937 rng(7);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        ++exprs;
        Tape t;
        try {
            Resolver r;
            r.param = [](const std::string &n) -> std::optional<double> {
                if (n == "time" || n == "pi" || n == "temper")
                    return std::nullopt;
                return param_value(n);
            };
            t = compile(resolve(parse_expression(line), r));
        } catch (const ExprError &e) {
            fail("parse/compile '" + line + "': " + e.what());
            continue;
        }
        const int n = static_cast<int>(t.controls.size());
        maxctl = std::max(maxctl, n);
        int valid = 0;
        for (int trial = 0; trial < 60 && valid < 12; ++trial) {
            std::vector<double> x(n);
            for (int k = 0; k < n; ++k) {
                const ControlRef &c = t.controls[k];
                if (c.kind == ControlRef::Kind::Time)
                    x[k] = std::uniform_real_distribution<double>(0, 10e-3)(rng);
                else if (c.kind == ControlRef::Kind::Current)
                    x[k] = std::uniform_real_distribution<double>(-2, 2)(rng);
                else
                    x[k] = std::uniform_real_distribution<double>(-1, 26)(rng);
            }
            const double vd = evaluate(t, x);
            std::vector<long double> xl(x.begin(), x.end());
            const long double vl = eval_ld(t, xl, -1, 0);
            if (!std::isfinite(vd) || !std::isfinite(static_cast<double>(vl)))
                continue;
            ++valid;
            ++points;
            near("value '" + line + "'", vd, static_cast<double>(vl), 1e-12);
            if (n == 0)
                continue;
            double dv = 0;
            std::vector<double> g;
            if (!dual_eval_n(t, x, dv, g))
                continue;
            near("dual value '" + line + "'", dv, vd, 1e-14);
            for (int k = 0; k < n; ++k) {
                const long double h = 1e-7L * std::max(1.0L, std::abs(xl[k]));
                const long double fd = (eval_ld(t, xl, k, h) - eval_ld(t, xl, k, -h)) / (2 * h);
                const long double fd2 =
                    (eval_ld(t, xl, k, h / 8) - eval_ld(t, xl, k, -h / 8)) / (2 * h / 8);
                // Branch switch or kink inside +-h: finite differences are meaningless.
                if (std::abs(fd - fd2) > 1e-6L * std::max(1.0L, std::abs(fd)) ||
                    !std::isfinite(static_cast<double>(fd))) {
                    ++nonsmooth;
                    continue;
                }
                ++grads;
                near("grad '" + line + "' d/" + t.control_names()[k], g[k], static_cast<double>(fd),
                     1e-6);
            }
        }
        if (valid == 0 && n > 0)
            fail("no finite sample for '" + line + "'");
    }
    std::printf("expressions: %d expressions, %d points, %d gradients checked, %d skipped "
                "at kinks, at most %d controls\n",
                exprs, points, grads, nonsmooth, maxctl);
    ++checks;
    if (exprs < 60)
        fail("expected at least 60 expressions in " + path);
}

/** @brief Mode breakpoints. */
void test_breakpoints() {
    auto bps = [](const std::string &text) {
        return compile(resolve(parse_expression(text), Resolver{})).breakpoints;
    };
    const auto b1 = bps("(0.5 + 1.0*u(time-4m) + 1.5*u(time-6m) - 1.5*u(time-7m))*"
                        "min(1, max(0, v(vo)/5))");
    ++checks;
    if (b1.size() != 3 || std::abs(b1[0] - 4e-3) > 1e-15 || std::abs(b1[1] - 6e-3) > 1e-15 ||
        std::abs(b1[2] - 7e-3) > 1e-15)
        fail("u(time-c) breakpoints");
    const auto b2 = bps("time > 2m ? 1 : 0");
    const auto b3 = bps("5u < time ? v(a) : 0");
    const auto b4 = bps("u(2*time - 1m) + uramp(time/2 - 1u)");
    const auto b5 = bps("v(a) > 1 ? 1 : 0");                // voltage threshold: none
    const auto b6 = bps("min(0.8, 0.8*time*1000*v(fswk))"); // time, but no step: none
    ++checks;
    if (b2 != std::vector<double>{2e-3} || b3 != std::vector<double>{5e-6} || b4.size() != 2 ||
        std::abs(b4[0] - 2e-6) > 1e-18 || std::abs(b4[1] - 0.5e-3) > 1e-18 || !b5.empty() ||
        !b6.empty())
        fail("breakpoint detection");
    const Tape t = compile(resolve(parse_expression("u(time-4m)*v(a)"), Resolver{}));
    ++checks;
    if (t.control_names() != std::vector<std::string>{"#T", "a"})
        fail("control names with time");
}

/** @brief Mode cse. */
void test_cse() {
    const std::string text =
        "0.5*(v(a)+sqrt((v(a)-v(b))**2+1e-4)) + 0.5*(v(a)+sqrt((v(a)-v(b))**2+1e-4))";
    const Tape t = compile(resolve(parse_expression(text), Resolver{}));
    ++checks;
    if (t.registers < 1)
        fail("no register for repeated subexpression");
    int loads = 0;
    for (const Instr &in : t.code)
        loads += in.op == OpCode::Load;
    ++checks;
    if (loads < 1)
        fail("no Load emitted");
    const std::vector<double> x = {1.3, 0.4};
    const double expect = 2 * 0.5 * (1.3 + std::sqrt(0.81 + 1e-4));
    near("cse value", evaluate(t, x), expect);
    std::printf("cse: %zu instructions, %d registers, stack depth %d\n", t.code.size(), t.registers,
                t.max_stack);
}

/** @brief Mode circuit: the tape lambda plugs into the existing Dual<N> path of Circuit. */
void test_circuit() {
    const Tape t = compile(resolve(parse_expression("min(max(v(a)*2, 0), v(b))"), Resolver{}));
    auto f = [t](const auto &v) { return evaluate(t, v); };
    static_assert(mspice::sim::is_dual_behaviour_v<decltype(f)>,
                  "tape lambda must satisfy is_dual_behaviour_v");
    const mspice::sim::BehaviourGrad g = mspice::sim::differentiate(f, t.controls.size());
    ++checks;
    if (!g) {
        fail("differentiate returned empty function");
        return;
    }
    const double x[2] = {1.0, 5.0};
    double grad[2] = {0, 0};
    near("differentiate value", g(x, grad), 2.0);
    near("differentiate grad0", grad[0], 2.0);
    near("differentiate grad1", grad[1], 0.0);
}

/** @brief Time per call in ns of the gradient function the engine would get for @p f:
 *  sim::differentiate() wraps it in Dual<n> and a BehaviourGrad (std::function), exactly as
 *  Circuit::behavioural_current/voltage do for generic expressions. */
template <class F> double time_ns(F f, std::size_t n, int reps) {
    const mspice::sim::BehaviourGrad g = mspice::sim::differentiate(f, n);
    std::mt19937 rng(3);
    std::vector<double> xs(64 * n);
    for (double &e : xs)
        e = std::uniform_real_distribution<double>(0.5, 5)(rng);
    double grad[8] = {};
    double sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r)
        sink += g(&xs[(r & 63) * n], grad) + grad[0];
    const auto t1 = std::chrono::steady_clock::now();
    if (sink == 42.4242)
        std::printf(" ");
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
}

/** @brief Mode bench: tape vs hand-written lambda for three typical expressions. */
void test_bench() {
    using std::max;
    using std::min;
    using std::pow;
    using std::sqrt;
    using std::tanh;
    auto tape_of = [](const std::string &text) {
        Resolver r;
        r.param = [](const std::string &n) -> std::optional<double> {
            static const std::map<std::string, double> p = {
                {"rdo", 0.5},   {"wz", 1e-6},  {"rl0", 150.0}, {"rln", -1.8},
                {"rh0", 170.0}, {"rhn", -1.6}, {"vref", 0.8},  {"gmea", 300e-6},
                {"gmeass", 80e-6}};
            auto it = p.find(n);
            if (it == p.end())
                return std::nullopt;
            return it->second;
        };
        return compile(resolve(parse_expression(text), r));
    };
    // 1: smooth dropout limiter (3 controls), 2: logic output stage with a supply-dependent
    // resistance (5 controls), 3: soft-start transconductance amplifier (2 controls).
    const Tape t1 =
        tape_of("0.5*((v(in,gnd)-v(p,gnd))/{rdo}+sqrt(((v(in,gnd)-v(p,gnd))/{rdo})**2+{wz}*{wz}))");
    const Tape t2 =
        tape_of("v(enl)*(v(vs)-v(y,gnd))/"
                "({rl0}*pow(max(v(vcc,gnd),0.3),{rln})+({rh0}*pow(max(v(vcc,gnd),0.3),{rhn})-{rl0}*"
                "pow(max(v(vcc,gnd),0.3),{rln}))*0.5*(1+tanh((v(vs)-v(y,gnd))/0.01)))");
    const Tape t3 = tape_of("(gmeass + (gmea-gmeass)*min(1, v(vss)/vref))*(v(vss)-v(fb))");
    struct Case {
        const char *name;
        double tape_ns, lambda_ns;
    };
    std::vector<Case> cases;
    const int reps = 400000;
    {
        // controls: in, gnd, p (gnd treated as a control like in the tape)
        auto lam = [](const auto &v) {
            const auto d = (v[0] - v[1] - (v[2] - v[1])) / 0.5;
            return 0.5 * (d + sqrt(d * d + 1e-6 * 1e-6));
        };
        cases.push_back({"dropout",
                         time_ns([&](const auto &v) { return evaluate(t1, v); }, 3, reps),
                         time_ns(lam, 3, reps)});
    }
    {
        auto lam = [](const auto &v) { // enl, vs, y, gnd, vcc
            const auto vy = v[2] - v[3];
            const auto vc = max(v[4] - v[3], 0.3);
            const auto rl = 150.0 * pow(vc, -1.8);
            const auto rh = 170.0 * pow(vc, -1.6);
            return v[0] * (v[1] - vy) / (rl + (rh - rl) * 0.5 * (1 + tanh((v[1] - vy) / 0.01)));
        };
        cases.push_back({"logic out",
                         time_ns([&](const auto &v) { return evaluate(t2, v); }, 5, reps),
                         time_ns(lam, 5, reps)});
    }
    {
        auto lam = [](const auto &v) { // vss, fb
            return (80e-6 + (300e-6 - 80e-6) * min(1.0, v[0] / 0.8)) * (v[0] - v[1]);
        };
        cases.push_back({"soft-start",
                         time_ns([&](const auto &v) { return evaluate(t3, v); }, 2, reps),
                         time_ns(lam, 2, reps)});
    }
    for (const Case &c : cases)
        std::printf("bench %-12s tape %7.1f ns  lambda %7.1f ns  ratio %.2f\n", c.name,
                    c.tape_ns, c.lambda_ns, c.tape_ns / c.lambda_ns);
    // Same values from both paths (guards the hand-written lambdas used for timing).
    const double x3[3] = {12.0, 0.0, 3.3};
    near("bench t1 value", evaluate(t1, x3),
         0.5 * ((12.0 - 3.3) / 0.5 + std::sqrt(std::pow((12.0 - 3.3) / 0.5, 2) + 1e-12)));
}

} // namespace

/** @brief Run the selected mode; exit code 0 when all checks pass. */
int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "parse";
    if (mode == "parse")
        test_parse();
    else if (mode == "semantics")
        test_semantics();
    else if (mode == "expressions")
        test_file(argc > 2 ? argv[2] : "tests/spice/expressions.txt");
    else if (mode == "breakpoints")
        test_breakpoints();
    else if (mode == "cse")
        test_cse();
    else if (mode == "circuit")
        test_circuit();
    else if (mode == "bench")
        test_bench();
    else {
        std::printf("unknown mode %s\n", mode.c_str());
        return 2;
    }
    std::printf("%s: %d checks, %d failures\n", mode.c_str(), checks, failures);
    return failures == 0 ? 0 : 1;
}
