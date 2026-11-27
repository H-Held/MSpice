// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
// batch_bench (feasibility study): time per variant for factor+solve of B matrices with one
// MNA pattern: SparsePlan loop, BatchedPlan scalar/SIMD, optional OpenMP over chunks.
// Usage: mspice_batch_bench [chunk=64] [repeats=3]
#include "mna_random.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mspice/sim/batched_plan.hpp>
#include <mspice/sim/sparse_plan.hpp>
#include <random>
#include <vector>
#ifdef MSPICE_HAVE_OPENMP
#include <omp.h>
#endif

using mspice::sim::BatchedPlan;
using mspice::sim::Pattern;
using mspice::sim::SparsePlan;
namespace kt = mspice::testing;

namespace {
/** @brief All variants of one benchmark size in both layouts (pristine copies). */
struct Batch {
    int nv{}, chunk{};
    std::vector<double> aos_vals, aos_rhs; ///< Variant-major: [variant][slot], [variant][row].
    std::vector<std::vector<double>> soa_vals, soa_rhs; ///< Per chunk: [slot][lane], [row][lane].
};

/** @brief Draw nv variants of structure m and pack them for all methods.
 *  @param m MNA structure.  @param p its pattern.  @param n size.  @param nnz entries.
 *  @param nv number of variants.  @param chunk SoA chunk width.  @param rng random engine. */
Batch make_batch(const kt::Mna &m, const Pattern &p, int n, int nnz, int nv, int chunk,
                 std::mt19937_64 &rng) {
    Batch b;
    b.nv = nv;
    b.chunk = chunk;
    b.aos_vals.resize(static_cast<std::size_t>(nv) * nnz);
    b.aos_rhs.resize(static_cast<std::size_t>(nv) * n);
    std::vector<double> a;
    std::vector<std::uint8_t> used;
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (int j = 0; j < nv; ++j) {
        kt::stamp(m, rng, a, used);
        kt::gather(p, a, b.aos_vals.data() + static_cast<std::size_t>(j) * nnz);
        for (int r = 0; r < n; ++r)
            b.aos_rhs[static_cast<std::size_t>(j) * n + r] = u(rng);
    }
    for (int j0 = 0; j0 < nv; j0 += chunk) {
        const int w = std::min(chunk, nv - j0);
        std::vector<double> v(static_cast<std::size_t>(nnz) * w),
            x(static_cast<std::size_t>(n) * w);
        for (int j = 0; j < w; ++j) {
            for (int s = 0; s < nnz; ++s)
                v[static_cast<std::size_t>(s) * w + j] =
                    b.aos_vals[static_cast<std::size_t>(j0 + j) * nnz + s];
            for (int r = 0; r < n; ++r)
                x[static_cast<std::size_t>(r) * w + j] =
                    b.aos_rhs[static_cast<std::size_t>(j0 + j) * n + r];
        }
        b.soa_vals.push_back(std::move(v));
        b.soa_rhs.push_back(std::move(x));
    }
    return b;
}

/** @brief Minimum over `repeats` runs of fn(), in ns per variant (fn handles reps*nv). */
double time_min(int repeats, long long variants, const std::function<double()> &fn, double &sink) {
    double best = 1e300;
    for (int r = 0; r < repeats; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        sink += fn();
        const double ns =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        best = std::min(best, ns / static_cast<double>(variants));
    }
    return best;
}

/** @brief (a) loop over variants, one SparsePlan factor+solve each (threads > 1: OpenMP). */
double run_single(const SparsePlan &plan, const Batch &b, int reps, int threads) {
    const int nnz = plan.nnz(), n = plan.size(), slots = plan.slots();
    double sum = 0.0;
#ifdef MSPICE_HAVE_OPENMP
#pragma omp parallel num_threads(threads) reduction(+ : sum) if (threads > 1)
#endif
    {
        (void)threads;
        std::vector<double> v(slots), x(n);
        // Flat (repetition, variant) index: no barrier per repetition, so small B measures
        // throughput of independent work items instead of OpenMP synchronisation.
        const long long items = static_cast<long long>(reps) * b.nv;
#ifdef MSPICE_HAVE_OPENMP
#pragma omp for schedule(static)
#endif
        for (long long it = 0; it < items; ++it) {
            {
                const int j = static_cast<int>(it % b.nv);
                std::memcpy(v.data(), b.aos_vals.data() + static_cast<std::size_t>(j) * nnz,
                            sizeof(double) * nnz);
                if (plan.factor(v.data())) {
                    std::memcpy(x.data(), b.aos_rhs.data() + static_cast<std::size_t>(j) * n,
                                sizeof(double) * n);
                    plan.solve(v.data(), x.data());
                    sum += x[j % n];
                }
            }
        }
    }
    return sum;
}

/** @brief (b)-(d) BatchedPlan over chunks of the batch (threads > 1: OpenMP over chunks). */
double run_batched(const BatchedPlan &bp, const Batch &b, int reps, BatchedPlan::Kernel kernel,
                   int threads) {
    const int nnz = bp.nnz(), n = bp.size(), slots = bp.slots();
    const int nchunks = static_cast<int>(b.soa_vals.size());
    double sum = 0.0;
#ifdef MSPICE_HAVE_OPENMP
#pragma omp parallel num_threads(threads) reduction(+ : sum) if (threads > 1)
#endif
    {
        (void)threads;
        std::vector<double> v(static_cast<std::size_t>(slots) * b.chunk),
            x(static_cast<std::size_t>(n) * b.chunk);
        std::vector<std::uint8_t> ok(b.chunk);
        const long long items = static_cast<long long>(reps) * nchunks; // see run_single
#ifdef MSPICE_HAVE_OPENMP
#pragma omp for schedule(static)
#endif
        for (long long it = 0; it < items; ++it) {
            {
                const int c = static_cast<int>(it % nchunks);
                const int w = std::min(b.chunk, b.nv - c * b.chunk);
                std::memcpy(v.data(), b.soa_vals[c].data(),
                            sizeof(double) * static_cast<std::size_t>(nnz) * w);
                bp.factor(v.data(), w, ok.data(), kernel);
                std::memcpy(x.data(), b.soa_rhs[c].data(),
                            sizeof(double) * static_cast<std::size_t>(n) * w);
                bp.solve(v.data(), w, x.data(), kernel);
                sum += x[static_cast<std::size_t>(c % n) * w] * ok[0];
            }
        }
    }
    return sum;
}
} // namespace

/** @brief Print a table of ns per variant per factor+solve for B = 1 ... 10000. */
int main(int argc, char **argv) {
    const int chunk_arg = argc > 1 ? std::atoi(argv[1]) : 64;
    const int repeats = argc > 2 ? std::atoi(argv[2]) : 3;
    std::mt19937_64 rng(42);
    const int n = 120;
    const kt::Mna m = kt::make_structure(rng, n, 210); // same seed/size as sparse_plan (d)
    std::vector<double> a;
    std::vector<std::uint8_t> used;
    kt::stamp(m, rng, a, used);
    const Pattern p = mspice::sim::pattern_from_stamped(used, n);
    std::vector<double> v0(p.row.size());
    kt::gather(p, a, v0.data());
    const SparsePlan plan = SparsePlan::build(p, v0.data());
    const BatchedPlan bp(plan);
    const int nnz = plan.nnz();
    std::printf("n=%d nnz=%d slots=%d updates=%d divisions=%d solve_macs=%d chunk=%d "
                "repeats=%d (min taken)\n",
                n, nnz, plan.slots(), bp.update_count(), bp.division_count(), bp.solve_mac_count(),
                chunk_arg, repeats);
#ifdef MSPICE_HAVE_OPENMP
    std::printf("OpenMP: yes, max threads %d\n", omp_get_max_threads());
    const std::vector<int> thread_counts{12, 24};
#else
    std::printf("OpenMP: no\n");
    const std::vector<int> thread_counts{};
#endif
    std::printf("%6s %10s %10s %10s %10s %10s", "B", "a_single", "b_scalar", "c_simd", "c_simd_wB",
                "a_omp24");
    for (const int t : thread_counts)
        std::printf("  d_omp%-4d", t);
    std::printf("   [ns per variant, factor+solve]\n");
    double sink = 0.0;
    for (const int nv : {1, 16, 256, 1000, 10000}) {
        const int chunk = std::min(chunk_arg, nv);
        const Batch b = make_batch(m, p, n, nnz, nv, chunk, rng);
        const Batch bw = make_batch(m, p, n, nnz, nv, nv, rng); // one chunk of width B
        const int reps = std::max(1, 200000 / nv);
        const long long total = static_cast<long long>(reps) * nv;
        const double ta =
            time_min(repeats, total, [&] { return run_single(plan, b, reps, 1); }, sink);
        const double tb = time_min(
            repeats, total,
            [&] { return run_batched(bp, b, reps, BatchedPlan::Kernel::Scalar, 1); }, sink);
        const double tc = time_min(
            repeats, total, [&] { return run_batched(bp, b, reps, BatchedPlan::Kernel::Simd, 1); },
            sink);
        const int reps_w = std::max(1, reps / 4);
        const double tcw = time_min(
            repeats, static_cast<long long>(reps_w) * nv,
            [&] { return run_batched(bp, bw, reps_w, BatchedPlan::Kernel::Simd, 1); }, sink);
        double ta24 = 0.0;
#ifdef MSPICE_HAVE_OPENMP
        ta24 = time_min(repeats, total, [&] { return run_single(plan, b, reps, 24); }, sink);
#endif
        std::printf("%6d %10.1f %10.1f %10.1f %10.1f %10.1f", nv, ta, tb, tc, tcw, ta24);
        for (const int t : thread_counts) {
            const double td = time_min(
                repeats, total,
                [&] { return run_batched(bp, b, reps, BatchedPlan::Kernel::Simd, t); }, sink);
            std::printf("  %10.1f", td);
        }
        std::printf("\n");
        std::fflush(stdout);
    }
    std::printf("(sink %.6g)\n", sink);
    return 0;
}
