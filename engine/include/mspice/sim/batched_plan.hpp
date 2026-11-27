// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#pragma once
#include <cstdint>
#include <mspice/sim/sparse_plan.hpp>
#include <vector>
namespace mspice::sim {
/** @brief Batched fixed-structure LU (feasibility study).
 *
 *  Runs the operation lists of one SparsePlan on w matrices with the same pattern at once.
 *  Values are stored structure-of-arrays: slot s of variant j lives at v[s * w + j]; the
 *  right-hand side likewise at rhs[r * w + j]. Every operation of the plan becomes one
 *  contiguous loop over the w variants, which the compiler can vectorise (AVX-512: 8
 *  variants per register). Large batches are split by the caller into chunks of w
 *  variants (e.g. 64) so one chunk fits in L2; chunks are independent and can run on
 *  different threads. The plan is immutable and can be shared between threads. */
class BatchedPlan {
  public:
    /** @brief Inner-loop variant used by factor()/solve(). */
    enum class Kernel {
        Scalar, ///< Same loop order, vectorisation disabled (`#pragma GCC novector`).
        Simd    ///< Inner loop over variants vectorised.
    };

    /** @brief Copy the operation lists of a built SparsePlan.
     *  @param plan plan built on one representative matrix of the pattern. */
    explicit BatchedPlan(const SparsePlan &plan);

    /** @brief Factor w matrices in place (SoA layout v[s * w + j]).
     *
     *  Same operations and pivot checks as SparsePlan::factor() for each variant. A
     *  failing variant does not stop the others; its values are then meaningless.
     *  @param v slots() * w values; the first nnz() slots per variant hold the matrix,
     *           fill-in slots are cleared here.
     *  @param w number of variants in this chunk (>= 1).
     *  @param ok w flags, set to 1 if the variant factored, 0 otherwise.
     *  @param kernel scalar or vectorised inner loop.
     *  @return number of variants that factored. */
    int factor(double *v, int w, std::uint8_t *ok, Kernel kernel = Kernel::Simd) const;

    /** @brief Solve A_j x_j = b_j for all w variants after factor().
     *  @param v factored slots, layout v[s * w + j].
     *  @param w number of variants.
     *  @param rhs n * w entries rhs[r * w + j]: b on input, x on output.
     *  @param kernel scalar or vectorised inner loop. */
    void solve(const double *v, int w, double *rhs, Kernel kernel = Kernel::Simd) const;

    /** @brief Number of slots per variant including fill-in. */
    int slots() const { return slots_; }
    /** @brief Number of original pattern entries per variant. */
    int nnz() const { return nnz_; }
    /** @brief Matrix dimension. */
    int size() const { return n_; }
    /** @brief Multiply-subtract updates per variant and factor(). */
    int update_count() const { return static_cast<int>(upd_.size() / 3); }
    /** @brief Divisions by a pivot per variant and factor(). */
    int division_count() const { return static_cast<int>(ldiv_.size()); }
    /** @brief Multiply-subtract operations per variant and solve() (L and U). */
    int solve_mac_count() const { return static_cast<int>((fwd_.size() + bwd_.size()) / 2); }

  private:
    /** @brief Kernel body; Vec selects the vectorised or the novector loops. */
    template <bool Vec> int factor_impl(double *v, int w, std::uint8_t *ok) const;
    /** @brief Kernel body; Vec selects the vectorised or the novector loops. */
    template <bool Vec> void solve_impl(const double *v, int w, double *rhs) const;

    int n_{}, nnz_{}, slots_{};
    std::vector<int> perm_r_, perm_c_, diag_, orig_row_ptr_;
    std::vector<double> build_ratio_;
    std::vector<std::int32_t> ldiv_ptr_, ldiv_, upd_ptr_, upd_, fwd_ptr_, fwd_, bwd_ptr_, bwd_;
};
} // namespace mspice::sim
