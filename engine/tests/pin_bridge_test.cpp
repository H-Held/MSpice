// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <cmath>
#include <mspice/firmware/pin_bridge.hpp>
#include <stdexcept>
/** @brief Verify edge ordering, PWM timing, hysteresis and deterministic jitter. */
int main() {
    using namespace mspice::firmware;
    PinBridge p;
    p.configure_output(5, {0, 3.3, 40, 1e-6, 1e-6});
    for (int i = 0; i < 4; ++i) {
        p.add_edge(i * 1e-3, 5, true);
        p.add_edge(i * 1e-3 + 0.3e-3, 5, false);
    }
    p.finalize();
    if (!(p.events().size() == 8))
        return 1;
    if (p.logic_at(5, 0.00031))
        return 1;
    if (!p.logic_at(5, 0.00101))
        return 1;
    if (!(std::abs(p.voltage_at(5, 0.0010005) - 1.65) < 0.001))
        return 1;
    if (p.sample_input(4, 0.5))
        return 1;
    if (!p.sample_input(4, 2.1, {}, 0.001))
        return 1;
    if (!p.sample_input(4, 1.4))
        return 1;
    if (p.sample_input(4, 0.3, {}, 0.002))
        return 1;
    if (p.input_events().size() != 2)
        return 2;
    if (!p.input_events()[0].high || p.input_events()[1].high)
        return 3;
    if (std::abs(p.input_events()[1].time - 0.002) > 1e-12)
        return 4;
    PinBridge overlap;
    overlap.configure_output(9, {0, 3.3, 40, 1e-6, 1e-6});
    overlap.add_edge(0, 9, true);
    overlap.add_edge(0.5e-6, 9, false);
    overlap.finalize();
    if (std::abs(overlap.voltage_at(9, 0.5e-6) - 1.65) > 1e-9)
        return 5;
    if (std::abs(overlap.voltage_at(9, 0.75e-6) - 1.2375) > 1e-9)
        return 6;
    PinBridge a, b;
    a.jitter(1e-6, 42);
    b.jitter(1e-6, 42);
    for (int i = 0; i < 20; ++i) {
        a.add_edge(i * 1e-3, 3, true);
        b.add_edge(i * 1e-3, 3, true);
    }
    a.finalize();
    b.finalize();
    for (size_t i = 0; i < a.events().size(); ++i)
        if (!(a.events()[i].time == b.events()[i].time))
            return 1;
}
