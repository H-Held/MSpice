// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <algorithm>
#include <cmath>
#include <iostream>
#include <mspice/sim/circuit.hpp>
#include <limits>
#include <string>
#include <vector>

namespace {
/** @brief Index of a net in Result::nodes (volts column), -1 when absent. */
int net_index(const mspice::sim::Result &r, const std::string &name) {
    const auto it = std::find(r.nodes.begin(), r.nodes.end(), name);
    return it == r.nodes.end() ? -1 : static_cast<int>(it - r.nodes.begin());
}
/** @brief Potential of a net at the sample nearest to time t. */
double volts_at(const mspice::sim::Result &r, const std::string &name, double t) {
    const mspice::sim::Sample *best = &r.samples.front();
    for (const auto &x : r.samples)
        if (std::abs(x.time - t) < std::abs(best->time - t))
            best = &x;
    return best->volts[static_cast<std::size_t>(net_index(r, name))];
}
/** @brief True when |a - b| <= abs + rel*|b|. */
bool near(double a, double b, double rel, double abs_tol = 0) {
    return std::abs(a - b) <= abs_tol + rel * std::abs(b);
}
/** @brief Zener operating point: reverse voltage at 10 V over r ohm for given diode parameters. */
double zener_volts(const mspice::sim::DiodeParams &m, double supply, double r) {
    using namespace mspice::sim;
    Circuit c;
    c.voltage("P", "0", {0, -supply});
    c.resistor("P", "A", r);
    c.diode("A", "0", m);
    Settings s;
    s.stop = 1e-5;
    s.max_step = 1e-6;
    const Result res = c.run(s);
    return -res.samples.back().volts[static_cast<std::size_t>(net_index(res, "A"))];
}
/** @brief SPICE depletion charge of a junction with zero-bias capacitance cj0 (reference for
 *  the junction-charge tests, written independently of the engine). */
double depletion_charge(double v, double cj0, double vj, double m, double fc) {
    if (v < fc * vj)
        return vj * cj0 * (1 - std::pow(1 - v / vj, 1 - m)) / (1 - m);
    const double f1 = vj * (1 - std::pow(1 - fc, 1 - m)) / (1 - m);
    const double f2 = std::pow(1 - fc, 1 + m), f3 = 1 - fc * (1 + m);
    const double d = fc * vj;
    return cj0 * f1 + cj0 / f2 * (f3 * (v - d) + m / (2 * vj) * (v * v - d * d));
}
/** @brief Charge delivered by probe current k between t0 and t1 (trapezoid quadrature). */
double probe_charge(const mspice::sim::Result &r, std::size_t k, double t0, double t1) {
    double q = 0;
    for (std::size_t i = 1; i < r.samples.size(); ++i) {
        const auto &a = r.samples[i - 1], &b = r.samples[i];
        if (a.time >= t0 - 1e-15 && b.time <= t1 + 1e-15)
            q += 0.5 * (a.amps[k] + b.amps[k]) * (b.time - a.time);
    }
    return q;
}
} // namespace

/** @brief Regressions from a solver review (BJT clamp, GMIN, BE restart) and the
 *  engine additions for the SPICE import (current-source waveforms, PULSE count, SIN, diode breakdown, BJT Vt,
 *  initial voltages, branch-current probes) and the junction and diffusion charges. */
int main() {
    using namespace mspice::sim;
    // 1) NPN: 5 V over 1 k into base, collector 1 k to 5 V. Real vbe stays < 0.9 V.
    //    With the old clamp the derivative was 0 above 0.905 V and Newton parked vbe at volts.
    {
        Circuit c;
        c.voltage("VCC", "0", {0, 5});
        c.resistor("VCC", "B", 1000);
        c.resistor("VCC", "C", 1000);
        c.npn("C", "B", "0");
        Settings s;
        s.stop = 1e-5;
        s.max_step = 1e-6;
        auto r = c.run(s);
        double vb = r.samples.back().volts[2];
        if (!(vb > 0.5 && vb < 0.9)) {
            std::cerr << "npn vbe=" << vb << "\n";
            return 1;
        }
    }
    // 2) GMIN: node only reached through an off NMOS must not make the matrix singular.
    {
        Circuit c;
        c.voltage("VDD", "0", {0, 12});
        c.voltage("G", "0", {0, 0});
        c.nmos("VDD", "G", "S", 0.1, 2.0);
        c.capacitor("S", "X", 1e-9);
        c.resistor("X", "0", 1000);
        Settings s;
        s.stop = 1e-6;
        s.max_step = 1e-7;
        try {
            c.run(s);
        } catch (const std::exception &e) {
            std::cerr << "gmin: " << e.what() << "\n";
            return 2;
        }
    }
    // 3) Trapezoid after a source step: series L-C driven by an ideal 0->1 V step via R.
    //    Without a BE restart at the breakpoint trap carries the old cap current and the
    //    capacitor voltage alternates +-; with restart the first sample after the edge is smooth.
    {
        Circuit c;
        Waveform w;
        w.pulse = true;
        w.low = 0;
        w.high = 1;
        w.delay = 1e-6;
        w.rise = 0;
        w.on = 1;
        w.fall = 0;
        w.period = 2;
        c.voltage("IN", "0", w);
        c.resistor("IN", "OUT", 1e-3);
        c.capacitor("OUT", "0", 1e-9);
        Settings s;
        s.stop = 3e-6;
        s.max_step = 1e-7;
        s.method = Method::Trapezoid;
        auto r = c.run(s);
        double worst = 0;
        for (const auto &x : r.samples)
            if (x.time > 1.05e-6)
                worst = std::max(worst, std::abs(x.volts[2] - 1));
        if (worst > 1e-3) {
            std::cerr << "trap ringing " << worst << "\n";
            return 3;
        }
    }

    // ---- Engine additions for the SPICE import (all opt-in) ----------------------------------
    // 4) Current source with PWL: v(N) = I(t) * R on a resistor (b side receives the current).
    {
        Circuit c;
        Waveform w;
        w.trace = {{0, 0}, {1e-3, 1e-3}, {2e-3, 0.5e-3}};
        c.current("0", "N", w);
        c.resistor("N", "0", 1000);
        Settings s;
        s.stop = 2e-3;
        s.max_step = 1e-4;
        const Result r = c.run(s);
        const int n = net_index(r, "N");
        double worst = 0;
        for (const auto &x : r.samples)
            worst = std::max(worst, std::abs(x.volts[n] - w.at(x.time) * 1000));
        if (worst > 1e-9 || !near(r.samples.back().volts[n], 0.5, 1e-9)) {
            std::cerr << "current PWL " << worst << "\n";
            return 4;
        }
    }
    // 5) PULSE count NP: two periods, then the source rests at low; zero count repeats forever.
    {
        Waveform w;
        w.pulse = true;
        w.low = 0;
        w.high = 1e-3;
        w.on = 0.5e-3;
        w.period = 1e-3;
        w.cycles = 2;
        Circuit c;
        c.current("0", "N", w);
        c.resistor("N", "0", 1000);
        Settings s;
        s.stop = 4e-3;
        s.max_step = 1e-4;
        const Result r = c.run(s);
        const double expected[] = {1, 0, 1, 0, 0, 0}; // at 0.3, 0.8, 1.3, 1.8, 2.3, 3.3 ms
        const double times[] = {0.3e-3, 0.8e-3, 1.3e-3, 1.8e-3, 2.3e-3, 3.3e-3};
        for (int k = 0; k < 6; ++k)
            if (!near(volts_at(r, "N", times[k]), expected[k], 1e-9, 1e-9)) {
                std::cerr << "pulse count at " << times[k] << "\n";
                return 5;
            }
        Waveform inf = w;
        inf.cycles = 0;
        if (!(w.next_event(1.9e-3) > 1.99e-3 && w.next_event(1.9e-3) < 2.01e-3) ||
            !std::isinf(w.next_event(2.0e-3)) || !std::isinf(w.next_event(3.1e-3)) ||
            std::isinf(inf.next_event(3.1e-3)) || inf.at(10.3e-3) != 1e-3 || w.at(10.3e-3) != 0) {
            std::cerr << "pulse count events\n";
            return 5;
        }
    }
    // 6) SIN source (offset, amplitude, frequency, delay, damping, phase) as voltage and current.
    {
        Waveform w;
        w.sine = true;
        w.low = 1;
        w.high = 2;
        w.freq = 1e3;
        w.delay = 1e-4;
        w.damping = 500;
        w.phase = 30;
        Circuit c;
        c.voltage("S", "0", w);
        c.resistor("S", "0", 1000);
        Waveform iw = w;
        iw.low = 0;
        iw.high = 1e-3;
        c.current("0", "T", iw);
        c.resistor("T", "0", 1000);
        Settings s;
        s.stop = 2e-3;
        s.max_step = 1e-5;
        const Result r = c.run(s);
        const double pi2 = 6.283185307179586;
        double worst = 0;
        for (const auto &x : r.samples) {
            const double t = x.time;
            if (t == 0)
                continue; // no operating point: the run starts from 0 V
            const double want_v = t <= 1e-4
                                      ? 1 + 2 * std::sin(pi2 * 30 / 360)
                                      : 1 + 2 * std::sin(pi2 * (1e3 * (t - 1e-4) + 30.0 / 360)) *
                                                std::exp(-(t - 1e-4) * 500);
            const double want_t = t <= 1e-4 ? 1 * std::sin(pi2 * 30 / 360)
                                            : std::sin(pi2 * (1e3 * (t - 1e-4) + 30.0 / 360)) *
                                                  std::exp(-(t - 1e-4) * 500);
            worst = std::max(worst, std::abs(x.volts[net_index(r, "S")] - want_v));
            worst = std::max(worst, std::abs(x.volts[net_index(r, "T")] - want_t));
        }
        if (worst > 1e-9) {
            std::cerr << "sine " << worst << "\n";
            return 6;
        }
        Waveform bad = w;
        bad.pulse = true;
        Circuit d;
        bool threw = false;
        try {
            d.voltage("S", "0", bad);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        Waveform neg;
        neg.cycles = 2; // pulse count without a pulse
        try {
            d.current("0", "T", neg);
            threw = false;
        } catch (const std::invalid_argument &) {
        }
        if (!threw) {
            std::cerr << "waveform validation\n";
            return 6;
        }
    }
    // 7) Diode breakdown: the Zener voltage at the current IBV is BV (ngspice knee), the slope
    //    is NBV*Vt per decade-e, forward and weak reverse operation are untouched bit for bit.
    {
        DiodeParams m;
        m.is = 1e-14;
        m.bv = 5.1;
        m.ibv = 5e-3;
        const double r_load = (10 - 5.1) / 5e-3;
        const double vz = zener_volts(m, 10, r_load);
        if (!near(vz, 5.1, 0, 5e-3)) {
            std::cerr << "zener at ibv " << vz << "\n";
            return 7;
        }
        DiodeParams m2;
        m2.is = 1e-14;
        m2.bv = 12;
        m2.ibv = 1e-3;
        m2.nbv = 2;
        const double vz2 = zener_volts(m2, 20, 600); // about 13.3 mA
        double i2 = 0;
        for (int k = 0; k < 50; ++k)
            i2 = (20 - (k ? 12 + 2 * 0.02585 * std::log(i2 / 1e-3) : 12)) / 600;
        const double vz2_ref = 12 + 2 * 0.02585 * std::log(i2 / 1e-3);
        if (!near(vz2, vz2_ref, 0, 3e-3)) {
            std::cerr << "zener nbv " << vz2 << " vs " << vz2_ref << "\n";
            return 7;
        }
        // Forward, and reverse above -bv: identical to the plain diode.
        for (const double supply : {5.0, -3.0}) {
            Circuit a, b;
            for (Circuit *c : {&a, &b}) {
                c->voltage("P", "0", {0, supply});
                c->resistor("P", "A", 1000);
            }
            a.diode("A", "0", 1e-14, 0.02585);
            b.diode("A", "0", m);
            Settings s;
            s.stop = 1e-5;
            s.max_step = 1e-6;
            const Result ra = a.run(s), rb = b.run(s);
            if (ra.samples.back().volts != rb.samples.back().volts ||
                ra.newton_iterations != rb.newton_iterations) {
                std::cerr << "breakdown changes normal region\n";
                return 7;
            }
        }
        bool threw = false;
        try {
            DiodeParams bad = m;
            bad.ibv = 0;
            Circuit c;
            c.diode("A", "0", bad);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        if (!threw)
            return 7;
    }
    // 8) BJT Vt: Ic = is*(exp(vbe/vt)-1) + is*(1+1/br) at fixed vbe; probe gives -Ic of the
    //    collector source (ngspice sign: current into the + terminal).
    {
        for (const double vt : {0.02585, 0.0517}) {
            Circuit c;
            c.voltage("B", "0", {0, 0.65});
            c.voltage("C", "0", {0, 5}, "vc");
            c.npn("C", "B", "0", 1e-12, 100, 2, vt);
            Settings s;
            s.stop = 1e-5;
            s.max_step = 1e-6;
            const Result r = c.run(s);
            const double ic = 1e-12 * (std::exp(0.65 / vt) - 1) + 1e-12 * (1 + 1.0 / 2);
            if (r.currents.size() != 1 || r.currents[0] != "vc" ||
                !near(-r.samples.back().amps[0], ic, 1e-3)) {
                std::cerr << "npn vt " << vt << " ic=" << -r.samples.back().amps[0] << " want "
                          << ic << "\n";
                return 8;
            }
        }
    }
    // 9) Branch-current probes of voltage source and inductor: RL charging, i = V/R (1-e^-t/tau).
    {
        Circuit c;
        c.voltage("V", "0", {0, 5}, "v1");
        c.resistor("V", "X", 10);
        c.inductor("X", "0", 1e-3, "l1");
        Settings s;
        s.stop = 1e-3;
        s.max_step = 1e-6;
        const Result r = c.run(s);
        const double want = 0.5 * (1 - std::exp(-10.0));
        if (r.currents.size() != 2 || r.currents[0] != "v1" || r.currents[1] != "l1" ||
            !near(r.samples.back().amps[1], want, 1e-3) ||
            !near(r.samples.back().amps[0], -want, 1e-3) || r.samples.front().amps[1] != 0) {
            std::cerr << "probe v1=" << r.samples.back().amps[0]
                      << " l1=" << r.samples.back().amps[1] << " want " << want << "\n";
            return 9;
        }
        // Naming an ammeter afterwards.
        Circuit d;
        d.voltage("V", "0", {0, 5});
        const std::string meter = d.ammeter("V", "Y");
        d.resistor("Y", "0", 100);
        d.probe_current(meter, "im");
        const Result rd = d.run(s);
        if (rd.currents.size() != 1 || !near(rd.samples.back().amps[0], 0.05, 1e-6)) {
            std::cerr << "ammeter probe\n";
            return 9;
        }
    }
    // 10) Initial voltage with uic: C discharging through R from the .ic value.
    {
        Circuit c;
        c.capacitor("N", "0", 1e-6);
        c.resistor("N", "0", 1000);
        c.initial_voltage("N", 2.0);
        Settings s;
        s.stop = 1e-3;
        s.max_step = 1e-5;
        s.adaptive = false;
        const Result r = c.run(s);
        const int n = net_index(r, "N");
        if (r.samples.front().volts[n] != 2.0 ||
            !near(r.samples.back().volts[n], 2 * std::exp(-1.0), 2e-3)) {
            std::cerr << "initial voltage " << r.samples.back().volts[n] << "\n";
            return 10;
        }
        bool threw = false;
        try {
            c.initial_voltage("GND", 1);
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        try {
            c.initial_voltage("N", std::numeric_limits<double>::quiet_NaN());
            threw = false;
        } catch (const std::invalid_argument &) {
        }
        if (!threw)
            return 10;
    }
    // 11) Depletion charge: a source ramps a diode 0 -> -5 V -> 0 and 0 -> +0.6 V (above
    //     fc*vj, linearised branch). The source delivers exactly q(v1) - q(v0) and nothing net
    //     after returning to 0 V, for every integration method.
    for (const Method method : {Method::Trapezoid, Method::BackwardEuler, Method::BDF2}) {
        for (const double peak : {-5.0, 0.6}) {
            Circuit c;
            Waveform w;
            w.trace = {{0, 0}, {1e-6, 0}, {2e-6, peak}, {3e-6, peak}, {4e-6, 0}, {5e-6, 0}};
            c.voltage("A", "0", w, "vs");
            DiodeParams d;
            d.is = 1e-30; // keep the DC current negligible against the charge
            d.cjo = 10e-12;
            d.vj = 0.7;
            d.m = 0.4;
            d.fc = 0.5;
            c.diode("A", "0", d);
            Settings s;
            s.stop = 5e-6;
            s.max_step = 5e-9;
            s.method = method;
            const Result r = c.run(s);
            // Branch current of "vs" flows + -> -, i.e. minus the diode current.
            const double q1 = -probe_charge(r, 0, 1e-6, 3e-6);
            const double want = depletion_charge(peak, 10e-12, 0.7, 0.4, 0.5);
            const double back = -probe_charge(r, 0, 0, 5e-6);
            if (!near(q1, want, 2e-3) || std::abs(back) > 1e-6 * std::abs(want)) {
                std::cerr << "cjo charge method " << static_cast<int>(method) << " peak " << peak
                          << ": q=" << q1 << " want " << want << " net " << back << "\n";
                return 11;
            }
        }
    }
    // 12) Diffusion charge: reverse recovery of a diode with tt = 1 us. Forward 10 V / 1 k
    //     (operating point), then -50 V. Charge control (q = tt*Id, i = Id + dq/dt) gives the
    //     storage time ts = tt*ln(1 + If/Ir) until the junction voltage crosses 0.
    {
        Circuit c;
        Waveform w;
        w.pulse = true;
        w.low = 10;
        w.high = -50;
        w.delay = 100e-9;
        w.rise = 1e-9;
        w.on = 1e-3;
        w.fall = 1e-9;
        w.period = 2e-3;
        c.voltage("P", "0", w);
        c.resistor("P", "A", 1000);
        DiodeParams d;
        d.is = 1e-14;
        d.tt = 1e-6;
        c.diode("A", "0", d);
        Settings s;
        s.stop = 400e-9;
        s.max_step = 0.5e-9;
        s.operating_point = true;
        const Result r = c.run(s);
        const int a = net_index(r, "A");
        const double vf = r.samples.front().volts[a];
        const double i_f = (10 - vf) / 1000, i_r = (50 + 0.5 * vf) / 1000;
        double cross = -1;
        for (std::size_t i = 1; i < r.samples.size(); ++i)
            if (r.samples[i].time > 100e-9 && r.samples[i - 1].volts[a] > 0 &&
                r.samples[i].volts[a] <= 0) {
                const auto &p0 = r.samples[i - 1], &p1 = r.samples[i];
                cross = p0.time + (p1.time - p0.time) * p0.volts[a] / (p0.volts[a] - p1.volts[a]);
                break;
            }
        const double ts = 1e-6 * std::log(1 + i_f / i_r);
        if (!r.operating_point || !near(cross - 100.5e-9, ts, 0.02)) {
            std::cerr << "tt recovery ts=" << cross - 100.5e-9 << " want " << ts << "\n";
            return 12;
        }
    }
    // 13) BJT: BjtParams without charges equals the scalar overload bit for bit; junction
    //     charges of a reverse-biased NPN (base 0 -> -3 V) and PNP (base 0 -> +3 V) equal
    //     qbe + qbc of the SPICE depletion law.
    {
        const auto inverter = [](bool with_params) {
            Circuit c;
            Waveform w;
            w.pulse = true;
            w.low = 0;
            w.high = 5;
            w.delay = 1e-6;
            w.rise = 10e-9;
            w.on = 2e-6;
            w.fall = 10e-9;
            w.period = 5e-6;
            c.voltage("IN", "0", w);
            c.voltage("VCC", "0", {0, 5});
            c.resistor("IN", "B", 10000);
            c.resistor("VCC", "C", 1000);
            c.capacitor("C", "0", 10e-12);
            if (with_params) {
                BjtParams m;
                m.is = 1e-14;
                m.bf = 200;
                m.br = 2;
                c.npn("C", "B", "0", m);
            } else {
                c.npn("C", "B", "0", 1e-14, 200, 2);
            }
            Settings s;
            s.stop = 5e-6;
            s.max_step = 5e-9;
            s.adaptive = true;
            return c.run(s);
        };
        const Result r0 = inverter(false), r1 = inverter(true);
        if (r0.samples.size() != r1.samples.size())
            return 13;
        for (std::size_t i = 0; i < r0.samples.size(); ++i)
            if (r0.samples[i].time != r1.samples[i].time ||
                r0.samples[i].volts != r1.samples[i].volts) {
                std::cerr << "BjtParams without charges differs at sample " << i << "\n";
                return 13;
            }
        for (const bool npn_type : {true, false}) {
            Circuit c;
            const double sgn = npn_type ? 1 : -1;
            Waveform w;
            w.trace = {{0, 0}, {1e-6, 0}, {2e-6, -3 * sgn}, {3e-6, -3 * sgn}};
            c.voltage("B", "0", w, "vb");
            BjtParams m;
            m.is = 1e-30;
            m.cje = 4e-12;
            m.vje = 0.8;
            m.mje = 0.35;
            m.cjc = 2e-12;
            m.vjc = 0.6;
            m.mjc = 0.3;
            if (npn_type)
                c.npn("0", "B", "0", m);
            else
                c.pnp("0", "B", "0", m);
            Settings s;
            s.stop = 3e-6;
            s.max_step = 5e-9;
            const Result r = c.run(s);
            const double q = -probe_charge(r, 0, 0, 3e-6);
            const double want = sgn * (depletion_charge(-3, 4e-12, 0.8, 0.35, 0.5) +
                                       depletion_charge(-3, 2e-12, 0.6, 0.3, 0.5));
            if (!near(q, want, 2e-3)) {
                std::cerr << (npn_type ? "npn" : "pnp") << " junction charge " << q << " want "
                          << want << "\n";
                return 13;
            }
        }
    }
    // 14) Invalid charge parameters are rejected.
    {
        int rejected = 0;
        for (int k = 0; k < 4; ++k) {
            DiodeParams d;
            d.cjo = 1e-12;
            if (k == 0)
                d.m = 1;
            else if (k == 1)
                d.fc = 1;
            else if (k == 2)
                d.vj = 0;
            else
                d.tt = -1;
            Circuit c;
            try {
                c.diode("A", "0", d);
            } catch (const std::invalid_argument &) {
                ++rejected;
            }
        }
        BjtParams m;
        m.cjc = std::numeric_limits<double>::quiet_NaN();
        Circuit c;
        try {
            c.npn("C", "B", "0", m);
        } catch (const std::invalid_argument &) {
            ++rejected;
        }
        if (rejected != 5)
            return 14;
    }
    return 0;
}
