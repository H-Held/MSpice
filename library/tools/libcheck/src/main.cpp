// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
//
// mspice-libcheck: validator for MSpice part libraries.
//
// Exit codes:
//   0  no errors (warnings allowed unless --deny-warnings)
//   1  errors found (or warnings with --deny-warnings)
//   2  usage error (unknown command or option, missing argument)
//   3  output error (merge could not write its output)

#include <algorithm>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "check.hpp"
#include "diag.hpp"
#include "explain.hpp"
#include "library.hpp"
#include "util.hpp"

namespace fs = std::filesystem;
using namespace libcheck;

namespace {

constexpr const char* kVersion = "0.1.0";

constexpr int kExitOk = 0;
constexpr int kExitErrors = 1;
constexpr int kExitUsage = 2;
constexpr int kExitOutput = 3;

const char* kUsage =
    "mspice-libcheck " "0.1.0" " - validator for MSpice part libraries (format 0.1)\n"
    "\n"
    "Usage:\n"
    "  mspice-libcheck check [options] <path>...\n"
    "      Check part files, directories or library manifests (library.toml).\n"
    "      A directory with a library.toml is checked as a library; otherwise\n"
    "      every .toml file below it is checked.\n"
    "  mspice-libcheck explain <code>\n"
    "      Print the long explanation of a diagnostic code, e.g. E0004.\n"
    "  mspice-libcheck codes [--format=text|json]\n"
    "      List all diagnostic codes.\n"
    "  mspice-libcheck merge [options] -o <dir> <library.toml>...\n"
    "      Merge libraries (later ones take precedence only through\n"
    "      [[overrides]]) and write the result to <dir>.\n"
    "  mspice-libcheck selftest\n"
    "      Check that every diagnostic code has an explanation.\n"
    "\n"
    "Options:\n"
    "  --format=text|json      output format of diagnostics (default text)\n"
    "  --deny-warnings         treat warnings as errors for the exit code\n"
    "  --on-conflict=error|first|last   merge only: how to resolve a part id\n"
    "                          defined differently by two libraries (default error)\n"
    "  --force                 merge only: write into a non-empty output directory\n"
    "  -o <dir>                merge only: output directory\n"
    "  --help, --version\n"
    "\n"
    "Exit codes: 0 ok, 1 errors found, 2 usage error, 3 output could not be written.\n";

int usage_error(const std::string& msg) {
    std::cerr << "mspice-libcheck: " << msg << "\n\nRun `mspice-libcheck --help` for usage.\n";
    return kExitUsage;
}

struct Options {
    std::string format = "text";
    bool deny_warnings = false;
    std::string on_conflict = "error";
    bool force = false;
    std::string out;
    std::vector<std::string> paths;
};

bool parse_options(const std::vector<std::string>& args, Options& o, bool merge, std::string& err) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (starts_with(a, "--format=")) {
            o.format = a.substr(9);
            if (o.format != "text" && o.format != "json") {
                err = "unknown format `" + o.format + "` (use text or json)";
                return false;
            }
        } else if (a == "--deny-warnings") {
            o.deny_warnings = true;
        } else if (merge && starts_with(a, "--on-conflict=")) {
            o.on_conflict = a.substr(14);
            if (o.on_conflict != "error" && o.on_conflict != "first" && o.on_conflict != "last") {
                err = "unknown --on-conflict value `" + o.on_conflict + "` (use error, first or last)";
                return false;
            }
        } else if (merge && a == "--force") {
            o.force = true;
        } else if (merge && a == "-o") {
            if (i + 1 >= args.size()) {
                err = "-o needs a directory";
                return false;
            }
            o.out = args[++i];
        } else if (starts_with(a, "-") && a != "-") {
            err = "unknown option `" + a + "`";
            return false;
        } else {
            o.paths.push_back(a);
        }
    }
    return true;
}

int finish(Diagnostics& d, std::size_t files, const Options& o) {
    d.finalize();
    if (o.format == "json") std::cout << render_report_json(d, files);
    else std::cout << render_report_text(d, files);
    if (d.errors() > 0) return kExitErrors;
    if (o.deny_warnings && d.warnings() > 0) return kExitErrors;
    return kExitOk;
}

bool is_manifest_file(const fs::path& p) { return p.filename() == "library.toml"; }

// Checks a set of loose part files as one group.
void check_loose(const std::vector<std::pair<std::string, std::string>>& files, Diagnostics& d, std::size_t& count,
                 std::deque<PartInfo>& store) {
    std::vector<const PartInfo*> ptrs;
    const std::size_t first = store.size();
    for (const auto& [path, display] : files) {
        store.push_back(check_part_file(path, display, d));
        ++count;
    }
    for (std::size_t i = first; i < store.size(); ++i) ptrs.push_back(&store[i]);
    check_part_set(ptrs, d, true, true);
}

void check_libraries(LibrarySet& set, const std::vector<Library*>& roots, Diagnostics& d) {
    for (Library* root : roots) {
        auto ordered = set.order({root});
        std::vector<MergeRecord> records;
        auto merged = set.merge(ordered, ConflictPolicy::Error, records);
        std::vector<const PartInfo*> ptrs;
        for (const auto& [id, mp] : merged) ptrs.push_back(mp.part);
        // parts without a valid id are not in `merged`; their references are still checked
        for (Library* l : ordered)
            for (const auto& p : l->parts)
                if (!p.loaded || p.id.empty()) ptrs.push_back(&p);
        check_part_set(ptrs, d, false, true);
    }
}

int cmd_check(const std::vector<std::string>& args) {
    Options o;
    std::string err;
    if (!parse_options(args, o, false, err)) return usage_error(err);
    if (o.paths.empty()) return usage_error("check needs at least one path");
    Diagnostics d;
    LibrarySet libs(d);
    std::vector<Library*> roots;
    std::deque<PartInfo> store;
    std::size_t loose_count = 0;

    for (const auto& arg : o.paths) {
        const fs::path p(arg);
        std::error_code ec;
        const std::string disp = display_path(arg);
        if (fs::is_directory(p, ec)) {
            if (fs::is_regular_file(p / "library.toml", ec)) {
                roots.push_back(libs.load((p / "library.toml").string(), display_path((fs::path(disp) / "library.toml").generic_string())));
                continue;
            }
            std::vector<std::string> all;
            for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec) && it->path().extension() == ".toml") all.push_back(it->path().string());
            std::sort(all.begin(), all.end());
            std::vector<Library*> local_roots;
            for (const auto& f : all)
                if (is_manifest_file(f)) {
                    std::string rel = fs::relative(f, p, ec).generic_string();
                    local_roots.push_back(libs.load(f, display_path((fs::path(disp) / rel).generic_string())));
                }
            // A directory with a library.toml belongs to that library: files below
            // it are checked only if the manifest lists them (`exclude` is honoured).
            std::vector<std::string> library_dirs;
            for (const auto& f : all)
                if (is_manifest_file(f))
                    library_dirs.push_back(fs::weakly_canonical(fs::path(f).parent_path(), ec).generic_string() + "/");
            std::vector<std::pair<std::string, std::string>> loose;
            for (const auto& f : all) {
                if (is_manifest_file(f)) continue;
                const std::string cf = fs::weakly_canonical(f, ec).generic_string();
                if (std::any_of(library_dirs.begin(), library_dirs.end(),
                                [&](const std::string& dir) { return starts_with(cf, dir); }))
                    continue;
                std::string rel = fs::relative(f, p, ec).generic_string();
                loose.push_back({f, display_path((fs::path(disp) / rel).generic_string())});
            }
            roots.insert(roots.end(), local_roots.begin(), local_roots.end());
            check_loose(loose, d, loose_count, store);
        } else if (fs::is_regular_file(p, ec)) {
            if (is_manifest_file(p)) roots.push_back(libs.load(arg, disp));
            else check_loose({{arg, disp}}, d, loose_count, store);
        } else {
            d.report("E0002", {disp, 0, 0}, "path does not exist", "check the spelling of the path");
        }
    }
    check_libraries(libs, roots, d);
    return finish(d, loose_count + libs.files_read(), o);
}

const char* find_doc(const std::string& code) {
    for (std::size_t i = 0; i < kExplainDocCount; ++i)
        if (code == kExplainDocs[i].code) return kExplainDocs[i].text;
    return nullptr;
}

int cmd_explain(const std::vector<std::string>& args) {
    if (args.size() != 1) return usage_error("explain needs exactly one code, e.g. `mspice-libcheck explain E0004`");
    const CodeInfo* c = find_code(args[0]);
    if (!c) {
        std::vector<std::string> codes;
        for (const auto& x : all_codes()) codes.emplace_back(x.code);
        std::string s = suggest(args[0], codes);
        return usage_error("unknown code `" + args[0] + "`" + (s.empty() ? "" : "; did you mean `" + s + "`?") +
                           " Run `mspice-libcheck codes` for the list.");
    }
    const char* doc = find_doc(c->code);
    if (!doc) {
        std::cout << c->code << ": " << c->title << "\n\n(no long explanation available)\n";
        return kExitOk;
    }
    std::cout << doc;
    return kExitOk;
}

int cmd_codes(const std::vector<std::string>& args) {
    Options o;
    std::string err;
    if (!parse_options(args, o, false, err)) return usage_error(err);
    if (!o.paths.empty()) return usage_error("codes takes no arguments");
    if (o.format == "json") {
        std::cout << "[\n";
        bool first = true;
        for (const auto& c : all_codes()) {
            std::cout << (first ? "" : ",\n") << "  {\"code\": \"" << c.code << "\", \"severity\": \""
                      << severity_name(c.severity) << "\", \"title\": \"" << json_escape(c.title) << "\"}";
            first = false;
        }
        std::cout << "\n]\n";
    } else {
        for (const auto& c : all_codes()) {
            std::string sev = severity_name(c.severity);
            sev.resize(8, ' ');
            std::cout << c.code << "  " << sev << c.title << "\n";
        }
    }
    return kExitOk;
}

int cmd_selftest() {
    int bad = 0;
    for (const auto& c : all_codes()) {
        const char* doc = find_doc(c.code);
        if (!doc) {
            std::cerr << "missing explanation: docs/errors/" << c.code << ".md\n";
            ++bad;
            continue;
        }
        const std::string text(doc);
        for (const char* section : {"## Wrong", "## Correct"})
            if (text.find(section) == std::string::npos) {
                std::cerr << c.code << ".md: missing section `" << section << "`\n";
                ++bad;
            }
        if (!starts_with(text, std::string("# ") + c.code)) {
            std::cerr << c.code << ".md: first line must be `# " << c.code << ": ...`\n";
            ++bad;
        }
    }
    for (std::size_t i = 0; i < kExplainDocCount; ++i)
        if (!find_code(kExplainDocs[i].code)) {
            std::cerr << "explanation without code: docs/errors/" << kExplainDocs[i].code << ".md\n";
            ++bad;
        }
    if (bad) return kExitErrors;
    std::cout << "selftest: " << all_codes().size() << " codes, all documented\n";
    return kExitOk;
}

bool write_text(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f << text;
    return static_cast<bool>(f);
}

std::string toml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

int cmd_merge(const std::vector<std::string>& args) {
    Options o;
    std::string err;
    if (!parse_options(args, o, true, err)) return usage_error(err);
    if (o.out.empty()) return usage_error("merge needs an output directory: -o <dir>");
    if (o.paths.size() < 2) return usage_error("merge needs at least two library manifests");
    Diagnostics d;
    LibrarySet libs(d);
    std::vector<Library*> roots;
    for (const auto& a : o.paths) {
        fs::path p(a);
        std::error_code ec;
        std::string disp = display_path(a);
        if (fs::is_directory(p, ec)) {
            p /= "library.toml";
            disp = display_path((fs::path(disp) / "library.toml").generic_string());
        }
        if (!fs::is_regular_file(p, ec)) {
            d.report("E0002", {disp, 0, 0}, "library manifest not found", "pass the path of a library.toml or its directory");
            continue;
        }
        roots.push_back(libs.load(p.string(), disp));
    }
    auto ordered = libs.order(roots);
    const ConflictPolicy pol = o.on_conflict == "first" ? ConflictPolicy::First
                             : o.on_conflict == "last"  ? ConflictPolicy::Last
                                                        : ConflictPolicy::Error;
    std::vector<MergeRecord> records;
    auto merged = libs.merge(ordered, pol, records);
    std::vector<const PartInfo*> ptrs;
    for (const auto& [id, mp] : merged) ptrs.push_back(mp.part);
    check_part_set(ptrs, d, false, true);
    d.finalize();
    if (d.errors() > 0 || (o.deny_warnings && d.warnings() > 0)) {
        int rc = finish(d, libs.files_read(), o);
        std::cerr << "merge: nothing written because of the errors above\n";
        return rc;
    }

    const fs::path out(o.out);
    std::error_code ec;
    if (fs::exists(out / "library.toml", ec) && !o.force) {
        std::cerr << "mspice-libcheck: `" << display_path(o.out)
                  << "/library.toml` already exists; use --force to overwrite\n";
        return kExitOutput;
    }
    // one output directory per library, unique names
    std::map<const Library*, std::string> subdir;
    std::set<std::string> used;
    for (const Library* l : ordered) {
        std::string base = l->name.empty() ? "library" : l->name;
        std::string name = base;
        for (int i = 2; used.count(name); ++i) name = base + "-" + std::to_string(i);
        used.insert(name);
        subdir[l] = name;
    }
    std::vector<std::string> part_paths;
    std::string report_parts;
    for (const auto& [id, mp] : merged) {
        const fs::path src = mp.part->path;
        const std::string rel_in_lib = fs::relative(src, mp.lib->dir, ec).generic_string();
        const std::string rel_out = subdir[mp.lib] + "/" + rel_in_lib;
        auto content = read_file(src.string());
        if (!content || !write_text(out / rel_out, *content)) {
            std::cerr << "mspice-libcheck: cannot write `" << display_path((out / rel_out).generic_string()) << "`\n";
            return kExitOutput;
        }
        for (const auto& df : mp.part->data_files) {
            const fs::path dsrc = src.parent_path() / df;
            const fs::path ddst = (out / rel_out).parent_path() / df;
            auto dc = read_file(dsrc.string());
            if (!dc || !write_text(ddst, *dc)) {
                std::cerr << "mspice-libcheck: cannot write `" << display_path(ddst.generic_string()) << "`\n";
                return kExitOutput;
            }
        }
        part_paths.push_back(rel_out);
        report_parts += std::string(report_parts.empty() ? "" : ",\n") + "    {\"id\": \"" + json_escape(id) +
                        "\", \"library\": \"" + json_escape(mp.lib->name) + "\", \"file\": \"" + json_escape(rel_out) + "\"}";
    }
    std::sort(part_paths.begin(), part_paths.end());
    std::string sources, report_libs;
    std::string license = ordered.empty() ? "CC-BY-4.0" : ordered.front()->license;
    for (const Library* l : ordered) {
        sources += std::string(sources.empty() ? "" : ", ") + l->name + " " + l->version;
        report_libs += std::string(report_libs.empty() ? "" : ",\n") + "    {\"name\": \"" + json_escape(l->name) +
                       "\", \"version\": \"" + json_escape(l->version) + "\", \"manifest\": \"" +
                       json_escape(l->manifest_display) + "\", \"license\": \"" + json_escape(l->license) + "\"}";
        if (l->license != license) license = "CC-BY-4.0";
    }
    std::string manifest = "# SPDX-License-Identifier: " + license + "\n# Generated by mspice-libcheck merge.\n" +
                           "format_version = \"0.1\"\n\n[library]\nname = \"merged\"\nversion = \"0.0.0\"\n" +
                           "description = \"Merged from: " + toml_escape(sources) + "\"\nlicense = \"" + license +
                           "\"\nparts = [\n";
    for (const auto& pp : part_paths) manifest += "  \"" + toml_escape(pp) + "\",\n";
    manifest += "]\n";
    std::string recs;
    for (const auto& r : records)
        recs += std::string(recs.empty() ? "" : ",\n") + "    {\"id\": \"" + json_escape(r.id) + "\", \"kept\": \"" +
                json_escape(r.kept_library) + "\", \"dropped\": \"" + json_escape(r.dropped_library) +
                "\", \"reason\": \"" + json_escape(r.reason) + "\"}";
    std::string report = "{\n  \"schema\": \"mspice-merge-report/1\",\n  \"libraries\": [\n" + report_libs +
                         "\n  ],\n  \"parts\": [\n" + report_parts + "\n  ],\n  \"replaced\": [\n" + recs + "\n  ]\n}\n";
    if (!write_text(out / "library.toml", manifest) || !write_text(out / "MERGE_REPORT.json", report)) {
        std::cerr << "mspice-libcheck: cannot write into `" << display_path(o.out) << "`\n";
        return kExitOutput;
    }
    int rc = finish(d, libs.files_read(), o);
    std::cerr << "merge: wrote " << merged.size() << " parts to " << display_path(o.out) << "\n";
    for (const auto& r : records)
        std::cerr << "  " << r.id << ": kept " << r.kept_library << ", dropped " << r.dropped_library << " (" << r.reason << ")\n";
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        std::cerr << kUsage;
        return kExitUsage;
    }
    const std::string cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        std::cout << kUsage;
        return kExitOk;
    }
    if (cmd == "--version") {
        std::cout << "mspice-libcheck " << kVersion << " (library format 0.1)\n";
        return kExitOk;
    }
    if (cmd == "check") return cmd_check(rest);
    if (cmd == "explain") return cmd_explain(rest);
    if (cmd == "codes") return cmd_codes(rest);
    if (cmd == "merge") return cmd_merge(rest);
    if (cmd == "selftest") return cmd_selftest();
    std::string s = suggest(cmd, {"check", "explain", "codes", "merge", "selftest"});
    return usage_error("unknown command `" + cmd + "`" + (s.empty() ? "" : "; did you mean `" + s + "`?"));
}
