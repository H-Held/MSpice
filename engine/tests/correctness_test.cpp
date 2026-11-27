// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <algorithm>
#include <cmath>
#include <iostream>
#include <mspice/sim/circuit.hpp>
#include <string>
using namespace mspice::sim;
// Independent resistor/MOS load-line oracle; bisection, not the solver Jacobian.
double drain(double gate) {
    double lo = 0, hi = 12;
    for (int i = 0; i < 70; ++i) {
        double vd = (lo + hi) / 2, ov = gate - 2;
        double id = ov <= 0 ? 0
                            : (vd < ov ? 0.5 * (ov * vd - 0.5 * vd * vd) : 0.25 * ov * ov) *
                                  (1 + 0.01 * vd);
        if (id > (12 - vd) / 100)
            hi = vd;
        else
            lo = vd;
    }
    return (lo + hi) / 2;
}
int main(int argc, char **argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "gnd") {
        Circuit c;
        for (const std::string name : {"0", "GND", "gnd", "Gnd", "gNd"})
            if (c.node(name) != 0)
                return 1;
        c.voltage("IN", "gnd", {0, 5});
        c.resistor("IN", "OuT", 1000);
        c.resistor("OuT", "GnD", 1000);
        Settings s;
        s.stop = 1e-5;
        s.max_step = 1e-6;
        auto r = c.run(s);
        if (r.nodes.size() != 3 || std::abs(r.samples.back().volts[2] - 2.5) > 1e-9)
            return 2;
        try {
            c.trace_driver("OUT", "gNd", {{0, 0}}, 100);
            return 3;
        } catch (const std::invalid_argument &) {
        }
    } else if (mode == "bdf2") {
        Circuit c;
        Waveform w;
        w.pulse = true;
        w.delay = 10.5e-6;
        w.high = 1;
        w.on = 1;
        w.period = 2;
        c.voltage("IN", "0", w);
        c.resistor("IN", "OUT", 1000);
        c.capacitor("OUT", "0", 1e-6);
        Settings s;
        s.stop = 100e-6;
        s.max_step = 10e-6;
        s.method = Method::BDF2;
        auto r = c.run(s);
        double previous = 0;
        bool shortened = false;
        for (size_t i = 1; i < r.samples.size(); ++i) {
            double dt = r.samples[i].time - r.samples[i - 1].time;
            if (previous > 0 && dt > 2 * previous * (1 + 1e-8))
                return 4;
            if (dt < 0.75e-6)
                shortened = true;
            if (!std::isfinite(r.samples[i].volts[2]))
                return 5;
            previous = dt;
        }
        if (!shortened || r.samples.back().volts[2] <= 0)
            return 6;
    } else if (mode == "nmos") {
        Circuit c;
        Waveform w;
        w.pulse = true;
        w.high = 5;
        w.delay = 10e-6;
        w.rise = w.fall = 100e-9;
        w.on = 50e-6;
        w.period = 100e-6;
        c.voltage("VDD", "0", {0, 12});
        c.voltage("G", "0", w);
        c.resistor("VDD", "D", 100);
        c.nmos("D", "G", "0", .5, 2, .01);
        Settings s;
        s.stop = 70e-6;
        s.max_step = 10e-9;
        s.method = Method::BackwardEuler;
        auto r = c.run(s);
        double worst = 0;
        for (size_t i = 2; i < r.samples.size(); ++i) {
            const auto &a = r.samples[i - 1];
            const auto &b = r.samples[i];
            for (double f : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                double t = a.time + f * (b.time - a.time);
                worst = std::max(
                    worst, std::abs(a.volts[3] + f * (b.volts[3] - a.volts[3]) - drain(w.at(t))));
            }
        }
        std::cout << "max oracle error V=" << worst << " samples=" << r.samples.size() << '\n';
        if (worst > .01 || r.rejected == 0)
            return 7;
    } else if (mode == "led") {
        // Independent DC diode equation at exponent > 40 (old continuation was wrong).
        Circuit dc;
        dc.current("0", "LED", 1e-3);
        dc.diode("LED", "0", 1e-25, .0517);
        Settings s;
        s.stop = 2e-8;
        s.max_step = 2e-8;
        auto r = dc.run(s);
        const double expected = .0517 * std::log1p(1e-3 / 1e-25);
        if (std::abs(r.samples.back().volts[1] - expected) > 1e-6)
            return 9;
        for (int channels : {1, 6}) {
            Circuit c;
            Waveform w;
            w.pulse = true;
            w.high = 5;
            w.delay = 1e-6;
            w.rise = w.fall = 100e-9;
            w.on = 1e-6;
            w.period = 4e-6;
            c.voltage("VIN", "0", {0, 24});
            c.voltage("G", "0", w);
            for (int j = 0; j < channels; ++j) {
                std::string prefix = "L" + std::to_string(j) + "_";
                c.resistor("VIN", prefix + "0", 150);
                for (int k = 0; k < 6; ++k)
                    c.diode(prefix + std::to_string(k), prefix + std::to_string(k + 1), 1e-25,
                            .0517);
                c.nmos(prefix + "6", "G", "0", .5, 2, .01);
            }
            s.stop = 7e-6;
            s.max_step = 20e-9;
            s.method = Method::BackwardEuler;
            auto wave = c.run(s);
            for (const auto &sample : wave.samples)
                for (double v : sample.volts)
                    if (!std::isfinite(v) || v < -1e-3 || v > 24.001)
                        return 10;
            if (std::abs(wave.samples.back().time - s.stop) > 1e-12)
                return 11;
        }
    } else
        return 8;
    return 0;
}
