// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file behaviour_grad_test.cpp
 * @brief Checks the exact (dual-number) gradients of generic behavioural model blocks.
 *
 * Every block is built once; each behavioural source must carry a gradient. At 20 random
 * operating points (std::mt19937 seed 1, controls uniform in [-1, 5]) the gradient is
 * compared with a Richardson-extrapolated central difference (steps h and h/2,
 * h = 1e-4*max(1,|x|)) of the same generic expression evaluated in long double. A check passes
 * if |g - fd| <= 1e-6*max(|g|,|fd|) + 1e-10*|f|/h (relative class; the second term is the
 * rounding level of the difference quotient), or if |fd| <= 1e-9 and |g| < 1e-12 (off region
 * of smax0 and similar). Both classes are counted per source, together with the largest
 * relative error of checks with |fd| <= 1e-9. Points where the difference quotient itself
 * is not converged (branch switch of min/max/abs/clamp or a steep tanh inside the stencil)
 * or where f is not finite are redrawn. The returned value must equal the plain function.
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <mspice/sim/circuit.hpp>
using mspice::sim::Circuit;

namespace {

// Block expressions are generic: they run on double and long double for values and on
// Dual<N> for exact gradients. Math functions are called unqualified; these declarations
// supply the floating-point versions, argument-dependent lookup the Dual versions.
using std::exp;
using std::log1p;
using std::pow;
using std::sqrt;
using std::tanh;

/** @brief Smooth max(x, 0): 0.5*(x + sqrt(x^2 + w^2)). */
template <class T> T smax0(const T &x, double w) { return 0.5 * (x + sqrt(x * x + w * w)); }
/** @brief Logistic step 0.5*(1+tanh(x/w)). */
template <class T> T step(const T &x, double w) { return 0.5 * (1 + tanh(x / w)); }

/** @brief Shunt reference: internal reference voltage plus a smooth cathode current. */
void shunt_reference(Circuit &c) {
    c.behavioural_voltage("vi", "a", {"k", "a"}, [](const auto &v) {
        return 1.24 + 1e-3 * (v[0] - v[1] - 1.24);
    });
    c.behavioural_current("k", "a", {"ref", "a", "vi"}, [](const auto &v) {
        return smax0(1e-4 + 0.5 * (v[0] - v[1] - (v[2] - v[1])), 2e-3) + 1e-6;
    });
}
/** @brief Low-dropout pass element with enable input. */
void ldo_pass(Circuit &c) {
    c.behavioural_current("in", "out", {"in", "out", "en"}, [](const auto &v) {
        const auto d = (v[0] - v[1]) / 0.5;
        return 0.5 * (d + sqrt(d * d + 1e-12)) * step(v[2] - 0.8, 0.05);
    });
}
/** @brief Open-drain comparator output stage. */
void comparator(Circuit &c) {
    c.behavioural_current("out", "g", {"inp", "inn", "out", "g"}, [](const auto &v) {
        return 1e-3 * step(v[0] - v[1], 0.01) * (v[2] - v[3]);
    });
}
/** @brief Push-pull logic output with supply-dependent output resistance. */
void logic_output(Circuit &c) {
    c.behavioural_current("vcc", "y", {"a", "vcc", "y", "g"}, [](const auto &v) {
        const auto vc = smax0(v[1] - v[3], 0.05) + 0.3;
        const auto target = step(v[0] - 0.5 * (v[1] - v[3]), 0.05) * (v[1] - v[3]);
        return (target - (v[2] - v[3])) / (20.0 * pow(vc, -0.5) + 5.0);
    });
}
/** @brief Transconductance amplifier with soft saturation and soft start. */
void ota(Circuit &c) {
    c.behavioural_current("comp", "0", {"ss", "fb"}, [](const auto &v) {
        return 2e-4 * tanh((v[0] - v[1]) / 0.5) * step(v[0], 0.1);
    });
}
/** @brief Diode-like softplus current and an exponential junction law. */
void junction_laws(Circuit &c) {
    c.behavioural_current("p", "n", {"p", "n"}, [](const auto &v) {
        return 1e-3 * log1p(exp((v[0] - v[1] - 0.6) / 0.05));
    });
    c.behavioural_current("p2", "n", {"p2", "n"}, [](const auto &v) {
        return 1e-12 * (exp((v[0] - v[1]) / 0.5) - 1);
    });
}

/** @brief Central difference of the long-double reference along component k with step h. */
long double central(const Circuit::BehaviouralSource &s, const std::vector<double> &x,
                    std::size_t k, long double h) {
    std::vector<long double> xl(x.begin(), x.end());
    const long double keep = xl[k];
    xl[k] = keep + h;
    const long double up = s.reference(xl.data());
    xl[k] = keep - h;
    const long double down = s.reference(xl.data());
    return (up - down) / (2 * h);
}

/** @brief Richardson extrapolation of the central difference with steps h and h/2. */
double richardson(const Circuit::BehaviouralSource &s, const std::vector<double> &x, std::size_t k,
                  long double h) {
    return static_cast<double>((4 * central(s, x, k, h / 2) - central(s, x, k, h)) / 3);
}

/** @brief Check every source of one model; returns the number of failures. */
int check_model(const std::string &name, const std::function<void(Circuit &)> &build) {
    Circuit c;
    build(c);
    const auto sources = c.behavioural_sources();
    std::mt19937 rng(1);
    std::uniform_real_distribution<double> dist(-1.0, 5.0);
    int failures = 0, checked = 0;
    constexpr double measurable = 1e-9; ///< |fd| above which the relative check applies.
    constexpr double off_limit = 1e-12; ///< |g| bound where the reference is not measurable.
    for (std::size_t si = 0; si < sources.size(); ++si) {
        const auto &s = sources[si];
        if (!s.gradient || !s.reference) {
            std::printf("FAIL %s source %zu: no exact gradient or reference\n", name.c_str(), si);
            ++failures;
            continue;
        }
        int accepted = 0, draws = 0, n_rel = 0, n_off = 0;
        double small_rel = 0; ///< Largest relative error of checks with |fd| <= 1e-9.
        std::vector<double> x(s.controls), grad(s.controls);
        while (accepted < 20 && draws < 5000) {
            ++draws;
            for (double &xi : x)
                xi = dist(rng);
            const double f = s.function(x);
            if (!std::isfinite(f))
                continue;
            const double fg = s.gradient(x.data(), grad.data());
            if (std::abs(fg - f) > 1e-12 * std::max(1.0, std::abs(f))) {
                std::printf("FAIL %s source %zu: value %.17g vs %.17g\n", name.c_str(), si, fg, f);
                ++failures;
                break;
            }
            bool resolvable = true;
            std::vector<double> fd(s.controls);
            std::vector<double> tol(s.controls);
            for (std::size_t k = 0; k < s.controls && resolvable; ++k) {
                const double h = 1e-4 * std::max(1.0, std::abs(x[k]));
                const double d1 = richardson(s, x, k, h);
                const double d2 = richardson(s, x, k, h / 4);
                tol[k] = 1e-6 * std::max(std::abs(grad[k]), std::abs(d2)) + 1e-10 * std::abs(f) / h;
                const double conv_tol =
                    1e-6 * std::max(std::abs(d1), std::abs(d2)) + 1e-10 * std::abs(f) / h;
                resolvable =
                    std::isfinite(d1) && std::isfinite(d2) && std::abs(d1 - d2) <= conv_tol;
                fd[k] = d2;
            }
            if (!resolvable)
                continue;
            ++accepted;
            for (std::size_t k = 0; k < s.controls; ++k) {
                const double err = std::abs(grad[k] - fd[k]);
                const bool rel_ok = err <= tol[k];
                const bool off_ok = std::abs(fd[k]) <= measurable && std::abs(grad[k]) < off_limit;
                if (std::abs(fd[k]) <= measurable && std::abs(grad[k]) >= off_limit)
                    small_rel =
                        std::max(small_rel, err / std::max(std::abs(grad[k]), std::abs(fd[k])));
                if (rel_ok)
                    ++n_rel;
                else if (off_ok)
                    ++n_off;
                else {
                    std::printf("FAIL %s source %zu control %zu: dual %.12g fd %.12g\n",
                                name.c_str(), si, k, grad[k], fd[k]);
                    ++failures;
                }
            }
        }
        if (accepted < 20) {
            std::printf("FAIL %s source %zu: only %d resolvable points in %d draws\n", name.c_str(),
                        si, accepted, draws);
            ++failures;
        }
        std::printf("  %s source %zu: %d relative, %d off-region checks; max rel. error at "
                    "|fd| <= 1e-9: %.3g\n",
                    name.c_str(), si, n_rel, n_off, small_rel);
        ++checked;
    }
    std::printf("%s: %d sources checked, %d failures\n", name.c_str(), checked, failures);
    return failures;
}

} // namespace

/** @brief Runs the gradient check for every block; exit code 1 on any failure. */
int main() {
    int failures = 0;
    failures += check_model("shunt_reference", shunt_reference);
    failures += check_model("ldo_pass", ldo_pass);
    failures += check_model("comparator", comparator);
    failures += check_model("logic_output", logic_output);
    failures += check_model("ota", ota);
    failures += check_model("junction_laws", junction_laws);
    if (failures) {
        std::printf("behaviour_grad: %d failures\n", failures);
        return 1;
    }
    std::printf("behaviour_grad: all block gradients match\n");
    return 0;
}
