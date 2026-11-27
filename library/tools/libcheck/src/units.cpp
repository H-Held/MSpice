// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors

#include "units.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace libcheck {

const std::vector<std::string>& known_units() {
    static const std::vector<std::string> units = {
        "1",       "%",          "V",          "A",          "Ohm",       "S",       "F",      "H",
        "W",       "J",          "C",          "s",          "Hz",        "K",       "degC",   "m",
        "kg",      "N",          "N*m",        "rad",        "rad/s",     "rpm",     "kg*m^2", "V*s/rad",
        "N*m/A",   "N*m*s/rad",  "Ohm/m",      "A/m",        "1/m",       "V/s",     "V/sqrt(Hz)",
        "A/sqrt(Hz)", "K/W",     "J/K",        "V/K",        "1/K",       "1/K^2",   "A/V^2",  "1/V",
        "sqrt(V)", "bit/s",      "dB",
    };
    return units;
}

bool is_known_unit(std::string_view u) {
    const auto& k = known_units();
    return std::find(k.begin(), k.end(), u) != k.end();
}

static double prefix_factor(std::string_view p, bool& ok) {
    ok = true;
    if (p == "f") return 1e-15;
    if (p == "p") return 1e-12;
    if (p == "n") return 1e-9;
    if (p == "u" || p == "\xC2\xB5" || p == "\xCE\xBC") return 1e-6;  // u, micro sign, Greek mu
    if (p == "m") return 1e-3;
    if (p == "k") return 1e3;
    if (p == "M") return 1e6;
    if (p == "G") return 1e9;
    if (p == "T") return 1e12;
    ok = false;
    return 1.0;
}

QuantityResult parse_quantity(std::string_view text) {
    QuantityResult r;
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    const std::size_t start = i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
    bool digits = false;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        ++i;
        digits = true;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            ++i;
            digits = true;
        }
    }
    if (!digits) {
        r.error = QuantityError::Malformed;
        r.bad_offset = static_cast<int>(start);
        return r;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < text.size() && (text[j] == '+' || text[j] == '-')) ++j;
        if (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) {
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
            i = j;
        }
    }
    const std::string number(text.substr(start, i - start));
    r.q.value = std::strtod(number.c_str(), nullptr);
    while (i < text.size() && text[i] == ' ') ++i;
    std::string rest(text.substr(i));
    while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) rest.pop_back();
    if (rest.empty()) return r;

    if (is_known_unit(rest)) {
        r.q.unit = rest;
        return r;
    }
    // prefix + unit; prefixes are one byte, except the two-byte micro signs
    for (std::size_t plen : {std::size_t{1}, std::size_t{2}}) {
        if (rest.size() < plen) continue;
        bool ok = false;
        double f = prefix_factor(std::string_view(rest).substr(0, plen), ok);
        if (!ok) continue;
        std::string unit = rest.substr(plen);
        if (unit.empty()) {
            r.q.value *= f;
            r.q.has_prefix = true;
            return r;
        }
        if (is_known_unit(unit)) {
            r.q.value *= f;
            r.q.unit = unit;
            r.q.has_prefix = true;
            return r;
        }
    }
    // Something follows the number that is not a unit. If it starts with a
    // digit or sign, the whole thing is malformed ("1.2.3", "5 6").
    if (std::isdigit(static_cast<unsigned char>(rest[0])) || rest[0] == '.' || rest[0] == '+' || rest[0] == '-') {
        r.error = QuantityError::Malformed;
        r.bad_offset = static_cast<int>(i);
        return r;
    }
    r.error = QuantityError::UnknownUnit;
    r.bad_unit = rest;
    r.bad_offset = static_cast<int>(i);
    return r;
}

}  // namespace libcheck
