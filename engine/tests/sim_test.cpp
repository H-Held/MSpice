// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <cmath>
#include <iostream>
#include <mspice/sim/circuit.hpp>
#include <stdexcept>
/** @brief Run independent RC, RLC, diode and NMOS smoke/regression scenarios. */
int main() {
    using namespace mspice::sim;
    Circuit rc;
    rc.voltage("VIN", "0", {0, 5});
    rc.resistor("VIN", "OUT", 1000);
    rc.capacitor("OUT", "0", 1e-6);
    Settings s;
    s.stop = 0.005;
    s.max_step = 1e-5;
    s.method = Method::Trapezoid;
    std::cerr << "rc fixed\n";
    auto r = rc.run(s);
    if (r.accepted <= 490)
        return 6;
    double expected = 5 * (1 - std::exp(-5));
    if (std::abs(r.samples.back().volts[2] - expected) > 0.03)
        return 1;
    s.adaptive = true;
    std::cerr << "rc adaptive\n";
    auto ra = rc.run(s);
    if (std::abs(ra.samples.back().volts[2] - expected) > 0.03)
        return 2;
    s.adaptive = false;
    s.reuse_matrix = true;
    auto rf = rc.run(s);
    if (rf.matrix_reuses < 10 || rf.samples.size() != r.samples.size())
        return 7;
    for (size_t i = 0; i < r.samples.size(); ++i)
        if (std::abs(r.samples[i].volts[2] - rf.samples[i].volts[2]) > 1e-10)
            return 8;
    s.method = Method::BDF2;
    s.reuse_matrix = false;
    auto rb = rc.run(s);
    if (std::abs(rb.samples.back().volts[2] - expected) > 0.03)
        return 9;
    s.method = Method::Trapezoid;
    Circuit rlc;
    rlc.voltage("P", "0", {0, 1});
    rlc.resistor("P", "Q", 100);
    rlc.inductor("Q", "OUT", 0.01);
    rlc.capacitor("OUT", "0", 1e-6);
    s.adaptive = false;
    std::cerr << "rlc\n";
    auto rr = rlc.run(s);
    if (!std::isfinite(rr.samples.back().volts[3]))
        return 3;
    Circuit diode;
    diode.voltage("P", "0", {0, 5});
    diode.resistor("P", "Q", 1000);
    diode.diode("Q", "0");
    s.stop = 0.001;
    std::cerr << "diode\n";
    auto rd = diode.run(s);
    auto v = rd.samples.back().volts[2];
    if (!(v > 0.3 && v < 1.0)) {
        std::cerr << "diode voltage " << v << '\n';
        return 4;
    }
    Circuit mos;
    mos.voltage("SUP", "0", {0, 5});
    mos.voltage("GATE", "0", {0, 5});
    mos.resistor("SUP", "DRAIN", 100);
    mos.nmos("DRAIN", "GATE", "0", 0.02, 2.0);
    std::cerr << "mos\n";
    auto rm = mos.run(s);
    if (!(rm.samples.back().volts[3] > 0 && rm.samples.back().volts[3] < 5))
        return 5;
    std::cout << "rc=" << r.samples.back().volts[2] << " diode=" << v << "\n";
}
