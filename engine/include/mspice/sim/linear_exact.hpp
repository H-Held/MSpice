// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#pragma once
#include <vector>
namespace mspice::sim {
/** @brief Linear MNA system `C x' + G x = B u(t)` with dense row-major matrices.
 *  G and C are n x n, B is n x p (p = B.size() / n). C may be singular (algebraic rows). */
struct LinearDae {
    int n{};
    std::vector<double> G, C, B;
};
/** @brief Near-exact fixed-step propagator for a LinearDae with piecewise-linear input.
 *
 *  Builds the affine trapezoid map `x1 = M x0 + P u0 + Q u1` for the sub-step h / 2^m and
 *  composes it m times by squaring, so one call of step() advances by h with the error of a
 *  trapezoid run at h / 2^m. The input is assumed linear between u0 (start) and u1 (end).
 *  The state passed to step() must be consistent with the algebraic rows at u0; consistency
 *  is then preserved (the trapezoid enforces the averaged constraint). */
class ExactStepper {
  public:
    /** @brief Precompute the step map for step size h using `doublings` squarings.
     *  @throws std::invalid_argument on bad dimensions, h <= 0 or doublings outside [0, 60]. */
    ExactStepper(const LinearDae &dae, double h, int doublings = 16);
    /** @brief Advance x (size n) by h; u0/u1 (size p) are the inputs at start and end. */
    void step(double *x, const double *u0, const double *u1) const;
    /** @brief Number of unknowns n. */
    int size() const { return n_; }
    /** @brief Number of inputs p. */
    int inputs() const { return p_; }
    /** @brief Step size h covered by one step() call. */
    double step_size() const { return h_; }
    /** @brief State transition matrix M (n x n, row-major). */
    const std::vector<double> &transition() const { return M_; }

  private:
    int n_{}, p_{};
    double h_{};
    std::vector<double> M_, P_, Q_;
};
} // namespace mspice::sim
