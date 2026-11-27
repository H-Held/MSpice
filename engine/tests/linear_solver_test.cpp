// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <cmath>
#include <iostream>
#include <mspice/sim/linear_solver.hpp>
#include <vector>
/** @brief Regression for pivoted LU with two independent voltage constraints. */
int main() {
    using mspice::sim::LinearSolver;
    // R,C, two ideal source branches: the historical implementation returned
    // a false 0.000992 V for x[2] instead of the exact 0 V constraint.
    std::vector<double> a = {.001, -.001, 0, 1, 0, -.001, .126, -.025, 0, 0, 0, -.025, .025,
                             0,    1,     1, 0, 0, 0,     0,    0,     0, 1, 0, 0};
    std::vector<std::uint8_t> pattern(a.size(), 1);
    LinearSolver lu;
    lu.factor(a, 5, pattern, true);
    auto x = lu.solve({0, 0, 0, 5, 0});
    if (std::abs(x[0] - 5) > 1e-12 || std::abs(x[2]) > 1e-12 ||
        std::abs(x[1] - 0.03968253968253968) > 1e-12) {
        std::cerr << "Source constraint violated: x0=" << x[0] << " x1=" << x[1] << " x2=" << x[2]
                  << '\n';
        return 1;
    }
    for (int i = 0; i < 5; ++i) {
        double rhs = i == 3 ? 5 : 0;
        for (int j = 0; j < 5; ++j)
            rhs -= a[i * 5 + j] * x[j];
        if (std::abs(rhs) > 1e-12)
            return 2;
    }
    if (!lu.matches(a, 5))
        return 3;
    // A 32x32 sparse tridiagonal system exercises the KLU path when enabled.
    // Both a changed numeric matrix and an unchanged structural pattern are
    // checked; the result is independently compared with dense LU.
    constexpr int n = 32;
    std::vector<double> matrix(n * n, 0), expected(n), rhs(n, 0);
    std::vector<std::uint8_t> used(n * n, 0);
    for (int i = 0; i < n; ++i) {
        expected[i] = 0.5 + 0.03 * i;
        matrix[i * n + i] = 4.0 + 0.01 * i;
        used[i * n + i] = 1;
        if (i) {
            matrix[i * n + i - 1] = -1;
            used[i * n + i - 1] = 1;
        }
        if (i + 1 < n) {
            matrix[i * n + i + 1] = -1;
            used[i * n + i + 1] = 1;
        }
    }
    for (int version = 0; version < 2; ++version) {
        if (version)
            for (int i = 0; i < n; ++i)
                matrix[i * n + i] += 0.5;
        for (int i = 0; i < n; ++i) {
            rhs[i] = 0;
            for (int j = 0; j < n; ++j)
                rhs[i] += matrix[i * n + j] * expected[j];
        }
        lu.factor(matrix, n, used, true);
        auto actual = lu.solve(rhs);
        for (int i = 0; i < n; ++i)
            if (std::abs(actual[i] - expected[i]) > 1e-10)
                return 4;
        LinearSolver dense;
        dense.factor(matrix, n, used, false);
        auto baseline = dense.solve(rhs);
        for (int i = 0; i < n; ++i)
            if (std::abs(actual[i] - baseline[i]) > 1e-10)
                return 5;
    }
    if (lu.using_sparse() && lu.symbolic_analyses() != 1)
        return 6;
    // A failed (singular) factorization must not be reported as reusable:
    // fast mode would otherwise solve with the stale LU of the previous matrix.
    std::vector<double> singular = {1, 1, 1, 1};
    std::vector<std::uint8_t> two(4, 1);
    LinearSolver reuse;
    reuse.factor({2, 0, 0, 2}, 2, two, false);
    try {
        reuse.factor(singular, 2, two, false);
    } catch (const std::exception &) {
    }
    if (reuse.matches(singular, 2))
        return 7;
    // Refactor regression: klu_refactor keeps the pivot order of the first
    // factorization. When the former diagonal pivot drops to 1e-14 the stale order loses all
    // accuracy; the solver must re-pivot (or refine/fall back) instead of returning it.
    constexpr int m = 8;
    std::vector<double> block(m * m, 0), want(m), b(m);
    std::vector<std::uint8_t> full(m * m, 0);
    for (int i = 0; i < m; ++i) {
        block[i * m + i] = 1;
        full[i * m + i] = 1;
        want[i] = 1.0 + i;
    }
    full[1] = full[m] = full[m + 1] = 1;
    LinearSolver pivoted;
    for (double corner : {1.0, 1e-14}) {
        block[0] = corner;
        block[1] = 1;
        block[m] = corner == 1.0 ? 0.5 : 1.0;
        block[m + 1] = 1;
        for (int i = 0; i < m; ++i) {
            b[i] = 0;
            for (int j = 0; j < m; ++j)
                b[i] += block[i * m + j] * want[j];
        }
        pivoted.factor(block, m, full, true);
        auto got = pivoted.solve(b);
        for (int i = 0; i < m; ++i)
            if (std::abs(got[i] - want[i]) > 1e-10 * std::abs(want[i])) {
                std::cerr << "refactor inaccurate: x" << i << "=" << got[i] << '\n';
                return 8;
            }
    }
    // Plan backend: same refactor sequence; the corner pivot 1e-14 must trigger a
    // plan rebuild or a dense fallback, never an inaccurate answer.
    using mspice::sim::LinearBackend;
    LinearSolver plan;
    for (double corner : {1.0, 1e-14, 1.0}) {
        block[0] = corner;
        block[m] = corner == 1.0 ? 0.5 : 1.0;
        for (int i = 0; i < m; ++i) {
            b[i] = 0;
            for (int j = 0; j < m; ++j)
                b[i] += block[i * m + j] * want[j];
        }
        plan.factor(block, m, full, LinearBackend::Plan);
        auto got = plan.solve(b);
        for (int i = 0; i < m; ++i)
            if (std::abs(got[i] - want[i]) > 1e-10 * std::abs(want[i])) {
                std::cerr << "plan inaccurate: x" << i << "=" << got[i] << '\n';
                return 9;
            }
    }
    if (plan.plan_builds() < 1 || !plan.matches(block, m))
        return 10;
    // Tridiagonal system through the plan: one build for an unchanged pattern.
    LinearSolver tri;
    for (int i = 0; i < n; ++i) {
        rhs[i] = 0;
        for (int j = 0; j < n; ++j)
            rhs[i] += matrix[i * n + j] * expected[j];
    }
    tri.factor(matrix, n, used, LinearBackend::Plan);
    tri.factor(matrix, n, used, LinearBackend::Plan);
    auto via_plan = tri.solve(rhs);
    for (int i = 0; i < n; ++i)
        if (std::abs(via_plan[i] - expected[i]) > 1e-10)
            return 11;
    if (!tri.using_plan() || tri.plan_builds() != 1 || tri.plan_fallbacks() != 0)
        return 12;
}
