// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mspice/sim/circuit.hpp>
#include <mspice/sim/linear_solver.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mspice::sim {
namespace {
/** @brief Conservative variable-step BDF2 growth bound, below 1+sqrt(2). */
constexpr double bdf2_max_ratio = 2.0;
bool is_ground(const std::string &name) {
    if (name == "0")
        return true;
    std::string upper = name;
    for (char &ch : upper)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return upper == "GND";
}
constexpr double huge = std::numeric_limits<double>::infinity();
/** @brief Stable approximate N-channel drain-to-source current. */
double mos_id(double vd, double vg, double vs, double k, double threshold, double lambda) {
    double vgs = vg - vs, vds = vd - vs;
    if (vds < 0)
        return -mos_id(vs, vg, vd, k, threshold, lambda);
    if (vgs <= threshold)
        return 0;
    double ov = vgs - threshold;
    double current = (vds < ov) ? k * (ov * vds - 0.5 * vds * vds) : 0.5 * k * ov * ov;
    return current * (1 + lambda * vds);
}
/** @brief P-channel drain-to-source current as the voltage-mirrored N-channel law. */
double pmos_id(double vd, double vg, double vs, double k, double threshold, double lambda) {
    return -mos_id(-vd, -vg, -vs, k, -threshold, lambda);
}
/** @brief Numerically stable ln(1+exp(x)). */
double softplus(double x) {
    return x > 30 ? x + std::log1p(std::exp(-x)) : std::log1p(std::exp(x));
}
/** @brief Numerically stable logistic function 1/(1+exp(-x)). */
double logistic(double x) {
    if (x >= 0)
        return 1 / (1 + std::exp(-x));
    const double e = std::exp(x);
    return e / (1 + e);
}
/** @brief Overflow-safe exp: linear continuation above 100 keeps value and slope,
 *  so Newton still sees a nonzero derivative (a hard clamp made every vbe > 0.9 V a
 *  false fixed point). */
double limited_exp(double a) {
    // Low-IS LEDs operate above exponent 40; keep their physical exponential intact.
    constexpr double top = 100.0;
    if (a > top)
        return std::exp(top) * (1 + a - top);
    return std::exp(std::max(a, -50.0));
}
/** @brief Leak conductance across every nonlinear branch (as SPICE GMIN). */
constexpr double gmin = 1e-12;
/** @brief Largest node-voltage change per Newton iteration in nonlinear circuits. */
constexpr double newton_step_limit = 0.3;
/** @brief Junction charge q(v) and capacitance dq/dv (as SPICE dioload/bjtload).
 *
 *  Depletion charge with the SPICE forward linearisation above fc*vj (continuous in q and C),
 *  plus the diffusion charge tt * I(v) with the junction current law of the diode stamp
 *  (is*(exp(v/vt)-1), reverse breakdown below -knee). Works on any type with the members of
 *  Circuit::Junction.
 *  @param j   junction parameters
 *  @param v   junction voltage anode - cathode in volts
 *  @param cap receives dq/dv in farads
 *  @return charge in coulomb (q(0) = 0)
 */
template <class J> double junction_charge(const J &j, double v, double &cap) {
    double q = 0;
    cap = 0;
    if (j.cj0 > 0) {
        const double fcv = j.fc * j.vj;
        if (v < fcv) {
            const double arg = 1 - v / j.vj;
            const double sarg = std::exp(-j.m * std::log(arg));
            q = j.vj * j.cj0 * (1 - arg * sarg) / (1 - j.m);
            cap = j.cj0 * sarg;
        } else {
            const double f1 = j.vj * (1 - std::pow(1 - j.fc, 1 - j.m)) / (1 - j.m);
            const double f2 = std::pow(1 - j.fc, 1 + j.m);
            const double f3 = 1 - j.fc * (1 + j.m);
            const double czof2 = j.cj0 / f2;
            q = j.cj0 * f1 + czof2 * (f3 * (v - fcv) + j.m / (2 * j.vj) * (v * v - fcv * fcv));
            cap = czof2 * (f3 + j.m * v / j.vj);
        }
    }
    if (j.tt > 0) {
        double current = j.is * (limited_exp(v / j.vt) - 1);
        double g = j.is * std::exp(std::clamp(v / j.vt, -50.0, 100.0)) / j.vt;
        if (j.knee > 0 && v < -j.knee) {
            const double ar = -(v + j.knee) / j.slope;
            current = -j.is * limited_exp(ar);
            g = j.is * std::exp(std::clamp(ar, -50.0, 100.0)) / j.slope;
        }
        q += j.tt * current;
        cap += j.tt * g;
    }
    return q;
}
/** @brief Validate the depletion/transit parameters of one junction charge. */
bool valid_charge(double cj0, double vj, double m, double fc, double tt) {
    return cj0 >= 0 && std::isfinite(cj0) && vj > 0 && std::isfinite(vj) && m >= 0 && m < 1 &&
           fc >= 0 && fc < 1 && tt >= 0 && std::isfinite(tt);
}
} // namespace

double LedCurve::amps(double u) const {
    const double x = (u - u0 + tc_v_per_k * (temp_c - 20)) / s;
    return k * std::pow(s * softplus(x), p) * 1e-3;
}
double LedCurve::siemens(double u) const {
    const double x = (u - u0 + tc_v_per_k * (temp_c - 20)) / s;
    const double base = std::max(s * softplus(x), 1e-300);
    return k * p * std::pow(base, p - 1) * logistic(x) * 1e-3;
}

double Waveform::at(double t) const {
    if (!trace.empty()) {
        auto it =
            std::upper_bound(trace.begin(), trace.end(), t,
                             [](double time, const WavePoint &point) { return time < point.time; });
        if (it == trace.begin())
            return trace.front().volts;
        if (it == trace.end())
            return trace.back().volts;
        const WavePoint &right = *it;
        const WavePoint &left = *(it - 1);
        if (right.time == left.time)
            return right.volts;
        return left.volts + (right.volts - left.volts) * (t - left.time) / (right.time - left.time);
    }
    if (sine) {
        constexpr double two_pi = 6.283185307179586476925286766559;
        if (t <= delay)
            return low + high * std::sin(two_pi * phase / 360.0);
        const double dt = t - delay;
        return low +
               high * std::sin(two_pi * (freq * dt + phase / 360.0)) * std::exp(-dt * damping);
    }
    if (!pulse)
        return high;
    if (t < delay)
        return low;
    if (cycles > 0 && t - delay >= cycles * period)
        return low;
    double tau = std::fmod(t - delay, period);
    if (tau < rise && rise > 0)
        return low + (high - low) * tau / rise;
    if (tau < rise + on)
        return high;
    if (tau < rise + on + fall && fall > 0)
        return high + (low - high) * (tau - rise - on) / fall;
    return low;
}
double Waveform::next_event(double t) const {
    if (!trace.empty()) {
        const double after = t + std::max(1e-15, std::abs(t) * 1e-12);
        auto it =
            std::upper_bound(trace.begin(), trace.end(), after,
                             [](double time, const WavePoint &point) { return time < point.time; });
        return it == trace.end() ? huge : it->time;
    }
    if (sine)
        return t < delay - 1e-14 ? delay : huge;
    if (!pulse)
        return huge;
    if (t < delay - 1e-14)
        return delay;
    const double end = delay + cycles * period; // only meaningful with a pulse count
    if (cycles > 0 && t >= end - 1e-13 * std::max(1.0, std::abs(t)))
        return huge;
    double cycle = std::floor((t - delay) / period);
    double start = delay + cycle * period;
    for (double boundary :
         {start, start + rise, start + rise + on, start + rise + on + fall, start + period}) {
        if (boundary > t + 1e-13 * std::max(1.0, std::abs(t)))
            return boundary;
    }
    return cycles > 0 ? huge : start + 2 * period;
}
int Circuit::node(const std::string &name) {
    if (is_ground(name))
        return 0;
    auto f = mapping_.find(name);
    if (f != mapping_.end())
        return f->second;
    int index = static_cast<int>(names_.size());
    names_.push_back(name);
    mapping_[name] = index;
    return index;
}
void Circuit::add(Part part) {
    part.instance = current_instance_;
    parts_.push_back(std::move(part));
}
void Circuit::resistor(const std::string &a, const std::string &b, double v) {
    if (!(v > 0 && std::isfinite(v)))
        throw std::invalid_argument("resistor must be positive finite");
    add({Type::R, node(a), node(b), 0, v});
}
void Circuit::capacitor(const std::string &a, const std::string &b, double v) {
    if (!(v > 0 && std::isfinite(v)))
        throw std::invalid_argument("capacitor must be positive finite");
    add({Type::C, node(a), node(b), 0, v});
}
void Circuit::inductor(const std::string &a, const std::string &b, double v,
                       const std::string &probe) {
    if (!(v > 0 && std::isfinite(v)))
        throw std::invalid_argument("inductor must be positive finite");
    Part p{Type::L, node(a), node(b), 0, v};
    p.probe = probe;
    add(p);
}
namespace {
/** @brief Validate a source waveform (shared by voltage and current sources). */
void check_waveform(const Waveform &w) {
    if (!w.trace.empty()) {
        double last = -1;
        for (const auto &point : w.trace) {
            if (!std::isfinite(point.time) || !std::isfinite(point.volts) || point.time < 0 ||
                point.time < last)
                throw std::invalid_argument(
                    "trace points must be finite with non-decreasing nonnegative times");
            last = point.time;
        }
    }
    if (w.pulse && !w.trace.empty())
        throw std::invalid_argument("pulse and trace are mutually exclusive");
    if (w.pulse && (!(w.period > 0) || w.rise < 0 || w.fall < 0 || w.on < 0 ||
                    w.rise + w.on + w.fall > w.period))
        throw std::invalid_argument("invalid pulse timing");
    if (w.cycles < 0 || (w.cycles > 0 && !w.pulse))
        throw std::invalid_argument("pulse count needs a pulse and must not be negative");
    if (w.sine && (w.pulse || !w.trace.empty()))
        throw std::invalid_argument("sine excludes pulse and trace");
    if (w.sine && !(std::isfinite(w.low) && std::isfinite(w.high) && std::isfinite(w.delay) &&
                    w.delay >= 0 && std::isfinite(w.freq) && w.freq >= 0 &&
                    std::isfinite(w.damping) && std::isfinite(w.phase)))
        throw std::invalid_argument("invalid sine parameters");
}
} // namespace
void Circuit::voltage(const std::string &a, const std::string &b, Waveform w,
                      const std::string &probe) {
    check_waveform(w);
    Part p{Type::V, node(a), node(b)};
    p.wave = w;
    p.probe = probe;
    add(p);
}
void Circuit::probe_current(const std::string &control, const std::string &probe) {
    if (probe.empty() || control.rfind("#I", 0) != 0)
        throw std::invalid_argument("probe_current needs a name and an \"#I<n>\" control");
    const int k = std::stoi(control.substr(2));
    if (k < 0 || k >= static_cast<int>(parts_.size()) ||
        (parts_[k].type != Type::V && parts_[k].type != Type::BV && parts_[k].type != Type::L))
        throw std::invalid_argument("probe_current must name a voltage source: " + control);
    parts_[k].probe = probe;
}
void Circuit::initial_voltage(const std::string &net, double volts) {
    if (is_ground(net) || !std::isfinite(volts))
        throw std::invalid_argument("initial voltage needs a non-ground net and a finite value");
    initial_[node(net)] = volts;
}
void Circuit::trace_driver(const std::string &net, const std::string &driver_node,
                           const std::vector<WavePoint> &trace, double output_ohms) {
    if (trace.empty() || !(output_ohms > 0) || !std::isfinite(output_ohms) || driver_node == net ||
        is_ground(driver_node))
        throw std::invalid_argument("invalid trace driver");
    Waveform source;
    source.trace = trace;
    voltage(driver_node, "0", std::move(source));
    resistor(driver_node, net, output_ohms);
}
void Circuit::current(const std::string &a, const std::string &b, double v) {
    if (!std::isfinite(v))
        throw std::invalid_argument("current must be finite");
    add({Type::I, node(a), node(b), 0, v});
}
void Circuit::current(const std::string &a, const std::string &b, Waveform w) {
    check_waveform(w);
    Part p{Type::I, node(a), node(b)};
    p.wave = std::move(w);
    p.shaped = true;
    add(p);
}
void Circuit::diode(const std::string &a, const std::string &b, double is, double vt) {
    if (!(is > 0 && vt > 0))
        throw std::invalid_argument("invalid diode parameters");
    add({Type::D, node(a), node(b), 0, is, vt});
}
void Circuit::diode(const std::string &a, const std::string &b, const DiodeParams &m) {
    if (!(m.is > 0 && m.vt > 0 && std::isfinite(m.is) && std::isfinite(m.vt)))
        throw std::invalid_argument("invalid diode parameters");
    if (!(m.bv >= 0 && std::isfinite(m.bv) && m.ibv > 0 && std::isfinite(m.ibv) && m.nbv > 0 &&
          std::isfinite(m.nbv)))
        throw std::invalid_argument("invalid diode breakdown parameters");
    if (!valid_charge(m.cjo, m.vj, m.m, m.fc, m.tt))
        throw std::invalid_argument("invalid diode charge parameters (cjo, vj, m, fc, tt)");
    Part p{Type::D, node(a), node(b), 0, m.is, m.vt};
    if (m.bv > 0) {
        // ngspice dioTemp: knee voltage so that the breakdown current at -bv equals ibv.
        constexpr double vth = 0.02585, reltol = 1e-3;
        double knee = m.bv;
        if (!(m.ibv < m.is * m.bv / vth)) {
            const double tol = reltol * m.ibv;
            double xbv = m.bv - m.nbv * vth * std::log(1 + m.ibv / m.is);
            for (int i = 0; i < 25; ++i) {
                xbv = m.bv - m.nbv * vth * std::log(m.ibv / m.is + 1 - xbv / vth);
                const double xcbv = m.is * (std::exp((m.bv - xbv) / (m.nbv * vth)) - 1 + xbv / vth);
                if (std::abs(xcbv - m.ibv) <= tol)
                    break;
            }
            knee = xbv;
        }
        if (!(knee > 0 && std::isfinite(knee)))
            throw std::invalid_argument("diode breakdown knee voltage not positive");
        p.r = knee;        // effective breakdown voltage
        p.s = m.nbv * vth; // breakdown slope voltage
        p.t = m.bv;        // as given, for the export
        p.u = m.ibv;
        p.q = m.nbv;
    }
    add(p);
    if (m.cjo > 0 || m.tt > 0) {
        Junction j;
        j.a = p.a;
        j.b = p.b;
        j.cj0 = m.cjo;
        j.vj = m.vj;
        j.m = m.m;
        j.fc = m.fc;
        j.tt = m.tt;
        j.is = m.is;
        j.vt = m.vt;
        j.knee = p.r;
        j.slope = p.s;
        j.part = parts_.size() - 1;
        junctions_.push_back(j);
    }
}
void Circuit::nmos(const std::string &d, const std::string &g, const std::string &s, double k,
                   double threshold, double lambda) {
    if (!(k > 0 && lambda >= 0))
        throw std::invalid_argument("invalid NMOS parameters");
    add({Type::NMOS, node(d), node(s), node(g), k, threshold, lambda});
}
void Circuit::npn(const std::string &c, const std::string &b, const std::string &e, double is,
                  double beta, double beta_r, double vt) {
    if (!(is > 0 && beta > 0 && beta_r > 0 && std::isfinite(is) && std::isfinite(beta) &&
          std::isfinite(beta_r) && vt > 0 && std::isfinite(vt)))
        throw std::invalid_argument("invalid NPN parameters");
    Part p{Type::NPN, node(c), node(e), node(b), is, beta, beta_r};
    p.s = vt;
    add(p);
}
void Circuit::pnp(const std::string &c, const std::string &b, const std::string &e, double is,
                  double beta, double beta_r, double vt) {
    if (!(is > 0 && beta > 0 && beta_r > 0 && std::isfinite(is) && std::isfinite(beta) &&
          std::isfinite(beta_r) && vt > 0 && std::isfinite(vt)))
        throw std::invalid_argument("invalid PNP parameters");
    Part p{Type::PNP, node(c), node(e), node(b), is, beta, beta_r};
    p.s = vt;
    add(p);
}
void Circuit::add_bjt_charges(const std::string &c, const std::string &b, const std::string &e,
                              const BjtParams &m, bool npn_type) {
    const std::size_t owner = parts_.size() - 1;
    // NPN: junctions b->e and b->c; PNP: e->b and c->b (ngspice multiplies by the type).
    const auto add_junction = [&](const std::string &other, double cj0, double vj, double mj,
                                  double tt, const char *role) {
        if (!(cj0 > 0 || tt > 0))
            return;
        Junction j;
        j.a = npn_type ? node(b) : node(other);
        j.b = npn_type ? node(other) : node(b);
        j.cj0 = cj0;
        j.vj = vj;
        j.m = mj;
        j.fc = m.fc;
        j.tt = tt;
        j.is = m.is;
        j.vt = m.vt;
        j.part = owner;
        j.role = role;
        junctions_.push_back(j);
    };
    add_junction(e, m.cje, m.vje, m.mje, m.tf, "be");
    add_junction(c, m.cjc, m.vjc, m.mjc, m.tr, "bc");
}
void Circuit::npn(const std::string &c, const std::string &b, const std::string &e,
                  const BjtParams &m) {
    if (!valid_charge(m.cje, m.vje, m.mje, m.fc, m.tf) ||
        !valid_charge(m.cjc, m.vjc, m.mjc, m.fc, m.tr))
        throw std::invalid_argument("invalid NPN charge parameters");
    npn(c, b, e, m.is, m.bf, m.br, m.vt);
    add_bjt_charges(c, b, e, m, true);
}
void Circuit::pnp(const std::string &c, const std::string &b, const std::string &e,
                  const BjtParams &m) {
    if (!valid_charge(m.cje, m.vje, m.mje, m.fc, m.tf) ||
        !valid_charge(m.cjc, m.vjc, m.mjc, m.fc, m.tr))
        throw std::invalid_argument("invalid PNP charge parameters");
    pnp(c, b, e, m.is, m.bf, m.br, m.vt);
    add_bjt_charges(c, b, e, m, false);
}
void Circuit::pmos(const std::string &d, const std::string &g, const std::string &s, double k,
                   double threshold, double lambda) {
    if (!(k > 0 && lambda >= 0 && std::isfinite(k) && std::isfinite(threshold) &&
          std::isfinite(lambda)))
        throw std::invalid_argument("invalid PMOS parameters");
    add({Type::PMOS, node(d), node(s), node(g), k, threshold, lambda});
}
void Circuit::voltage_switch(const std::string &a, const std::string &b, const std::string &cp,
                             const std::string &cn, double ron, double roff, double threshold,
                             double hysteresis, bool initially_on) {
    if (!(ron > 0 && roff > 0 && std::isfinite(ron) && std::isfinite(roff) &&
          std::isfinite(threshold) && hysteresis >= 0 && std::isfinite(hysteresis)))
        throw std::invalid_argument("invalid switch parameters");
    Part p{Type::SW, node(a), node(b), node(cp), ron, roff, threshold};
    p.d = node(cn);
    p.r = hysteresis;
    p.initial = initially_on;
    add(p);
}
void Circuit::gate_driver(const std::string &in, const std::string &out, const std::string &vdd,
                          const std::string &gnd, const GateDriver &m) {
    if (!(m.r_high > 0 && m.r_low > 0 && std::isfinite(m.r_high) && std::isfinite(m.r_low) &&
          std::isfinite(m.threshold) && m.hysteresis >= 0 && std::isfinite(m.hysteresis) &&
          m.delay_on >= 0 && m.delay_off >= 0 && std::isfinite(m.delay_on) &&
          std::isfinite(m.delay_off)))
        throw std::invalid_argument("invalid gate driver parameters");
    if (out == vdd || out == gnd || in == out)
        throw std::invalid_argument("gate driver output must be a separate net");
    Part p{Type::DRV, node(in), node(gnd), node(out), m.r_high, m.r_low, m.threshold};
    p.d = node(vdd);
    p.r = m.hysteresis;
    p.s = m.delay_on;
    p.t = m.delay_off;
    add(p);
}
void Circuit::led_strand(const std::string &a, const std::string &c, const LedCurve &curve,
                         const std::string &probe) {
    if (!(curve.k > 0 && curve.s > 0 && curve.p > 0 && std::isfinite(curve.u0) &&
          std::isfinite(curve.temp_c) && std::isfinite(curve.tc_v_per_k)))
        throw std::invalid_argument("invalid LED curve parameters");
    Part p{Type::LED, node(a), node(c)};
    p.led = curve;
    p.probe = probe;
    add(p);
}
int Circuit::control_term(const std::string &control) {
    if (control == time_control)
        return std::numeric_limits<int>::min();
    if (control.rfind("#I", 0) == 0) {
        const int k = std::stoi(control.substr(2));
        if (k < 0 || k >= static_cast<int>(parts_.size()) ||
            (parts_[k].type != Type::V && parts_[k].type != Type::BV))
            throw std::invalid_argument("current control must name a voltage source: " + control);
        return -(k + 1);
    }
    return node(control);
}
std::string Circuit::ammeter(const std::string &a, const std::string &b) {
    Waveform zero;
    voltage(a, b, zero);
    return "#I" + std::to_string(parts_.size() - 1);
}
void Circuit::behavioural_current(const std::string &a, const std::string &b,
                                  const std::vector<std::string> &controls, Behaviour function,
                                  const std::string &probe) {
    behavioural_current(a, b, controls, std::move(function), BehaviourGrad{}, probe);
}
void Circuit::behavioural_current(const std::string &a, const std::string &b,
                                  const std::vector<std::string> &controls, Behaviour function,
                                  BehaviourGrad gradient, const std::string &probe) {
    if (!function || controls.empty())
        throw std::invalid_argument("behavioural current needs a function and control nets");
    Part p{Type::BI, node(a), node(b)};
    for (const auto &c : controls)
        p.terms.push_back(control_term(c));
    p.fn = std::move(function);
    p.fg = std::move(gradient);
    p.probe = probe;
    add(p);
}
void Circuit::behavioural_voltage(const std::string &a, const std::string &b,
                                  const std::vector<std::string> &controls, Behaviour function) {
    behavioural_voltage(a, b, controls, std::move(function), BehaviourGrad{});
}
void Circuit::behavioural_voltage(const std::string &a, const std::string &b,
                                  const std::vector<std::string> &controls, Behaviour function,
                                  BehaviourGrad gradient) {
    if (!function || controls.empty())
        throw std::invalid_argument("behavioural voltage needs a function and control nets");
    if (a == b || (is_ground(a) && is_ground(b)))
        throw std::invalid_argument("behavioural voltage needs two different nets");
    Part p{Type::BV, node(a), node(b)};
    for (const auto &c : controls)
        p.terms.push_back(control_term(c));
    p.fn = std::move(function);
    p.fg = std::move(gradient);
    add(p);
}
std::vector<Circuit::BehaviouralSource> Circuit::behavioural_sources() const {
    std::vector<BehaviouralSource> out;
    for (const Part &p : parts_)
        if (p.type == Type::BI || p.type == Type::BV)
            out.push_back({p.terms.size(), p.fn, p.fg, p.fl});
    return out;
}
void Circuit::breakpoint(double t) {
    if (!std::isfinite(t))
        throw std::invalid_argument("breakpoint time must be finite");
    if (t <= 0)
        return;
    const auto it = std::lower_bound(breakpoints_.begin(), breakpoints_.end(), t);
    if (it == breakpoints_.end() || *it != t)
        breakpoints_.insert(it, t);
}
Circuit::InstanceScope::InstanceScope(Circuit &circuit, int index)
    : circuit_(circuit), previous_(circuit.current_instance_) {
    circuit_.current_instance_ = index;
}
Circuit::InstanceScope::~InstanceScope() { circuit_.current_instance_ = previous_; }
Circuit::InstanceScope Circuit::instance(const std::string &name, const std::string &model,
                                         std::vector<Port> ports) {
    instances_.push_back({name, model, std::move(ports)});
    return InstanceScope(*this, static_cast<int>(instances_.size()) - 1);
}
namespace {
/** @brief Quote and escape a string as a JSON string literal. */
std::string json_string(const std::string &s) {
    std::string out = "\"";
    for (const char ch : s) {
        const auto u = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (u < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", u);
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    return out + "\"";
}
/** @brief Format a number for JSON with round-trip precision; non-finite values give null. */
std::string json_number(double v) {
    if (!std::isfinite(v))
        return "null";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}
/** @brief Net name for the export: every ground alias is reported as "0". */
std::string export_net(const std::string &name) { return is_ground(name) ? "0" : name; }
} // namespace
std::string Circuit::describe_json() const {
    const auto net = [this](int i) { return json_string(names_.at(static_cast<std::size_t>(i))); };
    std::string out = "{\"nets\":[";
    for (std::size_t i = 0; i < names_.size(); ++i)
        out += (i ? "," : "") + json_string(names_[i]);
    out += "],\"parts\":[";
    bool first = true;
    for (std::size_t k = 0; k < parts_.size(); ++k) {
        const Part &p = parts_[k];
        const char *type = "";
        // Pin name and node index per terminal; params as (name, value) pairs.
        std::vector<std::pair<const char *, int>> pins;
        std::vector<std::pair<const char *, double>> params;
        double value = std::numeric_limits<double>::quiet_NaN();
        std::string wave;
        switch (p.type) {
        case Type::R:
        case Type::C:
        case Type::L:
            type = p.type == Type::R ? "R" : p.type == Type::C ? "C" : "L";
            pins = {{"1", p.a}, {"2", p.b}};
            value = p.v;
            break;
        case Type::V:
        case Type::I:
            type = p.type == Type::V ? "V" : "I";
            pins = {{"+", p.a}, {"-", p.b}};
            if (p.type == Type::I && !p.shaped) {
                value = p.v;
                break;
            }
            value = p.wave.at(0);
            wave = !p.wave.trace.empty() ? "pwl"
                   : p.wave.sine         ? "sin"
                   : p.wave.pulse        ? "pulse"
                                         : "dc";
            if (p.wave.pulse) {
                params = {{"low", p.wave.low},      {"high", p.wave.high}, {"delay", p.wave.delay},
                          {"rise", p.wave.rise},    {"fall", p.wave.fall}, {"on", p.wave.on},
                          {"period", p.wave.period}};
                if (p.wave.cycles > 0)
                    params.push_back({"cycles", static_cast<double>(p.wave.cycles)});
            } else if (p.wave.sine) {
                params = {{"offset", p.wave.low},      {"amplitude", p.wave.high},
                          {"freq", p.wave.freq},       {"delay", p.wave.delay},
                          {"damping", p.wave.damping}, {"phase", p.wave.phase}};
            } else if (!p.wave.trace.empty()) {
                params = {{"points", static_cast<double>(p.wave.trace.size())}};
            }
            break;
        case Type::D:
            type = "D";
            pins = {{"a", p.a}, {"k", p.b}};
            value = p.v;
            params = {{"is", p.v}, {"vt", p.p}};
            if (p.r > 0) {
                params.push_back({"bv", p.t});
                params.push_back({"ibv", p.u});
                params.push_back({"nbv", p.q});
            }
            break;
        case Type::NMOS:
        case Type::PMOS:
            type = p.type == Type::NMOS ? "NMOS" : "PMOS";
            pins = {{"d", p.a}, {"g", p.c}, {"s", p.b}};
            value = p.v;
            params = {{"k", p.v}, {"vto", p.p}, {"lambda", p.q}};
            break;
        case Type::NPN:
        case Type::PNP:
            type = p.type == Type::NPN ? "NPN" : "PNP";
            pins = {{"c", p.a}, {"b", p.c}, {"e", p.b}};
            value = p.p;
            params = {{"is", p.v}, {"bf", p.p}, {"br", p.q}};
            if (p.s != 0.02585)
                params.push_back({"vt", p.s});
            break;
        case Type::SW:
            type = "SW";
            pins = {{"1", p.a}, {"2", p.b}, {"cp", p.c}, {"cn", p.d}};
            params = {{"ron", p.v},
                      {"roff", p.p},
                      {"vt", p.q},
                      {"vh", p.r},
                      {"initial", p.initial ? 1.0 : 0.0}};
            break;
        case Type::DRV:
            type = "DRV";
            pins = {{"in", p.a}, {"out", p.c}, {"vdd", p.d}, {"gnd", p.b}};
            params = {{"r_high", p.v},     {"r_low", p.p},    {"threshold", p.q},
                      {"hysteresis", p.r}, {"delay_on", p.s}, {"delay_off", p.t}};
            break;
        case Type::LED:
            type = "LED";
            pins = {{"a", p.a}, {"k", p.b}};
            params = {{"k", p.led.k}, {"u0", p.led.u0},         {"s", p.led.s},
                      {"p", p.led.p}, {"temp_c", p.led.temp_c}, {"tc_v_per_k", p.led.tc_v_per_k}};
            break;
        case Type::BI:
        case Type::BV:
            type = p.type == Type::BI ? "BI" : "BV";
            pins = {{"+", p.a}, {"-", p.b}};
            break;
        }
        // Junction charges of this device, with SPICE parameter names (FC once).
        bool fc_listed = false;
        for (const Junction &j : junctions_) {
            if (j.part != k)
                continue;
            const std::string role = j.role;
            if (role.empty()) {
                params.insert(
                    params.end(),
                    {{"cjo", j.cj0}, {"vj", j.vj}, {"m", j.m}, {"fc", j.fc}, {"tt", j.tt}});
            } else if (role == "be") {
                params.insert(params.end(),
                              {{"cje", j.cj0}, {"vje", j.vj}, {"mje", j.m}, {"tf", j.tt}});
            } else {
                params.insert(params.end(),
                              {{"cjc", j.cj0}, {"vjc", j.vj}, {"mjc", j.m}, {"tr", j.tt}});
            }
            if (!role.empty() && !fc_listed)
                params.push_back({"fc", j.fc});
            fc_listed = true;
        }
        out += first ? "{" : ",{";
        first = false;
        out += "\"id\":" + json_string(type + std::to_string(k)) + ",\"type\":" + json_string(type);
        out += ",\"nodes\":{";
        for (std::size_t i = 0; i < pins.size(); ++i)
            out += (i ? "," : "") + json_string(pins[i].first) + ":" + net(pins[i].second);
        out += "},\"value\":" + json_number(value);
        if (!wave.empty())
            out += ",\"wave\":" + json_string(wave);
        if (!params.empty()) {
            out += ",\"params\":{";
            for (std::size_t i = 0; i < params.size(); ++i)
                out += (i ? "," : "") + json_string(params[i].first) + ":" +
                       json_number(params[i].second);
            out += "}";
        }
        if (p.type == Type::BI || p.type == Type::BV) {
            out += ",\"controls\":[";
            for (std::size_t i = 0; i < p.terms.size(); ++i) {
                const int t = p.terms[i];
                out += i ? "," : "";
                if (t == std::numeric_limits<int>::min())
                    out += json_string(time_control);
                else if (t < 0)
                    out += json_string("#I" + std::to_string(-t - 1));
                else
                    out += net(t);
            }
            out += "]";
        }
        if (!p.probe.empty())
            out += ",\"probe\":" + json_string(p.probe);
        if (p.instance >= 0) {
            const Instance &in = instances_[static_cast<std::size_t>(p.instance)];
            out += ",\"instance\":" + json_string(in.name) + ",\"model\":" + json_string(in.model);
        } else {
            out += ",\"instance\":null,\"model\":null";
        }
        out += "}";
    }
    // One part of type "X" per instance with declared ports (subcircuit view of the model).
    for (const Instance &in : instances_) {
        if (in.ports.empty())
            continue;
        out += first ? "{" : ",{";
        first = false;
        out += "\"id\":" + json_string(in.name) + ",\"type\":\"X\",\"nodes\":{";
        for (std::size_t i = 0; i < in.ports.size(); ++i)
            out += (i ? "," : "") + json_string(in.ports[i].first) + ":" +
                   json_string(export_net(in.ports[i].second));
        out += "},\"value\":null,\"instance\":" + json_string(in.name) +
               ",\"model\":" + json_string(in.model) + "}";
    }
    return out + "]}";
}
Result Circuit::run(const Settings &cfg) const {
    if (!(cfg.stop > 0 && cfg.max_step > 0 && cfg.abs_tol > 0 && cfg.rel_tol > 0))
        throw std::invalid_argument("invalid solver settings");
    int nodes = static_cast<int>(names_.size()) - 1;
    int branches = 0;
    std::vector<int> branch(parts_.size(), -1);
    for (size_t p = 0; p < parts_.size(); ++p)
        if (parts_[p].type == Type::L || parts_[p].type == Type::V || parts_[p].type == Type::BV)
            branch[p] = nodes + branches++;
    const int n = nodes + branches;
    if (n == 0)
        throw std::runtime_error("empty circuit");
    /** Unknowns that enter the adaptive local-error estimate (nodes and inductor currents). */
    // As ngspice, only charge-storage states: potentials of nets with a capacitor and inductor
    // currents. Nets driven algebraically (behavioural voltage sources, resistive nodes) are
    // left out: a B source with high gain (e.g. an error amplifier, V = 2000 * ve) turns the
    // Newton residue into a full/half difference that never shrinks with the step, and the
    // step collapsed to 1e-15 s in a regulator benchmark.
    std::vector<std::uint8_t> lte_unknown(n, 0);
    for (size_t p = 0; p < parts_.size(); ++p) {
        const Part &d = parts_[p];
        if (d.type == Type::C) {
            if (d.a > 0)
                lte_unknown[d.a - 1] = 1;
            if (d.b > 0)
                lte_unknown[d.b - 1] = 1;
        } else if (d.type == Type::L) {
            lte_unknown[branch[p]] = 1;
        }
    }
    // Junction charges are charge-storage states like capacitors.
    for (const Junction &j : junctions_) {
        if (j.a > 0)
            lte_unknown[j.a - 1] = 1;
        if (j.b > 0)
            lte_unknown[j.b - 1] = 1;
    }
    // Settings::lte_chgtol (coulomb): SPICE-like charge tolerance. The LTE norm of a
    // node is at least chgtol / (total capacitance at the node), so tiny internal capacitors
    // (1 pF behavioural states) do not force picosecond steps. 0 keeps the previous norm.
    std::vector<double> lte_floor(n, 0.0);
    {
        const double chgtol = cfg.lte_chgtol;
        if (chgtol > 0) {
            std::vector<double> csum(n, 0.0);
            for (const Part &d : parts_)
                if (d.type == Type::C) {
                    if (d.a > 0)
                        csum[d.a - 1] += d.v;
                    if (d.b > 0)
                        csum[d.b - 1] += d.v;
                }
            // Junction charges count with their zero-bias capacitance CJO.
            for (const Junction &j : junctions_) {
                if (j.a > 0)
                    csum[j.a - 1] += j.cj0;
                if (j.b > 0)
                    csum[j.b - 1] += j.cj0;
            }
            for (int i = 0; i < n; ++i)
                if (csum[i] > 0 && csum[i] <= cfg.lte_chgtol_cmax)
                    lte_floor[i] = chgtol / csum[i];
        }
    }
    std::vector<unsigned> lte_hits(n, 0);
    struct State {
        double t{};
        double previous_h{};
        unsigned steps{};
        std::vector<double> x, x2, cap_current;
        bool restart{}; // source discontinuity just passed: next step uses BE
        // Digital device state (switches, gate drivers): output level, sensed input level,
        // and a scheduled output change (time, level) for delayed gate drivers.
        std::vector<std::uint8_t> on{}, sensed{}, pending_level{};
        std::vector<double> pending{};
        // Predictor history (LteMethod::Predictor): x(t_{n-2}), the step before previous_h,
        // and how many of x, x2, x3 lie on one smooth segment (reset by BE restarts).
        std::vector<double> x3{};
        double previous_h2{};
        unsigned smooth{};
        // Junction charge currents dq/dt of the last accepted step (one per junction).
        std::vector<double> junction_current{};
    };
    State state{0,
                0,
                0,
                std::vector<double>(n, 0),
                std::vector<double>(n, 0),
                std::vector<double>(parts_.size(), 0)};
    state.on.assign(parts_.size(), 0);
    state.sensed.assign(parts_.size(), 0);
    state.pending_level.assign(parts_.size(), 0);
    state.pending.assign(parts_.size(), huge);
    state.x3.assign(n, 0);
    state.junction_current.assign(junctions_.size(), 0);
    for (size_t pi = 0; pi < parts_.size(); ++pi)
        if (parts_[pi].type == Type::SW)
            state.on[pi] = parts_[pi].initial ? 1 : 0;
    // Initial potentials (initial_voltage / ngspice .ic with uic); capacitors start charged to
    // the difference of their nets because their voltage is taken from x.
    for (const auto &[node_id, volts] : initial_)
        state.x[node_id - 1] = state.x2[node_id - 1] = state.x3[node_id - 1] = volts;
    LinearSolver cache;
    // The n*n matrix copy is only needed to detect identical matrices (reuse_matrix).
    cache.retain_matrix(cfg.reuse_matrix);
    // Auto with prefer_sparse off means dense, exactly as before linear_backend existed.
    const LinearBackend backend = cfg.linear_backend == LinearBackend::Auto && !cfg.prefer_sparse
                                      ? LinearBackend::Dense
                                      : cfg.linear_backend;
    unsigned reused = 0;
    const bool linear = std::none_of(parts_.begin(), parts_.end(), [](const Part &p) {
        return p.type == Type::D || p.type == Type::NMOS || p.type == Type::NPN ||
               p.type == Type::PNP || p.type == Type::PMOS || p.type == Type::LED ||
               p.type == Type::BI || p.type == Type::BV;
    });
    /** @brief Get potential at node index from unknown vector. */
    auto v_at = [&](const std::vector<double> &x, int node_id) -> double {
        return node_id ? x[node_id - 1] : 0.0;
    };
    constexpr int time_term = std::numeric_limits<int>::min();
    /** @brief Value of a behavioural control: node potential, source current or time. */
    auto control = [&](const std::vector<double> &x, int term, double t) -> double {
        if (term == time_term)
            return t;
        if (term >= 0)
            return v_at(x, term);
        return x[branch[-term - 1]];
    };
    /** @brief Matrix column of a behavioural control, -1 for ground and time. */
    auto column = [&](int term) -> int {
        if (term == time_term)
            return -1;
        if (term >= 0)
            return term ? term - 1 : -1;
        return branch[-term - 1];
    };
    /** Source/control time frozen at this value during the operating-point search (NaN: off). */
    double frozen_time = std::numeric_limits<double>::quiet_NaN();
    // Newton limit and step cut (Settings::newton_max, newton_cut), see circuit.hpp.
    const int exp_newton_max = std::max(2, cfg.newton_max);
    const double exp_newton_cut = std::max(1.5, cfg.newton_cut);
    const bool exp_predict_guess = cfg.predict_guess;
    std::FILE *steplog = cfg.steplog.empty() ? nullptr : std::fopen(cfg.steplog.c_str(), "w");
    struct SteplogCloser {
        std::FILE *f;
        ~SteplogCloser() {
            if (f)
                std::fclose(f);
        }
    } steplog_closer{steplog};
    /** @brief Perform one implicit integration step without mutating the input state. */
    // MNA buffers shared by all steps (avoids reallocation per step). The stamped pattern is
    // the union over the run, so it stays constant once every device has stamped (stable KLU
    // symbolic analysis), and only the touched entries are cleared per Newton iteration.
    std::vector<double> mna_a(static_cast<size_t>(n) * n, 0.0);
    std::vector<std::uint8_t> mna_stamped(static_cast<size_t>(n) * n, 0);
    std::vector<int> mna_touched;
    auto step = [&](const State &in, double h, unsigned &iterations) -> State {
        State out = in;
        const double ts = std::isnan(frozen_time) ? in.t + h : frozen_time;
        std::vector<double> guess = in.x;
        bool first = in.steps == 0;
        const unsigned iterations_at_start = iterations;
        // Experimental: linear extrapolation of the previous two time points as Newton start
        // (ngspice extrapolates device voltages in MODEINITPRED the same way).
        if (exp_predict_guess && !first && !in.restart && in.previous_h > 0 &&
            std::isnan(frozen_time)) {
            const double f = h / in.previous_h;
            for (int i = 0; i < n; ++i)
                guess[i] = in.x[i] + f * (in.x[i] - in.x2[i]);
        }
        // BE after a source edge damps the trapezoid ringing of the old cap current;
        // keep the conservative variable-step BDF2 growth bound below 1+sqrt(2).
        Method scheme = first || in.restart ? Method::BackwardEuler : cfg.method;
        if (scheme == Method::BDF2 && (in.previous_h <= 0 || h / in.previous_h > bdf2_max_ratio))
            scheme = Method::BackwardEuler;
        double alpha = 1 / h, beta = -1 / h, gamma = 0;
        if (scheme == Method::BDF2) {
            double r = h / in.previous_h;
            alpha = (1 + 2 * r) / ((1 + r) * h);
            beta = -(1 + r) / h;
            gamma = r * r / ((1 + r) * h);
        }
        // Junction charges at the two previous time points; q is a function of the
        // junction voltage, so recomputing it from the stored potentials conserves charge.
        std::vector<double> q_prev(junctions_.size()), q_prev2(junctions_.size());
        for (size_t ji = 0; ji < junctions_.size(); ++ji) {
            const Junction &j = junctions_[ji];
            double unused;
            q_prev[ji] = junction_charge(j, v_at(in.x, j.a) - v_at(in.x, j.b), unused);
            q_prev2[ji] = junction_charge(j, v_at(in.x2, j.a) - v_at(in.x2, j.b), unused);
        }
        // Newton buffers allocated once per step and cleared per iteration (no heap traffic
        // in the inner loop). rhs is moved into the solver, so it is recreated each time.
        std::vector<double> &a = mna_a;
        std::vector<std::uint8_t> &stamped = mna_stamped;
        std::vector<double> nl_values, bv_values, grad_values;
        // Simplified Newton (Settings::reuse_jacobian): the LU of an earlier iteration of this
        // step stays in use while the update contracts at least twofold per iteration.
        constexpr unsigned max_lu_reuses = 4;
        // Linear (not quadratic) convergence leaves an error of the order of the last update,
        // so a reused-LU iteration is accepted only well inside the tolerance; otherwise its
        // update is kept and one full Newton iteration finishes the step.
        constexpr double reused_accept = 0.1;
        bool refactor = true;
        unsigned lu_reuses = 0;
        double previous_worst = huge;
        const int newton_max = std::isnan(frozen_time) ? exp_newton_max : 200;
        for (int it = 0; it < newton_max; ++it) {
            ++iterations;
            // Only entries ever stamped can be non-zero: clear those (O(nnz) instead of n*n).
            for (const int idx : mna_touched)
                a[idx] = 0.0;
            std::vector<double> rhs(n, 0);
            /** @brief Add a coefficient, ignoring ground. */
            auto mat = [&](int row, int col, double val) {
                if (row >= 0 && col >= 0) {
                    const int idx = row * n + col;
                    if (!stamped[idx]) {
                        stamped[idx] = 1;
                        mna_touched.push_back(idx);
                    }
                    a[idx] += val;
                }
            };
            /** @brief Map node index to matrix row, -1 for ground. */
            auto row = [](int id) { return id ? id - 1 : -1; };
            /** @brief Add a conductance between two nodes. */
            auto conduct = [&](int p, int m, double g) {
                p = row(p);
                m = row(m);
                mat(p, p, g);
                mat(m, m, g);
                mat(p, m, -g);
                mat(m, p, -g);
            };
            /** @brief Add right-hand-side current injection into p from m. */
            auto inject = [&](int p, int m, double current) {
                p = row(p);
                m = row(m);
                if (p >= 0)
                    rhs[p] += current;
                if (m >= 0)
                    rhs[m] -= current;
            };
            /** @brief Linearize an arbitrary nonlinear current between two nodes; the
             *  Jacobian comes from gradient when given, else from forward differences. */
            auto nlcurrent = [&](int p, int m, const std::vector<int> &terminals,
                                 const std::function<double(const std::vector<double> &)> &function,
                                 const BehaviourGrad *gradient = nullptr) {
                std::vector<double> &vv = nl_values;
                vv.clear();
                for (int term : terminals)
                    vv.push_back(control(guess, term, ts));
                const bool exact = gradient && *gradient;
                if (exact)
                    grad_values.resize(vv.size());
                double i0 = exact ? (*gradient)(vv.data(), grad_values.data()) : function(vv);
                double total = 0;
                for (size_t k = 0; k < terminals.size(); ++k) {
                    const int col = column(terminals[k]);
                    if (col < 0)
                        continue; // ground or time: no Jacobian entry
                    double g;
                    if (exact) {
                        g = grad_values[k];
                    } else {
                        double eps = 1e-6 * std::max(1.0, std::abs(vv[k]));
                        const double keep = vv[k];
                        vv[k] += eps;
                        g = (function(vv) - i0) / eps;
                        vv[k] = keep;
                    }
                    mat(row(p), col, g);
                    mat(row(m), col, -g);
                    total += g * vv[k];
                }
                inject(p, m, total - i0);
            };
            for (size_t pi = 0; pi < parts_.size(); ++pi) {
                const Part &d = parts_[pi];
                double vp = v_at(in.x, d.a) - v_at(in.x, d.b);
                double vpp = v_at(in.x2, d.a) - v_at(in.x2, d.b);
                switch (d.type) {
                case Type::R:
                    conduct(d.a, d.b, 1 / d.v);
                    break;
                case Type::I:
                    inject(d.a, d.b, d.shaped ? -d.wave.at(ts) : -d.v);
                    break;
                case Type::C: {
                    double g = 0, inj = 0;
                    if (scheme == Method::Trapezoid) {
                        g = 2 * d.v / h;
                        inj = g * vp + in.cap_current[pi];
                    } else {
                        g = d.v * alpha;
                        inj = -d.v * (beta * vp + gamma * vpp);
                    }
                    conduct(d.a, d.b, g);
                    inject(d.a, d.b, inj);
                    break;
                }
                case Type::L: {
                    int br = branch[pi];
                    mat(row(d.a), br, 1);
                    mat(row(d.b), br, -1);
                    mat(br, row(d.a), 1);
                    mat(br, row(d.b), -1);
                    double g = 0, known = 0;
                    if (scheme == Method::Trapezoid) {
                        g = 2 * d.v / h;
                        known = -g * in.x[br] - vp;
                    } else {
                        g = d.v * alpha;
                        known = d.v * (beta * in.x[br] + gamma * in.x2[br]);
                    }
                    mat(br, br, -g);
                    rhs[br] += known;
                    break;
                }
                case Type::V: {
                    int br = branch[pi];
                    mat(row(d.a), br, 1);
                    mat(row(d.b), br, -1);
                    mat(br, row(d.a), 1);
                    mat(br, row(d.b), -1);
                    rhs[br] += d.wave.at(ts);
                    break;
                }
                case Type::D: {
                    double vd = v_at(guess, d.a) - v_at(guess, d.b);
                    double ee = limited_exp(vd / d.p);
                    double current = d.v * (ee - 1);
                    double gd = d.v * std::exp(std::clamp(vd / d.p, -50.0, 100.0)) / d.p;
                    if (d.r > 0 && vd < -d.r) {
                        // Reverse breakdown (ngspice dioload): -is*exp(-(v+bv)/(nbv*Vt)).
                        const double ar = -(vd + d.r) / d.s;
                        current = -d.v * limited_exp(ar);
                        gd = d.v * std::exp(std::clamp(ar, -50.0, 100.0)) / d.s;
                    }
                    conduct(d.a, d.b, gd + gmin);
                    inject(d.a, d.b, gd * vd - current);
                    break;
                }
                case Type::NMOS: {
                    nlcurrent(d.a, d.b, {d.a, d.c, d.b}, [&](const std::vector<double> &v) {
                        return mos_id(v[0], v[1], v[2], d.v, d.p, d.q);
                    });
                    conduct(d.a, d.b, gmin);
                    break;
                }
                case Type::NPN:
                case Type::PNP: {
                    // Ebers-Moll transport model; v = {collector, base, emitter}. The PNP is the
                    // mirror: junction voltages and terminal currents change sign.
                    const double sgn = d.type == Type::NPN ? 1.0 : -1.0;
                    const auto terminal = [&](const std::vector<double> &v, bool base) {
                        const double vbe = sgn * (v[1] - v[2]), vbc = sgn * (v[1] - v[0]);
                        const double icc = d.v * (limited_exp(vbe / d.s) - 1);
                        const double iec = d.v * (limited_exp(vbc / d.s) - 1);
                        return sgn * (base ? icc / d.p + iec / d.q : icc - iec * (1 + 1 / d.q));
                    };
                    nlcurrent(d.a, d.b, {d.a, d.c, d.b},
                              [&](const std::vector<double> &v) { return terminal(v, false); });
                    nlcurrent(d.c, d.b, {d.a, d.c, d.b},
                              [&](const std::vector<double> &v) { return terminal(v, true); });
                    conduct(d.c, d.b, gmin);
                    conduct(d.a, d.b, gmin);
                    conduct(d.c, d.a, gmin);
                    break;
                }
                case Type::PMOS: {
                    nlcurrent(d.a, d.b, {d.a, d.c, d.b}, [&](const std::vector<double> &v) {
                        return pmos_id(v[0], v[1], v[2], d.v, d.p, d.q);
                    });
                    conduct(d.a, d.b, gmin);
                    break;
                }
                case Type::SW:
                    conduct(d.a, d.b, 1 / (in.on[pi] ? d.v : d.p));
                    break;
                case Type::DRV:
                    // Output stage: closed resistor to VDD (high) or GND (low); the open side
                    // and the high-impedance input only see GMIN.
                    conduct(d.c, d.d, in.on[pi] ? 1 / d.v : gmin);
                    conduct(d.c, d.b, in.on[pi] ? gmin : 1 / d.p);
                    conduct(d.a, d.b, gmin);
                    break;
                case Type::LED: {
                    double u = v_at(guess, d.a) - v_at(guess, d.b);
                    double g = d.led.siemens(u);
                    conduct(d.a, d.b, g + gmin);
                    inject(d.a, d.b, g * u - d.led.amps(u));
                    break;
                }
                case Type::BI:
                    nlcurrent(d.a, d.b, d.terms, d.fn, &d.fg);
                    conduct(d.a, d.b, gmin);
                    break;
                case Type::BV: {
                    // Branch row: v(a) - v(b) - f(v) = 0, linearised around the Newton guess:
                    // v(a) - v(b) - sum_k g_k v_k = f(v0) - sum_k g_k v0_k.
                    int br = branch[pi];
                    mat(row(d.a), br, 1);
                    mat(row(d.b), br, -1);
                    mat(br, row(d.a), 1);
                    mat(br, row(d.b), -1);
                    std::vector<double> &vv = bv_values;
                    vv.clear();
                    for (int term : d.terms)
                        vv.push_back(control(guess, term, ts));
                    // Exact gradient when the source has one, else forward differences.
                    const bool exact = static_cast<bool>(d.fg);
                    if (exact)
                        grad_values.resize(vv.size());
                    const double f0 = exact ? d.fg(vv.data(), grad_values.data()) : d.fn(vv);
                    double known = f0;
                    for (size_t k = 0; k < d.terms.size(); ++k) {
                        const int col = column(d.terms[k]);
                        if (col < 0)
                            continue;
                        double g;
                        if (exact) {
                            g = grad_values[k];
                        } else {
                            const double eps = 1e-6 * std::max(1.0, std::abs(vv[k]));
                            const double keep = vv[k];
                            vv[k] += eps;
                            g = (d.fn(vv) - f0) / eps;
                            vv[k] = keep;
                        }
                        mat(br, col, -g);
                        known -= g * vv[k];
                    }
                    rhs[br] += known;
                    break;
                }
                }
            }
            // Junction charges: companion model of i = dq/dt linearised at the guess,
            // i = g v + (i0 - g v0) with i0 from the integration formula on q and g = C(v0)
            // times the formula's leading coefficient (as a linear capacitor for q = C v).
            for (size_t ji = 0; ji < junctions_.size(); ++ji) {
                const Junction &j = junctions_[ji];
                const double vg = v_at(guess, j.a) - v_at(guess, j.b);
                double cap;
                const double q = junction_charge(j, vg, cap);
                double g, i0;
                if (scheme == Method::Trapezoid) {
                    g = 2 * cap / h;
                    i0 = 2 / h * (q - q_prev[ji]) - in.junction_current[ji];
                } else {
                    g = alpha * cap;
                    i0 = alpha * q + beta * q_prev[ji] + gamma * q_prev2[ji];
                }
                conduct(j.a, j.b, g);
                inject(j.a, j.b, g * vg - i0);
            }
            // Operating point: a small conductance to ground keeps nodes that only see
            // capacitors (open at DC) solvable, as ngspice GMIN.
            if (!std::isnan(frozen_time))
                for (int k = 1; k <= nodes; ++k)
                    conduct(k, 0, 1e-12);
            const bool reuse_lu = cfg.reuse_jacobian && !refactor;
            std::vector<double> solved;
            if (reuse_lu) {
                // Residual form with the old factors: x = guess + LU_old^-1 (rhs - A guess).
                // Rows equal in the old and new Jacobian (sources, linear parts) stay exact.
                for (int i = 0; i < n; ++i) {
                    const double *a_row = &a[static_cast<size_t>(i) * n];
                    double r = rhs[i];
                    for (int j = 0; j < n; ++j)
                        r -= a_row[j] * guess[j];
                    rhs[i] = r;
                }
                solved = cache.solve(std::move(rhs));
                for (int i = 0; i < n; ++i)
                    solved[i] += guess[i];
                ++lu_reuses;
            } else {
                if (!cfg.reuse_matrix || !cache.matches(a, n)) {
                    // The pattern only grows, so its entry count identifies it.
                    cache.set_pattern_version(static_cast<long>(mna_touched.size()));
                    cache.factor(a, n, stamped, backend);
                } else
                    ++reused;
                solved = cache.solve(std::move(rhs));
                lu_reuses = 0;
            }
            // Damped Newton: scale the whole update so no PN junction (diode, NPN b-e)
            // moves more than the limit. Uniform scaling keeps KCL and source rows
            // consistent (the old per-diode node shift did not); linear nodes far away
            // (e.g. a collector at -400 V) do not slow convergence.
            if (!linear) {
                double biggest = 0;
                // PN junctions as (anode, cathode): diode, NPN b-e and b-c, PNP e-b and c-b.
                const auto limit = [&](int anode, int cathode) {
                    double before = v_at(guess, anode) - v_at(guess, cathode);
                    double after = v_at(solved, anode) - v_at(solved, cathode);
                    // Bound steps both into and out of forward conduction to prevent oscillation.
                    if (std::max(before, after) > 0.4)
                        biggest = std::max(biggest, std::abs(after - before));
                };
                for (const Part &j : parts_) {
                    if (j.type == Type::D) {
                        limit(j.a, j.b);
                        if (j.r > 0) {
                            // Breakdown junction: conducts for -(v + bv_eff) > 0, bounded like a
                            // forward junction (anode/cathode swapped, shifted by bv_eff).
                            const double before = v_at(guess, j.b) - v_at(guess, j.a) - j.r;
                            const double after = v_at(solved, j.b) - v_at(solved, j.a) - j.r;
                            if (std::max(before, after) > 0.4)
                                biggest = std::max(biggest, std::abs(after - before));
                        }
                    } else if (j.type == Type::NPN) {
                        limit(j.c, j.b);
                        limit(j.c, j.a);
                    } else if (j.type == Type::PNP) {
                        limit(j.b, j.c);
                        limit(j.a, j.c);
                    }
                }
                if (biggest > newton_step_limit) {
                    const double lambda = newton_step_limit / biggest;
                    for (int i = 0; i < n; ++i)
                        solved[i] = guess[i] + lambda * (solved[i] - guess[i]);
                }
            }
            double worst = 0;
            for (int i = 0; i < n; ++i) {
                double scale =
                    cfg.abs_tol + cfg.rel_tol * std::max(std::abs(guess[i]), std::abs(solved[i]));
                worst = std::max(worst, std::abs(solved[i] - guess[i]) / scale);
            }
            const bool contracting = worst <= 0.5 * previous_worst;
            if (cfg.reuse_jacobian) {
                // Guard: a reused LU that no longer halves the update is discarded and the
                // current Jacobian factored; after a fresh LU slow contraction keeps full Newton.
                if (reuse_lu && !contracting) {
                    refactor = true;
                    continue;
                }
                refactor = !contracting || lu_reuses >= max_lu_reuses;
                previous_worst = worst;
                if (reuse_lu && worst < 1.0 && worst >= reused_accept) {
                    refactor = true;
                    guess = std::move(solved);
                    continue;
                }
            }
            guess = std::move(solved);
            // As in ngspice NIiter, never accept the first nonlinear iteration: its devices
            // were linearised at the previous time point. A small source move (e.g. a gate
            // ramp on a 12 V rail) could otherwise pass the update test while a MOSFET
            // crossing its threshold is still evaluated at the old gate voltage.
            if (linear || (it > 0 && worst < 1.0)) {
                out.t = in.t + h;
                out.previous_h = h;
                out.steps++;
                out.restart = false;
                out.previous_h2 = in.previous_h;
                out.x3 = std::move(out.x2);
                out.x2 = in.x;
                // A BE start or restart step begins a new smooth segment (in.x and out.x).
                out.smooth = scheme == Method::Trapezoid && !first && !in.restart
                                 ? std::min(in.smooth + 1, 3u)
                                 : 2u;
                out.x = std::move(guess);
                // Floating-point ill-conditioning must fail loudly, not emit
                // impossible waveforms (e.g. a 30 V ideal source at 1e24 V).
                for (const Part &voltage_source : parts_) {
                    if (voltage_source.type != Type::V)
                        continue;
                    const double expected = voltage_source.wave.at(ts);
                    const double observed =
                        v_at(out.x, voltage_source.a) - v_at(out.x, voltage_source.b);
                    if (std::abs(observed - expected) >
                        cfg.abs_tol + cfg.rel_tol * std::max(1.0, std::abs(expected)))
                        throw std::runtime_error(
                            "MNA voltage-source constraint violated: ill-conditioned matrix or "
                            "unstable device model (t=" +
                            std::to_string(out.t) + ", expected=" + std::to_string(expected) +
                            ", observed=" + std::to_string(observed) + ")");
                }
                for (size_t pi = 0; pi < parts_.size(); ++pi) {
                    const Part &d = parts_[pi];
                    if (d.type != Type::C)
                        continue;
                    double vn = v_at(out.x, d.a) - v_at(out.x, d.b);
                    if (scheme == Method::Trapezoid)
                        out.cap_current[pi] =
                            2 * d.v / h * (vn - (v_at(in.x, d.a) - v_at(in.x, d.b))) -
                            in.cap_current[pi];
                    else
                        out.cap_current[pi] =
                            d.v * (alpha * vn + beta * (v_at(in.x, d.a) - v_at(in.x, d.b)) +
                                   gamma * (v_at(in.x2, d.a) - v_at(in.x2, d.b)));
                }
                for (size_t ji = 0; ji < junctions_.size(); ++ji) {
                    const Junction &j = junctions_[ji];
                    double unused;
                    const double q =
                        junction_charge(j, v_at(out.x, j.a) - v_at(out.x, j.b), unused);
                    out.junction_current[ji] =
                        scheme == Method::Trapezoid
                            ? 2 / h * (q - q_prev[ji]) - in.junction_current[ji]
                            : alpha * q + beta * q_prev[ji] + gamma * q_prev2[ji];
                }
                if (steplog)
                    std::fprintf(steplog, "%.12g,%.6g,%d,%u,1\n", in.t, h,
                                 scheme == Method::Trapezoid ? 1 : 0,
                                 iterations - iterations_at_start);
                return out;
            }
        }
        if (steplog)
            std::fprintf(steplog, "%.12g,%.6g,%d,%u,0\n", in.t, h,
                         scheme == Method::Trapezoid ? 1 : 0, iterations - iterations_at_start);
        throw std::runtime_error("Newton iteration failed; decrease max-step or relax tolerances");
    };
    Result result;
    result.nodes = names_;
    for (const Part &p : parts_)
        if ((p.type == Type::LED || p.type == Type::BI || p.type == Type::V || p.type == Type::BV ||
             p.type == Type::L) &&
            !p.probe.empty())
            result.currents.push_back(p.probe);
    auto record = [&](const State &s) {
        Sample sample;
        sample.time = s.t;
        sample.volts.push_back(0);
        for (int i = 0; i < nodes; ++i)
            sample.volts.push_back(s.x[i]);
        for (size_t pi = 0; pi < parts_.size(); ++pi) {
            const Part &p = parts_[pi];
            if (p.probe.empty())
                continue;
            if (p.type == Type::V || p.type == Type::BV || p.type == Type::L) {
                sample.amps.push_back(s.x[branch[pi]]);
            } else if (p.type == Type::LED) {
                sample.amps.push_back(p.led.amps(v_at(s.x, p.a) - v_at(s.x, p.b)));
            } else if (p.type == Type::BI) {
                std::vector<double> vv;
                for (int term : p.terms)
                    vv.push_back(control(s.x, term, s.t));
                sample.amps.push_back(p.fn(vv));
            }
        }
        result.samples.push_back(std::move(sample));
    };
    /** @brief Update switch and gate-driver states after an accepted step.
     *
     *  Switches change at the end of the step that crosses their threshold; the main loop
     *  first shortens that step to the interpolated crossing (switch_crossing). Gate drivers sense
     * their input with the configured hysteresis, interpolate the crossing time inside the step and
     * schedule the output change after the configured delay. Any change forces a BE restart.
     */
    auto settle = [&](const State &before, State &after) {
        const double tiny = 1e-15 + std::abs(after.t) * 1e-12;
        for (size_t pi = 0; pi < parts_.size(); ++pi) {
            const Part &d = parts_[pi];
            if (d.type == Type::SW) {
                double vc = v_at(after.x, d.c) - v_at(after.x, d.d);
                std::uint8_t next = after.on[pi];
                if (!next && vc > d.q + d.r)
                    next = 1;
                else if (next && vc < d.q - d.r)
                    next = 0;
                if (next != after.on[pi]) {
                    after.on[pi] = next;
                    after.restart = true;
                    ++result.switch_events;
                }
            } else if (d.type == Type::DRV) {
                double v0 = v_at(before.x, d.a) - v_at(before.x, d.b);
                double v1 = v_at(after.x, d.a) - v_at(after.x, d.b);
                std::uint8_t sensed = after.sensed[pi];
                double level = 0;
                if (!sensed && v1 > d.q + d.r) {
                    sensed = 1;
                    level = d.q + d.r;
                } else if (sensed && v1 < d.q - d.r) {
                    sensed = 0;
                    level = d.q - d.r;
                }
                if (sensed != after.sensed[pi]) {
                    after.sensed[pi] = sensed;
                    double frac = v1 != v0 ? std::clamp((level - v0) / (v1 - v0), 0.0, 1.0) : 1.0;
                    double crossing = before.t + frac * (after.t - before.t);
                    after.pending[pi] = crossing + (sensed ? d.s : d.t);
                    after.pending_level[pi] = sensed;
                }
                if (after.pending[pi] <= after.t + tiny) {
                    if (after.on[pi] != after.pending_level[pi]) {
                        after.on[pi] = after.pending_level[pi];
                        after.restart = true;
                        ++result.switch_events;
                    }
                    after.pending[pi] = huge;
                }
            }
        }
    };
    /** @brief Earliest interpolated switching time of a voltage switch inside the step
     *  (before.t, after.t]; infinity if no switch crosses its threshold. The caller shortens
     *  the step to this time, so the switch changes at the crossing instead of one step late. */
    auto switch_crossing = [&](const State &before, const State &after) {
        double first = huge;
        for (size_t pi = 0; pi < parts_.size(); ++pi) {
            const Part &d = parts_[pi];
            if (d.type != Type::SW)
                continue;
            const double vc0 = v_at(before.x, d.c) - v_at(before.x, d.d);
            const double vc1 = v_at(after.x, d.c) - v_at(after.x, d.d);
            const bool on = after.on[pi];
            const double level = on ? d.q - d.r : d.q + d.r;
            const bool toggles = on ? vc1 < level : vc1 > level;
            if (toggles && vc1 != vc0) {
                const double frac = std::clamp((level - vc0) / (vc1 - vc0), 0.0, 1.0);
                first = std::min(first, before.t + frac * (after.t - before.t));
            }
        }
        return first;
    };
    if (cfg.operating_point) {
        // Pseudo-transient continuation at frozen t = 0: backward-Euler steps with growing h
        // turn capacitors into opens (C/h -> 0) and inductors into shorts (L/h -> 0) while
        // every step starts from the previous solution, so diodes/transistors are reached
        // smoothly. The transient then starts from this state with zero capacitor currents.
        frozen_time = 0.0;
        State op = state;
        bool ok = true;
        for (double hk = 1e-9; hk <= 1e6 * 1.0001; hk *= std::sqrt(10.0)) {
            op.restart = true;
            op.steps = 0;
            try {
                op = step(op, hk, result.newton_iterations);
            } catch (const std::runtime_error &) {
                ok = false;
                break;
            }
            op.t = 0;
        }
        frozen_time = std::numeric_limits<double>::quiet_NaN();
        if (ok) {
            state.x = op.x;
            state.x2 = op.x;
            std::fill(state.cap_current.begin(), state.cap_current.end(), 0.0);
            std::fill(state.junction_current.begin(), state.junction_current.end(), 0.0);
            state.restart = true;
            result.operating_point = true;
        } else {
            result.warning = "Operating point not found; transient starts from 0 V.";
        }
    }
    record(state);
    double h = cfg.max_step;
    for (unsigned count = 0; state.t < cfg.stop - 1e-14; ++count) {
        if (count > 3000000)
            throw std::runtime_error("maximum step count exceeded");
        const unsigned events_before = result.switch_events;
        double limit = cfg.stop - state.t;
        for (const auto &d : parts_)
            if (d.type == Type::V || (d.type == Type::I && d.shaped))
                limit = std::min(limit, d.wave.next_event(state.t) - state.t);
        for (double due : state.pending)
            if (due > state.t)
                limit = std::min(limit, due - state.t);
        {
            // Same tolerance as Waveform::next_event: a breakpoint just reached is not hit again.
            const double after = state.t + std::max(1e-15, std::abs(state.t) * 1e-12);
            const auto bp = std::upper_bound(breakpoints_.begin(), breakpoints_.end(), after);
            if (bp != breakpoints_.end())
                limit = std::min(limit, *bp - state.t);
        }
        double dt = std::min(h, limit);
        // As ngspice after a breakpoint: the first-order (BE) restart step is kept short, so
        // its numerical damping stays small (a full-length BE step after every PWL vertex
        // damped a Q = 5 resonance by 0.5 %).
        if (state.restart && state.steps > 0)
            dt = std::min(dt, std::max(0.1 * h, cfg.max_step * 1e-6));
        if (cfg.method == Method::BDF2 && state.previous_h > 0)
            dt = std::min(dt, bdf2_max_ratio * state.previous_h);
        const bool hits_event = limit < h && limit < cfg.stop - state.t;
        if (dt <= 0)
            throw std::runtime_error("non-positive simulation timestep");
        // Predictor/corrector error estimate needs three smooth trapezoid points; otherwise
        // (start, after breakpoints, events, BE restarts) this step uses step doubling.
        const bool predict = cfg.adaptive && cfg.lte_method == LteMethod::Predictor &&
                             cfg.method == Method::Trapezoid && !state.restart &&
                             state.smooth >= 3 && state.previous_h > 0 && state.previous_h2 > 0;
        if (predict) {
            const State previous = state;
            State corrected;
            try {
                corrected = step(state, dt, result.newton_iterations);
            } catch (const std::runtime_error &error) {
                if (dt < cfg.max_step * 1e-6) {
                    char where[96];
                    std::snprintf(where, sizeof where, " (t=%.9g s, dt=%.3g s)", state.t, dt);
                    throw std::runtime_error(std::string(error.what()) + where);
                }
                h = dt / exp_newton_cut;
                result.rejected++;
                continue;
            }
            // Quadratic Lagrange extrapolation through t_n, t_{n-1}, t_{n-2} to t_n + dt. The
            // trapezoid error is (dt^3/12) x''' and the predictor error dt(dt+h1)(dt+h1+h2)/6
            // x''' with opposite sign, so their difference scales to the corrector LTE
            // (Milne's device, as ngspice CKTterr).
            const double h1 = state.previous_h, h2 = state.previous_h2;
            const double l0 = (dt + h1) * (dt + h1 + h2) / (h1 * (h1 + h2));
            const double l1 = -dt * (dt + h1 + h2) / (h1 * h2);
            const double l2 = dt * (dt + h1) / (h2 * (h1 + h2));
            const double trap = dt * dt * dt / 12;
            const double scale = trap / (trap + dt * (dt + h1) * (dt + h1 + h2) / 6);
            double ratio = 0;
            int worst_i = -1;
            for (int i = 0; i < n; ++i) {
                if (!lte_unknown[i])
                    continue;
                const double xp = l0 * state.x[i] + l1 * state.x2[i] + l2 * state.x3[i];
                const double norm =
                    std::max(cfg.abs_tol + cfg.rel_tol * std::abs(corrected.x[i]), lte_floor[i]);
                const double r = std::abs(corrected.x[i] - xp) * scale / norm;
                if (r > ratio) {
                    ratio = r;
                    worst_i = i;
                }
            }
            ratio /= cfg.trtol;
            if (steplog && worst_i >= 0 && ratio > 0.5)
                ++lte_hits[worst_i];
            if (ratio > 1 && dt > cfg.max_step * 1e-6) {
                result.rejected++;
                h = dt * std::clamp(0.9 / std::cbrt(ratio), 0.2, 0.8);
                continue;
            }
            const double tsw = switch_crossing(previous, corrected);
            if (tsw < corrected.t && corrected.t - tsw > 1e-3 * dt &&
                tsw - previous.t > cfg.max_step * 1e-6) {
                h = (tsw - previous.t) * (1 + 1e-6); // land just past the switching point
                continue;
            }
            state = std::move(corrected);
            state.restart = hits_event;
            // Accepted at the floor despite ratio > 1: grow, as in the step-doubling branch.
            h = ratio > 1
                    ? std::min(cfg.max_step, 2 * dt)
                    : std::min(cfg.max_step,
                               dt * std::clamp(0.9 / std::cbrt(std::max(ratio, 1e-6)), 0.2, 2.0));
            settle(previous, state);
        } else if (cfg.adaptive) {
            const State previous = state;
            State full, half1, half2;
            try {
                full = step(state, dt, result.newton_iterations);
                half1 = step(state, dt / 2, result.newton_iterations);
                half2 = step(half1, dt / 2, result.newton_iterations);
            } catch (const std::runtime_error &error) {
                if (dt < cfg.max_step * 1e-6) {
                    char where[96];
                    std::snprintf(where, sizeof where, " (t=%.9g s, dt=%.3g s)", state.t, dt);
                    throw std::runtime_error(std::string(error.what()) + where);
                }
                h = dt / exp_newton_cut;
                result.rejected++;
                continue;
            }
            // Local error only on node potentials and inductor currents (the integrated
            // states). Branch currents of ideal and behavioural voltage sources are algebraic:
            // a capacitor directly on a PWL source makes them jump at every breakpoint, so the
            // full/half-step difference never shrinks and the step would collapse (as ngspice,
            // whose LTE control only looks at charge-storage elements).
            double worst = 0;
            for (int i = 0; i < n; ++i) {
                if (!lte_unknown[i])
                    continue;
                double norm =
                    cfg.abs_tol + cfg.rel_tol * std::max(std::abs(full.x[i]), std::abs(half2.x[i]));
                worst = std::max(worst, std::abs(full.x[i] - half2.x[i]) / norm);
            }
            worst /= cfg.trtol; // ngspice TRTOL (Settings::trtol, default 1 = unchanged)
            if (worst > 1 && dt > cfg.max_step * 1e-6) {
                result.rejected++;
                h = dt * std::clamp(0.85 / std::sqrt(worst), 0.2, 0.8);
                continue;
            }
            const double tsw = switch_crossing(previous, half2);
            if (tsw < half2.t && half2.t - tsw > 1e-3 * dt &&
                tsw - previous.t > cfg.max_step * 1e-6) {
                h = (tsw - previous.t) * (1 + 1e-6); // land just past the switching point
                continue;
            }
            state = std::move(half2);
            state.restart = hits_event;
            // A step accepted at the floor (max_step * 1e-6) despite worst > 1 must not shrink
            // the next one again: grow, otherwise h stays below the floor for good.
            h = worst > 1
                    ? std::min(cfg.max_step, 2 * dt)
                    : std::min(cfg.max_step,
                               dt * std::clamp(0.85 / std::sqrt(std::max(worst, 1e-6)), 0.6, 2.0));
            settle(previous, state);
        } else {
            // Nonlinear algebraic outputs can bend sharply even under a linear input ramp.
            // Bound the actual midpoint interpolation error, rather than claiming that
            // convergence at sparse endpoints guarantees an accurate exported waveform.
            const State before = state;
            for (;;) {
                State candidate = step(before, dt, result.newton_iterations);
                // The initial zero state is not a solved algebraic operating point.
                bool ramp = false;
                if (!linear && before.steps > 0)
                    for (const auto &part : parts_) {
                        if ((part.type == Type::V || (part.type == Type::I && part.shaped)) &&
                            part.wave.at(before.t + dt * 0.25) !=
                                part.wave.at(before.t + dt * 0.75))
                            ramp = true;
                    }
                double worst = 0;
                if (ramp) {
                    State middle = step(before, dt / 2, result.newton_iterations);
                    for (int i = 0; i < nodes; ++i) {
                        double interpolated = (before.x[i] + candidate.x[i]) / 2;
                        double scale =
                            cfg.abs_tol +
                            cfg.rel_tol * std::max({std::abs(before.x[i]), std::abs(candidate.x[i]),
                                                    std::abs(middle.x[i])});
                        worst = std::max(worst, std::abs(middle.x[i] - interpolated) / scale);
                    }
                }
                if (worst <= 1) {
                    const double tsw = switch_crossing(before, candidate);
                    if (tsw < candidate.t && candidate.t - tsw > 1e-3 * dt &&
                        tsw - before.t > cfg.max_step * 1e-9) {
                        dt = (tsw - before.t) * (1 + 1e-6); // land just past the switching point
                        continue;
                    }
                    state = std::move(candidate);
                    break;
                }
                if (dt <= cfg.max_step * 1e-9)
                    throw std::runtime_error("source-ramp interpolation tolerance not reached");
                dt *= std::clamp(0.8 / std::sqrt(worst), 0.1, 0.5);
                ++result.rejected;
            }
            state.restart = hits_event && std::abs(dt - limit) <= dt * 1e-9;
            settle(before, state);
        }
        result.accepted++;
        record(state);
        if (result.switch_events != events_before && state.t < cfg.stop - 1e-14) {
            // A switch or gate driver changed at the end of this step. Re-solve the algebraic
            // nodes with the new state after a vanishing BE step (capacitor voltages and
            // inductor currents stay put), so the right-hand limit appears at the switching
            // time and not one step later (ngspice updates the switch inside Newton).
            const double tiny = std::max(1e-15, 1e-9 * cfg.max_step);
            state.restart = true;
            // The vanishing step makes C/h huge; with a stiff network Newton can fail there
            // (seen with a switching-regulator average model at rel_tol 1e-5). The step is only
            // the right-hand limit sample: then the next regular step starts from the
            // left-hand state with restart set, as before the switch fix.
            try {
                State after = step(state, tiny, result.newton_iterations);
                after.restart = true;
                state = std::move(after);
                result.accepted++;
                record(state);
            } catch (const std::runtime_error &) {
                state.restart = true;
                ++result.post_switch_skipped;
            }
        }
    }
    if (!cfg.adaptive && cfg.max_step > cfg.stop / 5)
        result.warning =
            "Coarse fixed step: fast dynamics may be lost; use adaptive mode or smaller step.";
    if (steplog) {
        // Diagnostics: unknowns that most often limited the predictor LTE (ratio > 0.5).
        std::vector<int> order(n);
        for (int i = 0; i < n; ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](int x, int y) { return lte_hits[x] > lte_hits[y]; });
        for (int k = 0; k < std::min(n, 15) && lte_hits[order[k]] > 0; ++k) {
            const int i = order[k];
            std::fprintf(stderr, "LTE-Grenze %6u  %s\n", lte_hits[i],
                         i < nodes ? names_[i + 1].c_str() : "(Zweigstrom)");
        }
    }
    result.matrix_reuses = reused;
    result.sparse_symbolic_analyses = cache.symbolic_analyses();
    result.sparse_fallbacks = cache.sparse_fallbacks();
    result.plan_builds = cache.plan_builds();
    result.plan_fallbacks = cache.plan_fallbacks();
    result.backend = cache.using_plan()     ? "plan"
                     : cache.using_sparse() ? "SuiteSparse-KLU"
                                            : "dense-LU";
    return result;
}
} // namespace mspice::sim
