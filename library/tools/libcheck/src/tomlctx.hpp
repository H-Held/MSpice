// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
//
// Helpers to check TOML tables against a list of allowed keys and to read
// typed values with good diagnostics.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <toml.hpp>

#include "diag.hpp"

namespace libcheck {

enum class VT {
    String,
    Int,
    Number,          // integer or float
    Bool,
    Table,
    Array,
    ArrayOfTables,
    ArrayOfStrings,
    Value,           // number or string (quantity or expression)
    Any
};

struct KeySpec {
    const char* name;
    VT type;
    bool required = false;
};

// A key that exists elsewhere in the format but is not allowed here.
struct Misplaced {
    std::string key;
    std::string hint;
};

class TomlCtx {
public:
    TomlCtx(Diagnostics& d, std::string display_file) : diags(d), file(std::move(display_file)) {}

    Diagnostics& diags;
    std::string file;

    Loc at(const toml::source_region& r) const;
    Loc at(const toml::node& n) const { return at(n.source()); }
    Loc key_at(const toml::table& t, std::string_view key) const;
    int key_len(std::string_view key) const { return static_cast<int>(key.size()); }
    int value_len(const toml::node& n) const;

    void report(std::string_view code, const Loc& loc, std::string msg, std::string hint = {}, int len = 1) {
        diags.report(code, loc, std::move(msg), std::move(hint), len);
    }

    // Reports unknown keys (E0004), missing required keys (E0005) and wrong
    // types (E0006). Returns false if anything was reported.
    bool keys(const toml::table& t, const std::string& where, const std::vector<KeySpec>& spec,
              const std::vector<Misplaced>& misplaced = {});

    static std::string type_name(const toml::node& n);
    static bool matches(const toml::node& n, VT type);
    static std::string vt_name(VT type);

    std::optional<std::string> str(const toml::table& t, std::string_view key) const;
    std::optional<bool> boolean(const toml::table& t, std::string_view key) const;
    std::vector<std::string> strings(const toml::table& t, std::string_view key) const;

    // Reads a string that must be one of `choices`; reports E0007.
    // Returns nullopt if absent or invalid.
    std::optional<std::string> choice(const toml::table& t, std::string_view key, const std::vector<std::string>& choices,
                                      const std::string& where);
};

}  // namespace libcheck
