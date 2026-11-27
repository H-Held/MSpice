// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors

#include "csv.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "units.hpp"
#include "util.hpp"

namespace libcheck {

int CsvTable::column_index(const std::string& name) const {
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) return static_cast<int>(i);
    return -1;
}

namespace {

struct Cell {
    std::string text;
    int col;  // 1-based
};

std::vector<Cell> split_cells(const std::string& line) {
    std::vector<Cell> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == ',') {
            std::string raw = line.substr(start, i - start);
            std::size_t lead = 0;
            while (lead < raw.size() && std::isspace(static_cast<unsigned char>(raw[lead]))) ++lead;
            out.push_back({trim(raw), static_cast<int>(start + lead + 1)});
            start = i + 1;
        }
    }
    return out;
}

bool parse_plain_number(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    // reject hex, inf, nan which strtod accepts
    for (char c : s)
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E'))
            return false;
    return true;
}

std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

}  // namespace

const CsvTable& load_csv(const std::string& path, const std::string& display, Diagnostics& diags) {
    static std::map<std::string, CsvTable> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    CsvTable& t = cache[path];

    auto text = read_file(path);
    if (!text) {
        diags.report("E0002", {display, 0, 0}, "cannot read data file", "check file permissions");
        return t;
    }
    diags.add_source(display, *text);
    auto lines = split(normalize_newlines(*text), '\n');
    if (!lines.empty() && starts_with(lines[0], "\xEF\xBB\xBF")) lines[0] = lines[0].substr(3);

    int stage = 0;  // 0 = header, 1 = units, 2 = data
    for (std::size_t li = 0; li < lines.size(); ++li) {
        const int lineno = static_cast<int>(li + 1);
        const std::string line = lines[li];
        const std::string tr = trim(line);
        if (tr.empty() || tr[0] == '#') continue;
        if (line.find('"') != std::string::npos || line.find(';') != std::string::npos) {
            int col = static_cast<int>(std::min(line.find('"'), line.find(';'))) + 1;
            diags.report("E0091", {display, lineno, col}, "quotes and semicolons are not allowed in data files",
                         "separate cells with `,` and use `.` as decimal separator");
            return t;
        }
        auto cells = split_cells(line);
        if (stage == 0) {
            std::set<std::string> seen;
            for (const auto& c : cells) {
                if (!is_lower_ident(c.text)) {
                    diags.report("E0091", {display, lineno, c.col},
                                 "column name `" + c.text + "` is not a valid name",
                                 "the first line must hold column names matching ^[a-z][a-z0-9_]*$, e.g. `time,current`",
                                 static_cast<int>(std::max<std::size_t>(1, c.text.size())));
                    return t;
                }
                if (!seen.insert(c.text).second) {
                    diags.report("E0091", {display, lineno, c.col}, "column name `" + c.text + "` appears twice",
                                 "give every column a different name", static_cast<int>(c.text.size()));
                    return t;
                }
                t.names.push_back(c.text);
                t.name_cols.push_back(c.col);
            }
            t.header_line = lineno;
            stage = 1;
            continue;
        }
        if (cells.size() != t.names.size()) {
            diags.report("E0091", {display, lineno, 1},
                         "row has " + std::to_string(cells.size()) + " cells, the header has " +
                             std::to_string(t.names.size()),
                         "every row needs one value per column");
            return t;
        }
        if (stage == 1) {
            for (const auto& c : cells) {
                if (!is_known_unit(c.text)) {
                    double dummy;
                    if (parse_plain_number(c.text, dummy)) {
                        diags.report("E0091", {display, lineno, c.col}, "the second line must hold units, found a number",
                                     "add a unit line after the column names, e.g. `s,A`");
                    } else {
                        diags.report("E0030", {display, lineno, c.col}, "unknown unit `" + c.text + "`",
                                     suggestion_hint(c.text, known_units(), "units", 8),
                                     static_cast<int>(std::max<std::size_t>(1, c.text.size())));
                    }
                    return t;
                }
                t.units.push_back(c.text);
            }
            t.unit_line = lineno;
            t.columns.assign(t.names.size(), {});
            stage = 2;
            continue;
        }
        for (std::size_t ci = 0; ci < cells.size(); ++ci) {
            double v = 0;
            if (!parse_plain_number(cells[ci].text, v)) {
                diags.report("E0091", {display, lineno, cells[ci].col},
                             "`" + cells[ci].text + "` is not a plain number",
                             "data cells hold numbers like `0.25` or `1e-3`, without prefixes or units",
                             static_cast<int>(std::max<std::size_t>(1, cells[ci].text.size())));
                return t;
            }
            t.columns[ci].push_back(v);
        }
        t.row_lines.push_back(lineno);
    }
    if (stage < 2) {
        diags.report("E0091", {display, 1, 1}, "data file needs a line of column names and a line of units",
                     "see SPEC.md section 13.3");
        return t;
    }
    if (t.row_lines.size() < 2) {
        diags.report("E0091", {display, t.unit_line, 1}, "data file needs at least two data rows",
                     "a curve needs at least two points");
        return t;
    }
    t.ok = true;
    check_increasing(t, 0, display, diags);
    return t;
}

bool check_increasing(const CsvTable& t, int col, const std::string& display, Diagnostics& diags) {
    static std::set<std::string> reported;
    if (!t.ok || col < 0 || col >= static_cast<int>(t.columns.size())) return false;
    const auto& c = t.columns[static_cast<std::size_t>(col)];
    for (std::size_t i = 1; i < c.size(); ++i) {
        if (!(c[i] > c[i - 1])) {
            const std::string key = display + "#" + std::to_string(col);
            if (reported.insert(key).second) {
                diags.report("E0092", {display, t.row_lines[i], 1},
                             "column `" + t.names[static_cast<std::size_t>(col)] + "` is not strictly increasing (" +
                                 fmt(c[i - 1]) + " is followed by " + fmt(c[i]) + ")",
                             "sort the rows by the x column and remove duplicate x values");
            }
            return false;
        }
    }
    return true;
}

}  // namespace libcheck
