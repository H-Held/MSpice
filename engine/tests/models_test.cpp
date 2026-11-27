// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
// Device model regressions: PMOS mirror, voltage-controlled switch, behavioural gate driver,
// fitted LED-strand curve, BJT and DC operating point. Each mode returns 0 on success and
// prints the evidence.
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mspice/sim/circuit.hpp>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
using namespace mspice::sim;

namespace {
/** @brief Column index of a node in a result, or -1. */
int column(const Result &r, const std::string &name) {
    for (size_t i = 0; i < r.nodes.size(); ++i)
        if (r.nodes[i] == name)
            return static_cast<int>(i);
    return -1;
}
/** @brief Linearly interpolated node voltage at time t. */
double at(const Result &r, const std::string &name, double t) {
    const int k = column(r, name);
    if (k < 0)
        throw std::runtime_error("missing node " + name);
    for (size_t i = 1; i < r.samples.size(); ++i) {
        const auto &a = r.samples[i - 1], &b = r.samples[i];
        if (b.time >= t) {
            if (b.time == a.time)
                return b.volts[k];
            return a.volts[k] + (b.volts[k] - a.volts[k]) * (t - a.time) / (b.time - a.time);
        }
    }
    return r.samples.back().volts[k];
}
/** @brief Independent re-statement of the fitted LED-strand formula (mA). */
double formula_ma(double u, double t, double u0, double s, double k, double p) {
    const double x = (u - u0 + 0.012 * (t - 20)) / s;
    const double soft = x > 30 ? x + std::log1p(std::exp(-x)) : std::log1p(std::exp(x));
    return k * std::pow(s * soft, p);
}
bool check(bool ok, const std::string &what) {
    std::cout << (ok ? "PASS " : "FAIL ") << what << '\n';
    return ok;
}

int pmos_mirror() {
    // A PMOS circuit must be the exact voltage mirror of the matching NMOS circuit.
    Waveform gate;
    gate.pulse = true;
    gate.delay = 10e-6;
    gate.rise = gate.fall = 100e-9;
    gate.on = 50e-6;
    gate.period = 100e-6;
    gate.high = 5;
    Waveform neg_gate = gate;
    neg_gate.high = -5;
    Circuit n, p;
    n.voltage("VDD", "0", {0, 12});
    n.voltage("G", "0", gate);
    n.resistor("VDD", "D", 100);
    n.nmos("D", "G", "0", 0.5, 2.0, 0.01);
    p.voltage("VDD", "0", {0, -12});
    p.voltage("G", "0", neg_gate);
    p.resistor("VDD", "D", 100);
    p.pmos("D", "G", "0", 0.5, -2.0, 0.01);
    Settings s;
    s.stop = 200e-6;
    s.max_step = 1e-7;
    s.method = Method::BackwardEuler;
    auto rn = n.run(s), rp = p.run(s);
    double worst = 0;
    for (double t : {5e-6, 20e-6, 40e-6, 70e-6, 115e-6, 150e-6, 190e-6})
        worst = std::max(worst, std::abs(at(rn, "D", t) + at(rp, "D", t)));
    std::cout << "max |V_nmos + V_pmos| = " << worst << " V\n";
    bool ok = check(worst < 1e-6, "PMOS mirrors NMOS within 1 uV");
    ok &= check(std::abs(at(rp, "D", 5e-6) + 12) < 1e-3, "PMOS off: drain at -12 V");
    ok &= check(at(rp, "D", 40e-6) > -1.0, "PMOS on: drain pulled towards source");
    try {
        Circuit bad;
        bad.pmos("D", "G", "S", -1, -2);
        ok &= check(false, "negative k rejected");
    } catch (const std::invalid_argument &) {
        ok &= check(true, "negative k rejected");
    }
    return ok ? 0 : 1;
}

int switch_basic() {
    // Control ramps 0 -> 5 V -> 0 V; switch closes above 3 V and opens below 2 V.
    Circuit c;
    c.voltage("VIN", "0", {0, 10});
    c.resistor("VIN", "OUT", 1000);
    Waveform ctrl;
    ctrl.trace = {{0, 0}, {100e-6, 5}, {200e-6, 0}};
    c.voltage("CTRL", "0", ctrl);
    c.voltage_switch("OUT", "0", "CTRL", "0", 1.0, 1e6, 2.5, 0.5);
    Settings s;
    s.stop = 200e-6;
    s.max_step = 1e-7;
    auto r = c.run(s);
    const double on_level = 10.0 * 1.0 / 1001.0, off_level = 10.0 * 1e6 / (1e6 + 1000);
    bool ok = true;
    ok &= check(std::abs(at(r, "OUT", 50e-6) - off_level) < 1e-6,
                "open at 2.5 V (rising, below 3 V)");
    ok &= check(std::abs(at(r, "OUT", 58e-6) - off_level) < 1e-6, "still open at 2.9 V");
    ok &= check(std::abs(at(r, "OUT", 62e-6) - on_level) < 1e-6, "closed at 3.1 V (Ron = 1 ohm)");
    ok &=
        check(std::abs(at(r, "OUT", 150e-6) - on_level) < 1e-6, "still closed at 2.5 V (falling)");
    ok &=
        check(std::abs(at(r, "OUT", 158e-6) - on_level) < 1e-6, "still closed at 2.1 V (falling)");
    ok &= check(std::abs(at(r, "OUT", 162e-6) - off_level) < 1e-6, "open at 1.9 V (Roff = 1 Mohm)");
    ok &= check(r.switch_events == 2, "exactly two state changes");
    try {
        Circuit bad;
        bad.voltage_switch("A", "0", "C", "0", 0, 1e6, 1);
        ok &= check(false, "Ron = 0 rejected");
    } catch (const std::invalid_argument &) {
        ok &= check(true, "Ron = 0 rejected");
    }
    return ok ? 0 : 1;
}

int switch_threshold() {
    // E1: control voltage exactly on the threshold, with jitter inside the hysteresis band.
    bool ok = true;
    Settings s;
    s.stop = 100e-6;
    s.max_step = 1e-7;
    for (bool initially_on : {false, true}) {
        Circuit c;
        c.voltage("VIN", "0", {0, 10});
        c.resistor("VIN", "OUT", 1000);
        Waveform ctrl;
        ctrl.trace = {{0, initially_on ? 5.0 : 0.0}, {10e-6, 2.5}};
        for (int i = 0; i < 80; ++i)
            ctrl.trace.push_back({11e-6 + i * 1e-6, 2.5 + (i % 2 ? 0.05 : -0.05)});
        c.voltage("CTRL", "0", ctrl);
        c.voltage_switch("OUT", "0", "CTRL", "0", 1.0, 1e6, 2.5, 0.1, initially_on);
        auto r = c.run(s);
        std::cout << "initially_on=" << initially_on << " switch_events=" << r.switch_events
                  << '\n';
        ok &= check(r.switch_events <= 1,
                    "jitter +-50 mV on threshold, hysteresis 100 mV: <= 1 change");
        ok &= check(r.switch_events == 0, "state kept (no change at all)");
    }
    {
        // Zero hysteresis, control held exactly at the threshold: strict comparison keeps state.
        Circuit c;
        c.voltage("VIN", "0", {0, 10});
        c.resistor("VIN", "OUT", 1000);
        c.voltage("CTRL", "0", {0, 2.5});
        c.voltage_switch("OUT", "0", "CTRL", "0", 1.0, 1e6, 2.5, 0.0);
        auto r = c.run(s);
        std::cout << "hysteresis=0 switch_events=" << r.switch_events << '\n';
        ok &=
            check(r.switch_events <= 1, "control exactly on threshold, no hysteresis: <= 1 change");
    }
    return ok ? 0 : 1;
}

int gate_driver() {
    const GateDriver drv{};
    bool ok = true;
    ok &= check(drv.r_high == 7 && drv.r_low == 7, "default output resistance 7 ohm");
    ok &= check(drv.delay_on == 20e-9 && drv.delay_off == 40e-9,
                "default delays 20 ns on, 40 ns off");
    ok &= check(drv.threshold == 1.6, "default input threshold 1.6 V");
    Waveform in;
    in.pulse = true;
    in.delay = 1e-6;
    in.rise = in.fall = 1e-9;
    in.on = 5e-6;
    in.period = 10e-6;
    in.high = 3.3;
    Settings s;
    s.stop = 10e-6;
    s.max_step = 1e-9;
    s.method = Method::BackwardEuler;
    const double rise_cross = 1e-6 + 1e-9 * 1.6 / 3.3;
    const double fall_cross = 1e-6 + 1e-9 + 5e-6 + 1e-9 * (3.3 - 1.6) / 3.3;
    {
        // Resistive load to GND equal to Rout: high level is VDD/2.
        Circuit c;
        c.voltage("VDD", "0", {0, 12});
        c.voltage("IN", "0", in);
        c.gate_driver("IN", "OUT", "VDD", "0");
        c.resistor("OUT", "0", 7);
        auto r = c.run(s);
        std::cout << std::setprecision(10)
                  << "OUT(rise_cross+19ns)=" << at(r, "OUT", rise_cross + 19e-9)
                  << " OUT(rise_cross+21ns)=" << at(r, "OUT", rise_cross + 21e-9)
                  << " OUT(3us)=" << at(r, "OUT", 3e-6) << '\n';
        ok &= check(std::abs(at(r, "OUT", 0.5e-6)) < 1e-6, "low before input edge");
        ok &= check(std::abs(at(r, "OUT", rise_cross + 19e-9)) < 1e-6,
                    "still low 19 ns after crossing");
        ok &= check(std::abs(at(r, "OUT", rise_cross + 21e-9) - 6) < 1e-3,
                    "high 21 ns after crossing");
        ok &= check(std::abs(at(r, "OUT", 3e-6) - 6) < 1e-6, "7 ohm into 7 ohm: 6 V");
        ok &= check(std::abs(at(r, "OUT", fall_cross + 39e-9) - 6) < 1e-3,
                    "still high 39 ns after falling crossing");
        ok &= check(std::abs(at(r, "OUT", fall_cross + 41e-9)) < 1e-3,
                    "low 41 ns after falling crossing");
        ok &= check(r.switch_events == 2, "two output transitions");
    }
    {
        // Pull-up load equal to Rout: low level is VDD/2, verifies sink resistance.
        Circuit c;
        c.voltage("VDD", "0", {0, 12});
        c.voltage("IN", "0", {0, 0});
        c.gate_driver("IN", "OUT", "VDD", "0");
        c.resistor("VDD", "OUT", 7);
        auto r = c.run(s);
        ok &=
            check(std::abs(at(r, "OUT", 5e-6) - 6) < 1e-6, "sink 7 ohm against 7 ohm pull-up: 6 V");
    }
    {
        // Gate load: 15 ohm + 2 nF, output reaches VDD with tau = (7+15)*2n.
        Circuit c;
        c.voltage("VDD", "0", {0, 12});
        c.voltage("IN", "0", in);
        c.gate_driver("IN", "OUT", "VDD", "0");
        c.resistor("OUT", "G", 15);
        c.capacitor("G", "0", 2e-9);
        auto r = c.run(s);
        const double t_on = rise_cross + 20e-9, tau = 22 * 2e-9;
        const double expected = 12 * (1 - std::exp(-tau / tau));
        std::cout << "G(t_on+tau)=" << at(r, "G", t_on + tau) << " expected " << expected << '\n';
        ok &=
            check(std::abs(at(r, "G", t_on + tau) - expected) < 0.1, "RC charge with Rout+15 ohm");
        ok &= check(std::abs(at(r, "G", 5e-6) - 12) < 1e-3, "gate settles at VDD");
    }
    try {
        Circuit bad;
        GateDriver m;
        m.r_high = 0;
        bad.gate_driver("I", "O", "V", "0", m);
        ok &= check(false, "Rout = 0 rejected");
    } catch (const std::invalid_argument &) {
        ok &= check(true, "Rout = 0 rejected");
    }
    return ok ? 0 : 1;
}

int led_curve() {
    struct Curve {
        const char *name;
        double u0, s, k, p;
    };
    // Two synthetic parameter sets of the fitted strand formula (U in V, I in mA).
    const Curve curves[] = {{"curve A", 15.0, 0.25, 200.0, 1.1}, {"curve B", 9.0, 0.15, 120.0, 1.2}};
    bool ok = true;
    double worst = 0;
    for (const auto &cv : curves) {
        LedCurve curve;
        curve.k = cv.k;
        curve.u0 = cv.u0;
        curve.s = cv.s;
        curve.p = cv.p;
        for (double du : {1.0, 5.0, 9.0}) {
            const double u = cv.u0 + du;
            const double expected = formula_ma(u, 20, cv.u0, cv.s, cv.k, cv.p) * 1e-3;
            // Strand fed through 0.1 ohm: the solver must place the operating point on the curve.
            Circuit c;
            const double rs = 0.1;
            const double i_guess = expected;
            c.voltage("VS", "0", {0, u + rs * i_guess});
            c.resistor("VS", "A", rs);
            c.led_strand("A", "0", curve, "LED");
            Settings s;
            s.stop = 1e-6;
            s.max_step = 1e-7;
            auto r = c.run(s);
            const auto &last = r.samples.back();
            const double ua = last.volts[column(r, "A")];
            const double kcl = (u + rs * i_guess - ua) / rs;
            const double model = last.amps.at(0);
            const double reference = formula_ma(ua, 20, cv.u0, cv.s, cv.k, cv.p) * 1e-3;
            const double dev_formula = std::abs(model / reference - 1);
            const double dev_kcl = std::abs(kcl / reference - 1);
            const double dev_direct = std::abs(curve.amps(u) / expected - 1);
            worst = std::max({worst, dev_formula, dev_kcl, dev_direct});
            std::cout << std::setprecision(8) << cv.name << " U=" << u << " V: formula "
                      << expected * 1e3 << " mA, model(U) " << curve.amps(u) * 1e3
                      << " mA; circuit U_A=" << ua << " V I_probe=" << model * 1e3
                      << " mA I_kcl=" << kcl * 1e3 << " mA ref(U_A)=" << reference * 1e3 << " mA\n";
        }
    }
    std::cout << "worst relative deviation " << worst * 100 << " %\n";
    ok &= check(worst <= 1e-3, "model vs formula <= 0.1 % at three voltages, 20 degC, two curves");
    // Temperature shift: at 50 degC the curve moves left by 30 K * 12 mV/K.
    LedCurve warm_curve;
    warm_curve.k = 200.0;
    warm_curve.u0 = 15.0;
    warm_curve.s = 0.25;
    warm_curve.p = 1.1;
    warm_curve.temp_c = 50;
    const double warm = warm_curve.amps(16) * 1e3;
    const double warm_ref = formula_ma(16, 50, 15.0, 0.25, 200.0, 1.1);
    std::cout << "curve A 16 V 50 degC: " << warm << " mA (formula " << warm_ref << ")\n";
    ok &= check(std::abs(warm / warm_ref - 1) < 1e-12, "temperature shift -12 mV/K");
    try {
        Circuit bad;
        LedCurve invalid;
        invalid.k = 0;
        bad.led_strand("A", "0", invalid);
        ok &= check(false, "invalid curve parameters rejected");
    } catch (const std::invalid_argument &) {
        ok &= check(true, "invalid curve parameters rejected");
    }
    return ok ? 0 : 1;
}
/** @brief Voltage switch closes at the interpolated threshold crossing, not one step late:
 *  control ramp 0 -> 1 V over 1 ms crosses 0.5137 V between two 10 us steps (fixed and
 *  adaptive stepping). */
int switch_timing() {
    bool ok = true;
    for (bool adaptive : {false, true}) {
        Circuit c;
        Waveform ctl;
        ctl.trace = {{0, 0}, {1e-3, 1}};
        c.voltage("ctl", "0", ctl);
        Waveform one;
        one.trace = {{0, 1}, {1e-3, 1}};
        c.voltage("vs", "0", one);
        c.voltage_switch("vs", "o", "ctl", "0", 1e-3, 1e9, 0.5137, 0.0);
        c.resistor("o", "0", 1e3);
        Settings s;
        s.stop = 1e-3;
        s.max_step = 10e-6;
        s.adaptive = adaptive;
        Result r = c.run(s);
        double t_on = -1;
        for (const auto &smp : r.samples)
            if (t_on < 0 && smp.volts.at(3) > 0.5)
                t_on = smp.time;
        const double late = t_on - 0.5137e-3;
        ok &= check(late >= 0 && late < 0.01e-6, std::string(adaptive ? "adaptive" : "fixed") +
                                                     ": switch closes at the crossing (delay " +
                                                     std::to_string(late * 1e9) +
                                                     " ns, before the fix one step = 10 us)");
        ok &= check(r.switch_events == 1, "exactly one switching event");
    }
    return ok ? 0 : 1;
}
/** @brief Time breakpoint (Circuit::breakpoint) for a behavioural source that jumps in time:
 *  1 mA steps on at t0 = 0.4567 ms (off the 10 us step grid) into 1 kOhm || 1 uF. Hand
 *  calculation: v = 1 V * (1 - exp(-(t - t0) / 1 ms)) after t0, 0 V before. Without the
 *  breakpoint the step control has to find the jump by rejecting steps. */
int time_breakpoint() {
    const double t0 = 0.4567e-3, tau = 1e-3;
    const auto worst_error = [&](bool with_bp, bool &landed, unsigned &rejected) {
        Circuit c;
        c.behavioural_current("0", "a", {Circuit::time_control},
                              [=](const std::vector<double> &v) { return v[0] > t0 ? 1e-3 : 0.0; });
        c.resistor("a", "0", 1e3);
        c.capacitor("a", "0", 1e-6);
        if (with_bp)
            c.breakpoint(t0);
        Settings s;
        s.stop = 3e-3;
        s.max_step = 10e-6;
        s.adaptive = true;
        s.rel_tol = 1e-4;
        s.abs_tol = 1e-9;
        Result r = c.run(s);
        rejected = r.rejected;
        double worst = 0;
        landed = false;
        for (const auto &smp : r.samples) {
            landed |= std::abs(smp.time - t0) < 1e-15;
            const double expect = smp.time > t0 ? 1 - std::exp(-(smp.time - t0) / tau) : 0.0;
            worst = std::max(worst, std::abs(at(r, "a", smp.time) - expect));
        }
        return worst;
    };
    bool landed_bp = false, landed_free = false;
    unsigned rej_bp = 0, rej_free = 0;
    const double with_bp = worst_error(true, landed_bp, rej_bp);
    const double without = worst_error(false, landed_free, rej_free);
    std::cout << "rejected steps: " << rej_bp << " with, " << rej_free << " without breakpoint\n";
    bool ok = check(landed_bp, "a sample lands exactly on the breakpoint");
    ok &= check(with_bp < 1e-4, "with breakpoint: worst error " + std::to_string(with_bp * 1e3) +
                                    " mV < 0.1 mV (without: " + std::to_string(without * 1e3) +
                                    " mV)");
    // The LTE control of this integrating node also finds the jump on its own, at the cost of
    // rejected steps; algebraic nodes (e.g. a switch node) are not covered by it.
    ok &= check(rej_bp < rej_free, "breakpoint avoids the rejected steps at the jump");
    Circuit bad;
    try {
        bad.breakpoint(std::numeric_limits<double>::quiet_NaN());
        ok &= check(false, "NaN breakpoint rejected");
    } catch (const std::invalid_argument &) {
        ok &= check(true, "NaN breakpoint rejected");
    }
    return ok ? 0 : 1;
}
/** @brief DC operating point (Settings::operating_point): capacitor open, inductor short,
 *  diode at its bias point already in the t = 0 sample; without the option the transient
 *  still starts from 0 V. Expected values are hand calculations. */
int operating_point() {
    const auto build = [](Circuit &c) {
        Waveform v;
        v.trace = {{0, 5}, {1e-3, 5}};
        c.voltage("vs", "0", v);
        c.resistor("vs", "a", 1e3); // 5 V -> diode over 1 kOhm, 1 uF across the diode
        c.diode("a", "0", 1e-14);
        c.capacitor("a", "0", 1e-6);
        c.resistor("vs", "l", 10); // inductor short at DC: I = 5 V / (10 + 10) Ohm
        c.inductor("l", "m", 1e-3);
        c.resistor("m", "0", 10);
        c.capacitor("vs", "f", 1e-9); // node only reached through a capacitor (needs GMIN)
        c.resistor("f", "g", 1e3);
        c.capacitor("g", "0", 1e-9);
    };
    Settings s;
    s.stop = 1e-4;
    s.max_step = 1e-6;
    s.adaptive = true;
    s.operating_point = true;
    Circuit c;
    build(c);
    Result r = c.run(s);
    bool ok = check(r.operating_point, "operating point found");
    // Diode: solve Is*(exp(v/Vt)-1) = (5 - v)/1k by bisection as the reference.
    double lo = 0, hi = 1;
    for (int i = 0; i < 200; ++i) {
        const double mid = (lo + hi) / 2;
        (1e-14 * (std::exp(mid / 0.02585) - 1) > (5 - mid) / 1e3 ? hi : lo) = mid;
    }
    const auto &s0 = r.samples.front();
    const auto v0 = [&](const std::string &name) {
        for (size_t i = 0; i < r.nodes.size(); ++i)
            if (r.nodes[i] == name)
                return s0.volts[i];
        throw std::runtime_error("missing node " + name);
    };
    ok &= check(std::abs(v0("a") - lo) < 1e-6, "diode bias at t = 0");
    ok &= check(std::abs(v0("m") - 2.5) < 1e-6, "inductor short: V(m) = 2.5 V at t = 0");
    ok &= check(std::abs(v0("a") - at(r, "a", 9e-5)) < 1e-6, "no start-up transient");
    ok &= check(std::isfinite(v0("f")) && std::abs(v0("f")) < 5.001, "capacitor-only node finite");
    Settings cold = s;
    cold.operating_point = false;
    Circuit c2;
    build(c2);
    Result r2 = c2.run(cold);
    ok &= check(!r2.operating_point && r2.samples.front().volts.at(1) == 0.0,
                "without option the transient starts from 0 V");
    return ok ? 0 : 1;
}
/** @brief Ebers-Moll NPN/PNP: saturation and active points against an independent solution
 *  (scipy fsolve of the transport equations, IS 1e-15, BF 100, BR 1, Vt 25.85 mV; see
 *  computed outside this repository). The old forward-only NPN gave
 *  V(c) = -27 V in saturation. */
int bjt_ebers_moll() {
    bool ok = true;
    const auto run_ce = [](bool npn, double vin) {
        Circuit c;
        Waveform s;
        s.trace = {{0, npn ? 5.0 : 5.0}, {1e-3, npn ? 5.0 : 5.0}};
        c.voltage("vcc", "0", s);
        Waveform w;
        w.trace = {{0, vin}, {1e-3, vin}};
        c.voltage("in", "0", w);
        c.resistor("in", "b", 10e3);
        if (npn) {
            c.resistor("vcc", "c", 1e3);
            c.npn("c", "b", "0", 1e-15, 100, 1);
        } else {
            c.pnp("c", "b", "vcc", 1e-15, 100, 1);
            c.resistor("c", "0", 1e3);
        }
        Settings st;
        st.stop = 1e-3;
        st.max_step = 1e-5;
        st.adaptive = true;
        st.abs_tol = 1e-9;
        Result r = c.run(st);
        return std::pair{at(r, "c", 0.9e-3), at(r, "b", 0.9e-3)};
    };
    const auto check = [&](const char *what, double v, double soll, double tol) {
        const bool pass = std::abs(v - soll) <= tol;
        std::cout << (pass ? "PASS " : "FAIL ") << what << " = " << v << " (soll " << soll << " +- "
                  << tol << ")\n";
        ok &= pass;
    };
    auto [c1, b1] = run_ce(true, 1.0);
    check("NPN aktiv V(c)", c1, 2.39056, 2e-3);
    check("NPN aktiv V(b)", b1, 0.739056, 1e-3);
    auto [c2, b2] = run_ce(true, 4.0);
    check("NPN Saettigung V(c)", c2, 0.0777729, 1e-3);
    check("NPN Saettigung V(b)", b2, 0.758147, 1e-3);
    auto [c3, b3] = run_ce(false, 4.0);
    check("PNP aktiv V(c)", c3, 2.60944, 2e-3);
    check("PNP aktiv V(b)", b3, 4.26094, 1e-3);
    auto [c4, b4] = run_ce(false, 0.0);
    check("PNP Saettigung V(c)", c4, 4.92929, 1e-3);
    check("PNP Saettigung V(b)", b4, 4.24091, 1e-3);
    return ok ? 0 : 1;
}
} // namespace

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    try {
        if (mode == "pmos")
            return pmos_mirror();
        if (mode == "switch")
            return switch_basic();
        if (mode == "switch_threshold")
            return switch_threshold();
        if (mode == "gate_driver")
            return gate_driver();
        if (mode == "led_curve")
            return led_curve();
        if (mode == "bjt")
            return bjt_ebers_moll();
        if (mode == "operating_point")
            return operating_point();
        if (mode == "switch_timing")
            return switch_timing();
        if (mode == "time_breakpoint")
            return time_breakpoint();
    } catch (const std::exception &e) {
        std::cout << "FAIL exception: " << e.what() << '\n';
        return 1;
    }
    std::cerr << "unknown mode\n";
    return 2;
}
