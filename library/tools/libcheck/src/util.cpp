// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors

#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace libcheck {

std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            std::size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string suggest(std::string_view word, const std::vector<std::string>& candidates) {
    const std::string lw = to_lower(word);
    std::string best;
    std::size_t best_d = static_cast<std::size_t>(-1);
    for (const auto& c : candidates) {
        const std::string lc = to_lower(c);
        std::size_t d = edit_distance(lw, lc);
        // Prefix matches ("resist" -> "resistance") count as close.
        if (lw.size() >= 3 && starts_with(lc, lw)) d = std::min<std::size_t>(d, 1);
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    if (best.empty()) return {};
    const std::size_t limit = std::max<std::size_t>(1, (word.size() + 2) / 3);
    if (best_d == 0 && best != word) return best;  // differs only in case
    if (best_d > 0 && best_d <= limit) return best;
    return {};
}

std::string suggestion_hint(std::string_view word, const std::vector<std::string>& candidates,
                            std::string_view what, std::size_t max_list) {
    const std::string s = suggest(word, candidates);
    if (!s.empty()) return "did you mean `" + s + "`?";
    if (candidates.empty()) return "no " + std::string(what) + " is defined here";
    return "allowed " + std::string(what) + ": " + join_quoted(candidates, max_list);
}

std::string join_quoted(const std::vector<std::string>& items, std::size_t max_items) {
    std::string out;
    std::size_t n = 0;
    for (const auto& it : items) {
        if (max_items != 0 && n == max_items) {
            out += ", ...";
            break;
        }
        if (n++) out += ", ";
        out += "`" + it + "`";
    }
    return out;
}

std::string to_lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view x) {
    return s.size() >= x.size() && s.substr(s.size() - x.size()) == x;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string json_escape(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) return std::nullopt;
    return ss.str();
}

std::string display_path(const std::string& path) {
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    while (starts_with(p, "./")) p = p.substr(2);
    return p;
}

static bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
static bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_lower_ident(std::string_view s) {
    if (s.empty() || !is_lower(s[0])) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return is_lower(c) || is_digit(c) || c == '_'; });
}

bool is_part_id(std::string_view s) {
    if (!starts_with(s, "generic_") || s.size() == 8) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return is_lower(c) || is_digit(c) || c == '_'; });
}

bool is_pin_name(std::string_view s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return is_lower(c) || is_upper(c) || is_digit(c) || c == '_'; });
}

bool is_element_name(std::string_view s) {
    if (s.empty() || !(is_lower(s[0]) || is_upper(s[0]))) return false;
    return is_pin_name(s);
}

bool is_library_name(std::string_view s) {
    if (s.empty() || !is_lower(s[0])) return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return is_lower(c) || is_digit(c) || c == '_' || c == '-'; });
}

bool is_category(std::string_view s) {
    if (s.empty()) return false;
    for (const auto& seg : split(s, '/')) {
        if (seg.empty()) return false;
        if (!std::all_of(seg.begin(), seg.end(), [](char c) { return is_lower(c) || is_digit(c) || c == '_'; }))
            return false;
    }
    return true;
}

bool is_iso_date(std::string_view s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (std::size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (!is_digit(s[i])) return false;
    int m = (s[5] - '0') * 10 + (s[6] - '0');
    int d = (s[8] - '0') * 10 + (s[9] - '0');
    return m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

static std::optional<long> parse_num_component(std::string_view s) {
    if (s.empty() || s.size() > 9) return std::nullopt;
    if (s.size() > 1 && s[0] == '0') return std::nullopt;
    long v = 0;
    for (char c : s) {
        if (!is_digit(c)) return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return v;
}

std::optional<SemVer> parse_semver(std::string_view s) {
    SemVer v;
    std::string_view core = s;
    auto dash = s.find('-');
    if (dash != std::string_view::npos) {
        core = s.substr(0, dash);
        v.pre = std::string(s.substr(dash + 1));
        if (v.pre.empty()) return std::nullopt;
        for (char c : v.pre)
            if (!(is_lower(c) || is_upper(c) || is_digit(c) || c == '.' || c == '-')) return std::nullopt;
    }
    auto parts = split(core, '.');
    if (parts.size() != 3) return std::nullopt;
    auto a = parse_num_component(parts[0]);
    auto b = parse_num_component(parts[1]);
    auto c = parse_num_component(parts[2]);
    if (!a || !b || !c) return std::nullopt;
    v.major = *a;
    v.minor = *b;
    v.patch = *c;
    return v;
}

int compare_semver(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre == b.pre) return 0;
    if (a.pre.empty()) return 1;   // release > prerelease
    if (b.pre.empty()) return -1;
    return a.pre < b.pre ? -1 : 1;
}

std::optional<std::vector<VersionComparator>> parse_version_req(std::string_view s) {
    std::vector<VersionComparator> out;
    for (auto& raw : split(s, ',')) {
        std::string item = trim(raw);
        if (item.empty()) return std::nullopt;
        std::string op;
        for (const char* cand : {">=", "<=", ">", "<", "=", "^", "~"}) {
            if (starts_with(item, cand)) {
                op = cand;
                break;
            }
        }
        if (op.empty()) op = "=";
        else item = trim(item.substr(op.size()));
        auto v = parse_semver(item);
        if (!v) return std::nullopt;
        out.push_back({op, *v});
    }
    if (out.empty()) return std::nullopt;
    return out;
}

bool version_satisfies(const SemVer& v, const std::vector<VersionComparator>& req) {
    for (const auto& c : req) {
        int cmp = compare_semver(v, c.version);
        bool ok = false;
        if (c.op == "=") ok = cmp == 0;
        else if (c.op == ">") ok = cmp > 0;
        else if (c.op == ">=") ok = cmp >= 0;
        else if (c.op == "<") ok = cmp < 0;
        else if (c.op == "<=") ok = cmp <= 0;
        else if (c.op == "^") ok = cmp >= 0 && v.major == c.version.major &&
                                   (c.version.major != 0 || v.minor == c.version.minor);
        else if (c.op == "~") ok = cmp >= 0 && v.major == c.version.major && v.minor == c.version.minor;
        if (!ok) return false;
    }
    return true;
}

std::string normalize_newlines(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            o += '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
            o += s[i];
        }
    }
    return o;
}

}  // namespace libcheck
