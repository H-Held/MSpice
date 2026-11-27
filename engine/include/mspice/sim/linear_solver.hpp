// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
namespace mspice::sim {
class SparsePlan;
/** @brief Linear-solver backend selection for LinearSolver::factor and Settings. */
enum class LinearBackend {
    Auto,  ///< SuiteSparse-KLU when compiled (n >= 8), else dense LU (historical default).
    Dense, ///< Partial-pivot dense LU only.
    Klu,   ///< SuiteSparse-KLU when compiled (n >= 8), else dense LU; same as Auto.
    Plan   ///< Fixed-structure sparse LU (SparsePlan), dense LU on repeated pivot failure.
};
/** @brief Factored linear-system solver, with optional SuiteSparse/KLU backend. */
class LinearSolver {
  public:
    /** @brief Initialize backend state. */
    LinearSolver();
    /** @brief Free backend allocations. */
    ~LinearSolver();
    LinearSolver(const LinearSolver &) = delete;
    LinearSolver &operator=(const LinearSolver &) = delete;
    /** @brief Reuse symbolic structure when available; refactor changed numeric coefficients. */
    void factor(const std::vector<double> &matrix, int n, const std::vector<std::uint8_t> &stamped,
                bool prefer_sparse);
    /** @brief Factor with an explicit backend.
     *
     *  Plan builds a SparsePlan from `stamped` on first use and whenever the stamp pattern
     *  changes, then refactors on the fixed operation list. If the plan rejects a pivot it
     *  is rebuilt once on the current values; a second rejection factors dense LU for this
     *  matrix (counted in plan_fallbacks).
     *  @param matrix n*n row-major coefficients.
     *  @param n dimension.
     *  @param stamped n*n structural mask (nonzero = entry may be nonzero).
     *  @param backend backend to use; Auto and Klu behave as factor(..., true). */
    void factor(const std::vector<double> &matrix, int n, const std::vector<std::uint8_t> &stamped,
                LinearBackend backend);
    /** @brief Solve Ax=b with an already factored A; KLU results are iteratively refined
     *  and fall back to dense LU (counted in sparse_fallbacks) if they stay inaccurate. */
    std::vector<double> solve(std::vector<double> rhs);
    /** @brief Return whether factors are valid for exactly this matrix. */
    /** @brief Keep a copy of the last factored matrix (default). With false the SuiteSparse-KLU
     *  path skips the n*n copy; matches() is then false, and a dense fallback rebuilds the
     *  matrix from the stamped pattern, so the caller must guarantee that every coefficient
     *  outside @p stamped is zero (Circuit::run does). */
    void retain_matrix(bool on);
    /** @brief Version number of the stamped pattern passed to the next factor() calls; the KLU
     *  path skips the n*n pattern comparison while it stays the same. The caller must change
     *  it whenever the pattern changes (Circuit::run: count of ever-stamped entries, the
     *  pattern only grows). -1 (default) always compares. */
    void set_pattern_version(long version);
    bool matches(const std::vector<double> &matrix, int n) const;
    /** @brief Number of symbolic sparse analyses (not numeric factorizations). */
    unsigned symbolic_analyses() const;
    /** @brief Whether current factors use KLU. */
    bool using_sparse() const;
    /** @brief Count KLU failures recovered through a dense fallback. */
    unsigned sparse_fallbacks() const;
    /** @brief Whether current factors use the fixed-structure SparsePlan. */
    bool using_plan() const;
    /** @brief Number of SparsePlan builds (first use, pattern changes, pivot rebuilds). */
    unsigned plan_builds() const;
    /** @brief Number of matrices the plan could not factor and dense LU took over. */
    unsigned plan_fallbacks() const;

  private:
    struct SparseState;
    std::unique_ptr<SparseState> sparse_;
    std::vector<double> original_, lu_;
    std::vector<int> pivots_;
    int size_{};
    bool sparse_active_{};
    unsigned sparse_fallbacks_{};
    struct PlanState;
    std::unique_ptr<PlanState> plan_;
    bool plan_active_{};
    bool retain_matrix_{true};
    long pattern_version_{-1};
    /** @brief Compute partial-pivot dense LU as a portable fallback. */
    void dense_factor(const std::vector<double> &matrix, int n);
    /** @brief Try the SparsePlan backend (build or rebuild as needed).
     *  @return true if plan factors are valid for matrix, false to fall back to dense. */
    bool plan_factor(const std::vector<double> &matrix, int n,
                     const std::vector<std::uint8_t> &stamped);
};
} // namespace mspice::sim
