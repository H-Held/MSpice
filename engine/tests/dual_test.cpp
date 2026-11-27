// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file dual_test.cpp
 * @brief Checks Dual<N> gradients against central finite differences.
 *
 * Every operation is evaluated at 20 random points (std::mt19937 seed 1)
 * inside its domain. The dual gradient must match the central difference
 * with h = 1e-6*max(1,|x|) to a relative error below 1e-7 (absolute error
 * where the gradient is close to zero). The smooth helpers step, smax0, smin
 * and softplus used by smooth behavioural models are tested as local templates.
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>

#include <mspice/sim/dual.hpp>

using mspice::sim::Dual;
using mspice::sim::make_variable;

namespace {

/** @brief Local template of smax0. */
template <class T> T smax0(const T &x, double w) {
    using std::sqrt;
    return 0.5 * (x + sqrt(x * x + w * w));
}
/** @brief Local template of smin. */
template <class T> T smin(const T &x, const T &y, double w) {
    using std::sqrt;
    return 0.5 * (x + y - sqrt((x - y) * (x - y) + w * w));
}
/** @brief Local template of step. */
template <class T> T step(const T &x, double w) {
    using std::tanh;
    return 0.5 * (1 + tanh(x / w));
}
/** @brief Local template of softplus. */
template <class T> T softplus(const T &x) {
    using std::abs;
    using std::exp;
    using std::log1p;
    using std::max;
    return max(x, 0.0) + log1p(exp(-abs(x)));
}

int failures = 0;
int checks = 0;

/** @brief Compare one gradient component with its central difference. */
void compare(const std::string &name, double x, double g, double fd) {
    ++checks;
    const double err = std::abs(g - fd);
    const double scale = std::max(std::abs(g), 1.0);
    if (!(err < 1e-7 * scale)) {
        ++failures;
        std::printf("FAIL %-14s x=%.6g grad=%.12g fd=%.12g err=%.3g\n", name.c_str(), x, g, fd,
                    err);
    }
}

/** @brief Step size for central differences. */
double step_h(double x) { return 1e-6 * std::max(1.0, std::abs(x)); }

/** @brief Check a unary function f over 20 points from [lo, hi]. */
template <class F> void check1(const std::string &name, double lo, double hi, F f) {
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> dist(lo, hi);
    for (int k = 0; k < 20; ++k) {
        const double x = dist(rng);
        const Dual<1> r = f(make_variable<1>(x, 0));
        const double fx = f(x);
        if (std::abs(r.v - fx) > 1e-12 * std::max(1.0, std::abs(fx))) {
            ++failures;
            std::printf("FAIL %-14s value mismatch at x=%.6g\n", name.c_str(), x);
        }
        const double h = step_h(x);
        compare(name, x, r.d[0], (f(x + h) - f(x - h)) / (2 * h));
    }
}

/** @brief Check a binary function f over 20 points from [lo0,hi0] x [lo1,hi1]. */
template <class F>
void check2(const std::string &name, double lo0, double hi0, double lo1, double hi1, F f) {
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> d0(lo0, hi0), d1(lo1, hi1);
    for (int k = 0; k < 20; ++k) {
        const double x = d0(rng), y = d1(rng);
        const Dual<2> r = f(make_variable<2>(x, 0), make_variable<2>(y, 1));
        const double fx = f(x, y);
        if (std::abs(r.v - fx) > 1e-12 * std::max(1.0, std::abs(fx))) {
            ++failures;
            std::printf("FAIL %-14s value mismatch at (%.6g, %.6g)\n", name.c_str(), x, y);
        }
        const double hx = step_h(x), hy = step_h(y);
        compare(name + "/dx", x, r.d[0], (f(x + hx, y) - f(x - hx, y)) / (2 * hx));
        compare(name + "/dy", y, r.d[1], (f(x, y + hy) - f(x, y - hy)) / (2 * hy));
    }
}

} // namespace

int main() {
    using std::abs, std::exp, std::log, std::log1p, std::tanh, std::sqrt, std::pow;
    using std::min, std::max, std::clamp;
    using std::sin, std::cos, std::atan, std::sinh, std::cosh, std::floor, std::ceil;

    // Arithmetic Dual (x) Dual.
    check2("add", -5, 5, -5, 5, [](auto a, auto b) { return a + b; });
    check2("sub", -5, 5, -5, 5, [](auto a, auto b) { return a - b; });
    check2("mul", -5, 5, -5, 5, [](auto a, auto b) { return a * b; });
    check2("div", -5, 5, 0.5, 5, [](auto a, auto b) { return a / b; });
    // Arithmetic Dual (x) double, both orders.
    check1("add_d", -5, 5, [](auto a) { return a + 2.5; });
    check1("d_add", -5, 5, [](auto a) { return 2.5 + a; });
    check1("sub_d", -5, 5, [](auto a) { return a - 2.5; });
    check1("d_sub", -5, 5, [](auto a) { return 2.5 - a; });
    check1("mul_d", -5, 5, [](auto a) { return a * 2.5; });
    check1("d_mul", -5, 5, [](auto a) { return 2.5 * a; });
    check1("div_d", -5, 5, [](auto a) { return a / 2.5; });
    check1("d_div", 0.5, 5, [](auto a) { return 2.5 / a; });
    check1("neg", -5, 5, [](auto a) { return -a; });
    // Compound assignment.
    check2("compound", -3, 3, 0.5, 3, [](auto a, auto b) {
        auto r = a;
        r += b;
        r *= a;
        r -= b;
        r /= b;
        return r;
    });
    // Elementary functions.
    check1("exp", -5, 5, [](auto a) { return exp(a); });
    check1("log", 0.1, 10, [](auto a) { return log(a); });
    check1("log1p", -0.9, 10, [](auto a) { return log1p(a); });
    check1("tanh", -3, 3, [](auto a) { return tanh(a); });
    check1("sqrt", 0.1, 10, [](auto a) { return sqrt(a); });
    check1("pow_d", 0.1, 5, [](auto a) { return pow(a, 2.7); });
    check1("pow_int", -3, 3, [](auto a) { return pow(a, 3.0); });
    check2("pow_dual", 0.2, 4, -2, 2, [](auto a, auto b) { return pow(a, b); });
    check1("abs", -5, 5, [](auto a) { return abs(a); });
    // Additions for SPICE expressions: smooth functions and piecewise constants.
    check1("sin", -5, 5, [](auto a) { return sin(a); });
    check1("cos", -5, 5, [](auto a) { return cos(a); });
    check1("atan", -5, 5, [](auto a) { return atan(a); });
    check1("sinh", -3, 3, [](auto a) { return sinh(a); });
    check1("cosh", -3, 3, [](auto a) { return cosh(a); });
    check1("floor", 0.1, 0.9, [](auto a) { return floor(a); });
    check1("ceil", 0.1, 0.9, [](auto a) { return ceil(a); });
    // Branch selection via value.
    check2("min", -5, 5, -5, 5, [](auto a, auto b) { return min(a, b); });
    check2("max", -5, 5, -5, 5, [](auto a, auto b) { return max(a, b); });
    check1("min_d", -5, 5, [](auto a) { return min(a, 0.7); });
    check1("max_d", -5, 5, [](auto a) { return max(0.7, a); });
    check1("clamp", -5, 5, [](auto a) { return clamp(a, -2.0, 2.0); });
    check2("clamp_dual", -5, 5, 0.5, 4, [](auto a, auto b) { return clamp(a, -b, b); });
    // Comparisons on value.
    {
        const Dual<1> a = make_variable<1>(1.0, 0), b(2.0);
        const bool ok = a < b && b > a && a <= b && b >= a && a != b && !(a == b) && a < 1.5 &&
                        1.5 > a && a <= 1.0 && 1.0 >= a && mspice::sim::value(a) == 1.0;
        ++checks;
        if (!ok) {
            ++failures;
            std::printf("FAIL comparisons\n");
        }
    }
    // Smooth model helpers as templates.
    check1("step", -1, 1, [](auto a) { return step(a, 0.2); });
    check1("step_w05", -2, 2, [](auto a) { return step(a - 0.3, 0.5); });
    check1("smax0", -2, 2, [](auto a) { return smax0(a, 0.1); });
    check1("smax0_w1", -5, 5, [](auto a) { return smax0(a, 1.0); });
    check2("smin", -2, 2, -2, 2, [](auto a, auto b) { return smin(a, b, 0.1); });
    check1("softplus", -5, 5, [](auto a) { return softplus(a); });
    check1("softplus50", -0.2, 0.2, [](auto a) { return softplus(50 * (0.1 - a)) / 50; });
    // Mixed expression as used by behavioural models (current-limit fold-back).
    check2("foldback", -1, 3, 0.5, 2, [](auto a, auto b) {
        return 0.4 * b / (b + smax0(a - b, 0.2)) + step(a, 0.3) * softplus(b);
    });

    std::printf("dual_numbers: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
