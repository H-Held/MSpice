// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
//
// Unit vocabulary and quantity strings ("4.7 kOhm"), SPEC section 5.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace libcheck {

const std::vector<std::string>& known_units();
bool is_known_unit(std::string_view u);

struct Quantity {
    double value = 0.0;
    std::string unit;       // empty if no unit was written
    bool has_prefix = false;
};

enum class QuantityError {
    None,
    Malformed,      // number part missing or invalid
    UnknownUnit     // number fine, text after it is not a unit / prefix+unit / prefix
};

struct QuantityResult {
    QuantityError error = QuantityError::None;
    Quantity q;
    std::string bad_unit;  // for UnknownUnit
    int bad_offset = 0;    // 0-based offset of the problem in the string
};

QuantityResult parse_quantity(std::string_view text);

}  // namespace libcheck
