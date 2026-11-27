// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
//
// Checks for part files (SPEC sections 4-15) and for sets of parts
// (cross-file references, duplicate ids).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "diag.hpp"

namespace libcheck {

struct NamedLoc {
    std::string name;
    Loc loc;
    int len = 1;
};

struct ParamInfo {
    std::string name;
    std::string type;
    std::string unit;
    bool adjustable = true;
    std::vector<std::string> choices;
};

// A use of another part inside a model (`[[model.elements]]` with `part`).
struct PartRef {
    std::string element;          // element name, for messages
    std::string target;           // referenced part id
    Loc loc;                      // location of the `part` value
    int len = 1;
    std::vector<NamedLoc> params;    // instance parameter names
    std::vector<NamedLoc> features;  // instance feature names
    bool nodes_is_table = false;
    int node_count = 0;              // array form
    std::vector<NamedLoc> node_pins; // table form: pin names
    Loc nodes_loc;
};

struct PartInfo {
    bool loaded = false;        // TOML parsed and [part].id read
    std::string path;           // file system path
    std::string display;        // path used in diagnostics
    std::string library;        // library name ("" for loose files)
    std::string id;
    std::string version;
    Loc id_loc;
    int id_len = 1;
    std::vector<std::string> pins;
    std::map<std::string, ParamInfo> params;
    std::vector<std::string> features;
    std::vector<PartRef> refs;
    std::vector<std::string> data_files;  // relative to the part file directory
    std::string content;                  // file text with normalised line endings
};

// Checks one part file. Never throws; problems are reported to `diags`.
PartInfo check_part_file(const std::string& path, const std::string& display, Diagnostics& diags);

// Cross-file checks on a set of parts that are used together: duplicate
// ids (only if `check_duplicates`), references to other parts, recursion.
void check_part_set(const std::vector<const PartInfo*>& parts, Diagnostics& diags, bool check_duplicates,
                    bool check_refs = true);

}  // namespace libcheck
