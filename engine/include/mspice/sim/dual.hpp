// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file dual.hpp
 * @brief Forward-mode dual numbers for exact gradients of behavioural sources.
 *
 * A Dual<N> carries a value and its gradient with respect to N independent
 * variables. Smooth functions propagate the gradient by the chain rule;
 * non-smooth selectors (abs, min, max, clamp) pick the branch from the value
 * and return the derivative of the chosen branch, as the numeric code does.
 */
#pragma once

#include <array>
#include <cmath>

namespace mspice::sim {

/**
 * @brief Value with gradient over N variables.
 * @tparam N number of independent variables
 */
template <int N> struct Dual {
    /** @brief Function value. */
    double v = 0.0;
    /** @brief Partial derivatives d v / d x_i. */
    std::array<double, N> d{};

    /** @brief Zero value, zero gradient. */
    constexpr Dual() = default;
    /** @brief Constant: value c, zero gradient (implicit to allow mixing with double). */
    constexpr Dual(double c) : v(c), d{} {}
    /** @brief Value and explicit gradient. */
    constexpr Dual(double value, const std::array<double, N> &grad) : v(value), d(grad) {}

    /** @brief Add another dual in place. */
    Dual &operator+=(const Dual &o) {
        v += o.v;
        for (int i = 0; i < N; ++i)
            d[i] += o.d[i];
        return *this;
    }
    /** @brief Subtract another dual in place. */
    Dual &operator-=(const Dual &o) {
        v -= o.v;
        for (int i = 0; i < N; ++i)
            d[i] -= o.d[i];
        return *this;
    }
    /** @brief Multiply by another dual in place (product rule). */
    Dual &operator*=(const Dual &o) {
        for (int i = 0; i < N; ++i)
            d[i] = d[i] * o.v + v * o.d[i];
        v *= o.v;
        return *this;
    }
    /** @brief Divide by another dual in place (quotient rule). */
    Dual &operator/=(const Dual &o) {
        const double inv = 1.0 / o.v;
        const double q = v * inv;
        for (int i = 0; i < N; ++i)
            d[i] = (d[i] - q * o.d[i]) * inv;
        v = q;
        return *this;
    }
};

/** @brief Independent variable: value x, unit gradient in slot i. */
template <int N> Dual<N> make_variable(double x, int i) {
    Dual<N> r(x);
    r.d[i] = 1.0;
    return r;
}

/** @brief Array of N independent variables seeded from xs[0..N-1]. */
template <int N> std::array<Dual<N>, N> make_variables(const double *xs) {
    std::array<Dual<N>, N> r;
    for (int i = 0; i < N; ++i)
        r[i] = make_variable<N>(xs[i], i);
    return r;
}

/** @brief Value of a dual number. */
template <int N> double value(const Dual<N> &x) { return x.v; }
/** @brief Value of a plain double (identity, for generic code). */
inline double value(double x) { return x; }

/** @brief Apply chain rule: result value f, derivative df times x's gradient. */
template <int N> Dual<N> chain(const Dual<N> &x, double f, double df) {
    Dual<N> r(f);
    for (int i = 0; i < N; ++i)
        r.d[i] = df * x.d[i];
    return r;
}

/** @brief Unary plus. */
template <int N> Dual<N> operator+(const Dual<N> &a) { return a; }
/** @brief Unary minus. */
template <int N> Dual<N> operator-(const Dual<N> &a) { return chain(a, -a.v, -1.0); }

/** @brief Dual + Dual. */
template <int N> Dual<N> operator+(Dual<N> a, const Dual<N> &b) { return a += b; }
/** @brief Dual + double. */
template <int N> Dual<N> operator+(Dual<N> a, double b) {
    a.v += b;
    return a;
}
/** @brief double + Dual. */
template <int N> Dual<N> operator+(double a, Dual<N> b) {
    b.v += a;
    return b;
}

/** @brief Dual - Dual. */
template <int N> Dual<N> operator-(Dual<N> a, const Dual<N> &b) { return a -= b; }
/** @brief Dual - double. */
template <int N> Dual<N> operator-(Dual<N> a, double b) {
    a.v -= b;
    return a;
}
/** @brief double - Dual. */
template <int N> Dual<N> operator-(double a, const Dual<N> &b) { return chain(b, a - b.v, -1.0); }

/** @brief Dual * Dual. */
template <int N> Dual<N> operator*(Dual<N> a, const Dual<N> &b) { return a *= b; }
/** @brief Dual * double. */
template <int N> Dual<N> operator*(const Dual<N> &a, double b) { return chain(a, a.v * b, b); }
/** @brief double * Dual. */
template <int N> Dual<N> operator*(double a, const Dual<N> &b) { return chain(b, a * b.v, a); }

/** @brief Dual / Dual. */
template <int N> Dual<N> operator/(Dual<N> a, const Dual<N> &b) { return a /= b; }
/** @brief Dual / double. */
template <int N> Dual<N> operator/(const Dual<N> &a, double b) {
    return chain(a, a.v / b, 1.0 / b);
}
/** @brief double / Dual. */
template <int N> Dual<N> operator/(double a, const Dual<N> &b) {
    const double q = a / b.v;
    return chain(b, q, -q / b.v);
}

/** @brief Comparison on values: a < b. */
template <int N> bool operator<(const Dual<N> &a, const Dual<N> &b) { return a.v < b.v; }
/** @brief Comparison on values: a > b. */
template <int N> bool operator>(const Dual<N> &a, const Dual<N> &b) { return a.v > b.v; }
/** @brief Comparison on values: a <= b. */
template <int N> bool operator<=(const Dual<N> &a, const Dual<N> &b) { return a.v <= b.v; }
/** @brief Comparison on values: a >= b. */
template <int N> bool operator>=(const Dual<N> &a, const Dual<N> &b) { return a.v >= b.v; }
/** @brief Comparison on values: a == b. */
template <int N> bool operator==(const Dual<N> &a, const Dual<N> &b) { return a.v == b.v; }
/** @brief Comparison on values: a != b. */
template <int N> bool operator!=(const Dual<N> &a, const Dual<N> &b) { return a.v != b.v; }
/** @brief Comparison on values: Dual < double. */
template <int N> bool operator<(const Dual<N> &a, double b) { return a.v < b; }
/** @brief Comparison on values: Dual > double. */
template <int N> bool operator>(const Dual<N> &a, double b) { return a.v > b; }
/** @brief Comparison on values: Dual <= double. */
template <int N> bool operator<=(const Dual<N> &a, double b) { return a.v <= b; }
/** @brief Comparison on values: Dual >= double. */
template <int N> bool operator>=(const Dual<N> &a, double b) { return a.v >= b; }
/** @brief Comparison on values: double < Dual. */
template <int N> bool operator<(double a, const Dual<N> &b) { return a < b.v; }
/** @brief Comparison on values: double > Dual. */
template <int N> bool operator>(double a, const Dual<N> &b) { return a > b.v; }
/** @brief Comparison on values: double <= Dual. */
template <int N> bool operator<=(double a, const Dual<N> &b) { return a <= b.v; }
/** @brief Comparison on values: double >= Dual. */
template <int N> bool operator>=(double a, const Dual<N> &b) { return a >= b.v; }

/** @brief Exponential; d/dx exp(x) = exp(x). */
template <int N> Dual<N> exp(const Dual<N> &x) {
    const double e = std::exp(x.v);
    return chain(x, e, e);
}
/** @brief Natural logarithm (x > 0); d/dx = 1/x. */
template <int N> Dual<N> log(const Dual<N> &x) { return chain(x, std::log(x.v), 1.0 / x.v); }
/** @brief log(1+x) (x > -1); d/dx = 1/(1+x). */
template <int N> Dual<N> log1p(const Dual<N> &x) {
    return chain(x, std::log1p(x.v), 1.0 / (1.0 + x.v));
}
/** @brief Hyperbolic tangent; d/dx = 1 - tanh^2. */
template <int N> Dual<N> tanh(const Dual<N> &x) {
    const double t = std::tanh(x.v);
    return chain(x, t, 1.0 - t * t);
}
/** @brief Square root (x > 0 for a finite gradient); d/dx = 1/(2 sqrt x). */
template <int N> Dual<N> sqrt(const Dual<N> &x) {
    const double s = std::sqrt(x.v);
    return chain(x, s, 0.5 / s);
}
/** @brief Power with constant exponent; d/dx x^p = p x^(p-1). */
template <int N> Dual<N> pow(const Dual<N> &x, double p) {
    return chain(x, std::pow(x.v, p), p * std::pow(x.v, p - 1.0));
}
/** @brief Power with dual exponent (x > 0): d = y x^(y-1) dx + x^y ln(x) dy. */
template <int N> Dual<N> pow(const Dual<N> &x, const Dual<N> &y) {
    const double f = std::pow(x.v, y.v);
    const double dx = y.v * std::pow(x.v, y.v - 1.0);
    const double dy = f * std::log(x.v);
    Dual<N> r(f);
    for (int i = 0; i < N; ++i)
        r.d[i] = dx * x.d[i] + dy * y.d[i];
    return r;
}
/** @brief Absolute value; branch by sign of the value (derivative +1 at 0). */
template <int N> Dual<N> abs(const Dual<N> &x) { return x.v < 0 ? -x : x; }
/** @brief Minimum; the branch with the smaller value is returned (a on ties). */
template <int N> Dual<N> min(const Dual<N> &a, const Dual<N> &b) { return b.v < a.v ? b : a; }
/** @brief Maximum; the branch with the larger value is returned (a on ties). */
template <int N> Dual<N> max(const Dual<N> &a, const Dual<N> &b) { return a.v < b.v ? b : a; }
/** @brief min(Dual, double). */
template <int N> Dual<N> min(const Dual<N> &a, double b) { return min(a, Dual<N>(b)); }
/** @brief min(double, Dual). */
template <int N> Dual<N> min(double a, const Dual<N> &b) { return min(Dual<N>(a), b); }
/** @brief max(Dual, double). */
template <int N> Dual<N> max(const Dual<N> &a, double b) { return max(a, Dual<N>(b)); }
/** @brief max(double, Dual). */
template <int N> Dual<N> max(double a, const Dual<N> &b) { return max(Dual<N>(a), b); }
/** @brief Clamp x to [lo, hi]; the selected branch carries the gradient. */
template <int N> Dual<N> clamp(const Dual<N> &x, const Dual<N> &lo, const Dual<N> &hi) {
    if (x.v < lo.v)
        return lo;
    if (hi.v < x.v)
        return hi;
    return x;
}
/** @brief Clamp x to constant bounds [lo, hi]. */
template <int N> Dual<N> clamp(const Dual<N> &x, double lo, double hi) {
    return clamp(x, Dual<N>(lo), Dual<N>(hi));
}

/** @brief Sine; d/dx sin(x) = cos(x) (SPICE expressions). */
template <int N> Dual<N> sin(const Dual<N> &x) { return chain(x, std::sin(x.v), std::cos(x.v)); }
/** @brief Cosine; d/dx cos(x) = -sin(x). */
template <int N> Dual<N> cos(const Dual<N> &x) { return chain(x, std::cos(x.v), -std::sin(x.v)); }
/** @brief Arc tangent; d/dx atan(x) = 1/(1+x^2). */
template <int N> Dual<N> atan(const Dual<N> &x) {
    return chain(x, std::atan(x.v), 1.0 / (1.0 + x.v * x.v));
}
/** @brief Hyperbolic sine; d/dx sinh(x) = cosh(x). */
template <int N> Dual<N> sinh(const Dual<N> &x) { return chain(x, std::sinh(x.v), std::cosh(x.v)); }
/** @brief Hyperbolic cosine; d/dx cosh(x) = sinh(x). */
template <int N> Dual<N> cosh(const Dual<N> &x) { return chain(x, std::cosh(x.v), std::sinh(x.v)); }
/** @brief Floor; piecewise constant, so the derivative is 0 everywhere it exists. */
template <int N> Dual<N> floor(const Dual<N> &x) { return Dual<N>(std::floor(x.v)); }
/** @brief Ceiling; piecewise constant, derivative 0. */
template <int N> Dual<N> ceil(const Dual<N> &x) { return Dual<N>(std::ceil(x.v)); }
/** @brief Unit step as ngspice u(x): 0 for x < 0, 1 for x > 0, @p at_zero at x = 0;
 *  derivative 0 (the jump itself is handled by breakpoints, not by Newton). */
template <int N> Dual<N> ustep(const Dual<N> &x, double at_zero = 0.5) {
    return Dual<N>(x.v > 0.0 ? 1.0 : (x.v < 0.0 ? 0.0 : at_zero));
}

} // namespace mspice::sim
