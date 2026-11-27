// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <algorithm>
#include <cmath>
#include <mspice/firmware/pin_bridge.hpp>
#include <limits>
#include <stdexcept>
namespace mspice::firmware {
void PinBridge::configure_output(int pin, OutputCharacteristics value) {
    if (pin < 0 || !(value.ohms > 0) || value.rise_s < 0 || value.fall_s < 0)
        throw std::invalid_argument("invalid output characteristics");
    output_[pin] = value;
}
void PinBridge::add_edge(double time, int pin, bool high) {
    if (!std::isfinite(time) || time < 0 || pin < 0)
        throw std::invalid_argument("invalid pin edge");
    if (jitter_s_ > 0) {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        double unit = (static_cast<double>(rng_ >> 11) / 9007199254740992.0) * 2.0 - 1.0;
        time = std::max(0.0, time + unit * jitter_s_);
    }
    events_.push_back({time, pin, high});
}
void PinBridge::finalize() {
    std::stable_sort(events_.begin(), events_.end(),
                     [](const Edge &a, const Edge &b) { return a.time < b.time; });
}
bool PinBridge::logic_at(int pin, double t) const {
    bool logic = false;
    for (const auto &e : events_) {
        if (e.time > t)
            break;
        if (e.pin == pin)
            logic = e.high;
    }
    return logic;
}
double PinBridge::voltage_at(int pin, double t) const {
    OutputCharacteristics d{};
    if (auto it = output_.find(pin); it != output_.end())
        d = it->second;
    double start_time = 0.0, start_v = d.low_v, target_v = d.low_v, duration = 0.0;
    /** @brief Interpolate the active output-driver transition. */
    auto level_at = [&](double now) {
        if (duration <= 0)
            return target_v;
        const double fraction = std::clamp((now - start_time) / duration, 0.0, 1.0);
        return start_v + (target_v - start_v) * fraction;
    };
    for (const auto &edge : events_) {
        if (edge.time > t)
            break;
        if (edge.pin != pin)
            continue;
        const double before = level_at(edge.time);
        start_time = edge.time;
        start_v = before;
        target_v = edge.high ? d.high_v : d.low_v;
        duration = edge.high ? d.rise_s : d.fall_s;
    }
    return level_at(t);
}
double PinBridge::next_edge(double t) const {
    for (const auto &e : events_)
        if (e.time > t)
            return e.time;
    return std::numeric_limits<double>::infinity();
}
bool PinBridge::sample_input(int pin, double volts, InputCharacteristics th, double time) {
    if (!(th.low_v < th.high_v))
        throw std::invalid_argument("invalid Schmitt thresholds");
    if (!std::isfinite(volts) || !std::isfinite(time) || time < 0)
        throw std::invalid_argument("invalid sample voltage or time");
    bool prior = input_state_[pin];
    bool current = prior;
    if (volts >= th.high_v)
        current = true;
    if (volts <= th.low_v)
        current = false;
    input_state_[pin] = current;
    if (current != prior)
        observed_.push_back({time, pin, current});
    return current;
}
void PinBridge::jitter(double amount, std::uint64_t seed) {
    if (amount < 0 || !std::isfinite(amount))
        throw std::invalid_argument("invalid jitter");
    jitter_s_ = amount;
    rng_ = seed ? seed : 1;
}
const std::vector<Edge> &PinBridge::events() const { return events_; }
const std::vector<InputEdge> &PinBridge::input_events() const { return observed_; }
} // namespace mspice::firmware
