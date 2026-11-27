// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors

#include "tomlctx.hpp"

#include <algorithm>

#include "util.hpp"

namespace libcheck {

Loc TomlCtx::at(const toml::source_region& r) const {
    return {file, static_cast<int>(r.begin.line), static_cast<int>(r.begin.column)};
}

Loc TomlCtx::key_at(const toml::table& t, std::string_view key) const {
    for (auto&& [k, v] : t) {
        if (k.str() == key) {
            if (k.source().begin.line > 0) return at(k.source());
            return at(v);
        }
    }
    return at(t);
}

int TomlCtx::value_len(const toml::node& n) const {
    if (auto s = n.as_string()) return static_cast<int>(s->get().size()) + 2;
    const auto& src = n.source();
    if (src.begin.line == src.end.line && src.end.column > src.begin.column)
        return static_cast<int>(src.end.column - src.begin.column);
    return 1;
}

std::string TomlCtx::type_name(const toml::node& n) {
    switch (n.type()) {
        case toml::node_type::string: return "a string";
        case toml::node_type::integer: return "an integer";
        case toml::node_type::floating_point: return "a float";
        case toml::node_type::boolean: return "a boolean";
        case toml::node_type::table: return "a table";
        case toml::node_type::array: return "an array";
        case toml::node_type::date: return "a date";
        case toml::node_type::time: return "a time";
        case toml::node_type::date_time: return "a date-time";
        default: return "an unknown value";
    }
}

std::string TomlCtx::vt_name(VT type) {
    switch (type) {
        case VT::String: return "a string";
        case VT::Int: return "an integer";
        case VT::Number: return "a number";
        case VT::Bool: return "a boolean (true or false)";
        case VT::Table: return "a table";
        case VT::Array: return "an array";
        case VT::ArrayOfTables: return "an array of tables ([[...]])";
        case VT::ArrayOfStrings: return "an array of strings";
        case VT::Value: return "a number or a string (quantity like \"4.7 kOhm\" or expression \"{...}\")";
        case VT::Any: return "a value";
    }
    return "a value";
}

bool TomlCtx::matches(const toml::node& n, VT type) {
    switch (type) {
        case VT::String: return n.is_string();
        case VT::Int: return n.is_integer();
        case VT::Number: return n.is_integer() || n.is_floating_point();
        case VT::Bool: return n.is_boolean();
        case VT::Table: return n.is_table();
        case VT::Array: return n.is_array();
        case VT::ArrayOfTables: {
            auto a = n.as_array();
            if (!a) return false;
            for (auto&& e : *a)
                if (!e.is_table()) return false;
            return true;
        }
        case VT::ArrayOfStrings: {
            auto a = n.as_array();
            if (!a) return false;
            for (auto&& e : *a)
                if (!e.is_string()) return false;
            return true;
        }
        case VT::Value: return n.is_integer() || n.is_floating_point() || n.is_string();
        case VT::Any: return true;
    }
    return false;
}

bool TomlCtx::keys(const toml::table& t, const std::string& where, const std::vector<KeySpec>& spec,
                   const std::vector<Misplaced>& misplaced) {
    bool ok = true;
    std::vector<std::string> suggested;  // required keys already named in a "did you mean" hint
    std::vector<std::string> names;
    for (const auto& s : spec) names.emplace_back(s.name);
    for (auto&& [k, v] : t) {
        const std::string key(k.str());
        auto it = std::find_if(spec.begin(), spec.end(), [&](const KeySpec& s) { return key == s.name; });
        if (it == spec.end()) {
            ok = false;
            auto m = std::find_if(misplaced.begin(), misplaced.end(), [&](const Misplaced& x) { return x.key == key; });
            if (m != misplaced.end()) {
                report("E0004", at(k.source()), "key `" + key + "` is not allowed in " + where, m->hint,
                       key_len(key));
            } else {
                const std::string s = suggest(key, names);
                if (!s.empty() && !t.contains(s)) suggested.push_back(s);
                report("E0004", at(k.source()), "unknown key `" + key + "` in " + where,
                       suggestion_hint(key, names, "keys"), key_len(key));
            }
            continue;
        }
        if (!matches(v, it->type)) {
            ok = false;
            std::string hint;
            if (it->type == VT::ArrayOfTables && v.is_table())
                hint = "write `[[" + key + "]]` for each entry instead of a single table";
            else if (it->type == VT::Value && v.is_boolean())
                hint = "booleans are not values; write a number such as 1 or a quantity such as \"1 V\"";
            else if (it->type == VT::String && (v.is_integer() || v.is_floating_point()))
                hint = "put the value in quotes";
            report("E0006", at(v), "`" + key + "` in " + where + " must be " + vt_name(it->type) + ", found " +
                                       type_name(v),
                   hint, value_len(v));
        }
    }
    for (const auto& s : spec) {
        if (s.required && !t.contains(s.name) &&
            std::find(suggested.begin(), suggested.end(), s.name) == suggested.end()) {
            ok = false;
            report("E0005", at(t), "missing required key `" + std::string(s.name) + "` in " + where,
                   "add `" + std::string(s.name) + " = ...` (" + vt_name(s.type) + ")");
        }
    }
    return ok;
}

std::optional<std::string> TomlCtx::str(const toml::table& t, std::string_view key) const {
    if (auto n = t.get(key))
        if (auto s = n->as_string()) return s->get();
    return std::nullopt;
}

std::optional<bool> TomlCtx::boolean(const toml::table& t, std::string_view key) const {
    if (auto n = t.get(key))
        if (auto b = n->as_boolean()) return b->get();
    return std::nullopt;
}

std::vector<std::string> TomlCtx::strings(const toml::table& t, std::string_view key) const {
    std::vector<std::string> out;
    if (auto n = t.get(key))
        if (auto a = n->as_array())
            for (auto&& e : *a)
                if (auto s = e.as_string()) out.push_back(s->get());
    return out;
}

std::optional<std::string> TomlCtx::choice(const toml::table& t, std::string_view key,
                                           const std::vector<std::string>& choices, const std::string& where) {
    auto n = t.get(key);
    if (!n) return std::nullopt;
    auto s = n->as_string();
    if (!s) return std::nullopt;  // type error already reported by keys()
    const std::string v = s->get();
    if (std::find(choices.begin(), choices.end(), v) != choices.end()) return v;
    std::string hint = suggest(v, choices);
    hint = hint.empty() ? "allowed values: " + join_quoted(choices) : "did you mean `" + hint + "`?";
    report("E0007", at(*n), "`" + std::string(key) + "` in " + where + " must be one of " + join_quoted(choices) +
                                ", found `" + v + "`",
           hint, value_len(*n));
    return std::nullopt;
}

}  // namespace libcheck
