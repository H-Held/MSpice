// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
// batched_plan: BatchedPlan (SoA, many variants) must reproduce SparsePlan per variant.
#include "mna_random.hpp"

#include <cmath>
#include <cstdio>
#include <mspice/sim/batched_plan.hpp>
#include <mspice/sim/sparse_plan.hpp>
#include <random>
#include <vector>

using mspice::sim::BatchedPlan;
using mspice::sim::Pattern;
using mspice::sim::SparsePlan;
namespace kt = mspice::testing;

namespace {
/** @brief Relative max-norm difference between x and y. */
double rel_diff(const double *x, const double *y, int n) {
    double d = 0.0, s = 0.0;
    for (int i = 0; i < n; ++i) {
        d = std::max(d, std::abs(x[i] - y[i]));
        s = std::max(s, std::abs(y[i]));
    }
    return d / std::max(s, 1e-300);
}

/** @brief Compare batched factor+solve in chunks of width w against per-variant results.
 *  @return worst relative difference, or -1 on a mismatch of the ok flags. */
double check(const BatchedPlan &bp, const std::vector<std::vector<double>> &vals,
             const std::vector<std::vector<double>> &rhs,
             const std::vector<std::vector<double>> &ref, const std::vector<char> &ref_ok,
             int chunk, BatchedPlan::Kernel kernel) {
    const int nv = static_cast<int>(vals.size()), n = bp.size(), slots = bp.slots();
    double worst = 0.0;
    for (int j0 = 0; j0 < nv; j0 += chunk) {
        const int w = std::min(chunk, nv - j0);
        std::vector<double> v(static_cast<std::size_t>(slots) * w, 7.0), x(n * w);
        for (int j = 0; j < w; ++j) {
            for (int s = 0; s < bp.nnz(); ++s)
                v[static_cast<std::size_t>(s) * w + j] = vals[j0 + j][s];
            for (int r = 0; r < n; ++r)
                x[r * w + j] = rhs[j0 + j][r];
        }
        std::vector<std::uint8_t> ok(w);
        bp.factor(v.data(), w, ok.data(), kernel);
        bp.solve(v.data(), w, x.data(), kernel);
        for (int j = 0; j < w; ++j) {
            if (static_cast<char>(ok[j]) != ref_ok[j0 + j])
                return -1.0;
            if (!ok[j])
                continue;
            std::vector<double> xj(n);
            for (int r = 0; r < n; ++r)
                xj[r] = x[r * w + j];
            worst = std::max(worst, rel_diff(xj.data(), ref[j0 + j].data(), n));
        }
    }
    return worst;
}
} // namespace

/** @brief 64 random variants of one n=120 MNA pattern (one made singular, one NaN):
 *  batched results must match SparsePlan per variant (rel < 1e-12, identical ok flags)
 *  for both kernels and chunk widths 64, 13, 8 and 1. */
int main() {
    std::mt19937_64 rng(25);
    const int n = 120, nv = 64;
    const kt::Mna m = kt::make_structure(rng, n, 210);
    std::vector<double> a;
    std::vector<std::uint8_t> used;
    kt::stamp(m, rng, a, used);
    const Pattern p = mspice::sim::pattern_from_stamped(used, n);
    std::vector<double> v0(p.row.size());
    kt::gather(p, a, v0.data());
    const SparsePlan plan = SparsePlan::build(p, v0.data());
    const BatchedPlan bp(plan);
    std::vector<std::vector<double>> vals(nv), rhs(nv), ref(nv);
    std::vector<char> ref_ok(nv);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (int j = 0; j < nv; ++j) {
        std::vector<std::uint8_t> used2;
        kt::stamp(m, rng, a, used2);
        if (used2 != used)
            return 1;
        vals[j].assign(plan.slots(), 0.0);
        kt::gather(p, a, vals[j].data());
        if (j == 17) // all-zero matrix: every pivot vanishes
            std::fill(vals[j].begin(), vals[j].end(), 0.0);
        if (j == 40)
            vals[j][3] = std::nan("");
        rhs[j].resize(n);
        for (auto &x : rhs[j])
            x = u(rng);
        std::vector<double> work = vals[j];
        ref_ok[j] = static_cast<char>(plan.factor(work.data()));
        ref[j] = rhs[j];
        if (ref_ok[j])
            plan.solve(work.data(), ref[j].data());
    }
    if (ref_ok[17] || ref_ok[40])
        return 2;
    double worst = 0.0;
    for (const auto kernel : {BatchedPlan::Kernel::Scalar, BatchedPlan::Kernel::Simd})
        for (const int chunk : {64, 13, 8, 1}) {
            const double d = check(bp, vals, rhs, ref, ref_ok, chunk, kernel);
            if (d < 0.0) {
                std::fprintf(stderr, "ok flags differ (chunk %d)\n", chunk);
                return 3;
            }
            if (!(d < 1e-12)) {
                std::fprintf(stderr, "chunk %d rel diff %.3g\n", chunk, d);
                return 4;
            }
            worst = std::max(worst, d);
        }
    int good = 0;
    for (const char c : ref_ok)
        good += c;
    std::printf("batched_plan: %d variants (%d factorable), n=%d slots=%d, worst rel diff %.3g\n",
                nv, good, n, plan.slots(), worst);
    return 0;
}
