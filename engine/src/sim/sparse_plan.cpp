// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <mspice/sim/sparse_plan.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace mspice::sim {
namespace {
/** @brief Mix a range of integers into an FNV-1a 64-bit hash. */
template <class T> void fnv_mix(std::uint64_t &h, const std::vector<T> &v) {
    for (const T x : v) {
        auto u = static_cast<std::uint64_t>(static_cast<std::int64_t>(x));
        for (int b = 0; b < 8; ++b) {
            h ^= (u >> (8 * b)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
}
/** @brief Flatten per-step lists into an offset array plus a flat entry array. */
void flatten(const std::vector<std::vector<std::int32_t>> &lists, std::vector<std::int32_t> &ptr,
             std::vector<std::int32_t> &flat) {
    ptr.assign(lists.size() + 1, 0);
    flat.clear();
    for (std::size_t k = 0; k < lists.size(); ++k) {
        flat.insert(flat.end(), lists[k].begin(), lists[k].end());
        ptr[k + 1] = static_cast<std::int32_t>(flat.size());
    }
}
} // namespace

Pattern pattern_from_stamped(const std::vector<std::uint8_t> &stamped, int n) {
    if (n <= 0 || stamped.size() != static_cast<std::size_t>(n) * n)
        throw std::invalid_argument("pattern_from_stamped: size mismatch");
    Pattern p;
    p.n = n;
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c)
            if (stamped[static_cast<std::size_t>(r) * n + c]) {
                p.row.push_back(r);
                p.col.push_back(c);
            }
    return p;
}

SparsePlan SparsePlan::build(const Pattern &pattern, const double *values) {
    const int n = pattern.n;
    const std::size_t nnz = pattern.row.size();
    if (n <= 0 || pattern.col.size() != nnz || nnz == 0 || !values)
        throw std::invalid_argument("SparsePlan: invalid pattern");
    for (std::size_t i = 0; i < nnz; ++i) {
        const int r = pattern.row[i], c = pattern.col[i];
        if (r < 0 || r >= n || c < 0 || c >= n)
            throw std::invalid_argument("SparsePlan: index out of range");
        if (i > 0 && std::tie(pattern.row[i - 1], pattern.col[i - 1]) >= std::tie(r, c))
            throw std::invalid_argument("SparsePlan: pattern not sorted/unique");
        if (!std::isfinite(values[i]))
            throw std::runtime_error("SparsePlan: non-finite value");
    }
    SparsePlan p;
    p.n_ = n;
    p.nnz_ = static_cast<int>(nnz);
    const auto at = [n](int r, int c) { return static_cast<std::size_t>(r) * n + c; };
    // Dense work tables exist only during build; n is a few hundred for MNA systems.
    std::vector<int> id(static_cast<std::size_t>(n) * n, -1);
    std::vector<double> val(static_cast<std::size_t>(n) * n, 0.0);
    std::vector<std::vector<int>> rows(n), cols(n);
    std::vector<long long> rcnt(n, 0), ccnt(n, 0);
    std::vector<double> scale(n, 0.0);
    p.orig_row_ptr_.assign(n + 1, 0);
    for (std::size_t i = 0; i < nnz; ++i) {
        const int r = pattern.row[i], c = pattern.col[i];
        id[at(r, c)] = static_cast<int>(i);
        val[at(r, c)] = values[i];
        rows[r].push_back(c);
        cols[c].push_back(r);
        ++rcnt[r];
        ++ccnt[c];
        scale[r] = std::max(scale[r], std::abs(values[i]));
        p.orig_row_ptr_[r + 1] = static_cast<int>(i + 1);
    }
    for (int r = 0; r < n; ++r) // rows without entries keep the previous end offset
        p.orig_row_ptr_[r + 1] = std::max(p.orig_row_ptr_[r + 1], p.orig_row_ptr_[r]);
    int slots = static_cast<int>(nnz);
    std::vector<char> act_r(n, 1), act_c(n, 1);
    std::vector<std::vector<std::int32_t>> ldiv(n), upd(n);
    // Raw L entries (row, slot, step) and U entries (col, slot) per step; mapped to
    // solve-order source steps once the full ordering is known.
    std::vector<std::tuple<int, int, int>> l_raw;
    std::vector<std::vector<std::pair<int, int>>> u_raw(n);
    p.perm_r_.resize(n);
    p.perm_c_.resize(n);
    p.diag_.resize(n);
    p.build_ratio_.resize(n);
    std::vector<int> lrows, ucols;
    for (int k = 0; k < n; ++k) {
        // Markowitz search with threshold pivoting. As in Sparse 1.3 (ngspice), diagonal
        // candidates are searched first; off-diagonal pivots only when no diagonal entry
        // passes the threshold (e.g. voltage-source rows with zero diagonal). Equal cost:
        // larger |a|/max|column| first (limits element growth), then smallest (row, col).
        long long best = std::numeric_limits<long long>::max();
        double best_rel = 0.0;
        int br = -1, bc = -1;
        for (int pass = 0; pass < 2 && br < 0; ++pass) {
            for (int c = 0; c < n; ++c) {
                if (!act_c[c] || (pass == 0 && !act_r[c]))
                    continue;
                double cmax = 0.0;
                for (const int r : cols[c])
                    if (act_r[r])
                        cmax = std::max(cmax, std::abs(val[at(r, c)]));
                if (cmax == 0.0)
                    continue;
                const double thr = pivot_threshold * cmax;
                for (const int r : cols[c]) {
                    if (!act_r[r] || (pass == 0 && r != c))
                        continue;
                    const double a = std::abs(val[at(r, c)]);
                    if (a == 0.0 || a < thr)
                        continue;
                    const long long cost = (rcnt[r] - 1) * (ccnt[c] - 1);
                    const double rel = a / cmax;
                    if (cost < best ||
                        (cost == best && (rel > best_rel || (rel == best_rel &&
                                                             std::tie(r, c) < std::tie(br, bc))))) {
                        best = cost;
                        best_rel = rel;
                        br = r;
                        bc = c;
                    }
                }
            }
        }
        if (br < 0)
            throw std::runtime_error("SparsePlan: singular matrix, no acceptable pivot");
        const double piv = val[at(br, bc)];
        p.perm_r_[k] = br;
        p.perm_c_[k] = bc;
        p.diag_[k] = id[at(br, bc)];
        p.build_ratio_[k] = scale[br] > 0.0 ? std::abs(piv) / scale[br] : 0.0;
        lrows.clear();
        ucols.clear();
        for (const int r : cols[bc])
            if (act_r[r] && r != br)
                lrows.push_back(r);
        for (const int c : rows[br])
            if (act_c[c] && c != bc)
                ucols.push_back(c);
        std::sort(lrows.begin(), lrows.end());
        std::sort(ucols.begin(), ucols.end());
        for (const int c : ucols)
            u_raw[k].emplace_back(c, id[at(br, c)]);
        for (const int r : lrows) {
            const int ls = id[at(r, bc)];
            const double m = val[at(r, bc)] / piv;
            val[at(r, bc)] = m;
            ldiv[k].push_back(ls);
            l_raw.emplace_back(r, ls, k);
            for (const int c : ucols) {
                int &dst = id[at(r, c)];
                if (dst < 0) { // symbolic fill-in, kept even if it cancels numerically
                    dst = slots++;
                    rows[r].push_back(c);
                    cols[c].push_back(r);
                    ++rcnt[r];
                    ++ccnt[c];
                }
                val[at(r, c)] -= m * val[at(br, c)];
                upd[k].insert(upd[k].end(), {dst, ls, id[at(br, c)]});
            }
        }
        act_r[br] = 0;
        act_c[bc] = 0;
        for (const int c : ucols)
            --ccnt[c];
        for (const int r : lrows)
            --rcnt[r];
    }
    p.slots_ = slots;
    flatten(ldiv, p.ldiv_ptr_, p.ldiv_);
    flatten(upd, p.upd_ptr_, p.upd_);
    std::vector<int> step_r(n), step_c(n);
    for (int k = 0; k < n; ++k) {
        step_r[p.perm_r_[k]] = k;
        step_c[p.perm_c_[k]] = k;
    }
    std::vector<std::vector<std::int32_t>> fwd(n), bwd(n);
    for (const auto &[r, s, k] : l_raw) // L(step_r[r], k) with k < step_r[r]
        fwd[step_r[r]].insert(fwd[step_r[r]].end(), {s, k});
    for (int k = 0; k < n; ++k)
        for (const auto &[c, s] : u_raw[k]) // U(k, step_c[c]) with step_c[c] > k
            bwd[k].insert(bwd[k].end(), {s, step_c[c]});
    flatten(fwd, p.fwd_ptr_, p.fwd_);
    flatten(bwd, p.bwd_ptr_, p.bwd_);
    p.csr_ptr_.assign(n + 1, 0);
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c)
            if (id[at(r, c)] >= 0) {
                p.csr_col_.push_back(c);
                p.csr_slot_.push_back(id[at(r, c)]);
            }
        p.csr_ptr_[r + 1] = static_cast<int>(p.csr_col_.size());
    }
    return p;
}

bool SparsePlan::factor(double *v) const {
    thread_local std::vector<double> scale;
    scale.assign(n_, 0.0);
    for (int r = 0; r < n_; ++r)
        for (int s = orig_row_ptr_[r]; s < orig_row_ptr_[r + 1]; ++s) {
            if (!std::isfinite(v[s]))
                return false;
            scale[r] = std::max(scale[r], std::abs(v[s]));
        }
    std::fill(v + nnz_, v + slots_, 0.0);
    const std::int32_t *ld = ldiv_.data();
    const std::int32_t *up = upd_.data();
    for (int k = 0; k < n_; ++k) {
        const double piv = v[diag_[k]];
        const double a = std::abs(piv), s = scale[perm_r_[k]];
        if (!std::isfinite(piv) || a == 0.0 || a < row_scale_tol * s ||
            a < growth_tol * build_ratio_[k] * s)
            return false;
        for (int i = ldiv_ptr_[k]; i < ldiv_ptr_[k + 1]; ++i)
            v[ld[i]] /= piv;
        for (int i = upd_ptr_[k]; i < upd_ptr_[k + 1]; i += 3)
            v[up[i]] -= v[up[i + 1]] * v[up[i + 2]];
    }
    return true;
}

void SparsePlan::solve(const double *v, double *rhs) const {
    thread_local std::vector<double> w;
    w.resize(n_);
    const std::int32_t *f = fwd_.data();
    const std::int32_t *b = bwd_.data();
    for (int k = 0; k < n_; ++k) { // L z = P b, unit diagonal
        double x = rhs[perm_r_[k]];
        for (int i = fwd_ptr_[k]; i < fwd_ptr_[k + 1]; i += 2)
            x -= v[f[i]] * w[f[i + 1]];
        w[k] = x;
    }
    for (int k = n_ - 1; k >= 0; --k) { // U y = z
        double x = w[k];
        for (int i = bwd_ptr_[k]; i < bwd_ptr_[k + 1]; i += 2)
            x -= v[b[i]] * w[b[i + 1]];
        w[k] = x / v[diag_[k]];
    }
    for (int k = 0; k < n_; ++k)
        rhs[perm_c_[k]] = w[k];
}

int SparsePlan::slot(int r, int c) const {
    if (r < 0 || r >= n_ || c < 0 || c >= n_)
        return -1;
    const auto first = csr_col_.begin() + csr_ptr_[r];
    const auto last = csr_col_.begin() + csr_ptr_[r + 1];
    const auto it = std::lower_bound(first, last, c);
    return (it != last && *it == c) ? csr_slot_[it - csr_col_.begin()] : -1;
}

std::uint64_t SparsePlan::hash() const {
    std::uint64_t h = 1469598103934665603ull;
    fnv_mix(h, std::vector<int>{n_, nnz_, slots_});
    fnv_mix(h, csr_ptr_);
    fnv_mix(h, csr_col_);
    fnv_mix(h, csr_slot_);
    fnv_mix(h, perm_r_);
    fnv_mix(h, perm_c_);
    fnv_mix(h, diag_);
    fnv_mix(h, ldiv_ptr_);
    fnv_mix(h, ldiv_);
    fnv_mix(h, upd_ptr_);
    fnv_mix(h, upd_);
    fnv_mix(h, fwd_ptr_);
    fnv_mix(h, fwd_);
    fnv_mix(h, bwd_ptr_);
    fnv_mix(h, bwd_);
    return h;
}
} // namespace mspice::sim
