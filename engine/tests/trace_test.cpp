// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <cmath>
#include <iostream>
#include <mspice/sim/circuit.hpp>
#include <stdexcept>
/** @brief Verify physical slew, precise breakpoints and finite-impedance analog stimulation. */
int main() {
    using namespace mspice::sim;
    Waveform trace;
    trace.trace = {{0, 0}, {0.0002, 0}, {0.0002001, 3.3}, {0.0006, 3.3}, {0.0006001, 0}};
    if (trace.at(0.00020005) < 1.649 || trace.at(0.00020005) > 1.651)
        return 1;
    if (std::abs(trace.next_event(0.0002) - 0.0002001) > 1e-12)
        return 2;
    Circuit c;
    c.trace_driver("OUT", "DRIVE", trace.trace, 40);
    c.resistor("OUT", "0", 1000);
    c.capacitor("OUT", "0", 1e-8);
    Settings s;
    s.stop = 0.001;
    s.max_step = 1e-5;
    s.adaptive = false;
    auto r = c.run(s);
    if (r.samples.back().volts[1] > 0.1)
        return 3;
    bool found = false;
    for (const auto &v : r.samples)
        if (std::abs(v.time - 0.0002) < 1e-12)
            found = true;
    if (!found)
        return 4;
    bool rejected = false;
    try {
        c.trace_driver("OUT", "BAD", {{0, 1}, {-1, 0}}, 40);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    if (!rejected)
        return 5;
    std::cout << "trace samples=" << r.samples.size() << " backend=" << r.backend << '\n';
}
