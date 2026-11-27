// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file spice_netlist.hpp
 * @brief SPICE netlist reading and flattening: logical lines, includes, definitions,
 *        .param scopes, subcircuit expansion with ngspice names, report of unsupported items.
 *
 * Flow: read_spice_file()/read_spice_text() -> logical lines (title removed, `+` joined,
 * comments stripped, lower case except include paths) -> collect_definitions() (two passes:
 * all .subckt/.model/.param first, then elements) -> flatten() -> FlatNetlist.
 *
 * Names after flattening follow ngspice: node `xk6.g`, element `v.xk6.vdq` (letter, path,
 * local name), nested instance paths `xu3a.x1`. Node `0` is global. Ports are replaced by
 * the connected nets before a name is formed, so a port called `gnd` never becomes ground
 * inside a subcircuit; a local node `gnd` becomes `<path>.gnd`.
 *
 * Parameter scopes as ngspice numparam: global .param -> subcircuit defaults -> instance
 * values (with or without `params:`, evaluated in the caller's scope) -> .param inside the
 * subcircuit (in order). Lookup walks the instance stack outwards (dynamic scope).
 */
#pragma once

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <mspice/io/spice_expr.hpp>

namespace mspice::io {

/** @brief Source position of a logical line. */
struct SourceLoc {
    std::string file; ///< file path as opened
    int line = 0;     ///< 1-based physical line of the first part of the logical line
};

/** @brief One entry of the import report.
 *
 *  kind: `unsupported` (not supported, a strict run must stop), `approximation` (mapped with
 *  a simplification), `ignored` (no effect), `note` (information).
 */
struct ReportItem {
    std::string file;
    int line = 0;
    std::string element; ///< element or directive name (flattened when known)
    std::string feature; ///< what is affected, e.g. "K", "function foo", ".func"
    std::string kind;    ///< unsupported | approximation | ignored | note
    std::string reason;
};

/** @brief Logical line after joining continuations and removing comments. */
struct Line {
    SourceLoc loc;
    std::string text; ///< lower case (include/lib paths keep their case in orig)
    std::string orig; ///< original case
};

/** @brief Options for reading. */
struct ReadOptions {
    /** Placeholders `@NAME@` in include paths (key without @, case-insensitive),
     *  e.g. {"LIB", "/path/to/models"}. */
    std::map<std::string, std::string> path_vars;
    /** Maximum include nesting. */
    int max_include_depth = 20;
};

/** @brief Result of reading: logical lines of the deck, title, read problems. */
struct Deck {
    std::string title;
    std::vector<Line> lines;        ///< includes expanded in place, .end stops
    std::vector<Line> control;      ///< lines between .control and .endc (lower case)
    std::vector<std::string> files; ///< all files read (top file first)
    std::vector<ReportItem> report; ///< missing files, unknown .lib sections, ...
};

/** @brief Read a netlist file (first line is the title).
 *  @param path file path
 *  @param opt placeholders and limits
 *  @return deck with logical lines; problems are in Deck::report
 */
Deck read_spice_file(const std::string &path, const ReadOptions &opt = {});

/** @brief Read netlist text (first line is the title); includes resolve against @p base_dir.
 *  @param text netlist text
 *  @param base_dir directory for relative includes
 *  @param name file name used in SourceLoc
 *  @param opt placeholders and limits
 *  @return deck
 */
Deck read_spice_text(const std::string &text, const std::string &base_dir = ".",
                     const std::string &name = "<text>", const ReadOptions &opt = {});

/** @brief Split a line into tokens: whitespace and ',' separate; '(' ')' '=' are tokens of
 *  their own; `{...}` and `'...'` stay single tokens (balanced).
 *  @param text line text
 *  @return tokens
 */
std::vector<std::string> tokenize_line(const std::string &text);

/** @brief Split `a=1 b={x*2} c = 'y'` into (key, value-text) pairs. A new pair starts at a
 *  top-level identifier followed by '=' (not '=='); the value is everything before it.
 *  @param text assignment list
 *  @param ok set to false when text before the first assignment is not empty
 *  @return pairs, keys lower case, values trimmed
 */
std::vector<std::pair<std::string, std::string>> split_assignments(const std::string &text,
                                                                   bool *ok = nullptr);

/** @brief Model definition (.model) before parameter evaluation. */
struct ModelDef {
    std::string name;
    std::string type; ///< d, npn, pnp, nmos, pmos, vdmos, sw, ...
    std::vector<std::pair<std::string, std::string>> params; ///< raw values; flags as "1"
    SourceLoc loc;
};

struct SubcktDef;

/** @brief Definitions and elements of one scope (top level or one .subckt body). */
struct Scope {
    const Scope *parent = nullptr; ///< lexically enclosing scope
    std::map<std::string, std::shared_ptr<SubcktDef>> subckts;
    std::map<std::string, ModelDef> models;
    std::vector<std::pair<std::string, std::string>> params; ///< .param in order
    std::vector<SourceLoc> param_locs;                       ///< location of each param
    std::vector<Line> elements;                              ///< element lines in order
    std::vector<Line> directives;                            ///< other dot lines in order
};

/** @brief Subcircuit definition. */
struct SubcktDef {
    std::string name;
    std::vector<std::string> ports;
    std::vector<std::pair<std::string, std::string>> defaults; ///< params: k=v
    SourceLoc loc;
    Scope body;
};

/** @brief Model after evaluating its parameters in the scope of its definition. */
struct FlatModel {
    std::string name; ///< flattened key (global: name, local: <path>:<name>)
    std::string type;
    std::vector<std::pair<std::string, double>> params; ///< keys lower case; flags = 1
    SourceLoc loc;
};

/** @brief Element after flattening. */
struct FlatElement {
    char type = 0;                  ///< lower-case letter
    std::string name;               ///< ngspice name: `r1` or `r.xk6.r01`
    std::string path;               ///< instance path (`xk6`, `xu3a.x1`), empty at top
    std::string local_name;         ///< name inside its subcircuit (`r01`)
    std::vector<std::string> nodes; ///< flattened node names
    std::string model;              ///< FlatNetlist::models key (D, Q, M, S, W, J)
    std::vector<std::string> refs; ///< flattened device names (F, H, W control source; K inductors)
    /** Remaining tokens after nodes/model/refs; `{...}` and `'...'` replaced by numbers. */
    std::vector<std::string> args;
    std::string expr_key;  ///< B/E/G/F/H value form: "v", "i", "cur", "value", "vol"
    std::string expr_text; ///< original expression text (lower case)
    ExprPtr expr;          ///< resolved and folded expression (nodes/devices renamed), or null
    SourceLoc loc;
};

/** @brief Top-level directive (.tran, .options, .ic, .save, ...). */
struct Directive {
    std::string name;                ///< ".tran", ".options", ...
    std::vector<std::string> tokens; ///< tokenize_line() of the text, braces evaluated
    std::string text;                ///< lower-case text
    SourceLoc loc;
};

/** @brief Flattened netlist (intermediate form for mapping to a Circuit). */
struct FlatNetlist {
    std::string title;
    std::vector<FlatElement> elements;
    std::map<std::string, FlatModel> models;
    std::vector<Directive> directives;
    std::vector<Line> control;
    std::map<std::string, double> global_params;
    std::vector<std::string> files;
    std::vector<ReportItem> report;

    /** @brief Number of elements per letter after flattening. */
    std::map<char, int> count_by_type() const;
    /** @brief Report entries of kind `unsupported`. */
    std::vector<ReportItem> missing() const;
    /** @brief Element by flattened name, nullptr when absent. */
    const FlatElement *find(const std::string &name) const;
};

/** @brief Options for flattening. */
struct FlattenOptions {
    int max_depth = 50; ///< maximum subcircuit nesting
};

/** @brief Collect definitions of a deck into a scope tree (first pass).
 *  @param deck logical lines
 *  @param report receives syntax problems
 *  @return top-level scope
 */
std::unique_ptr<Scope> collect_definitions(const Deck &deck, std::vector<ReportItem> &report);

/** @brief Flatten a deck: evaluate parameters, expand subcircuits, parse expressions.
 *  @param deck deck from read_spice_file()/read_spice_text()
 *  @param opt limits
 *  @return flat netlist; every problem is listed in FlatNetlist::report
 */
FlatNetlist flatten(const Deck &deck, const FlattenOptions &opt = {});

/** @brief Read and flatten a file in one call. */
FlatNetlist load_spice_file(const std::string &path, const ReadOptions &ropt = {},
                            const FlattenOptions &fopt = {});

} // namespace mspice::io
