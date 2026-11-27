// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <mspice/sim/batched_plan.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <stdexcept>

// Loop pragmas: the scalar kernel keeps the SoA loop order but forbids vectorisation, so
// the scalar/SIMD comparison measures only the vector units.
#if defined(__clang__)
#define MSPICE_NOVEC _Pragma("clang loop vectorize(disable) interleave(disable)")
#define MSPICE_VEC _Pragma("clang loop vectorize(enable)")
#elif defined(__GNUC__) && __GNUC__ >= 14
#define MSPICE_NOVEC _Pragma("GCC novector")
#define MSPICE_VEC _Pragma("GCC ivdep")
#else
#define MSPICE_NOVEC
#define MSPICE_VEC
#endif

/// Loop over the w variants of one slot row; VEC selects the vectorised kernel.
#define MSPICE_LANES(VEC, ...)                                                                    \
    do {                                                                                           \
        if constexpr (VEC) {                                                                       \
            MSPICE_VEC for (int j = 0; j < w; ++j) { __VA_ARGS__ }                                \
        } else {                                                                                   \
            MSPICE_NOVEC for (int j = 0; j < w; ++j) { __VA_ARGS__ }                              \
        }                                                                                          \
    } while (0)

namespace mspice::sim {

BatchedPlan::BatchedPlan(const SparsePlan &p)
    : n_(p.n_), nnz_(p.nnz_), slots_(p.slots_), perm_r_(p.perm_r_), perm_c_(p.perm_c_),
      diag_(p.diag_), orig_row_ptr_(p.orig_row_ptr_), build_ratio_(p.build_ratio_),
      ldiv_ptr_(p.ldiv_ptr_), ldiv_(p.ldiv_), upd_ptr_(p.upd_ptr_), upd_(p.upd_),
      fwd_ptr_(p.fwd_ptr_), fwd_(p.fwd_), bwd_ptr_(p.bwd_ptr_), bwd_(p.bwd_) {
    if (n_ <= 0)
        throw std::invalid_argument("BatchedPlan: plan not built");
}

template <bool Vec> int BatchedPlan::factor_impl(double *v, int w, std::uint8_t *ok) const {
    thread_local std::vector<double> scale_buf;
    scale_buf.assign(static_cast<std::size_t>(n_) * w, 0.0);
    double *scale = scale_buf.data();
    std::fill(ok, ok + w, std::uint8_t{1});
    // Row scale and finiteness of the original entries, per variant.
    for (int r = 0; r < n_; ++r) {
        double *__restrict sc = scale + static_cast<std::size_t>(r) * w;
        for (int s = orig_row_ptr_[r]; s < orig_row_ptr_[r + 1]; ++s) {
            const double *__restrict x = v + static_cast<std::size_t>(s) * w;
            MSPICE_LANES(Vec, const double a = std::abs(x[j]); sc[j] = a > sc[j] ? a : sc[j];
                          ok[j] &= static_cast<std::uint8_t>(a <= DBL_MAX););
        }
    }
    std::memset(v + static_cast<std::size_t>(nnz_) * w, 0,
                sizeof(double) * static_cast<std::size_t>(slots_ - nnz_) * w);
    const std::int32_t *ld = ldiv_.data();
    const std::int32_t *up = upd_.data();
    for (int k = 0; k < n_; ++k) {
        const double *__restrict pv = v + static_cast<std::size_t>(diag_[k]) * w;
        const double *__restrict sc = scale + static_cast<std::size_t>(perm_r_[k]) * w;
        const double g = SparsePlan::growth_tol * build_ratio_[k];
        // Same acceptance test as SparsePlan::factor(), evaluated without branches.
        MSPICE_LANES(Vec, const double a = std::abs(pv[j]); const double s = sc[j];
                      const bool good = (a <= DBL_MAX) & (a != 0.0) &
                                        !(a < SparsePlan::row_scale_tol * s) & !(a < g * s);
                      ok[j] &= static_cast<std::uint8_t>(good););
        for (int i = ldiv_ptr_[k]; i < ldiv_ptr_[k + 1]; ++i) {
            double *__restrict x = v + static_cast<std::size_t>(ld[i]) * w;
            MSPICE_LANES(Vec, x[j] /= pv[j];);
        }
        for (int i = upd_ptr_[k]; i < upd_ptr_[k + 1]; i += 3) {
            double *__restrict d = v + static_cast<std::size_t>(up[i]) * w;
            const double *__restrict l = v + static_cast<std::size_t>(up[i + 1]) * w;
            const double *__restrict u = v + static_cast<std::size_t>(up[i + 2]) * w;
            MSPICE_LANES(Vec, d[j] -= l[j] * u[j];);
        }
    }
    int good = 0;
    for (int j = 0; j < w; ++j)
        good += ok[j];
    return good;
}

template <bool Vec> void BatchedPlan::solve_impl(const double *v, int w, double *rhs) const {
    thread_local std::vector<double> work;
    work.resize(static_cast<std::size_t>(n_) * w);
    double *wk = work.data();
    const std::int32_t *f = fwd_.data();
    const std::int32_t *b = bwd_.data();
    for (int k = 0; k < n_; ++k) { // L z = P b, unit diagonal
        double *__restrict x = wk + static_cast<std::size_t>(k) * w;
        const double *__restrict src = rhs + static_cast<std::size_t>(perm_r_[k]) * w;
        MSPICE_LANES(Vec, x[j] = src[j];);
        for (int i = fwd_ptr_[k]; i < fwd_ptr_[k + 1]; i += 2) {
            const double *__restrict l = v + static_cast<std::size_t>(f[i]) * w;
            const double *__restrict z = wk + static_cast<std::size_t>(f[i + 1]) * w;
            MSPICE_LANES(Vec, x[j] -= l[j] * z[j];);
        }
    }
    for (int k = n_ - 1; k >= 0; --k) { // U y = z
        double *__restrict x = wk + static_cast<std::size_t>(k) * w;
        for (int i = bwd_ptr_[k]; i < bwd_ptr_[k + 1]; i += 2) {
            const double *__restrict u = v + static_cast<std::size_t>(b[i]) * w;
            const double *__restrict z = wk + static_cast<std::size_t>(b[i + 1]) * w;
            MSPICE_LANES(Vec, x[j] -= u[j] * z[j];);
        }
        const double *__restrict d = v + static_cast<std::size_t>(diag_[k]) * w;
        MSPICE_LANES(Vec, x[j] /= d[j];);
    }
    for (int k = 0; k < n_; ++k) {
        const double *__restrict x = wk + static_cast<std::size_t>(k) * w;
        double *__restrict dst = rhs + static_cast<std::size_t>(perm_c_[k]) * w;
        MSPICE_LANES(Vec, dst[j] = x[j];);
    }
}

int BatchedPlan::factor(double *v, int w, std::uint8_t *ok, Kernel kernel) const {
    if (w < 1 || !v || !ok)
        throw std::invalid_argument("BatchedPlan::factor: bad arguments");
    return kernel == Kernel::Simd ? factor_impl<true>(v, w, ok) : factor_impl<false>(v, w, ok);
}

void BatchedPlan::solve(const double *v, int w, double *rhs, Kernel kernel) const {
    if (w < 1 || !v || !rhs)
        throw std::invalid_argument("BatchedPlan::solve: bad arguments");
    if (kernel == Kernel::Simd)
        solve_impl<true>(v, w, rhs);
    else
        solve_impl<false>(v, w, rhs);
}
} // namespace mspice::sim
