// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors

#include "diag.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "util.hpp"

namespace libcheck {

const std::vector<CodeInfo>& all_codes() {
    using S = Severity;
    static const std::vector<CodeInfo> codes = {
        {"E0001", S::Error, "TOML syntax error"},
        {"E0002", S::Error, "file cannot be read"},
        {"E0003", S::Error, "missing or unsupported format_version"},
        {"E0004", S::Error, "unknown or misplaced key"},
        {"E0005", S::Error, "missing required key"},
        {"E0006", S::Error, "wrong value type"},
        {"E0007", S::Error, "value is not one of the allowed words"},
        {"E0010", S::Error, "invalid part id"},
        {"E0011", S::Error, "duplicate part id"},
        {"E0012", S::Error, "invalid name"},
        {"E0013", S::Error, "duplicate name"},
        {"E0014", S::Error, "invalid version or version requirement"},
        {"E0020", S::Error, "incomplete source"},
        {"E0021", S::Error, "licence not accepted"},
        {"E0022", S::Error, "quality level requirements not met"},
        {"E0030", S::Error, "unknown unit"},
        {"E0031", S::Error, "unit mismatch"},
        {"E0032", S::Error, "malformed quantity"},
        {"E0033", S::Error, "value out of range"},
        {"E0034", S::Error, "minimum greater than maximum"},
        {"E0035", S::Error, "invalid tolerance"},
        {"E0040", S::Error, "unknown pin or node"},
        {"W0041", S::Warning, "pin not used by the model"},
        {"E0042", S::Error, "wrong number of nodes or unconnected pin"},
        {"E0050", S::Error, "parameter is not adjustable"},
        {"E0060", S::Error, "unknown feature"},
        {"E0061", S::Error, "feature dependency cycle"},
        {"E0070", S::Error, "unknown element parameter"},
        {"E0071", S::Error, "missing element parameter"},
        {"E0072", S::Error, "conflicting or missing alternative keys"},
        {"E0073", S::Error, "unknown part"},
        {"E0074", S::Error, "part contains itself"},
        {"E0075", S::Error, "unknown instance parameter"},
        {"E0080", S::Error, "expression syntax error"},
        {"E0081", S::Error, "unknown name in expression"},
        {"E0082", S::Error, "unknown function or wrong argument count"},
        {"W0083", S::Warning, "ambiguous SPICE suffix M"},
        {"E0090", S::Error, "data file not found"},
        {"E0091", S::Error, "malformed CSV file"},
        {"E0092", S::Error, "x axis not strictly increasing"},
        {"E0093", S::Error, "CSV column not found"},
        {"E0094", S::Error, "unknown curve"},
        {"E0095", S::Error, "data file path not allowed"},
        {"E0100", S::Error, "unknown state"},
        {"E0101", S::Error, "unreachable state"},
        {"W0102", S::Warning, "state without way out"},
        {"E0103", S::Error, "unknown branch in state values"},
        {"W0110", S::Warning, "part has no reference tests"},
        {"E0111", S::Error, "expectation without tolerance"},
        {"E0112", S::Error, "expectation outside the analysis range"},
        {"E0113", S::Error, "unknown stimulus"},
        {"W0120", S::Warning, "pattern matches no file"},
        {"E0121", S::Error, "dependency not found"},
        {"E0122", S::Error, "dependency version not satisfied"},
        {"E0123", S::Error, "merge conflict"},
        {"W0124", S::Warning, "identical part defined twice"},
        {"E0125", S::Error, "invalid override"},
        {"E0126", S::Error, "dependency cycle"},
    };
    return codes;
}

const CodeInfo* find_code(std::string_view code) {
    std::string up(code);
    for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& c : all_codes())
        if (up == c.code) return &c;
    return nullptr;
}

std::string severity_name(Severity s) { return s == Severity::Error ? "error" : "warning"; }

void Diagnostics::report(std::string_view code, const Loc& loc, std::string message, std::string hint, int len) {
    Diagnostic d;
    d.code = std::string(code);
    const CodeInfo* info = find_code(code);
    d.severity = info ? info->severity : Severity::Error;
    d.message = std::move(message);
    d.loc = loc;
    d.len = len < 1 ? 1 : len;
    d.hint = std::move(hint);
    items_.push_back(std::move(d));
}

void Diagnostics::add_source(const std::string& display_file, const std::string& text) {
    if (sources_.count(display_file)) return;
    std::vector<std::string> lines;
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    lines.push_back(cur);
    sources_[display_file] = std::move(lines);
}

void Diagnostics::finalize() {
    std::stable_sort(items_.begin(), items_.end(), [](const Diagnostic& a, const Diagnostic& b) {
        if (a.loc.file != b.loc.file) return a.loc.file < b.loc.file;
        if (a.loc.line != b.loc.line) return a.loc.line < b.loc.line;
        return a.loc.col < b.loc.col;
    });
    std::vector<Diagnostic> uniq;
    for (auto& d : items_) {
        bool dup = false;
        for (const auto& u : uniq)
            if (u.code == d.code && u.message == d.message && u.loc.file == d.loc.file && u.loc.line == d.loc.line &&
                u.loc.col == d.loc.col)
                dup = true;
        if (!dup) uniq.push_back(d);
    }
    items_ = std::move(uniq);
}

std::size_t Diagnostics::errors() const {
    return static_cast<std::size_t>(
        std::count_if(items_.begin(), items_.end(), [](const Diagnostic& d) { return d.severity == Severity::Error; }));
}
std::size_t Diagnostics::warnings() const { return items_.size() - errors(); }

std::string Diagnostics::render_text(const Diagnostic& d) const {
    std::ostringstream o;
    o << severity_name(d.severity) << "[" << d.code << "]: " << d.message << "\n";
    std::string where = d.loc.file;
    if (d.loc.line > 0) where += ":" + std::to_string(d.loc.line) + ":" + std::to_string(std::max(1, d.loc.col));
    const std::string lineno = d.loc.line > 0 ? std::to_string(d.loc.line) : std::string();
    const std::string pad(lineno.size(), ' ');
    o << pad << "--> " << where << "\n";
    auto it = sources_.find(d.loc.file);
    if (d.loc.line > 0 && it != sources_.end() && d.loc.line <= static_cast<int>(it->second.size())) {
        std::string text = it->second[static_cast<std::size_t>(d.loc.line - 1)];
        for (auto& c : text)
            if (c == '\t') c = ' ';
        o << pad << " |\n";
        o << lineno << " | " << text << "\n";
        int col = std::max(1, d.loc.col);
        int len = d.len;
        if (col - 1 + len > static_cast<int>(text.size())) len = std::max(1, static_cast<int>(text.size()) - col + 1);
        o << pad << " | " << std::string(static_cast<std::size_t>(col - 1), ' ')
          << std::string(static_cast<std::size_t>(len), '^') << "\n";
    }
    if (!d.hint.empty()) o << pad << " = hint: " << d.hint << "\n";
    return o.str();
}

std::string render_report_text(const Diagnostics& d, std::size_t files) {
    std::ostringstream o;
    std::vector<std::string> codes;
    for (const auto& item : d.items()) {
        o << d.render_text(item) << "\n";
        if (std::find(codes.begin(), codes.end(), item.code) == codes.end()) codes.push_back(item.code);
    }
    const auto e = d.errors(), w = d.warnings();
    o << "checked " << files << (files == 1 ? " file" : " files") << ": " << e << (e == 1 ? " error" : " errors")
      << ", " << w << (w == 1 ? " warning" : " warnings") << "\n";
    if (!codes.empty())
        o << "For details on a code, run `mspice-libcheck explain " << codes.front() << "`.\n";
    return o.str();
}

std::string render_report_json(const Diagnostics& d, std::size_t files) {
    std::ostringstream o;
    o << "{\n  \"schema\": \"mspice-libcheck-diagnostics/1\",\n  \"diagnostics\": [";
    bool first = true;
    for (const auto& x : d.items()) {
        o << (first ? "\n" : ",\n");
        first = false;
        o << "    {\"code\": \"" << x.code << "\", \"severity\": \"" << severity_name(x.severity)
          << "\", \"message\": \"" << json_escape(x.message) << "\", \"file\": \"" << json_escape(x.loc.file)
          << "\", \"line\": " << x.loc.line << ", \"column\": " << x.loc.col << ", \"length\": " << x.len
          << ", \"hint\": \"" << json_escape(x.hint) << "\", \"explain\": \"mspice-libcheck explain " << x.code
          << "\"}";
    }
    o << (first ? "],\n" : "\n  ],\n");
    o << "  \"summary\": {\"files\": " << files << ", \"errors\": " << d.errors() << ", \"warnings\": " << d.warnings()
      << "}\n}\n";
    return o.str();
}

}  // namespace libcheck
