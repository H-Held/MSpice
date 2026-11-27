// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mspice/sim/linear_solver.hpp>
#include <mspice/sim/sparse_plan.hpp>
#include <optional>
#include <stdexcept>
#include <utility>
#ifdef MSPICE_WITH_KLU
#include <klu.h>
#endif
namespace mspice::sim {
namespace {
/** @brief True if any of the @p count values is infinite or NaN. One vectorisable integer pass
 *  (all exponent bits set, IEEE 754) instead of std::all_of(isfinite) with its early exit; an
 *  OR reduction is associative, so the compiler can vectorise it. */
bool has_non_finite(const double *values, size_t count) {
    constexpr std::uint64_t exponent = 0x7ff0000000000000ULL;
    std::uint64_t non_finite = 0;
    for (size_t i = 0; i < count; ++i) {
        std::uint64_t bits;
        std::memcpy(&bits, &values[i], sizeof bits);
        non_finite |= static_cast<std::uint64_t>((bits & exponent) == exponent);
    }
    return non_finite != 0;
}
} // namespace
struct LinearSolver::SparseState {
    unsigned analyses{};
#ifdef MSPICE_WITH_KLU
    klu_common common{};
    klu_symbolic *symbolic{};
    klu_numeric *numeric{};
    int dimension{};
    std::vector<std::uint8_t> pattern;
    long version{-1}; ///< Caller pattern version of `pattern` (-1: unknown).
    std::vector<int> ap, ai;
    std::vector<double> ax;
    // klu_refactor keeps the pivot order of the last klu_factor. On badly scaled MNA
    // (1e-12 .. 1e9) a stale order can lose several digits without any KLU error, enough
    // to stall Newton (seen in a large mixed-signal benchmark). rcond of the last full factorization is the
    // yardstick for refactor quality; `refactored` marks factors not freshly pivoted.
    double factor_rcond{};
    bool refactored{};
    std::vector<double> residual;
    /** @brief Release numeric and symbolic factorizations, preserving counters. */
    void reset() {
        if (numeric)
            klu_free_numeric(&numeric, &common);
        if (symbolic)
            klu_free_symbolic(&symbolic, &common);
        numeric = nullptr;
        symbolic = nullptr;
        dimension = 0;
        ap.clear();
        ai.clear();
        ax.clear();
        pattern.clear();
        version = -1;
    }
    /** @brief Initialize SuiteSparse configuration. */
    SparseState() { klu_defaults(&common); }
    /** @brief Release KLU resources. */
    ~SparseState() { reset(); }
    /** @brief Set sparse pattern and factorize (or refactor) the coefficients. */
    bool factor(const std::vector<double> &a, int n, const std::vector<std::uint8_t> &used,
                long pattern_version) {
        // A matching caller version proves the pattern unchanged without the n*n comparison.
        const bool same_version =
            dimension == n && pattern_version >= 0 && pattern_version == version;
        if (!same_version && (dimension != n || pattern != used)) {
            reset();
            dimension = n;
            pattern = used;
            ap.reserve(n + 1);
            ap.push_back(0);
            for (int c = 0; c < n; ++c) {
                for (int r = 0; r < n; ++r)
                    if (used[r * n + c])
                        ai.push_back(r);
                ap.push_back(static_cast<int>(ai.size()));
            }
            symbolic = klu_analyze(n, ap.data(), ai.data(), &common);
            ++analyses;
            if (!symbolic) {
                reset();
                return false;
            }
        }
        version = pattern_version;
        ax.clear();
        ax.reserve(ai.size());
        for (int c = 0; c < n; ++c)
            for (int offset = ap[c]; offset < ap[c + 1]; ++offset)
                ax.push_back(a[ai[offset] * n + c]);
        if (numeric) {
            // Keep the cheap refactor only while its pivots stay as well conditioned as the
            // freshly pivoted ones; otherwise re-pivot with a full factorization.
            if (klu_refactor(ap.data(), ai.data(), ax.data(), symbolic, numeric, &common) &&
                common.status == KLU_OK && klu_rcond(symbolic, numeric, &common) &&
                common.rcond >= 1e-3 * factor_rcond) {
                refactored = true;
                return true;
            }
        }
        return full_factor();
    }
    /** @brief Numerically factorize ax with fresh partial pivoting (klu_factor).
     *  @return false (state reset) on failure or a singular matrix, so the caller can
     *  fall back to dense LU, which then reports the singularity itself. */
    bool full_factor() {
        if (numeric)
            klu_free_numeric(&numeric, &common);
        numeric = klu_factor(ap.data(), ai.data(), ax.data(), symbolic, &common);
        // KLU_SINGULAR is a positive warning status and still returns factors whose
        // solve yields Inf/NaN; treat it as failure.
        if (!numeric || common.status != KLU_OK || !klu_rcond(symbolic, numeric, &common)) {
            reset();
            return false;
        }
        factor_rcond = common.rcond;
        refactored = false;
        return true;
    }
    /** @brief Solve A x = b in-place with iterative refinement against the exact CSC matrix.
     *  @param b right-hand side on entry, solution on success.
     *  @return false if KLU fails or refinement does not reach ~1e-10 relative per
     *  component within a few sweeps (inaccurate factors); b is then undefined. */
    bool solve(std::vector<double> &b) {
        if (!symbolic || !numeric)
            return false;
        const std::vector<double> rhs = b;
        if (!klu_solve(symbolic, numeric, dimension, 1, b.data(), &common))
            return false;
        residual.resize(dimension);
        for (int sweep = 0; sweep < 4; ++sweep) {
            residual = rhs;
            double scale = 0;
            for (int c = 0; c < dimension; ++c) {
                scale = std::max(scale, std::abs(b[c]));
                for (int offset = ap[c]; offset < ap[c + 1]; ++offset)
                    residual[ai[offset]] -= ax[offset] * b[c];
            }
            if (!klu_solve(symbolic, numeric, dimension, 1, residual.data(), &common))
                return false;
            bool converged = std::isfinite(scale);
            for (int i = 0; i < dimension; ++i) {
                b[i] += residual[i];
                if (!(std::abs(residual[i]) <= 1e-10 * std::abs(b[i]) + 1e-14 * scale))
                    converged = false;
            }
            if (converged)
                return true;
        }
        return false;
    }
#else
    /** @brief No KLU is installed; dense solver remains available. */
    SparseState() = default;
#endif
};
/** @brief SparsePlan backend state: stamp mask, plan and its slot values. */
struct LinearSolver::PlanState {
    int n{};
    std::vector<std::uint8_t> mask; ///< Stamp mask the plan was built for.
    Pattern pattern;                ///< Pattern read from mask.
    std::vector<int> index;         ///< Dense row-major index of each pattern entry.
    std::vector<double> gathered;   ///< Matrix values in pattern order.
    std::vector<double> vals;       ///< Slot array (pattern entries plus fill-in).
    std::optional<SparsePlan> plan; ///< Current plan; empty until built.
    unsigned builds{}, fallbacks{};
    /** @brief Build a plan on the gathered values.
     *  @return false (plan cleared) if the pattern or matrix admits no plan. */
    bool build() {
        try {
            plan = SparsePlan::build(pattern, gathered.data());
        } catch (const std::exception &) {
            plan.reset();
            return false;
        }
        ++builds;
        vals.assign(static_cast<size_t>(plan->slots()), 0.0);
        return true;
    }
    /** @brief Copy the gathered values into the slot array and factor in place.
     *  @return result of SparsePlan::factor (false on a rejected pivot). */
    bool refactor() {
        std::copy(gathered.begin(), gathered.end(), vals.begin());
        return plan->factor(vals.data());
    }
    /** @brief Solve A x = b with iterative refinement against the exact pattern values,
     *  with the same acceptance as the KLU path (~1e-10 relative per component).
     *  @param b right-hand side on entry, solution on success.
     *  @return false if refinement does not converge within four sweeps. */
    bool solve(std::vector<double> &b) {
        const std::vector<double> rhs = b;
        plan->solve(vals.data(), b.data());
        residual.resize(b.size());
        for (int sweep = 0; sweep < 4; ++sweep) {
            residual = rhs;
            double scale = 0;
            for (double x : b)
                scale = std::max(scale, std::abs(x));
            for (size_t i = 0; i < gathered.size(); ++i)
                residual[pattern.row[i]] -= gathered[i] * b[pattern.col[i]];
            plan->solve(vals.data(), residual.data());
            bool converged = std::isfinite(scale);
            for (size_t i = 0; i < b.size(); ++i) {
                b[i] += residual[i];
                if (!(std::abs(residual[i]) <= 1e-10 * std::abs(b[i]) + 1e-14 * scale))
                    converged = false;
            }
            if (converged)
                return true;
        }
        return false;
    }
    std::vector<double> residual; ///< Refinement scratch.
};
LinearSolver::LinearSolver()
    : sparse_(std::make_unique<SparseState>()), plan_(std::make_unique<PlanState>()) {}
LinearSolver::~LinearSolver() = default;
void LinearSolver::retain_matrix(bool on) { retain_matrix_ = on; }
void LinearSolver::set_pattern_version(long version) { pattern_version_ = version; }
bool LinearSolver::matches(const std::vector<double> &matrix, int n) const {
    return n == size_ && !original_.empty() && matrix == original_;
}
void LinearSolver::dense_factor(const std::vector<double> &matrix, int n) {
    lu_ = matrix;
    pivots_.resize(n);
    for (int k = 0; k < n; ++k) {
        int pivot = k;
        for (int r = k + 1; r < n; ++r)
            if (std::abs(lu_[r * n + k]) > std::abs(lu_[pivot * n + k]))
                pivot = r;
        if (!std::isfinite(lu_[pivot * n + k]) || std::abs(lu_[pivot * n + k]) < 1e-20)
            throw std::runtime_error("MNA singular or ill-conditioned matrix");
        pivots_[k] = pivot;
        for (int c = 0; c < n; ++c)
            std::swap(lu_[k * n + c], lu_[pivot * n + c]);
        for (int r = k + 1; r < n; ++r) {
            // MNA rows are mostly empty: a zero multiplier would only subtract 0 * finite
            // values, so skipping it gives the same result at a fraction of the cost.
            if (lu_[r * n + k] == 0.0)
                continue;
            const double mult = lu_[r * n + k] / lu_[k * n + k];
            lu_[r * n + k] = mult;
            for (int c = k + 1; c < n; ++c)
                lu_[r * n + c] -= mult * lu_[k * n + c];
        }
    }
}
bool LinearSolver::plan_factor(const std::vector<double> &matrix, int n,
                               const std::vector<std::uint8_t> &stamped) {
    PlanState &p = *plan_;
    if (p.n != n || p.mask != stamped) {
        p.plan.reset();
        p.n = n;
        p.mask = stamped;
        p.pattern = pattern_from_stamped(stamped, n);
        p.index.resize(p.pattern.row.size());
        for (size_t i = 0; i < p.index.size(); ++i)
            p.index[i] = p.pattern.row[i] * n + p.pattern.col[i];
    }
    p.gathered.resize(p.index.size());
    for (size_t i = 0; i < p.index.size(); ++i)
        p.gathered[i] = matrix[p.index[i]];
    if (!p.plan && !p.build())
        return false;
    if (p.refactor())
        return true;
    // Pivot too small or grown away from the build-time ordering: re-order once on the
    // current values, as Sparse 1.3 does, before giving this matrix to dense LU.
    return p.build() && p.refactor();
}
void LinearSolver::factor(const std::vector<double> &matrix, int n,
                          const std::vector<std::uint8_t> &stamped, bool prefer_sparse) {
    factor(matrix, n, stamped, prefer_sparse ? LinearBackend::Auto : LinearBackend::Dense);
}
void LinearSolver::factor(const std::vector<double> &matrix, int n,
                          const std::vector<std::uint8_t> &stamped, LinearBackend backend) {
    if (n <= 0 || static_cast<int>(matrix.size()) != n * n || stamped.size() != matrix.size())
        throw std::invalid_argument("invalid MNA matrix dimensions");
    // Without retain_matrix the caller guarantees zeros outside the stamped pattern, so the KLU
    // path checks only the gathered pattern values instead of all n*n coefficients.
    bool deferred_check = false;
#ifdef MSPICE_WITH_KLU
    deferred_check = !retain_matrix_ && backend != LinearBackend::Dense &&
                     backend != LinearBackend::Plan && n >= 8;
#endif
    if (!deferred_check && has_non_finite(matrix.data(), matrix.size()))
        throw std::runtime_error("MNA has NaN or infinite coefficients");
    // Forget the old matrix first: a throwing factorization must never leave
    // matches() true for a matrix whose LU was not completed.
    original_.clear();
    size_ = 0;
    sparse_active_ = false;
    plan_active_ = false;
    if (backend == LinearBackend::Plan) {
        if (plan_factor(matrix, n, stamped)) {
            plan_active_ = true;
            original_ = matrix;
            size_ = n;
            return;
        }
        ++plan_->fallbacks;
        dense_factor(matrix, n);
        original_ = matrix;
        size_ = n;
        return;
    }
#ifdef MSPICE_WITH_KLU
    if (backend != LinearBackend::Dense && n >= 8) {
        const bool sparse_ok = sparse_->factor(matrix, n, stamped, pattern_version_);
        if (deferred_check && has_non_finite(sparse_->ax.data(), sparse_->ax.size())) {
            sparse_->reset();
            throw std::runtime_error("MNA has NaN or infinite coefficients");
        }
        if (sparse_ok) {
            sparse_active_ = true;
            // Without retain_matrix the n*n copy is skipped (15 % of the run time of a large benchmark);
            // a dense fallback rebuilds the matrix from the KLU column storage instead.
            if (retain_matrix_)
                original_ = matrix;
            size_ = n;
            return;
        }
        ++sparse_fallbacks_;
    }
#endif
    if (deferred_check && has_non_finite(matrix.data(), matrix.size()))
        throw std::runtime_error("MNA has NaN or infinite coefficients");
    dense_factor(matrix, n);
    original_ = matrix;
    size_ = n;
}
std::vector<double> LinearSolver::solve(std::vector<double> b) {
    if (static_cast<int>(b.size()) != size_)
        throw std::invalid_argument("incorrect RHS size");
    if (plan_active_) {
        std::vector<double> x = b;
        if (plan_->solve(x))
            return x;
        // Inaccurate plan factors: re-order once on this matrix, else go dense.
        if (plan_->build() && plan_->refactor()) {
            x = b;
            if (plan_->solve(x))
                return x;
        }
        ++plan_->fallbacks;
        plan_active_ = false;
        std::vector<double> matrix = std::move(original_);
        const int n = size_;
        original_.clear();
        size_ = 0;
        dense_factor(matrix, n);
        original_ = std::move(matrix);
        size_ = n;
    }
#ifdef MSPICE_WITH_KLU
    if (sparse_active_) {
        std::vector<double> x = b;
        if (sparse_->solve(x))
            return x;
        if (original_.empty()) {
            // retain_matrix(false): restore the dense matrix from the KLU column storage
            // (the stamped pattern; all other coefficients are zero by the contract).
            original_.assign(static_cast<size_t>(size_) * size_, 0.0);
            for (int c = 0; c < size_; ++c)
                for (int k = sparse_->ap[c]; k < sparse_->ap[c + 1]; ++k)
                    original_[static_cast<size_t>(sparse_->ai[k]) * size_ + c] = sparse_->ax[k];
        }
        // Inaccurate KLU factors: re-pivot a refactored matrix once, else go dense.
        if (sparse_->refactored && sparse_->full_factor()) {
            x = b;
            if (sparse_->solve(x))
                return x;
        }
        ++sparse_fallbacks_;
        sparse_active_ = false;
        // As in factor(): forget the matrix while factoring, so a throwing dense_factor
        // cannot leave matches() true for half-eliminated LU data.
        std::vector<double> matrix = std::move(original_);
        const int n = size_;
        original_.clear();
        size_ = 0;
        dense_factor(matrix, n);
        original_ = std::move(matrix);
        size_ = n;
    }
#endif
    // Apply the complete final row permutation BEFORE forward substitution.
    // Interleaving swaps and forward elimination uses already-updated RHS
    // entries with L coefficients that change under later pivot swaps.
    for (int k = 0; k < size_; ++k)
        std::swap(b[k], b[pivots_[k]]);
    for (int k = 0; k < size_; ++k)
        for (int r = k + 1; r < size_; ++r)
            b[r] -= lu_[r * size_ + k] * b[k];
    std::vector<double> x(size_);
    for (int r = size_ - 1; r >= 0; --r) {
        double v = b[r];
        for (int c = r + 1; c < size_; ++c)
            v -= lu_[r * size_ + c] * x[c];
        x[r] = v / lu_[r * size_ + r];
        if (!std::isfinite(x[r]))
            throw std::runtime_error("non-finite MNA result");
    }
    return x;
}
unsigned LinearSolver::symbolic_analyses() const { return sparse_->analyses; }
bool LinearSolver::using_sparse() const { return sparse_active_; }
unsigned LinearSolver::sparse_fallbacks() const { return sparse_fallbacks_; }
bool LinearSolver::using_plan() const { return plan_active_; }
unsigned LinearSolver::plan_builds() const { return plan_->builds; }
unsigned LinearSolver::plan_fallbacks() const { return plan_->fallbacks; }
} // namespace mspice::sim
