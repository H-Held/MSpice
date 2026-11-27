// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#pragma once
// Random MNA-like test matrices (same construction as tests/sparse_plan_test.cpp), shared
// by the batched_plan test and the batch_bench tool.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mspice/sim/sparse_plan.hpp>
#include <random>
#include <set>
#include <utility>
#include <vector>

namespace mspice::testing {
/** @brief Random MNA-like structure: node conductances plus ideal voltage sources. */
struct Mna {
    int n{}, nodes{};
    std::vector<std::pair<int, int>> edges;   ///< Conductances between two nodes.
    std::vector<std::pair<int, int>> couple;  ///< Small unsymmetric (controlled) entries.
    std::vector<std::pair<int, int>> sources; ///< (node, node or -1 for ground).
    std::vector<char> zero_diag;              ///< Source row stores a structural 0 diagonal.
};

/** @brief Draw a connected random node graph with nedges edges and n/7 voltage sources.
 *  @param rng random engine.
 *  @param n number of unknowns (nodes plus source currents).
 *  @param nedges target number of conductance edges.
 *  @return structure; values come from stamp(). */
inline Mna make_structure(std::mt19937_64 &rng, int n, int nedges) {
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

/** @brief Stamp random values for a fixed structure into a dense row-major matrix.
 *  @param m structure.
 *  @param rng random engine (conductances log-uniform in [1e-2, 1e1]).
 *  @param a output n*n matrix.
 *  @param used output n*n structural mask (independent of the values drawn). */
inline void stamp(const Mna &m, std::mt19937_64 &rng, std::vector<double> &a,
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
    for (int i = 0; i < m.nodes; ++i) // ground leak (capacitor companion + diode gmin)
        add(i, i, std::exp(std::uniform_real_distribution<double>(std::log(1e-1), 0.0)(rng)));
    for (const auto &[i, j] : m.edges) {
        const double g = std::exp(e(rng));
        add(i, i, g);
        add(j, j, g);
        add(i, j, -g);
        add(j, i, -g);
    }
    for (const auto &[i, j] : m.couple)
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
            add(k, k, 0.0);
    }
}

/** @brief Gather dense values into pattern order (first nnz slots of a plan).
 *  @param p pattern.
 *  @param a dense row-major matrix.
 *  @param vals output, at least p.row.size() entries. */
inline void gather(const mspice::sim::Pattern &p, const std::vector<double> &a, double *vals) {
    for (std::size_t i = 0; i < p.row.size(); ++i)
        vals[i] = a[static_cast<std::size_t>(p.row[i]) * p.n + p.col[i]];
}
} // namespace mspice::testing
