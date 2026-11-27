// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
//
// Parser for the expression subset of SPEC section 15. The parser does not
// evaluate; it checks syntax and records every name that is used, so that the
// part checker can resolve them.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace libcheck {

struct ExprUse {
    enum class Kind {
        Name,       // identifier used as a value
        Call,       // function call (name, argc)
        Node,       // node name inside V()
        Current,    // pin or element name inside I()
        EdgeArg,    // argument of rising()/falling()
        StateArg    // argument of in_state()
    };
    Kind kind;
    std::string name;
    int pos = 0;    // 0-based offset in the expression text
    int argc = 0;   // for Call
};

struct ExprResult {
    bool ok = true;
    int err_pos = 0;
    std::string error;
    std::vector<ExprUse> uses;
    std::vector<int> mega_warnings;  // offsets of numbers with suffix "M"
};

ExprResult parse_expression(std::string_view text);

// Parses "target = expression". `target` and `target_pos` are filled on success.
ExprResult parse_assignment(std::string_view text, std::string& target, int& target_pos);

struct FunctionInfo {
    const char* name;
    int min_args;
    int max_args;  // -1 = unlimited
};
const std::vector<FunctionInfo>& builtin_functions();
const FunctionInfo* find_function(std::string_view name);

// Built-in names usable as values (time, temp, pi, time_in_state).
const std::vector<std::string>& builtin_names();

// Names that may not be used for parameters, signals, variables or curves.
bool is_reserved_name(std::string_view name);

}  // namespace libcheck
