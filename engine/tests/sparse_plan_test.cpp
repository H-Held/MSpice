// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <mspice/sim/linear_solver.hpp>
#include <mspice/sim/sparse_plan.hpp>
#include <random>
#include <set>
#include <utility>
#include <vector>

using mspice::sim::LinearSolver;
using mspice::sim::Pattern;
using mspice::sim::SparsePlan;

namespace {
/** @brief Random MNA-like structure: node conductances plus ideal voltage sources. */
struct Mna {
    int n{}, nodes{};
    std::vector<std::pair<int, int>> edges;   ///< Conductances between two nodes.
    std::vector<std::pair<int, int>> couple;  ///< Small unsymmetric (controlled) entries.
    std::vector<std::pair<int, int>> sources; ///< (node, node or -1 for ground).
    std::vector<char> zero_diag;              ///< Source row stores a structural 0 diagonal.
};

/** @brief Draw a connected random node graph with nedges edges and n-nodes sources. */
Mna make_structure(std::mt19937_64 &rng, int n, int nedges) {
    Mna m;
    m.n = n;
    const int ns = std::max(1, n / 7);
    m.nodes = n - ns;
    std::uniform_int_distribution<int> node(0, m.nodes - 1);
    std::set<std::pair<int, int>> seen;
    for (int i = 1; i < m.nodes; ++i) { // spanning tree keeps the graph connected
        const int j = std::uniform_int_distribution<int>(0, i - 1)(rng);
        seen.insert({j, i});
        m.edges.emplace_back(j, i);
    }
    for (int t = 0; t < 20 * nedges && static_cast<int>(m.edges.size()) < nedges; ++t) {
        int a = node(rng), b = node(rng);
        if (a == b)
            continue;
        if (a > b)
            std::swap(a, b);
        if (seen.insert({a, b}).second)
            m.edges.emplace_back(a, b);
    }
    for (int t = 0; t < m.nodes / 10; ++t) {
        const int a = node(rng), b = node(rng);
        if (a != b)
            m.couple.emplace_back(a, b);
    }
    // Each source owns a distinct "plus" node; "minus" is ground or a node that is no
    // plus node, so sources never form a loop.
    std::vector<int> perm(m.nodes);
    for (int i = 0; i < m.nodes; ++i)
        perm[i] = i;
    std::shuffle(perm.begin(), perm.end(), rng);
    for (int s = 0; s < ns; ++s) {
        const int a = perm[s];
        int b = -1;
        if (rng() % 2 && m.nodes > ns)
            b = perm[ns + static_cast<int>(rng() % (m.nodes - ns))];
        m.sources.emplace_back(a, b);
        m.zero_diag.push_back(static_cast<char>(rng() % 2));
    }
    return m;
}

/** @brief Stamp fresh random values for a fixed structure into a dense row-major matrix. */
void stamp(const Mna &m, std::mt19937_64 &rng, std::vector<double> &a,
           std::vector<std::uint8_t> &used) {
    const int n = m.n;
    a.assign(static_cast<std::size_t>(n) * n, 0.0);
    used.assign(a.size(), 0);
    auto add = [&](int r, int c, double v) {
        a[static_cast<std::size_t>(r) * n + c] += v;
        used[static_cast<std::size_t>(r) * n + c] = 1;
    };
    std::uniform_real_distribution<double> e(std::log(1e-2), std::log(1e1));
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (int i = 0; i < m.nodes; ++i) // ground leak
        add(i, i, std::exp(std::uniform_real_distribution<double>(std::log(1e-1), 0.0)(rng)));
    for (const auto &[i, j] : m.edges) {
        const double g = std::exp(e(rng));
        add(i, i, g);
        add(j, j, g);
        add(i, j, -g);
        add(j, i, -g);
    }
    for (const auto &[i, j] : m.couple) // controlled source, keeps diagonal dominance
        add(i, j, 0.05 * u(rng) * a[static_cast<std::size_t>(i) * n + i]);
    for (std::size_t s = 0; s < m.sources.size(); ++s) {
        const int k = m.nodes + static_cast<int>(s);
        const auto [p, q] = m.sources[s];
        add(p, k, 1.0);
        add(k, p, 1.0);
        if (q >= 0) {
            add(q, k, -1.0);
            add(k, q, -1.0);
        }
        if (m.zero_diag[s])
            add(k, k, 0.0); // structurally present, numerically zero
    }
}

/** @brief Gather dense values into pattern order (first nnz slots of the plan). */
void gather(const Pattern &p, const std::vector<double> &a, std::vector<double> &vals) {
    for (std::size_t i = 0; i < p.row.size(); ++i)
        vals[i] = a[static_cast<std::size_t>(p.row[i]) * p.n + p.col[i]];
}

/** @brief Relative max-norm difference between two vectors. */
double rel_diff(const std::vector<double> &x, const std::vector<double> &y) {
    double d = 0.0, s = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        d = std::max(d, std::abs(x[i] - y[i]));
        s = std::max(s, std::abs(y[i]));
    }
    return d / std::max(s, 1e-300);
}

/** @brief Random right-hand side. */
std::vector<double> random_rhs(std::mt19937_64 &rng, int n) {
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::vector<double> b(n);
    for (auto &x : b)
        x = u(rng);
    return b;
}

/** @brief Factor + solve with a plan; returns empty vector when factor() refuses. */
std::vector<double> plan_solve(const SparsePlan &plan, const Pattern &p,
                               const std::vector<double> &a, const std::vector<double> &b) {
    std::vector<double> vals(plan.slots());
    gather(p, a, vals);
    if (!plan.factor(vals.data()))
        return {};
    std::vector<double> x = b;
    plan.solve(vals.data(), x.data());
    return x;
}

/** @brief (a) 200 random MNA matrices, n = 5..300, against dense LinearSolver. */
int test_random() {
    std::mt19937_64 rng(20261125);
    double worst = 0.0;
    for (int t = 0; t < 200; ++t) {
        const int n = 5 + static_cast<int>(rng() % 296);
        const Mna m = make_structure(rng, n, 2 * n);
        std::vector<double> a;
        std::vector<std::uint8_t> used;
        stamp(m, rng, a, used);
        const Pattern p = mspice::sim::pattern_from_stamped(used, n);
        std::vector<double> v0(p.row.size());
        gather(p, a, v0);
        const SparsePlan plan = SparsePlan::build(p, v0.data());
        const auto b = random_rhs(rng, n);
        const auto x = plan_solve(plan, p, a, b);
        if (x.empty()) {
            std::cerr << "(a) factor refused case " << t << " n=" << n << '\n';
            return 1;
        }
        LinearSolver dense;
        dense.factor(a, n, used, false);
        const auto xd = dense.solve(b);
        const double err = rel_diff(x, xd);
        worst = std::max(worst, err);
        if (!(err < 1e-10)) {
            std::cerr << "(a) case " << t << " n=" << n << " rel err " << err << '\n';
            return 1;
        }
        for (int i = 0; i < static_cast<int>(p.row.size());
             ++i) // original entries keep their slot index
            if (plan.slot(p.row[i], p.col[i]) != i)
                return 2;
        if (plan.slot(-1, 0) != -1 || plan.slot(0, n) != -1)
            return 3;
        const SparsePlan again = SparsePlan::build(p, v0.data());
        if (again.hash() != plan.hash()) // deterministic ordering (fixed tie-break)
            return 4;
    }
    std::printf("(a) 200 random MNA matrices ok, worst rel err %.3g\n", worst);
    return 0;
}

/** @brief (b) build once, then factor 100 new value sets with the same pattern. */
int test_refactor() {
    std::mt19937_64 rng(7);
    const int n = 150;
    const Mna m = make_structure(rng, n, 2 * n);
    std::vector<double> a;
    std::vector<std::uint8_t> used;
    stamp(m, rng, a, used);
    const Pattern p = mspice::sim::pattern_from_stamped(used, n);
    std::vector<double> v0(p.row.size());
    gather(p, a, v0);
    const SparsePlan plan = SparsePlan::build(p, v0.data());
    double worst = 0.0;
    for (int t = 0; t < 100; ++t) {
        std::vector<std::uint8_t> used2;
        stamp(m, rng, a, used2);
        if (used2 != used)
            return 10;
        const auto b = random_rhs(rng, n);
        const auto x = plan_solve(plan, p, a, b);
        if (x.empty()) {
            std::cerr << "(b) factor refused at " << t << '\n';
            return 11;
        }
        LinearSolver dense;
        dense.factor(a, n, used, false);
        const double err = rel_diff(x, dense.solve(b));
        worst = std::max(worst, err);
        if (!(err < 1e-10)) {
            std::cerr << "(b) refactor " << t << " rel err " << err << '\n';
            return 12;
        }
    }
    std::printf("(b) 100 refactorizations n=%d nnz=%d slots=%d ok, worst rel err %.3g\n", n,
                plan.nnz(), plan.slots(), worst);
    return 0;
}

/** @brief (c) a pivot that vanishes after build must make factor() return false. */
int test_vanishing_pivot() {
    // Full 2x2 pattern; the second matrix is exactly singular, so the last pivot is 0.
    const Pattern p{2, {0, 0, 1, 1}, {0, 1, 0, 1}};
    const double good[] = {1.0, 2.0, 2.0, 5.0};
    const SparsePlan plan = SparsePlan::build(p, good);
    std::vector<double> vals(plan.slots());
    std::copy(good, good + 4, vals.begin());
    if (!plan.factor(vals.data()))
        return 20;
    const double singular[] = {1.0, 2.0, 2.0, 4.0};
    std::copy(singular, singular + 4, vals.begin());
    if (plan.factor(vals.data()))
        return 21;
    // Growth guard: the second pivot shrinks from 1 to 1e-8 (row scale 4); the row
    // scale test alone (1e-13) would still accept it.
    const double shrunk[] = {1.0, 2.0, 2.0, 4.0 + 1e-8};
    std::copy(shrunk, shrunk + 4, vals.begin());
    if (plan.factor(vals.data()))
        return 26;
    // MNA case: a valid factorization, then all values zero (every pivot vanishes).
    std::mt19937_64 rng(3);
    const Mna m = make_structure(rng, 40, 80);
    std::vector<double> a;
    std::vector<std::uint8_t> used;
    stamp(m, rng, a, used);
    const Pattern q = mspice::sim::pattern_from_stamped(used, 40);
    std::vector<double> v(q.row.size());
    gather(q, a, v);
    const SparsePlan plan2 = SparsePlan::build(q, v.data());
    std::vector<double> w(plan2.slots());
    gather(q, a, w);
    if (!plan2.factor(w.data()))
        return 22;
    gather(q, a, w);
    // Zero every original entry: all pivots vanish, factor() must refuse.
    std::fill(w.begin(), w.end(), 0.0);
    if (plan2.factor(w.data()))
        return 23;
    // Non-finite input is refused as well.
    gather(q, a, w);
    w[0] = std::nan("");
    if (plan2.factor(w.data()))
        return 24;
    // Building on a singular matrix throws.
    try {
        (void)SparsePlan::build(p, singular);
        return 25;
    } catch (const std::runtime_error &) {
    }
    std::printf("(c) vanishing pivot rejected ok\n");
    return 0;
}

/** @brief (d) timing n = 120, nnz ~ 600: plan factor+solve against dense LinearSolver. */
int test_timing() {
    std::mt19937_64 rng(42);
    const int n = 120;
    Mna m = make_structure(rng, n, 210);
    std::vector<double> a;
    std::vector<std::uint8_t> used;
    stamp(m, rng, a, used);
    const Pattern p = mspice::sim::pattern_from_stamped(used, n);
    std::vector<double> v0(p.row.size());
    gather(p, a, v0);
    const auto t_build = std::chrono::steady_clock::now();
    const SparsePlan plan = SparsePlan::build(p, v0.data());
    const double build_us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t_build)
            .count();
    const auto b = random_rhs(rng, n);
    using clk = std::chrono::steady_clock;
    const int reps_sparse = 20000, reps_dense = 500;
    std::vector<double> vals(plan.slots()), x(n);
    double sink = 0.0;
    auto t0 = clk::now();
    for (int r = 0; r < reps_sparse; ++r) {
        std::copy(v0.begin(), v0.end(), vals.begin());
        if (!plan.factor(vals.data()))
            return 30;
        std::copy(b.begin(), b.end(), x.begin());
        plan.solve(vals.data(), x.data());
        sink += x[r % n];
    }
    const double sparse_us =
        std::chrono::duration<double, std::micro>(clk::now() - t0).count() / reps_sparse;
    LinearSolver dense;
    std::vector<double> xd;
    t0 = clk::now();
    for (int r = 0; r < reps_dense; ++r) {
        a[0] += 1e-15; // defeat any cache on identical matrices
        dense.factor(a, n, used, false);
        xd = dense.solve(b);
        sink += xd[r % n];
    }
    const double dense_us =
        std::chrono::duration<double, std::micro>(clk::now() - t0).count() / reps_dense;
    std::printf("(d) n=%d nnz=%d slots=%d updates=%d build=%.1f us\n", n, plan.nnz(), plan.slots(),
                plan.update_count(), build_us);
    std::printf("(d) factor+solve: plan %.2f us, dense %.2f us, ratio %.1fx (sink %.3g)\n",
                sparse_us, dense_us, dense_us / sparse_us, sink);
    return 0;
}
} // namespace

/** @brief Run all SparsePlan checks (a)-(d); nonzero exit code identifies the failure. */
int main() {
    if (int rc = test_random())
        return rc;
    if (int rc = test_refactor())
        return rc;
    if (int rc = test_vanishing_pivot())
        return rc;
    return test_timing();
}
