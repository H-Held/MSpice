// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file spice_lexer.cpp
 * @brief Logical lines, includes with placeholders, .lib sections, tokens.
 */
#include <mspice/io/spice_netlist.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mspice::io {

namespace {

namespace fs = std::filesystem;

/** @brief Path from a UTF-8 string (non-ASCII characters in Windows paths survive). */
fs::path to_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
/** @brief UTF-8 string of a path. */
std::string from_path(const fs::path &p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

/** @brief Lower-case copy (ASCII). */
std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/** @brief Trim ASCII whitespace. */
std::string trim(const std::string &s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

/** @brief First whitespace-separated word of @p s (lower case). */
std::string first_word(const std::string &s) {
    std::size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    std::size_t b = a;
    while (b < s.size() && !std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    return lower(s.substr(a, b - a));
}

/** @brief Remove ';' comments and ' $ ' comments (a '$' after whitespace). */
std::string strip_comment(const std::string &s) {
    bool quote = false;
    int brace = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\'' && brace == 0)
            quote = !quote;
        else if (c == '{')
            ++brace;
        else if (c == '}' && brace > 0)
            --brace;
        if (quote || brace > 0)
            continue;
        if (c == ';')
            return s.substr(0, i);
        if (c == '$' && (i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1]))))
            return s.substr(0, i);
    }
    return s;
}

/** @brief Physical lines of a file content. */
std::vector<std::string> split_physical(const std::string &content) {
    std::vector<std::string> r;
    std::string cur;
    for (char c : content) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r')
                cur.pop_back();
            r.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        r.push_back(cur);
    return r;
}

/** @brief Join continuation lines and drop comments; first line skipped when @p title. */
std::vector<Line> logical_lines(const std::string &content, const std::string &file, bool title,
                                std::string *title_out) {
    std::vector<Line> out;
    const std::vector<std::string> phys = split_physical(content);
    std::size_t start = 0;
    if (title && !phys.empty()) {
        if (title_out)
            *title_out = trim(phys[0]);
        start = 1;
    }
    for (std::size_t k = start; k < phys.size(); ++k) {
        std::string s = phys[k];
        // Strip a UTF-8 byte order mark at the very start of a file.
        if (k == 0 && s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
            static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF)
            s.erase(0, 3);
        const std::string t = trim(s);
        if (t.empty() || t[0] == '*')
            continue;
        const std::string body = trim(strip_comment(t));
        if (t[0] == '+') {
            const std::string cont = trim(strip_comment(t.substr(1)));
            if (!out.empty()) {
                if (!cont.empty())
                    out.back().orig += " " + cont;
                continue;
            }
            // A continuation without a preceding line is kept as its own line.
            if (cont.empty())
                continue;
            out.push_back({{file, static_cast<int>(k + 1)}, "", cont});
            continue;
        }
        if (body.empty())
            continue;
        out.push_back({{file, static_cast<int>(k + 1)}, "", body});
    }
    for (Line &l : out)
        l.text = lower(l.orig);
    return out;
}

/** @brief Read a whole file; false when it cannot be opened. */
bool read_file(const std::string &path, std::string &content) {
    std::ifstream in(to_path(path), std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    content = ss.str();
    return true;
}

/** @brief Path argument of .include/.lib from the original text (quotes removed). */
std::string path_argument(const std::string &orig, std::string *rest) {
    std::size_t i = 0;
    while (i < orig.size() && !std::isspace(static_cast<unsigned char>(orig[i])))
        ++i; // skip the command word
    while (i < orig.size() && std::isspace(static_cast<unsigned char>(orig[i])))
        ++i;
    std::string p;
    if (i < orig.size() && (orig[i] == '"' || orig[i] == '\'')) {
        const char q = orig[i];
        const std::size_t e = orig.find(q, i + 1);
        p = orig.substr(i + 1, e == std::string::npos ? std::string::npos : e - i - 1);
        i = e == std::string::npos ? orig.size() : e + 1;
    } else {
        const std::size_t b = i;
        while (i < orig.size() && !std::isspace(static_cast<unsigned char>(orig[i])))
            ++i;
        p = orig.substr(b, i - b);
    }
    if (rest)
        *rest = trim(orig.substr(i));
    return p;
}

/** @brief Reader state shared over the include tree. */
struct Reader {
    const ReadOptions &opt;
    Deck &deck;
    bool in_control = false;

    /** @brief Add a report entry. */
    void note(const SourceLoc &loc, const std::string &elem, const std::string &feature,
              const std::string &kind, const std::string &reason) {
        deck.report.push_back({loc.file, loc.line, elem, feature, kind, reason});
    }

    /** @brief Replace @NAME@ placeholders and resolve relative to @p base. */
    std::string resolve_path(const std::string &raw, const std::string &base, const Line &l) {
        std::string p = raw;
        std::size_t at = 0;
        while ((at = p.find('@', at)) != std::string::npos) {
            const std::size_t end = p.find('@', at + 1);
            if (end == std::string::npos)
                break;
            const std::string key = lower(p.substr(at + 1, end - at - 1));
            bool found = false;
            for (const auto &kv : opt.path_vars) {
                if (lower(kv.first) == key) {
                    p.replace(at, end - at + 1, kv.second);
                    at += kv.second.size();
                    found = true;
                    break;
                }
            }
            if (!found) {
                note(l.loc, "", "placeholder @" + key + "@", "unsupported",
                     "placeholder without a value (ReadOptions::path_vars)");
                at = end + 1;
            }
        }
        fs::path fp = to_path(p);
        if (fp.is_relative())
            fp = to_path(base) / fp;
        return from_path(fp.lexically_normal());
    }

    /** @brief Process the logical lines of one file. @p section: only lines of this .lib
     *  section; empty for the whole file. Returns false when .end was seen. */
    bool process(const std::vector<Line> &lines, const std::string &dir, int depth,
                 const std::string &section) {
        bool in_section = section.empty();
        int skip_section = 0; // > 0 while inside a .lib section that was not requested
        for (const Line &l : lines) {
            const std::string cmd = first_word(l.text);
            if (in_control) {
                if (cmd == ".endc")
                    in_control = false;
                else
                    deck.control.push_back(l);
                continue;
            }
            if (cmd == ".lib") {
                std::string rest;
                const std::string arg = path_argument(l.orig, &rest);
                if (rest.empty()) { // section start ".lib name"
                    if (!section.empty() && !in_section && skip_section == 0 &&
                        lower(arg) == section) {
                        in_section = true;
                    } else {
                        if (section.empty() && skip_section == 0)
                            note(l.loc, "", ".lib " + lower(arg), "note",
                                 "section skipped, not requested");
                        ++skip_section;
                    }
                    continue;
                }
                if (!in_section || skip_section > 0)
                    continue;
                include(l, arg, lower(trim(rest)), dir, depth);
                continue;
            }
            if (cmd == ".endl") {
                if (skip_section > 0)
                    --skip_section;
                else if (!section.empty() && in_section)
                    return true; // end of the requested section
                continue;
            }
            if (!in_section || skip_section > 0)
                continue;
            if (cmd == ".include" || cmd == ".inc") {
                include(l, path_argument(l.orig, nullptr), "", dir, depth);
                continue;
            }
            if (cmd == ".control") {
                in_control = true;
                continue;
            }
            if (cmd == ".end")
                return false;
            deck.lines.push_back(l);
        }
        if (!section.empty() && !in_section && !lines.empty())
            note(lines.front().loc, "", ".lib " + section, "unsupported",
                 "section not found in " + lines.front().loc.file);
        return true;
    }

    /** @brief Read an included file (whole or one .lib section). */
    void include(const Line &l, const std::string &raw, const std::string &section,
                 const std::string &dir, int depth) {
        const std::string path = resolve_path(raw, dir, l);
        if (depth + 1 > opt.max_include_depth) {
            note(l.loc, "", ".include", "unsupported", "include depth exceeded: " + path);
            return;
        }
        std::string content;
        if (!read_file(path, content)) {
            note(l.loc, "", ".include " + raw, "unsupported", "file not found: " + path);
            return;
        }
        deck.files.push_back(path);
        const std::vector<Line> lines = logical_lines(content, path, false, nullptr);
        process(lines, from_path(to_path(path).parent_path()), depth + 1, section);
    }
};

} // namespace

Deck read_spice_text(const std::string &text, const std::string &base_dir, const std::string &name,
                     const ReadOptions &opt) {
    Deck deck;
    deck.files.push_back(name);
    Reader r{opt, deck};
    const std::vector<Line> lines = logical_lines(text, name, true, &deck.title);
    r.process(lines, base_dir, 0, "");
    if (r.in_control)
        deck.report.push_back({name, 0, "", ".control", "note", ".endc missing"});
    return deck;
}

Deck read_spice_file(const std::string &path, const ReadOptions &opt) {
    std::string content;
    if (!read_file(path, content)) {
        Deck d;
        d.files.push_back(path);
        d.report.push_back({path, 0, "", "file", "unsupported", "file not readable: " + path});
        return d;
    }
    const std::string dir = from_path(to_path(path).parent_path());
    return read_spice_text(content, dir.empty() ? "." : dir, path, opt);
}

std::vector<std::string> tokenize_line(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            flush();
        } else if (c == '(' || c == ')' || c == '=') {
            flush();
            out.emplace_back(1, c);
        } else if (c == '{') {
            flush();
            int depth = 0;
            std::size_t j = i;
            for (; j < text.size(); ++j) {
                if (text[j] == '{')
                    ++depth;
                else if (text[j] == '}' && --depth == 0)
                    break;
            }
            out.push_back(text.substr(i, j - i + 1));
            i = j;
        } else if (c == '\'') {
            flush();
            const std::size_t j = text.find('\'', i + 1);
            const std::size_t e = j == std::string::npos ? text.size() - 1 : j;
            out.push_back(text.substr(i, e - i + 1));
            i = e;
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

std::vector<std::pair<std::string, std::string>> split_assignments(const std::string &text,
                                                                   bool *ok) {
    // Find key starts: identifier at depth 0, preceded by start/space/comma, followed by '='.
    struct Key {
        std::size_t start, eq;
        std::string name;
    };
    std::vector<Key> keys;
    int depth = 0;
    bool quote = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\'') {
            quote = !quote;
            continue;
        }
        if (quote)
            continue;
        if (c == '(' || c == '{')
            ++depth;
        else if ((c == ')' || c == '}') && depth > 0)
            --depth;
        if (depth != 0)
            continue;
        const bool boundary =
            i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == ',';
        if (!boundary || !(std::isalpha(static_cast<unsigned char>(c)) || c == '_'))
            continue;
        std::size_t j = i;
        while (j < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_'))
            ++j;
        std::size_t k = j;
        while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k])))
            ++k;
        if (k < text.size() && text[k] == '=' && (k + 1 >= text.size() || text[k + 1] != '=')) {
            keys.push_back({i, k, lower(text.substr(i, j - i))});
            i = k;
        }
    }
    std::vector<std::pair<std::string, std::string>> out;
    if (ok)
        *ok = keys.empty() ? trim(text).empty() : trim(text.substr(0, keys[0].start)).empty();
    for (std::size_t n = 0; n < keys.size(); ++n) {
        const std::size_t vb = keys[n].eq + 1;
        const std::size_t ve = n + 1 < keys.size() ? keys[n + 1].start : text.size();
        std::string v = trim(text.substr(vb, ve - vb));
        while (!v.empty() && v.back() == ',')
            v.pop_back();
        out.emplace_back(keys[n].name, trim(v));
    }
    return out;
}

} // namespace mspice::io
