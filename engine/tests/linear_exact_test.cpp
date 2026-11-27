// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mspice/sim/linear_exact.hpp>
#include <mspice/sim/linear_solver.hpp>
#include <numbers>
#include <vector>
using mspice::sim::ExactStepper;
using mspice::sim::LinearDae;

namespace {
/** @brief Add a conductance g between nodes a and b (-1 = ground) into row-major G. */
void stamp_g(std::vector<double> &g, int n, int a, int b, double v) {
    if (a >= 0)
        g[a * n + a] += v;
    if (b >= 0)
        g[b * n + b] += v;
    if (a >= 0 && b >= 0) {
        g[a * n + b] -= v;
        g[b * n + a] -= v;
    }
}
/** @brief Stamp an ideal voltage source a(+) b(-) with branch row k driven by input column col. */
void stamp_v(LinearDae &d, int p, int a, int b, int k, int col) {
    const int n = d.n;
    if (a >= 0) {
        d.G[a * n + k] += 1;
        d.G[k * n + a] += 1;
    }
    if (b >= 0) {
        d.G[b * n + k] -= 1;
        d.G[k * n + b] -= 1;
    }
    d.B[k * p + col] = 1;
}
/** @brief Stamp an inductor a-b with branch current row k: v_a - v_b - L i' = 0. */
void stamp_l(LinearDae &d, int a, int b, int k, double l) {
    const int n = d.n;
    if (a >= 0) {
        d.G[a * n + k] += 1;
        d.G[k * n + a] -= 1;
    }
    if (b >= 0) {
        d.G[b * n + k] -= 1;
        d.G[k * n + b] += 1;
    }
    d.C[k * n + k] = l;
}
/** @brief Allocate an empty DAE with n unknowns and p inputs. */
LinearDae make(int n, int p) {
    return LinearDae{n, std::vector<double>(n * n), std::vector<double>(n * n),
                     std::vector<double>(n * p)};
}

/** @brief RC low-pass step response at h = tau, analytic, relative error < 1e-9. */
int test_rc() {
    const double r = 1e3, c = 1e-6, tau = r * c;
    // Unknowns: v_in, v_out, i_V.
    auto d = make(3, 1);
    stamp_g(d.G, 3, 0, 1, 1 / r);
    d.C[1 * 3 + 1] = c;
    stamp_v(d, 1, 0, -1, 2, 0);
    const int m = 16;
    ExactStepper st(d, tau, m);
    std::vector<double> x = {1.0, 0.0, -1.0 / r}; // consistent with u = 1 at t = 0+
    const double u = 1.0;
    double worst = 0;
    for (int k = 1; k <= 8; ++k) {
        st.step(x.data(), &u, &u);
        const double ref = 1 - std::exp(-k);
        worst = std::max(worst, std::abs(x[1] - ref) / ref);
    }
    std::printf("rc: doublings=%d max_rel_err=%.3e\n", m, worst);
    return worst < 1e-9 ? 0 : 1;
}

/** @brief LC tank 1 mH || 1 uF || 1 kOhm fed via 100 Ohm, h = 10 us, 300 steps, < 1e-8 V. */
int test_lc_tank() {
    const double rs = 100, rl = 1e3, l = 1e-3, c = 1e-6;
    // Unknowns: v_in, v_t, i_V, i_L.
    auto d = make(4, 1);
    stamp_g(d.G, 4, 0, 1, 1 / rs);
    stamp_g(d.G, 4, 1, -1, 1 / rl);
    d.C[1 * 4 + 1] = c;
    stamp_v(d, 1, 0, -1, 2, 0);
    stamp_l(d, 1, -1, 3, l);
    const double h = 10e-6, u = 1.0;
    const int m = 16;
    ExactStepper st(d, h, m);
    std::vector<double> x = {1.0, 0.0, -1.0 / rs, 0.0};
    const double alpha = (1 / rs + 1 / rl) / (2 * c);
    const double wd = std::sqrt(1 / (l * c) - alpha * alpha);
    const double dv0 = u / rs / c, iinf = u / rs;
    double worst_v = 0, worst_i = 0;
    for (int k = 1; k <= 300; ++k) {
        st.step(x.data(), &u, &u);
        const double t = k * h, e = std::exp(-alpha * t);
        const double v = dv0 / wd * e * std::sin(wd * t);
        const double i = iinf * (1 - e * (std::cos(wd * t) + alpha / wd * std::sin(wd * t)));
        worst_v = std::max(worst_v, std::abs(x[1] - v));
        worst_i = std::max(worst_i, std::abs(x[3] - i));
    }
    std::printf("lc_tank: doublings=%d max_err_v=%.3e V max_err_iL=%.3e A\n", m, worst_v, worst_i);
    return worst_v < 1e-8 && worst_i < 1e-10 ? 0 : 1;
}

/** @brief Plain trapezoid reference with linear input interpolation and `sub` sub-steps per h. */
std::vector<std::vector<double>> trapezoid_ref(const LinearDae &d, double h, int sub, int steps,
                                               const std::function<double(double)> &u,
                                               std::vector<double> x) {
    const int n = d.n;
    const double s = h / sub;
    std::vector<double> a(n * n), b(n * n);
    for (int i = 0; i < n * n; ++i) {
        a[i] = d.C[i] / s + 0.5 * d.G[i];
        b[i] = d.C[i] / s - 0.5 * d.G[i];
    }
    mspice::sim::LinearSolver lu;
    lu.factor(a, n, std::vector<std::uint8_t>(n * n, 1), false);
    std::vector<std::vector<double>> out;
    for (int k = 0; k < steps; ++k) {
        const double u0 = u(k * h), u1 = u((k + 1) * h);
        for (int j = 0; j < sub; ++j) {
            const double ua = u0 + (u1 - u0) * j / sub, ub = u0 + (u1 - u0) * (j + 1) / sub;
            std::vector<double> rhs(n, 0.0);
            for (int i = 0; i < n; ++i) {
                for (int q = 0; q < n; ++q)
                    rhs[i] += b[i * n + q] * x[q];
                rhs[i] += d.B[i] * 0.5 * (ua + ub);
            }
            x = lu.solve(rhs);
        }
        out.push_back(x);
    }
    return out;
}

/** @brief Series LC with floating capacitor (lc_bandpass_5k topology), PWL sine input. */
int test_floating_cap() {
    // Vin in 0; Rs in a 10; L1 a m 10m; C1 m out 100n; RL out 0 50.
    // Unknowns: in, a, m, out, i_V, i_L.
    auto d = make(6, 1);
    stamp_g(d.G, 6, 0, 1, 1 / 10.0);
    stamp_g(d.G, 6, 3, -1, 1 / 50.0);
    const double c = 100e-9;
    d.C[2 * 6 + 2] += c;
    d.C[3 * 6 + 3] += c;
    d.C[2 * 6 + 3] -= c;
    d.C[3 * 6 + 2] -= c;
    stamp_v(d, 1, 0, -1, 4, 0);
    stamp_l(d, 1, 2, 5, 10e-3);
    const double fr = 5033.0, h = 1 / (fr * 20);
    const int steps = 200, m = 16;
    auto u = [&](double t) {
        return std::sin(2 * std::numbers::pi * fr * t);
    }; // PWL samples at k*h
    ExactStepper st(d, h, m);
    std::vector<double> x0(6, 0.0); // consistent: u(0) = 0, all quiet
    auto r1 = trapezoid_ref(d, h, 1000, steps, u, x0);
    auto r2 = trapezoid_ref(d, h, 2000, steps, u, x0);
    std::vector<double> x = x0;
    double err_plain = 0, err_rich = 0, peak = 0;
    for (int k = 0; k < steps; ++k) {
        const double u0 = u(k * h), u1 = u((k + 1) * h);
        st.step(x.data(), &u0, &u1);
        for (int i = 0; i < 4; ++i) {
            const double rich = (4 * r2[k][i] - r1[k][i]) / 3;
            err_plain = std::max(err_plain, std::abs(x[i] - r1[k][i]));
            err_rich = std::max(err_rich, std::abs(x[i] - rich));
            peak = std::max(peak, std::abs(x[i]));
        }
    }
    std::printf("floating_cap: doublings=%d err_vs_trap(h/1000)=%.3e V err_vs_richardson=%.3e V "
                "peak=%.3f V\n",
                m, err_plain, err_rich, peak);
    // Richardson floor ~7e-9 V is reference round-off (400k sub-steps); m = 12 would give 3e-8 V.
    return err_plain < 1e-6 && err_rich < 2e-8 ? 0 : 1;
}
} // namespace

/** @brief Run the exact-stepper checks; nonzero exit on failure. */
int main() {
    int rc = test_rc();
    rc |= test_lc_tank() << 1;
    rc |= test_floating_cap() << 2;
    return rc;
}
