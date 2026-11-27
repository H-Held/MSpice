// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#pragma once
#include <cstdint>
#include <vector>
namespace mspice::sim {
class BatchedPlan;
/** @brief Sparsity pattern of an n x n matrix in coordinate form.
 *
 *  Entries are sorted by (row, col) and unique. Entry i corresponds to slot i of a
 *  SparsePlan built from this pattern, so callers fill the first `row.size()` slots in
 *  pattern order. */
struct Pattern {
    int n{};                       ///< Matrix dimension.
    std::vector<std::int32_t> row; ///< Row index of each entry.
    std::vector<std::int32_t> col; ///< Column index of each entry.
};

/** @brief Read a Pattern from a dense row-major stamp mask (as used by LinearSolver).
 *  @param stamped n*n mask, nonzero marks a structural entry.
 *  @param n matrix dimension.
 *  @return pattern sorted by (row, col). */
Pattern pattern_from_stamped(const std::vector<std::uint8_t> &stamped, int n);

/** @brief Fixed-structure sparse LU as a precomputed operation list.
 *
 *  build() chooses a Markowitz ordering with threshold pivoting on one numeric matrix
 *  and computes the symbolic fill once. Afterwards factor() and solve() only walk flat
 *  index arrays over the structurally nonzero slots of L and U. One plan per circuit;
 *  the plan itself is immutable after build() and can be shared between threads, each
 *  thread owning its own value array. */
class SparsePlan {
  public:
    /** @brief Pivot threshold: candidate |a| >= pivot_threshold * max |active column|. */
    static constexpr double pivot_threshold = 1e-3;
    /** @brief factor() rejects |pivot| < row_scale_tol * max |original row entry|. */
    static constexpr double row_scale_tol = 1e-13;
    /** @brief factor() rejects pivots that shrank below growth_tol of the build pivot
     *  (both measured relative to their row scale). */
    static constexpr double growth_tol = 1e-6;

    /** @brief Choose ordering and compute symbolic fill on a first numeric matrix.
     *
     *  Markowitz cost (r-1)(c-1) among entries with |a| >= 1e-3 max|active column|;
     *  diagonal candidates are searched first (Sparse 1.3 style), off-diagonal pivots only
     *  if none qualifies. Ties: larger |a|/max|column|, then smallest (row, col).
     *  @param pattern sorted, unique entries; every row and column needs one entry.
     *  @param values pattern.row.size() values in pattern order.
     *  @return plan; throws std::invalid_argument for a bad pattern and
     *          std::runtime_error when no acceptable pivot exists (singular matrix). */
    static SparsePlan build(const Pattern &pattern, const double *values);

    /** @brief Factor in place: vals has slots() entries, the first pattern-size slots
     *  hold the matrix in pattern order, fill-in slots are cleared here.
     *  @return false if a pivot is not finite, below 1e-13 x row scale, or shrank below
     *          1e-6 x its build-time size (caller then rebuilds or falls back to dense). */
    bool factor(double *vals) const;

    /** @brief Solve A x = b using slots produced by a successful factor().
     *  @param vals factored slot array.
     *  @param rhs n entries: b on input, x on output. */
    void solve(const double *vals, double *rhs) const;

    /** @brief Slot of matrix entry (r, c) including fill-in, -1 if not in the pattern. */
    int slot(int r, int c) const;
    /** @brief Number of slots including fill-in. */
    int slots() const { return slots_; }
    /** @brief Number of original pattern entries (slots 0 .. nnz()-1). */
    int nnz() const { return nnz_; }
    /** @brief Matrix dimension. */
    int size() const { return n_; }
    /** @brief Number of multiply-subtract updates per factor() (cost measure). */
    int update_count() const { return static_cast<int>(upd_.size() / 3); }
    /** @brief FNV-1a hash over pattern, ordering and operation lists (cache key). */
    std::uint64_t hash() const;

  private:
    friend class BatchedPlan; ///< Copies the operation lists (batched LU).
    int n_{}, nnz_{}, slots_{};
    std::vector<int> perm_r_, perm_c_;          ///< Row and column chosen at elimination step k.
    std::vector<int> diag_;                     ///< Pivot slot of step k.
    std::vector<double> build_ratio_;           ///< |pivot| / row scale at build, per step.
    std::vector<int> orig_row_ptr_;             ///< Original slots of row r: [ptr[r], ptr[r+1]).
    std::vector<std::int32_t> ldiv_ptr_, ldiv_; ///< Per step: L slots divided by the pivot.
    std::vector<std::int32_t> upd_ptr_, upd_;   ///< Per step: triples v[dst] -= v[l]*v[u].
    std::vector<std::int32_t> fwd_ptr_, fwd_;   ///< Per step: pairs (L slot, source step).
    std::vector<std::int32_t> bwd_ptr_, bwd_;   ///< Per step: pairs (U slot, source step).
    std::vector<int> csr_ptr_, csr_col_, csr_slot_; ///< Filled pattern for slot().
};
} // namespace mspice::sim
