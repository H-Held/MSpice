// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
//
// Small helpers shared by all parts of mspice-libcheck.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libcheck {

// Edit distance (Levenshtein) between two strings.
std::size_t edit_distance(std::string_view a, std::string_view b);

// Returns the closest candidate if it is close enough to be a likely typo,
// otherwise an empty string. Comparison ignores ASCII case.
std::string suggest(std::string_view word, const std::vector<std::string>& candidates);

// "did you mean `x`?" or a list of allowed names (at most `max_list`).
std::string suggestion_hint(std::string_view word, const std::vector<std::string>& candidates,
                            std::string_view what, std::size_t max_list = 12);

// Joins with ", " and wraps every item in backticks.
std::string join_quoted(const std::vector<std::string>& items, std::size_t max_items = 0);

std::string to_lower(std::string_view s);
std::string trim(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
std::vector<std::string> split(std::string_view s, char sep);

std::string json_escape(std::string_view s);

// Reads a whole file. Returns nullopt if the file cannot be read.
std::optional<std::string> read_file(const std::string& path);

// Converts a path to the display form used in diagnostics (forward slashes).
std::string display_path(const std::string& path);

// Simple regular-expression-free checks for the naming rules of the spec.
bool is_lower_ident(std::string_view s);        // ^[a-z][a-z0-9_]*$
bool is_part_id(std::string_view s);            // ^generic_[a-z0-9_]+$
bool is_pin_name(std::string_view s);           // ^[A-Za-z0-9_]+$
bool is_element_name(std::string_view s);       // ^[A-Za-z][A-Za-z0-9_]*$
bool is_library_name(std::string_view s);       // ^[a-z][a-z0-9_-]*$
bool is_category(std::string_view s);           // ^[a-z0-9_]+(/[a-z0-9_]+)*$
bool is_iso_date(std::string_view s);           // YYYY-MM-DD

// Semantic versions: MAJOR.MINOR.PATCH[-prerelease]
struct SemVer {
    long major = 0, minor = 0, patch = 0;
    std::string pre;
};
std::optional<SemVer> parse_semver(std::string_view s);
int compare_semver(const SemVer& a, const SemVer& b);

// Version requirement such as ">=1.2.0, <2.0.0", "^0.1.0", "~1.2.0", "=1.0.0".
struct VersionComparator {
    std::string op;
    SemVer version;
};
std::optional<std::vector<VersionComparator>> parse_version_req(std::string_view s);
bool version_satisfies(const SemVer& v, const std::vector<VersionComparator>& req);

// Normalises line endings (CRLF and CR to LF) for content comparison.
std::string normalize_newlines(std::string_view s);

}  // namespace libcheck
