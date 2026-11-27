// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
//
// Diagnostics: stable codes, collection, text and JSON rendering.
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace libcheck {

enum class Severity { Error, Warning };

struct CodeInfo {
    const char* code;   // "E0004"
    Severity severity;
    const char* title;  // short title, shown by `codes`
};

// All diagnostic codes, in ascending order.
const std::vector<CodeInfo>& all_codes();
const CodeInfo* find_code(std::string_view code);

struct Loc {
    std::string file;  // display path
    int line = 0;      // 1-based, 0 = whole file
    int col = 0;       // 1-based
};

struct Diagnostic {
    std::string code;
    Severity severity = Severity::Error;
    std::string message;
    Loc loc;
    int len = 1;       // caret length in the source snippet
    std::string hint;
};

class Diagnostics {
public:
    // Severity is taken from the code table.
    void report(std::string_view code, const Loc& loc, std::string message, std::string hint = {}, int len = 1);

    // Remembers the text of a file so that snippets can be shown.
    void add_source(const std::string& display_file, const std::string& text);

    // Sorts by file, line and column (stable), removes exact duplicates.
    void finalize();

    std::size_t errors() const;
    std::size_t warnings() const;
    const std::vector<Diagnostic>& items() const { return items_; }

    std::string render_text(const Diagnostic& d) const;

private:
    std::vector<Diagnostic> items_;
    std::map<std::string, std::vector<std::string>> sources_;
};

std::string severity_name(Severity s);

// Renders the whole report. `files` is the number of files that were read.
std::string render_report_text(const Diagnostics& d, std::size_t files);
std::string render_report_json(const Diagnostics& d, std::size_t files);

}  // namespace libcheck
