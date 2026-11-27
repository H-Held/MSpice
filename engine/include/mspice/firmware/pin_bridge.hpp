// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace mspice::firmware {
/** @brief A timestamped logic output change, independent of any firmware or emulator. */
struct Edge {
    double time{};
    int pin{};
    bool high{};
};
/** @brief An observed interrupt-relevant Schmitt input transition. */
struct InputEdge {
    double time{};
    int pin{};
    bool high{};
};
/** @brief Electrical properties of a digital output stage. */
struct OutputCharacteristics {
    double low_v{0}, high_v{3.3}, ohms{40}, rise_s{5e-9}, fall_s{5e-9};
};
/** @brief Schmitt-trigger receiver configuration with hysteresis. */
struct InputCharacteristics {
    double low_v{0.8}, high_v{2.0};
};
/** @brief Deterministic pin event bridge (digital edges with jitter and Schmitt inputs);
 *  not a microcontroller emulator. */
class PinBridge {
  public:
    /** @brief Set a source's output-stage properties. */
    void configure_output(int pin, OutputCharacteristics value);
    /** @brief Add an output edge with an optional bounded deterministic time offset. */
    void add_edge(double time, int pin, bool high);
    /** @brief Sort and validate event ordering; call before replay. */
    void finalize();
    /** @brief Compute the logic level at time t (initially low). */
    bool logic_at(int pin, double t) const;
    /** @brief Compute nominal driven voltage including transition time. */
    double voltage_at(int pin, double t) const;
    /** @brief Return first input event time strictly after t, or infinity. */
    double next_edge(double t) const;
    /** @brief Sample external analog voltage at pin with Schmitt hysteresis. */
    bool sample_input(int pin, double volts, InputCharacteristics threshold = {},
                      double time = 0.0);
    /** @brief Return input transitions that an interrupt controller could consume. */
    const std::vector<InputEdge> &input_events() const;
    /** @brief Enable bounded deterministic timing variation with reproducible seed. */
    void jitter(double amplitude_s, std::uint64_t seed);
    /** @brief Return all scheduled events for serialization and testing. */
    const std::vector<Edge> &events() const;

  private:
    std::vector<Edge> events_;
    std::vector<InputEdge> observed_;
    std::map<int, OutputCharacteristics> output_;
    std::map<int, bool> input_state_;
    double jitter_s_{};
    std::uint64_t rng_{1};
};
} // namespace mspice::firmware
