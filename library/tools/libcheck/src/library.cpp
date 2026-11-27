// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors

#include "library.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <set>

#include "tomlctx.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace libcheck {

namespace {

const std::vector<std::string> kAcceptedLibraryLicenses = {"CC-BY-4.0", "CC0-1.0"};

bool match_segment(std::string_view p, std::string_view s) {
    // classic wildcard match with backtracking for '*'
    std::size_t pi = 0, si = 0, star = std::string_view::npos, mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == s[si])) {
            ++pi;
            ++si;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

bool match_path(const std::vector<std::string>& ps, std::size_t i, const std::vector<std::string>& ss, std::size_t j) {
    if (i == ps.size()) return j == ss.size();
    if (ps[i] == "**") {
        for (std::size_t k = j; k <= ss.size(); ++k)
            if (match_path(ps, i + 1, ss, k)) return true;
        return false;
    }
    if (j == ss.size()) return false;
    return match_segment(ps[i], ss[j]) && match_path(ps, i + 1, ss, j + 1);
}

std::string canonical(const std::string& p) {
    std::error_code ec;
    auto c = fs::weakly_canonical(fs::path(p), ec);
    if (ec) return fs::absolute(fs::path(p)).lexically_normal().generic_string();
    return c.generic_string();
}

std::string join_display(const std::string& base_dir_display, const std::string& rel) {
    fs::path p = fs::path(base_dir_display) / fs::path(rel);
    return display_path(p.lexically_normal().generic_string());
}

}  // namespace

bool has_wildcards(const std::string& pattern) { return pattern.find_first_of("*?") != std::string::npos; }

std::vector<std::string> glob_files(const std::string& dir, const std::string& pattern) {
    std::vector<std::string> out;
    const auto ps = split(pattern, '/');
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = fs::relative(it->path(), dir, ec).generic_string();
        if (match_path(ps, 0, split(rel, '/'), 0)) out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

Library* LibrarySet::load(const std::string& manifest_path, const std::string& display) {
    std::vector<std::string> stack;
    return load_impl(manifest_path, display, stack);
}

Library* LibrarySet::load_impl(const std::string& manifest_path, const std::string& display,
                               std::vector<std::string>& stack) {
    const std::string key = canonical(manifest_path);
    if (auto it = libs_.find(key); it != libs_.end()) return it->second.get();
    auto owned = std::make_unique<Library>();
    Library* lib = owned.get();
    libs_[key] = std::move(owned);
    lib->manifest_path = manifest_path;
    lib->manifest_display = display;
    lib->dir = fs::path(manifest_path).parent_path().string();
    if (lib->dir.empty()) lib->dir = ".";
    const std::string dir_display = display_path(fs::path(display).parent_path().generic_string());

    TomlCtx ctx(diags_, display);
    auto text = read_file(manifest_path);
    ++files_;
    if (!text) {
        ctx.report("E0002", {display, 0, 0}, "cannot read library manifest", "check that the path exists and is readable");
        return lib;
    }
    diags_.add_source(display, *text);
    toml::table root;
    try {
        root = toml::parse(*text, manifest_path);
    } catch (const toml::parse_error& e) {
        ctx.report("E0001", ctx.at(e.source()), "TOML syntax error: " + std::string(e.description()),
                   "see https://toml.io/en/v1.0.0 for the syntax; strings need double quotes");
        return lib;
    }
    if (auto fv = root.get("format_version")) {
        auto s = fv->as_string();
        if (!s || s->get() != "0.1")
            ctx.report("E0003", ctx.at(*fv), "unsupported format_version " + (s ? "`" + s->get() + "`" : TomlCtx::type_name(*fv)),
                       "this validator reads format_version = \"0.1\"", ctx.value_len(*fv));
    } else {
        ctx.report("E0003", {display, 1, 1}, "missing `format_version`", "add `format_version = \"0.1\"` at the top of the file");
    }
    const std::string misplaced_hint = "this looks like a part file; only library manifests may be named `library.toml`";
    ctx.keys(root, "the library manifest",
             {{"format_version", VT::Any}, {"library", VT::Table, true}, {"dependencies", VT::ArrayOfTables}, {"overrides", VT::ArrayOfTables}},
             {{"part", misplaced_hint}, {"pins", misplaced_hint}, {"model", misplaced_hint},
              {"parts", "`parts` belongs inside [library]"}, {"name", "`name` belongs inside [library]"},
              {"version", "`version` belongs inside [library]"}});
    auto lt = root["library"].as_table();
    if (!lt) return lib;
    lib->manifest_ok = true;
    ctx.keys(*lt, "[library]",
             {{"name", VT::String, true}, {"version", VT::String, true}, {"description", VT::String, true},
              {"license", VT::String, true}, {"authors", VT::ArrayOfStrings}, {"parts", VT::ArrayOfStrings, true},
              {"exclude", VT::ArrayOfStrings}},
             {{"dependencies", "write dependencies as top-level [[dependencies]] tables"},
              {"overrides", "write overrides as top-level [[overrides]] tables"}});
    if (auto n = lt->get("name"); n && n->is_string()) {
        lib->name = n->as_string()->get();
        if (!is_library_name(lib->name))
            ctx.report("E0012", ctx.at(*n), "library name `" + lib->name + "` is not valid",
                       "use lower-case letters, digits, `_` and `-`, starting with a letter", ctx.value_len(*n));
    }
    if (auto n = lt->get("version"); n && n->is_string()) {
        lib->version = n->as_string()->get();
        if (!parse_semver(lib->version))
            ctx.report("E0014", ctx.at(*n), "invalid version `" + lib->version + "`",
                       "use a semantic version MAJOR.MINOR.PATCH, e.g. \"0.1.0\"", ctx.value_len(*n));
    }
    if (auto n = lt->get("license"); n && n->is_string()) {
        lib->license = n->as_string()->get();
        if (std::find(kAcceptedLibraryLicenses.begin(), kAcceptedLibraryLicenses.end(), lib->license) ==
            kAcceptedLibraryLicenses.end())
            ctx.report("E0021", ctx.at(*n), "licence `" + lib->license + "` is not accepted for a part library",
                       "use `CC-BY-4.0` or `CC0-1.0`", ctx.value_len(*n));
    }

    // part files
    std::vector<std::string> excluded;
    for (const auto& pat : ctx.strings(*lt, "exclude"))
        for (const auto& f : glob_files(lib->dir, pat)) excluded.push_back(f);
    std::vector<std::string> files;
    if (auto pa = (*lt)["parts"].as_array()) {
        for (auto&& e : *pa) {
            auto s = e.as_string();
            if (!s) continue;
            const std::string pat = s->get();
            if (!has_wildcards(pat)) {
                std::error_code ec;
                if (!fs::is_regular_file(fs::path(lib->dir) / pat, ec)) {
                    ctx.report("E0002", ctx.at(e), "file `" + pat + "` listed in `parts` does not exist",
                               "paths are relative to the directory of library.toml", ctx.value_len(e));
                    continue;
                }
                files.push_back(fs::path(pat).lexically_normal().generic_string());
                continue;
            }
            auto matched = glob_files(lib->dir, pat);
            std::vector<std::string> kept;
            for (const auto& m : matched)
                if (ends_with(m, ".toml") && m != "library.toml") kept.push_back(m);
            if (kept.empty())
                ctx.report("W0120", ctx.at(e), "pattern `" + pat + "` matches no part file",
                           "check the pattern; `*` does not cross `/`, use `**` for subdirectories", ctx.value_len(e));
            files.insert(files.end(), kept.begin(), kept.end());
        }
    }
    std::vector<std::string> unique_files;
    for (const auto& f : files)
        if (std::find(unique_files.begin(), unique_files.end(), f) == unique_files.end() &&
            std::find(excluded.begin(), excluded.end(), f) == excluded.end())
            unique_files.push_back(f);
    for (const auto& f : unique_files) {
        const std::string full = (fs::path(lib->dir) / f).string();
        PartInfo pi = check_part_file(full, join_display(dir_display, f), diags_);
        ++files_;
        pi.library = lib->name;
        lib->parts.push_back(std::move(pi));
    }

    // dependencies
    stack.push_back(key);
    int di = 0;
    if (auto da = root["dependencies"].as_array(); da && TomlCtx::matches(*da, VT::ArrayOfTables)) {
        for (auto&& dn : *da) {
            const toml::table& d = *dn.as_table();
            const std::string where = "[[dependencies]] entry " + std::to_string(++di);
            ctx.keys(d, where, {{"name", VT::String, true}, {"path", VT::String, true}, {"version", VT::String}});
            auto name = ctx.str(d, "name");
            auto path = ctx.str(d, "path");
            if (!name || !path) continue;
            Library::Dependency dep;
            dep.name = *name;
            dep.path = *path;
            dep.loc = ctx.at(*d.get("path"));
            dep.len = ctx.value_len(*d.get("path"));
            fs::path target = fs::path(lib->dir) / *path;
            std::string target_display = join_display(dir_display, *path);
            std::error_code ec;
            if (fs::is_directory(target, ec)) {
                target /= "library.toml";
                target_display = join_display(target_display, "library.toml");
            }
            if (!fs::is_regular_file(target, ec)) {
                ctx.report("E0121", dep.loc, "dependency `" + dep.name + "`: no library manifest at `" + target_display + "`",
                           "`path` is relative to this library.toml and names a directory or its library.toml", dep.len);
                continue;
            }
            const std::string tkey = canonical(target.string());
            if (std::find(stack.begin(), stack.end(), tkey) != stack.end()) {
                ctx.report("E0126", dep.loc, "dependency cycle: `" + dep.name + "` depends back on `" + lib->name + "`",
                           "libraries must not depend on each other in a circle; move shared parts into a third library",
                           dep.len);
                continue;
            }
            Library* dl = load_impl(target.string(), target_display, stack);
            dep.lib = dl;
            if (dl->manifest_ok && !dl->name.empty() && dl->name != dep.name) {
                ctx.report("E0121", ctx.at(*d.get("name")),
                           "dependency `" + dep.name + "` points to library `" + dl->name + "`",
                           "set name = \"" + dl->name + "\" or fix `path`", ctx.value_len(*d.get("name")));
            }
            if (auto vn = d.get("version"); vn && vn->is_string() && dl->manifest_ok) {
                auto req = parse_version_req(vn->as_string()->get());
                if (!req) {
                    ctx.report("E0014", ctx.at(*vn), "invalid version requirement `" + vn->as_string()->get() + "`",
                               "e.g. \">=0.1.0, <0.2.0\", \"^1.2.0\" or \"=1.0.0\"", ctx.value_len(*vn));
                } else if (auto v = parse_semver(dl->version); v && !version_satisfies(*v, *req)) {
                    ctx.report("E0122", ctx.at(*vn),
                               "dependency `" + dep.name + "` requires version `" + vn->as_string()->get() + "`, found " + dl->version,
                               "update the dependency or change the requirement", ctx.value_len(*vn));
                }
            }
            lib->deps.push_back(dep);
        }
    }
    stack.pop_back();

    int oi = 0;
    if (auto oa = root["overrides"].as_array(); oa && TomlCtx::matches(*oa, VT::ArrayOfTables)) {
        for (auto&& on : *oa) {
            const toml::table& o = *on.as_table();
            const std::string where = "[[overrides]] entry " + std::to_string(++oi);
            ctx.keys(o, where, {{"id", VT::String, true}, {"library", VT::String, true}, {"reason", VT::String, true}});
            auto id = ctx.str(o, "id");
            auto ln = ctx.str(o, "library");
            if (!id || !ln) continue;
            Library::Override ov;
            ov.id = *id;
            ov.library = *ln;
            ov.reason = ctx.str(o, "reason").value_or("");
            ov.loc = ctx.at(*o.get("id"));
            ov.len = ctx.value_len(*o.get("id"));
            ov.lib_loc = ctx.at(*o.get("library"));
            ov.lib_len = ctx.value_len(*o.get("library"));
            lib->overrides.push_back(ov);
        }
    }

    // duplicate ids inside this library
    std::vector<const PartInfo*> ptrs;
    for (const auto& p : lib->parts) ptrs.push_back(&p);
    check_part_set(ptrs, diags_, true, false);
    return lib;
}

std::vector<Library*> LibrarySet::order(const std::vector<Library*>& roots) const {
    std::vector<Library*> out;
    std::set<const Library*> seen;
    std::function<void(Library*)> visit = [&](Library* l) {
        if (!l || seen.count(l)) return;
        seen.insert(l);
        for (const auto& d : l->deps) visit(d.lib);
        out.push_back(l);
    };
    for (Library* r : roots) visit(r);
    return out;
}

std::map<std::string, MergedPart> LibrarySet::merge(const std::vector<Library*>& ordered, ConflictPolicy policy,
                                                    std::vector<MergeRecord>& records) {
    std::map<std::string, MergedPart> merged;
    // overrides must name an earlier library that defines the id
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        const Library* l = ordered[i];
        for (const auto& ov : l->overrides) {
            const Library* target = nullptr;
            for (std::size_t j = 0; j < i; ++j)
                if (ordered[j]->name == ov.library) target = ordered[j];
            if (!target) {
                std::vector<std::string> names;
                for (std::size_t j = 0; j < i; ++j) names.push_back(ordered[j]->name);
                diags_.report("E0125", ov.lib_loc,
                              "override of `" + ov.id + "` names library `" + ov.library + "`, which is not a dependency of `" + l->name + "`",
                              names.empty() ? "add the library to [[dependencies]] first" : suggestion_hint(ov.library, names, "libraries"),
                              ov.lib_len);
                continue;
            }
            bool found = std::any_of(target->parts.begin(), target->parts.end(), [&](const PartInfo& p) { return p.id == ov.id; });
            if (!found) {
                std::vector<std::string> ids;
                for (const auto& p : target->parts) ids.push_back(p.id);
                diags_.report("E0125", ov.loc, "library `" + ov.library + "` has no part `" + ov.id + "` to override",
                              suggestion_hint(ov.id, ids, "parts", 6), ov.len);
            }
        }
    }
    for (const Library* l : ordered) {
        std::set<std::string> seen_here;
        for (const auto& p : l->parts) {
            if (!p.loaded || p.id.empty() || !seen_here.insert(p.id).second) continue;
            auto it = merged.find(p.id);
            if (it == merged.end()) {
                merged[p.id] = {&p, l};
                continue;
            }
            const MergedPart& prev = it->second;
            auto ov = std::find_if(l->overrides.begin(), l->overrides.end(),
                                   [&](const Library::Override& o) { return o.id == p.id && o.library == prev.lib->name; });
            if (ov != l->overrides.end()) {
                records.push_back({p.id, l->name, prev.lib->name, "override: " + ov->reason});
                it->second = {&p, l};
                continue;
            }
            if (prev.part->content == p.content) {
                diags_.report("W0124", p.id_loc,
                              "part `" + p.id + "` is identical in libraries `" + prev.lib->name + "` and `" + l->name + "`; it is used once",
                              "remove one copy, or add an [[overrides]] entry to state which library owns it", p.id_len);
                records.push_back({p.id, prev.lib->name, l->name, "identical"});
                continue;
            }
            if (policy == ConflictPolicy::Error) {
                diags_.report("E0123", p.id_loc,
                              "part `" + p.id + "` is defined by library `" + prev.lib->name + "` (version " + prev.part->version + ", " +
                                  prev.part->display + ") and by library `" + l->name + "` (version " + p.version + ")",
                              "add [[overrides]] id = \"" + p.id + "\", library = \"" + prev.lib->name + "\" to the manifest of `" +
                                  l->name + "`, or rename one of the parts",
                              p.id_len);
            } else if (policy == ConflictPolicy::First) {
                records.push_back({p.id, prev.lib->name, l->name, "on-conflict=first"});
            } else {
                records.push_back({p.id, l->name, prev.lib->name, "on-conflict=last"});
                it->second = {&p, l};
            }
        }
    }
    return merged;
}

}  // namespace libcheck
