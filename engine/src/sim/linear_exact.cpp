// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <cmath>
#include <cstdint>
#include <mspice/sim/linear_exact.hpp>
#include <mspice/sim/linear_solver.hpp>
#include <stdexcept>
namespace mspice::sim {
namespace {
/** @brief Dense product of row-major A (r x k) and B (k x c). */
std::vector<double> matmul(const std::vector<double> &a, const std::vector<double> &b, int r, int k,
                           int c) {
    std::vector<double> out(static_cast<std::size_t>(r) * c, 0.0);
    for (int i = 0; i < r; ++i)
        for (int l = 0; l < k; ++l) {
            const double ail = a[static_cast<std::size_t>(i) * k + l];
            if (ail == 0.0)
                continue;
            for (int j = 0; j < c; ++j)
                out[static_cast<std::size_t>(i) * c + j] +=
                    ail * b[static_cast<std::size_t>(l) * c + j];
        }
    return out;
}
/** @brief Solve A X = R column by column for row-major R (n x c) with a factored A. */
std::vector<double> solve_columns(LinearSolver &lu, const std::vector<double> &rhs, int n, int c) {
    std::vector<double> out(static_cast<std::size_t>(n) * c);
    std::vector<double> col(n);
    for (int j = 0; j < c; ++j) {
        for (int i = 0; i < n; ++i)
            col[i] = rhs[static_cast<std::size_t>(i) * c + j];
        auto x = lu.solve(col);
        for (int i = 0; i < n; ++i)
            out[static_cast<std::size_t>(i) * c + j] = x[i];
    }
    return out;
}
} // namespace

/** @brief Build the sub-step trapezoid map and square it `doublings` times. */
ExactStepper::ExactStepper(const LinearDae &dae, double h, int doublings) : n_(dae.n), h_(h) {
    const auto nn = static_cast<std::size_t>(n_) * n_;
    if (n_ <= 0 || dae.G.size() != nn || dae.C.size() != nn || dae.B.size() % n_ != 0)
        throw std::invalid_argument("LinearDae: inconsistent dimensions");
    if (!(h > 0) || !std::isfinite(h))
        throw std::invalid_argument("ExactStepper: h must be positive");
    if (doublings < 0 || doublings > 60)
        throw std::invalid_argument("ExactStepper: doublings out of range");
    p_ = static_cast<int>(dae.B.size() / n_);
    const double s = std::ldexp(h, -doublings);
    // Sub-step trapezoid: (C/s + G/2) x1 = (C/s - G/2) x0 + B (u0 + u1) / 2.
    std::vector<double> a(nn), rhs(nn);
    for (std::size_t i = 0; i < nn; ++i) {
        a[i] = dae.C[i] / s + 0.5 * dae.G[i];
        rhs[i] = dae.C[i] / s - 0.5 * dae.G[i];
    }
    LinearSolver lu;
    lu.factor(a, n_, std::vector<std::uint8_t>(nn, 1), false);
    M_ = solve_columns(lu, rhs, n_, n_);
    std::vector<double> halfb(dae.B);
    for (auto &v : halfb)
        v *= 0.5;
    P_ = solve_columns(lu, halfb, n_, p_);
    Q_ = P_;
    // Two equal halves with linear input (u_mid = (u0 + u1) / 2):
    // M' = M^2, P' = M P + R, Q' = R + Q with R = (M Q + P) / 2.
    for (int k = 0; k < doublings; ++k) {
        auto mq = matmul(M_, Q_, n_, n_, p_);
        auto mp = matmul(M_, P_, n_, n_, p_);
        std::vector<double> r(mq.size());
        for (std::size_t i = 0; i < r.size(); ++i)
            r[i] = 0.5 * (mq[i] + P_[i]);
        for (std::size_t i = 0; i < r.size(); ++i) {
            P_[i] = mp[i] + r[i];
            Q_[i] = r[i] + Q_[i];
        }
        M_ = matmul(M_, M_, n_, n_, n_);
    }
}

/** @brief Apply x <- M x + P u0 + Q u1. */
void ExactStepper::step(double *x, const double *u0, const double *u1) const {
    std::vector<double> out(n_, 0.0);
    for (int i = 0; i < n_; ++i) {
        double acc = 0.0;
        const std::size_t row = static_cast<std::size_t>(i);
        for (int j = 0; j < n_; ++j)
            acc += M_[row * n_ + j] * x[j];
        for (int j = 0; j < p_; ++j)
            acc += P_[row * p_ + j] * u0[j] + Q_[row * p_ + j] * u1[j];
        out[i] = acc;
    }
    for (int i = 0; i < n_; ++i)
        x[i] = out[i];
}
} // namespace mspice::sim
