// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
//
// Library manifests (SPEC section 2), dependencies and merging (section 3).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "diag.hpp"

namespace libcheck {

struct Library {
    std::string manifest_path;     // file system path of library.toml
    std::string manifest_display;  // display path
    std::string dir;               // file system directory
    std::string name;
    std::string version;
    std::string license;
    bool manifest_ok = false;      // TOML parsed and [library] present

    struct Dependency {
        std::string name;
        std::string path;
        Loc loc;        // location of `path`
        int len = 1;
        Library* lib = nullptr;
    };
    struct Override {
        std::string id;
        std::string library;
        std::string reason;
        Loc loc;        // location of `id`
        int len = 1;
        Loc lib_loc;
        int lib_len = 1;
    };
    std::vector<Dependency> deps;
    std::vector<Override> overrides;
    std::vector<PartInfo> parts;
};

enum class ConflictPolicy { Error, First, Last };

struct MergedPart {
    const PartInfo* part = nullptr;
    const Library* lib = nullptr;
};

struct MergeRecord {
    std::string id;
    std::string kept_library;
    std::string dropped_library;
    std::string reason;  // "override", "identical", "on-conflict=first", "on-conflict=last"
};

class LibrarySet {
public:
    explicit LibrarySet(Diagnostics& d) : diags_(d) {}

    // Loads a manifest and, recursively, its dependencies; checks all part
    // files. Returns nullptr if the manifest cannot be read at all.
    Library* load(const std::string& manifest_path, const std::string& display);

    // Dependencies first, in listed order; each library once.
    std::vector<Library*> order(const std::vector<Library*>& roots) const;

    // Merges the libraries in the given order (see SPEC section 3).
    // `implicit_deps` treats every library as depending on all earlier ones
    // (used by the `merge` command).
    std::map<std::string, MergedPart> merge(const std::vector<Library*>& ordered, ConflictPolicy policy,
                                            std::vector<MergeRecord>& records);

    std::size_t files_read() const { return files_; }

private:
    Library* load_impl(const std::string& manifest_path, const std::string& display, std::vector<std::string>& stack);

    Diagnostics& diags_;
    std::map<std::string, std::unique_ptr<Library>> libs_;  // by canonical manifest path
    std::size_t files_ = 0;
};

// Expands a glob pattern relative to `dir` (SPEC section 2). Returns
// relative paths with `/`, sorted.
std::vector<std::string> glob_files(const std::string& dir, const std::string& pattern);
bool has_wildcards(const std::string& pattern);

}  // namespace libcheck
