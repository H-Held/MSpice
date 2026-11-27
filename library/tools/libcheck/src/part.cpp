// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
//
// Checks for one part file. The order of the checks follows SPEC.md.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <regex>
#include <set>

#include "check.hpp"
#include "csv.hpp"
#include "expr.hpp"
#include "tomlctx.hpp"
#include "units.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace libcheck {

namespace {

const std::vector<std::string> kAcceptedPartLicenses = {"CC-BY-4.0", "CC0-1.0"};
const std::vector<std::string> kOpenModelLicenses = {"CC0-1.0",      "CC-BY-4.0",  "MIT", "BSD-2-Clause",
                                                     "BSD-3-Clause", "Apache-2.0", "Zlib"};
const std::vector<std::string> kQualities = {"draft", "datasheet", "measured", "vendor-verified"};
const std::vector<std::string> kSourceKinds = {"datasheet",   "application_note", "standard", "measurement",
                                               "spice_model", "textbook",         "estimate"};
const std::vector<std::string> kRoles = {"power", "ground", "input", "output", "bidirectional", "analog"};
const std::vector<std::string> kDrivers = {"push_pull", "open_drain", "tristate"};
const std::vector<std::string> kParamTypes = {"real", "integer", "boolean", "string", "enum"};
const std::vector<std::string> kModelKinds = {"primitive", "subcircuit", "behavioral", "load", "noise_source"};

struct PrimParam {
    const char* name;
    const char* unit;
    bool required;
};

struct PrimSpec {
    const char* type;
    const char* what;
    int nmin, nmax;
    const char* node_names;
    std::vector<PrimParam> params;
    std::vector<std::string> polarities;
};

const std::vector<PrimSpec>& prim_specs() {
    static const std::vector<PrimSpec> s = {
        {"R", "resistor", 2, 2, "p n", {{"r", "Ohm", true}, {"tc1", "1/K", false}, {"tc2", "1/K^2", false}}, {}},
        {"C", "capacitor", 2, 2, "p n", {{"c", "F", true}, {"ic", "V", false}}, {}},
        {"L", "inductor", 2, 2, "p n", {{"l", "H", true}, {"ic", "A", false}}, {}},
        {"D", "diode", 2, 2, "a k",
         {{"is", "A", false}, {"n", "1", false}, {"rs", "Ohm", false}, {"bv", "V", false}, {"ibv", "A", false},
          {"cjo", "F", false}, {"vj", "V", false}, {"m", "1", false}, {"tt", "s", false}, {"eg", "V", false},
          {"xti", "1", false}},
         {}},
        {"Q", "bipolar transistor", 3, 3, "c b e",
         {{"is", "A", false},  {"bf", "1", false},  {"br", "1", false},  {"nf", "1", false},  {"nr", "1", false},
          {"vaf", "V", false}, {"var", "V", false}, {"ikf", "A", false}, {"ikr", "A", false}, {"ise", "A", false},
          {"ne", "1", false},  {"rb", "Ohm", false}, {"re", "Ohm", false}, {"rc", "Ohm", false}, {"cje", "F", false},
          {"vje", "V", false}, {"mje", "1", false}, {"cjc", "F", false}, {"vjc", "V", false}, {"mjc", "1", false},
          {"tf", "s", false},  {"tr", "s", false}},
         {"npn", "pnp"}},
        {"M", "MOSFET", 3, 4, "d g s [b]",
         {{"vto", "V", false}, {"kp", "A/V^2", false}, {"lambda", "1/V", false}, {"gamma", "sqrt(V)", false},
          {"phi", "V", false}, {"rd", "Ohm", false}, {"rs", "Ohm", false}, {"cgso", "F", false}, {"cgdo", "F", false},
          {"cbd", "F", false}, {"cbs", "F", false}, {"w", "m", false}, {"l", "m", false}, {"is", "A", false}},
         {"nmos", "pmos"}},
        {"SW", "switch", 4, 4, "p n cp cn",
         {{"ron", "Ohm", false}, {"roff", "Ohm", false}, {"vt", "V", false}, {"vh", "V", false}},
         {}},
        {"V", "voltage source", 2, 2, "p n", {{"dc", "V", false}}, {}},
        {"I", "current source", 2, 2, "p n", {{"dc", "A", false}}, {}},
        {"E", "voltage-controlled voltage source", 4, 4, "p n cp cn", {{"gain", "1", true}}, {}},
        {"G", "voltage-controlled current source", 4, 4, "p n cp cn", {{"gm", "S", true}}, {}},
        {"B", "behavioral source", 2, 2, "p n", {}, {}},
    };
    return s;
}

const PrimSpec* find_prim(const std::string& t) {
    for (const auto& s : prim_specs())
        if (t == s.type) return &s;
    return nullptr;
}

std::vector<std::string> prim_types(bool with_b) {
    std::vector<std::string> v;
    for (const auto& s : prim_specs())
        if (with_b || std::string(s.type) != "B") v.emplace_back(s.type);
    return v;
}

std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// A value after parsing (SPEC 5.1).
struct Val {
    bool present = false;
    bool ok = false;
    bool is_expr = false;
    double num = 0.0;
    std::string unit;
};

enum class ExprKind { Normal, Edge, Action };

struct PendingExpr {
    std::string text;
    Loc loc;           // location of the first character of the text
    ExprKind kind;
    std::string what;  // for messages, e.g. "`expr` of output `tach`"
};

// Names that expressions of the model may use.
struct Scope {
    std::map<std::string, std::string> values;  // name -> kind ("parameter", "signal", ...)
    std::set<std::string> curves;
    std::set<std::string> nodes;
    std::set<std::string> currents;
    std::set<std::string> states;
    std::set<std::string> inputs;
    bool has_sm = false;
    bool test = false;
};

class PartChecker {
public:
    PartChecker(const std::string& path, const std::string& display, Diagnostics& d)
        : path_(path), dir_(fs::path(path).parent_path()), ctx_(d, display) {
        info_.path = path;
        info_.display = display;
    }

    PartInfo run();

private:
    // sections
    void check_part(const toml::table& t);
    void check_source(const toml::table& t, const toml::node& node);
    void check_pins(const toml::array& a);
    void check_parameters(const toml::array& a);
    void check_features(const toml::array& a);
    void check_curves(const toml::array& a);
    void check_model(const toml::table& m);
    void check_tests(const toml::array& a);
    void check_quality(const toml::table& part);

    // model pieces
    void check_primitive(const toml::table& m);
    void check_element(const toml::table& e, const std::string& where, bool in_test, std::set<std::string>* test_nodes,
                       std::set<std::string>& names);
    void check_prim_params(const PrimSpec& ps, const toml::node* params, const std::string& where);
    void check_signals(const toml::array& a);
    void check_outputs(const toml::array& a);
    void check_branches(const toml::array& a, bool has_sm);
    void check_state_machine(const toml::table& sm);
    void check_noise(const toml::table& n);

    // helpers
    Val value(const toml::table& t, std::string_view key, const std::string& unit, bool allow_expr,
              const std::string& where, ExprKind kind = ExprKind::Normal);
    void expression(const toml::table& t, std::string_view key, const std::string& where,
                    ExprKind kind = ExprKind::Normal);
    void add_expr(const toml::node& n, const std::string& text, std::size_t offset, ExprKind kind,
                  const std::string& what);
    bool define_value_name(const std::string& name, const std::string& kind, const toml::table& t);
    bool check_lower_name(const toml::table& t, const std::string& where, std::string& out, bool reserved_check);
    void node_ref(const toml::node& n, const std::string& name, const std::string& where);
    void check_nodes_array(const toml::table& t, std::string_view key, const std::string& where, int nmin, int nmax,
                           const char* node_names);
    void feature_ref(const toml::table& t, std::string_view key);
    bool data_file(const toml::table& t, std::string_view key, std::string& full, std::string& display);
    void resolve(const PendingExpr& p, const Scope& s);
    void resolve_all_model();

    std::string path_;
    fs::path dir_;
    TomlCtx ctx_;
    PartInfo info_;
    std::string quality_;

    // collected names
    std::map<std::string, std::string> value_names_;  // expression namespace
    std::set<std::string> pin_set_;
    std::vector<std::string> pin_roles_;
    std::set<std::string> internal_nodes_;
    std::set<std::string> components_;               // elements, outputs, branches
    std::map<std::string, std::string> branch_mode_;  // branch -> mode
    std::map<std::string, std::string> branch_unit_;
    std::set<std::string> curves_;
    std::set<std::string> states_;
    std::set<std::string> inputs_;
    std::set<std::string> features_;
    std::vector<std::pair<std::string, NamedLoc>> feature_refs_;  // (key, ref)
    std::vector<PendingExpr> model_exprs_;
    std::set<std::string> nodes_used_;
    bool has_sm_ = false;
    int test_count_ = 0;
};

// ---------------------------------------------------------------- helpers

void PartChecker::add_expr(const toml::node& n, const std::string& text, std::size_t offset, ExprKind kind,
                           const std::string& what) {
    Loc l = ctx_.at(n);
    l.col += 1 + static_cast<int>(offset);  // skip the opening quote
    model_exprs_.push_back({text, l, kind, what});
}

Val PartChecker::value(const toml::table& t, std::string_view key, const std::string& unit, bool allow_expr,
                       const std::string& where, ExprKind kind) {
    Val v;
    auto n = t.get(key);
    if (!n) return v;
    v.present = true;
    const std::string k(key);
    if (n->is_integer() || n->is_floating_point()) {
        v.ok = true;
        v.num = n->value<double>().value_or(0.0);
        return v;
    }
    auto s = n->as_string();
    if (!s) return v;  // type error reported by keys()
    const std::string text = s->get();
    const std::string tr = trim(text);
    if (!tr.empty() && tr.front() == '{') {
        if (!allow_expr) {
            ctx_.report("E0006", ctx_.at(*n), "`" + k + "` in " + where + " must be a number or quantity, not an expression",
                        "write a fixed value such as \"5 V\"", ctx_.value_len(*n));
            return v;
        }
        if (tr.back() != '}') {
            ctx_.report("E0080", ctx_.at(*n), "expression in `" + k + "` starts with `{` but does not end with `}`",
                        "write the whole value as \"{expression}\"", ctx_.value_len(*n));
            return v;
        }
        const std::size_t open = text.find('{');
        const std::size_t close = text.rfind('}');
        v.ok = true;
        v.is_expr = true;
        add_expr(*n, text.substr(open + 1, close - open - 1), open + 1, kind, "`" + k + "` in " + where);
        return v;
    }
    QuantityResult q = parse_quantity(text);
    if (q.error == QuantityError::Malformed) {
        ctx_.report("E0032", ctx_.at(*n), "`" + text + "` is not a valid quantity",
                    "write a number with optional SI prefix and unit, e.g. \"4.7 kOhm\" or \"100 mA\"" +
                        std::string(allow_expr ? ", or an expression in braces such as \"{2 * r}\"" : ""),
                    ctx_.value_len(*n));
        return v;
    }
    if (q.error == QuantityError::UnknownUnit) {
        Loc l = ctx_.at(*n);
        l.col += 1 + q.bad_offset;
        std::string hint = suggest(q.bad_unit, known_units());
        if (!hint.empty()) hint = "did you mean `" + hint + "`?";
        else if (!unit.empty() && unit != "*") hint = "the unit of `" + k + "` is `" + unit + "`";
        else hint = "see the unit list in SPEC.md section 5.3";
        ctx_.report("E0030", l, "unknown unit `" + q.bad_unit + "` in `" + text + "`", hint,
                    static_cast<int>(q.bad_unit.size()));
        return v;
    }
    if (!q.q.unit.empty() && unit != "*" && q.q.unit != unit) {
        std::string hint = "`" + k + "` is in `" + (unit.empty() ? std::string("1") : unit) + "`";
        if (unit == "Ohm" && q.q.unit == "m") hint += "; for milliohm write \"mOhm\"";
        ctx_.report("E0031", ctx_.at(*n),
                    "`" + text + "` is in `" + q.q.unit + "`, but `" + k + "` in " + where + " expects `" +
                        (unit.empty() ? std::string("1") : unit) + "`",
                    hint, ctx_.value_len(*n));
        return v;
    }
    v.ok = true;
    v.num = q.q.value;
    v.unit = q.q.unit;
    return v;
}

void PartChecker::expression(const toml::table& t, std::string_view key, const std::string& where, ExprKind kind) {
    auto n = t.get(key);
    if (!n) return;
    auto s = n->as_string();
    if (!s) return;
    std::string text = s->get();
    std::size_t offset = 0;
    const std::string tr = trim(text);
    if (!tr.empty() && tr.front() == '{' && tr.back() == '}') {
        // Braces are optional here; accept them.
        offset = text.find('{') + 1;
        text = text.substr(offset, text.rfind('}') - offset);
    }
    add_expr(*n, text, offset, kind, "`" + std::string(key) + "` in " + where);
}

bool PartChecker::check_lower_name(const toml::table& t, const std::string& where, std::string& out,
                                   bool reserved_check) {
    auto n = t.get("name");
    auto s = n ? n->as_string() : nullptr;
    if (!s) return false;
    out = s->get();
    if (!is_lower_ident(out)) {
        ctx_.report("E0012", ctx_.at(*n), "name `" + out + "` of " + where + " is not valid",
                    "use lower-case letters, digits and `_`, starting with a letter, e.g. `" +
                        to_lower(std::regex_replace(out, std::regex("[^A-Za-z0-9_]"), "_")) + "`",
                    ctx_.value_len(*n));
        return true;  // keep the name to avoid follow-up errors
    }
    if (reserved_check && is_reserved_name(out)) {
        ctx_.report("E0012", ctx_.at(*n), "name `" + out + "` of " + where + " is reserved",
                    "built-in variables and function names cannot be used as names; choose e.g. `" + out + "_value`",
                    ctx_.value_len(*n));
    }
    return true;
}

bool PartChecker::define_value_name(const std::string& name, const std::string& kind, const toml::table& t) {
    auto it = value_names_.find(name);
    if (it != value_names_.end()) {
        auto n = t.get("name");
        ctx_.report("E0013", n ? ctx_.at(*n) : ctx_.at(t),
                    "name `" + name + "` is already used by a " + it->second,
                    "parameters, signals, variables, inputs and curves share one namespace; rename one of them",
                    n ? ctx_.value_len(*n) : 1);
        return false;
    }
    value_names_[name] = kind;
    return true;
}

void PartChecker::node_ref(const toml::node& n, const std::string& name, const std::string& where) {
    nodes_used_.insert(name);
    if (pin_set_.count(name) || internal_nodes_.count(name) || name == "0") return;
    std::vector<std::string> cands(pin_set_.begin(), pin_set_.end());
    cands.insert(cands.end(), internal_nodes_.begin(), internal_nodes_.end());
    ctx_.report("E0040", ctx_.at(n), "unknown node `" + name + "` in " + where,
                suggestion_hint(name, cands, "nodes (pins, internal_nodes, `0`)"), ctx_.value_len(n));
}

void PartChecker::check_nodes_array(const toml::table& t, std::string_view key, const std::string& where, int nmin,
                                    int nmax, const char* node_names) {
    auto n = t.get(key);
    if (!n) return;
    auto a = n->as_array();
    if (!a) return;
    for (auto&& e : *a) {
        if (auto s = e.as_string()) node_ref(e, s->get(), where);
        else
            ctx_.report("E0006", ctx_.at(e), "node names in " + where + " must be strings, found " + TomlCtx::type_name(e),
                        "write node names in quotes, e.g. \"0\"", ctx_.value_len(e));
    }
    const int cnt = static_cast<int>(a->size());
    if (cnt < nmin || cnt > nmax) {
        std::string expect = nmin == nmax ? std::to_string(nmin) : std::to_string(nmin) + " or " + std::to_string(nmax);
        ctx_.report("E0042", ctx_.at(*n),
                    where + " needs " + expect + " nodes, found " + std::to_string(cnt),
                    std::string("node order: ") + node_names, ctx_.value_len(*n));
    }
}

void PartChecker::feature_ref(const toml::table& t, std::string_view key) {
    auto n = t.get(key);
    if (!n) return;
    if (auto s = n->as_string()) feature_refs_.push_back({std::string(key), {s->get(), ctx_.at(*n), ctx_.value_len(*n)}});
}

bool PartChecker::data_file(const toml::table& t, std::string_view key, std::string& full, std::string& display) {
    auto n = t.get(key);
    if (!n) return false;
    auto s = n->as_string();
    if (!s) return false;
    const std::string rel = s->get();
    bool bad = rel.empty() || rel[0] == '/' || rel.find('\\') != std::string::npos ||
               (rel.size() > 1 && rel[1] == ':');
    for (const auto& seg : split(rel, '/'))
        if (seg == ".." ) bad = true;
    if (bad) {
        ctx_.report("E0095", ctx_.at(*n), "data file path `" + rel + "` is not allowed",
                    "use a relative path with `/` that stays inside the directory of the part file, e.g. "
                    "\"data/curve.csv\"",
                    ctx_.value_len(*n));
        return false;
    }
    fs::path p = dir_ / fs::path(rel);
    full = p.string();
    display = display_path((fs::path(ctx_.file).parent_path() / fs::path(rel)).generic_string());
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        ctx_.report("E0090", ctx_.at(*n), "data file `" + rel + "` not found",
                    "the path is relative to the part file; expected it at `" + display + "`", ctx_.value_len(*n));
        return false;
    }
    if (std::find(info_.data_files.begin(), info_.data_files.end(), rel) == info_.data_files.end())
        info_.data_files.push_back(rel);
    return true;
}

// ---------------------------------------------------------------- run

PartInfo PartChecker::run() {
    auto text = read_file(path_);
    if (!text) {
        ctx_.report("E0002", {ctx_.file, 0, 0}, "cannot read file", "check that the path exists and is readable");
        return info_;
    }
    info_.content = normalize_newlines(*text);
    ctx_.diags.add_source(ctx_.file, *text);
    toml::table root;
    try {
        root = toml::parse(*text, path_);
    } catch (const toml::parse_error& e) {
        Loc l = ctx_.at(e.source());
        ctx_.report("E0001", l, "TOML syntax error: " + std::string(e.description()),
                    "see https://toml.io/en/v1.0.0 for the syntax; strings need double quotes");
        return info_;
    }

    // format_version
    if (auto fv = root.get("format_version")) {
        auto s = fv->as_string();
        if (!s || s->get() != "0.1") {
            ctx_.report("E0003", ctx_.at(*fv),
                        "unsupported format_version " + (s ? "`" + s->get() + "`" : TomlCtx::type_name(*fv)),
                        "this validator reads format_version = \"0.1\"", ctx_.value_len(*fv));
        }
    } else {
        ctx_.report("E0003", {ctx_.file, 1, 1}, "missing `format_version`",
                    "add `format_version = \"0.1\"` at the top of the file");
    }

    ctx_.keys(root, "the part file",
              {{"format_version", VT::Any},
               {"part", VT::Table, true},
               {"pins", VT::ArrayOfTables, true},
               {"parameters", VT::ArrayOfTables},
               {"features", VT::ArrayOfTables},
               {"curves", VT::ArrayOfTables},
               {"model", VT::Table, true},
               {"tests", VT::ArrayOfTables}},
              {{"library", "this looks like a library manifest; manifests must be named `library.toml`"},
               {"parts", "this looks like a library manifest; manifests must be named `library.toml`"}});

    const toml::table* part = root["part"].as_table();
    if (part) check_part(*part);
    if (auto a = root["pins"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) check_pins(*a);
    if (auto a = root["features"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) check_features(*a);
    if (auto a = root["parameters"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) check_parameters(*a);
    if (auto a = root["curves"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) check_curves(*a);

    const std::size_t errors_before_model = ctx_.diags.errors();
    if (auto m = root["model"].as_table()) check_model(*m);
    resolve_all_model();

    // feature references (parameters, elements, outputs, branches)
    for (const auto& [key, ref] : feature_refs_) {
        if (!features_.count(ref.name)) {
            std::vector<std::string> c(features_.begin(), features_.end());
            ctx_.report("E0060", ref.loc, "unknown feature `" + ref.name + "`",
                        features_.empty() ? "declare it with [[features]] name = \"" + ref.name + "\""
                                          : suggestion_hint(ref.name, c, "features"),
                        ref.len);
        }
    }

    // Unused pins (only if the model itself is consistent)
    if (root["model"].as_table() && ctx_.diags.errors() == errors_before_model) {
        auto pins = root["pins"].as_array();
        if (pins) {
            for (auto&& p : *pins) {
                auto pt = p.as_table();
                if (!pt) continue;
                auto nm = ctx_.str(*pt, "name");
                if (nm && !nodes_used_.count(*nm)) {
                    auto nn = pt->get("name");
                    ctx_.report("W0041", ctx_.at(*nn), "pin `" + *nm + "` is not used by the model",
                                "connect it in the model, or remove the pin if the part does not have it",
                                ctx_.value_len(*nn));
                }
            }
        }
    }

    if (auto a = root["tests"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) check_tests(*a);
    if (part) check_quality(*part);

    info_.features.assign(features_.begin(), features_.end());
    return info_;
}

// ---------------------------------------------------------------- [part]

void PartChecker::check_part(const toml::table& t) {
    ctx_.keys(t, "[part]",
              {{"id", VT::String, true},
               {"name", VT::String, true},
               {"version", VT::String, true},
               {"category", VT::String, true},
               {"description", VT::String, true},
               {"compatible", VT::ArrayOfStrings},
               {"author", VT::String, true},
               {"license", VT::String, true},
               {"quality", VT::String, true},
               {"tags", VT::ArrayOfStrings},
               {"source", VT::Table, true}},
              {{"pins", "pins are top-level: write [[pins]], not [[part.pins]] or pins inside [part]"},
               {"parameters", "parameters are top-level: write [[parameters]]"},
               {"model", "the model is top-level: write [model]"}});

    if (auto n = t.get("id"); n && n->is_string()) {
        const std::string id = n->as_string()->get();
        info_.id = id;
        info_.id_loc = ctx_.at(*n);
        info_.id_len = ctx_.value_len(*n);
        info_.loaded = true;
        if (!is_part_id(id)) {
            std::string fixed = to_lower(std::regex_replace(id, std::regex("[^A-Za-z0-9_]+"), "_"));
            if (!starts_with(fixed, "generic_")) fixed = "generic_" + fixed;
            std::string hint = "part ids match ^generic_[a-z0-9_]+$, e.g. `" + fixed + "`";
            if (std::any_of(id.begin(), id.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)); }))
                hint += "; real part numbers belong in `compatible`";
            ctx_.report("E0010", info_.id_loc, "invalid part id `" + id + "`", hint, info_.id_len);
        }
    }
    if (auto n = t.get("version"); n && n->is_string()) {
        info_.version = n->as_string()->get();
        if (!parse_semver(info_.version))
            ctx_.report("E0014", ctx_.at(*n), "invalid version `" + info_.version + "`",
                        "use a semantic version MAJOR.MINOR.PATCH, e.g. \"0.1.0\"", ctx_.value_len(*n));
    }
    if (auto n = t.get("category"); n && n->is_string()) {
        const std::string c = n->as_string()->get();
        if (!is_category(c))
            ctx_.report("E0012", ctx_.at(*n), "category `" + c + "` is not valid",
                        "use lower-case words separated by `/`, e.g. \"passive/resistor\"", ctx_.value_len(*n));
    }
    if (auto n = t.get("license"); n && n->is_string()) {
        const std::string l = n->as_string()->get();
        if (std::find(kAcceptedPartLicenses.begin(), kAcceptedPartLicenses.end(), l) == kAcceptedPartLicenses.end()) {
            std::string s = suggest(l, kAcceptedPartLicenses);
            ctx_.report("E0021", ctx_.at(*n), "licence `" + l + "` is not accepted for part data",
                        s.empty() ? "part data must be licensed as `CC-BY-4.0` or `CC0-1.0`"
                                  : "did you mean `" + s + "`? Part data must be `CC-BY-4.0` or `CC0-1.0`",
                        ctx_.value_len(*n));
        }
    }
    if (auto q = ctx_.choice(t, "quality", kQualities, "[part]")) quality_ = *q;
    if (auto n = t.get("source"); n && n->is_table()) check_source(*n->as_table(), *n);
}

void PartChecker::check_source(const toml::table& s, const toml::node& node) {
    (void)node;
    ctx_.keys(s, "[part.source]",
              {{"kind", VT::String},
               {"title", VT::String},
               {"vendor", VT::String},
               {"document_id", VT::String},
               {"revision", VT::String},
               {"section", VT::String},
               {"date", VT::String},
               {"url", VT::String},
               {"model_license", VT::String},
               {"notes", VT::String}});
    auto missing = [&](const char* key, const std::string& why) {
        auto v = ctx_.str(s, key);
        if (v && !trim(*v).empty()) return;
        ctx_.report("E0020", v ? ctx_.key_at(s, key) : ctx_.at(s), "source is missing `" + std::string(key) + "`" + why,
                    std::string("add `") + key + " = \"...\"` to [part.source]; see SPEC.md section 4.3");
    };
    if (!s.contains("kind")) missing("kind", "");
    missing("title", "");
    auto kind = ctx_.choice(s, "kind", kSourceKinds, "[part.source]");
    if (kind) {
        if (*kind == "datasheet" || *kind == "application_note" || *kind == "spice_model")
            missing("vendor", " (required for kind `" + *kind + "`)");
        if (*kind == "measurement") missing("date", " (required for kind `measurement`)");
        if (*kind == "spice_model") {
            auto ml = ctx_.str(s, "model_license");
            if (!ml) missing("model_license", " (required for kind `spice_model`)");
            else if (std::find(kOpenModelLicenses.begin(), kOpenModelLicenses.end(), *ml) == kOpenModelLicenses.end()) {
                auto n = s.get("model_license");
                ctx_.report("E0021", ctx_.at(*n), "SPICE model licence `" + *ml + "` is not an accepted open licence",
                            "only openly licensed models may be converted (" + join_quoted(kOpenModelLicenses) +
                                "); vendor models with proprietary terms must not be used",
                            ctx_.value_len(*n));
            }
        } else if (s.contains("model_license")) {
            ctx_.report("E0004", ctx_.key_at(s, "model_license"),
                        "key `model_license` is not allowed in [part.source] with kind `" + *kind + "`",
                        "`model_license` is only used with kind = \"spice_model\"", 13);
        }
    }
    if (auto d = s.get("date"); d && d->is_string() && !is_iso_date(d->as_string()->get()))
        ctx_.report("E0006", ctx_.at(*d), "`date` in [part.source] must be an ISO date YYYY-MM-DD",
                    "e.g. \"2026-11-27\"", ctx_.value_len(*d));
}

void PartChecker::check_quality(const toml::table& part) {
    const toml::table* src = part["source"].as_table();
    auto qn = part.get("quality");
    if (quality_ == "vendor-verified") {
        ctx_.report("E0022", ctx_.at(*qn), "quality `vendor-verified` cannot be used in format 0.1",
                    "vendor verification needs signatures (roadmap stage 5); use `datasheet` or `measured`",
                    ctx_.value_len(*qn));
        return;
    }
    if (quality_ == "datasheet" || quality_ == "measured") {
        std::vector<std::string> missing;
        std::string kind = src ? ctx_.str(*src, "kind").value_or("") : "";
        if (quality_ == "datasheet") {
            if (kind != "datasheet" && kind != "application_note" && kind != "standard")
                missing.push_back("source.kind = \"datasheet\", \"application_note\" or \"standard\"");
            if (kind != "standard" && !(src && ctx_.str(*src, "vendor"))) missing.push_back("source.vendor");
            if (!(src && ctx_.str(*src, "revision"))) missing.push_back("source.revision");
            if (!(src && ctx_.str(*src, "section"))) missing.push_back("source.section");
        } else {
            if (kind != "measurement") missing.push_back("source.kind = \"measurement\"");
            if (!(src && ctx_.str(*src, "date"))) missing.push_back("source.date");
        }
        if (test_count_ == 0) missing.push_back("at least one [[tests]] entry");
        if (!missing.empty()) {
            std::string list;
            for (std::size_t i = 0; i < missing.size(); ++i) list += (i ? ", " : "") + missing[i];
            ctx_.report("E0022", ctx_.at(*qn), "quality `" + quality_ + "` requires " + list,
                        "add the missing information, or set quality = \"draft\"", ctx_.value_len(*qn));
        }
        return;
    }
    if (test_count_ == 0 && !quality_.empty()) {
        ctx_.report("W0110", info_.id_loc.line ? info_.id_loc : ctx_.at(part), "part has no reference tests",
                    "add at least one [[tests]] entry with an expected value (SPEC.md section 14)", info_.id_len);
    }
}

// ---------------------------------------------------------------- pins

void PartChecker::check_pins(const toml::array& a) {
    if (a.empty()) {
        ctx_.report("E0005", ctx_.at(a), "a part needs at least one pin", "add a [[pins]] entry");
        return;
    }
    std::vector<std::pair<const toml::table*, std::string>> refs;
    int idx = 0;
    for (auto&& node : a) {
        ++idx;
        const toml::table& p = *node.as_table();
        const std::string where = "[[pins]] entry " + std::to_string(idx);
        ctx_.keys(p, where,
                  {{"name", VT::String, true},
                   {"role", VT::String, true},
                   {"description", VT::String},
                   {"reference", VT::String},
                   {"abs_max_voltage", VT::Value},
                   {"abs_min_voltage", VT::Value},
                   {"abs_max_current", VT::Value},
                   {"driver", VT::String}});
        std::string name;
        if (auto n = p.get("name"); n && n->is_string()) {
            name = n->as_string()->get();
            if (!is_pin_name(name))
                ctx_.report("E0012", ctx_.at(*n), "pin name `" + name + "` is not valid",
                            "pin names use letters, digits and `_` only, e.g. `vdd`, `out_a`, `1`",
                            ctx_.value_len(*n));
            if (!pin_set_.insert(name).second)
                ctx_.report("E0013", ctx_.at(*n), "pin `" + name + "` is defined twice", "pin names must be unique",
                            ctx_.value_len(*n));
            else
                info_.pins.push_back(name);
        }
        const std::string pw = name.empty() ? where : "pin `" + name + "`";
        auto role = ctx_.choice(p, "role", kRoles, pw);
        pin_roles_.push_back(role.value_or(""));
        ctx_.choice(p, "driver", kDrivers, pw);
        if (role && p.contains("driver") && *role != "output" && *role != "bidirectional")
            ctx_.report("E0004", ctx_.key_at(p, "driver"), "key `driver` is not allowed for " + pw + " with role `" + *role + "`",
                        "`driver` describes outputs; use it only with role output or bidirectional", 6);
        Val vmax = value(p, "abs_max_voltage", "V", false, pw);
        Val vmin = value(p, "abs_min_voltage", "V", false, pw);
        Val imax = value(p, "abs_max_current", "A", false, pw);
        if (imax.ok && imax.num < 0)
            ctx_.report("E0033", ctx_.at(*p.get("abs_max_current")), "`abs_max_current` of " + pw + " is negative",
                        "write the magnitude of the current limit", ctx_.value_len(*p.get("abs_max_current")));
        if (vmax.ok && vmin.ok && vmin.num > vmax.num)
            ctx_.report("E0034", ctx_.at(*p.get("abs_min_voltage")),
                        "`abs_min_voltage` (" + fmt(vmin.num) + " V) of " + pw + " is greater than `abs_max_voltage` (" +
                            fmt(vmax.num) + " V)",
                        "check the order of the two limits", ctx_.value_len(*p.get("abs_min_voltage")));
        if (p.contains("reference")) refs.push_back({&p, name});
    }
    for (const auto& [p, name] : refs) {
        auto n = p->get("reference");
        auto s = n->as_string();
        if (!s) continue;
        const std::string r = s->get();
        if (r == name) {
            ctx_.report("E0040", ctx_.at(*n), "pin `" + name + "` cannot be its own reference",
                        "name a ground or supply pin", ctx_.value_len(*n));
        } else if (!pin_set_.count(r)) {
            ctx_.report("E0040", ctx_.at(*n), "unknown pin `" + r + "` in `reference` of pin `" + name + "`",
                        suggestion_hint(r, info_.pins, "pins"), ctx_.value_len(*n));
        }
    }
}

// ---------------------------------------------------------------- parameters

void PartChecker::check_parameters(const toml::array& a) {
    int idx = 0;
    for (auto&& node : a) {
        ++idx;
        const toml::table& p = *node.as_table();
        std::string where = "[[parameters]] entry " + std::to_string(idx);
        ctx_.keys(p, where,
                  {{"name", VT::String, true},
                   {"type", VT::String, true},
                   {"unit", VT::String},
                   {"default", VT::Any, true},
                   {"min", VT::Value},
                   {"max", VT::Value},
                   {"adjustable", VT::Bool},
                   {"choices", VT::ArrayOfStrings},
                   {"tolerance", VT::Table},
                   {"feature", VT::String},
                   {"description", VT::String}});
        std::string name;
        if (check_lower_name(p, where, name, true)) {
            where = "parameter `" + name + "`";
            define_value_name(name, "parameter", p);
        }
        ParamInfo pi;
        pi.name = name;
        pi.adjustable = ctx_.boolean(p, "adjustable").value_or(true);
        auto type = ctx_.choice(p, "type", kParamTypes, where);
        feature_ref(p, "feature");
        if (!type) {
            if (!name.empty()) info_.params[name] = pi;
            continue;
        }
        pi.type = *type;
        const bool numeric = *type == "real" || *type == "integer";
        std::string unit;
        if (auto un = p.get("unit"); un && un->is_string()) {
            unit = un->as_string()->get();
            if (!numeric) {
                ctx_.report("E0004", ctx_.key_at(p, "unit"), "key `unit` is not allowed for " + where + " of type `" + *type + "`",
                            "only real and integer parameters have a unit", 4);
            } else if (!is_known_unit(unit)) {
                ctx_.report("E0030", ctx_.at(*un), "unknown unit `" + unit + "` for " + where,
                            suggestion_hint(unit, known_units(), "units", 10), ctx_.value_len(*un));
                unit = "*";
            }
        } else if (numeric && !p.contains("unit")) {
            ctx_.report("E0005", ctx_.at(p), "missing required key `unit` in " + where,
                        "real and integer parameters need a unit; use unit = \"1\" for dimensionless values");
            unit = "*";
        }
        pi.unit = unit;
        for (const char* k : {"min", "max"})
            if (!numeric && p.contains(k))
                ctx_.report("E0004", ctx_.key_at(p, k), "key `" + std::string(k) + "` is not allowed for " + where + " of type `" + *type + "`",
                            "only real and integer parameters have limits", 3);
        if (*type == "enum") {
            pi.choices = ctx_.strings(p, "choices");
            if (!p.contains("choices"))
                ctx_.report("E0005", ctx_.at(p), "missing required key `choices` in " + where,
                            "enum parameters list their allowed values, e.g. choices = [\"x7r\", \"c0g\"]");
        } else if (p.contains("choices")) {
            ctx_.report("E0004", ctx_.key_at(p, "choices"), "key `choices` is not allowed for " + where + " of type `" + *type + "`",
                        "`choices` is only used with type = \"enum\"", 7);
        }

        // default
        auto dn = p.get("default");
        Val dv;
        if (dn) {
            if (*type == "real") {
                if (!TomlCtx::matches(*dn, VT::Value))
                    ctx_.report("E0006", ctx_.at(*dn), "`default` of " + where + " must be a number or quantity, found " + TomlCtx::type_name(*dn),
                                "e.g. default = \"10 kOhm\"", ctx_.value_len(*dn));
                else
                    dv = value(p, "default", unit, false, where);
            } else if (*type == "integer") {
                if (!dn->is_integer())
                    ctx_.report("E0006", ctx_.at(*dn), "`default` of " + where + " must be an integer, found " + TomlCtx::type_name(*dn),
                                "write a whole number without quotes, e.g. default = 3", ctx_.value_len(*dn));
                else
                    dv = value(p, "default", unit, false, where);
            } else if (*type == "boolean") {
                if (!dn->is_boolean())
                    ctx_.report("E0006", ctx_.at(*dn), "`default` of " + where + " must be a boolean, found " + TomlCtx::type_name(*dn),
                                "write true or false without quotes", ctx_.value_len(*dn));
            } else if (*type == "string") {
                if (!dn->is_string())
                    ctx_.report("E0006", ctx_.at(*dn), "`default` of " + where + " must be a string, found " + TomlCtx::type_name(*dn),
                                "put the value in quotes", ctx_.value_len(*dn));
            } else if (*type == "enum") {
                if (!dn->is_string())
                    ctx_.report("E0006", ctx_.at(*dn), "`default` of " + where + " must be a string, found " + TomlCtx::type_name(*dn),
                                "use one of the choices in quotes", ctx_.value_len(*dn));
                else if (p.contains("choices"))
                    ctx_.choice(p, "default", pi.choices, where);
            }
        }
        Val mn, mx;
        if (numeric) {
            mn = value(p, "min", unit, false, where);
            mx = value(p, "max", unit, false, where);
            if (mn.ok && mx.ok && mn.num > mx.num)
                ctx_.report("E0034", ctx_.at(*p.get("min")), "`min` (" + fmt(mn.num) + ") of " + where + " is greater than `max` (" + fmt(mx.num) + ")",
                            "swap the two limits", ctx_.value_len(*p.get("min")));
            else if (dv.ok && ((mn.ok && dv.num < mn.num) || (mx.ok && dv.num > mx.num)))
                ctx_.report("E0033", ctx_.at(*dn), "`default` (" + fmt(dv.num) + ") of " + where + " is outside [" +
                                                       (mn.ok ? fmt(mn.num) : "-inf") + ", " + (mx.ok ? fmt(mx.num) : "inf") + "]",
                            "change the default or the limits", ctx_.value_len(*dn));
        }
        // tolerance
        if (auto tn = p.get("tolerance"); tn && tn->is_table()) {
            const toml::table& tt = *tn->as_table();
            const std::string tw = "`tolerance` of " + where;
            ctx_.keys(tt, tw, {{"relative", VT::Number}, {"absolute", VT::Value}, {"distribution", VT::String}, {"sigma", VT::Number}});
            if (!numeric) {
                ctx_.report("E0035", ctx_.at(*tn), tw + " is not allowed for type `" + *type + "`",
                            "only real and integer parameters can have a tolerance", 1);
            } else if (tt.contains("relative") == tt.contains("absolute")) {
                ctx_.report("E0072", ctx_.at(*tn), tw + " needs exactly one of `relative` and `absolute`",
                            "e.g. tolerance = { relative = 0.05 } for 5 %");
            } else {
                if (auto r = tt.get("relative"); r && (r->is_integer() || r->is_floating_point())) {
                    double rv = r->value<double>().value_or(0);
                    if (rv < 0 || rv >= 1)
                        ctx_.report("E0035", ctx_.at(*r), "relative tolerance " + fmt(rv) + " of " + where + " is outside [0, 1)",
                                    "write a fraction: 0.05 means 5 %", ctx_.value_len(*r));
                }
                Val av = value(tt, "absolute", unit, false, tw);
                if (av.ok && av.num < 0)
                    ctx_.report("E0035", ctx_.at(*tt.get("absolute")), "absolute tolerance of " + where + " is negative",
                                "write the magnitude", ctx_.value_len(*tt.get("absolute")));
            }
            auto dist = ctx_.choice(tt, "distribution", {"uniform", "normal"}, tw);
            if (auto sg = tt.get("sigma")) {
                double sv = sg->value<double>().value_or(0);
                if (dist.value_or("uniform") != "normal")
                    ctx_.report("E0035", ctx_.at(*sg), "`sigma` in " + tw + " needs distribution = \"normal\"",
                                "remove `sigma` or set distribution = \"normal\"", ctx_.value_len(*sg));
                else if (sv <= 0)
                    ctx_.report("E0035", ctx_.at(*sg), "`sigma` in " + tw + " must be positive",
                                "sigma = 3 means the tolerance covers 3 standard deviations", ctx_.value_len(*sg));
            }
        }
        if (!name.empty()) info_.params[name] = pi;
    }
}

// ---------------------------------------------------------------- features

void PartChecker::check_features(const toml::array& a) {
    std::map<std::string, std::vector<NamedLoc>> requires_;
    std::map<std::string, Loc> floc;
    int idx = 0;
    for (auto&& node : a) {
        ++idx;
        const toml::table& f = *node.as_table();
        std::string where = "[[features]] entry " + std::to_string(idx);
        ctx_.keys(f, where,
                  {{"name", VT::String, true}, {"description", VT::String, true}, {"default", VT::Bool}, {"requires", VT::ArrayOfStrings}});
        std::string name;
        if (!check_lower_name(f, where, name, false)) continue;
        if (!features_.insert(name).second) {
            ctx_.report("E0013", ctx_.at(*f.get("name")), "feature `" + name + "` is defined twice",
                        "feature names must be unique", ctx_.value_len(*f.get("name")));
            continue;
        }
        floc[name] = ctx_.at(*f.get("name"));
        if (auto r = f["requires"].as_array())
            for (auto&& e : *r)
                if (auto s = e.as_string()) requires_[name].push_back({s->get(), ctx_.at(e), ctx_.value_len(e)});
    }
    for (const auto& [name, reqs] : requires_)
        for (const auto& r : reqs)
            if (!features_.count(r.name)) {
                std::vector<std::string> c(features_.begin(), features_.end());
                ctx_.report("E0060", r.loc, "feature `" + name + "` requires unknown feature `" + r.name + "`",
                            suggestion_hint(r.name, c, "features"), r.len);
            }
    // cycles
    std::map<std::string, int> state;
    std::set<std::string> reported;
    std::function<bool(const std::string&, std::vector<std::string>&)> dfs = [&](const std::string& f,
                                                                                std::vector<std::string>& stack) {
        state[f] = 1;
        stack.push_back(f);
        for (const auto& r : requires_[f]) {
            if (!features_.count(r.name)) continue;
            if (state[r.name] == 1) {
                auto it = std::find(stack.begin(), stack.end(), r.name);
                std::string chain;
                for (; it != stack.end(); ++it) chain += "`" + *it + "` -> ";
                chain += "`" + r.name + "`";
                if (reported.insert(r.name).second)
                    ctx_.report("E0061", r.loc, "feature requirements form a cycle: " + chain,
                                "remove one of the `requires` entries", r.len);
                return true;
            }
            if (state[r.name] == 0 && dfs(r.name, stack)) return true;
        }
        stack.pop_back();
        state[f] = 2;
        return false;
    };
    for (const auto& f : features_) {
        std::vector<std::string> stack;
        if (state[f] == 0) dfs(f, stack);
    }
}

// ---------------------------------------------------------------- curves

void PartChecker::check_curves(const toml::array& a) {
    int idx = 0;
    for (auto&& node : a) {
        ++idx;
        const toml::table& c = *node.as_table();
        std::string where = "[[curves]] entry " + std::to_string(idx);
        ctx_.keys(c, where,
                  {{"name", VT::String, true},
                   {"file", VT::String, true},
                   {"x", VT::String, true},
                   {"y", VT::String, true},
                   {"x_unit", VT::String, true},
                   {"y_unit", VT::String, true},
                   {"interpolation", VT::String},
                   {"extrapolation", VT::String},
                   {"description", VT::String}});
        std::string name;
        if (check_lower_name(c, where, name, true)) {
            where = "curve `" + name + "`";
            if (define_value_name(name, "curve", c)) curves_.insert(name);
        }
        ctx_.choice(c, "interpolation", {"linear", "step", "cubic"}, where);
        ctx_.choice(c, "extrapolation", {"clamp", "linear", "zero", "error"}, where);
        for (const char* uk : {"x_unit", "y_unit"})
            if (auto u = c.get(uk); u && u->is_string() && !is_known_unit(u->as_string()->get()))
                ctx_.report("E0030", ctx_.at(*u), "unknown unit `" + u->as_string()->get() + "` in " + where,
                            suggestion_hint(u->as_string()->get(), known_units(), "units", 10), ctx_.value_len(*u));
        std::string full, disp;
        if (!data_file(c, "file", full, disp)) continue;
        const CsvTable& t = load_csv(full, disp, ctx_.diags);
        if (!t.ok) continue;
        for (const auto& [col_key, unit_key] : {std::pair{"x", "x_unit"}, std::pair{"y", "y_unit"}}) {
            auto cn = c.get(col_key);
            if (!cn || !cn->is_string()) continue;
            const std::string col = cn->as_string()->get();
            int ci = t.column_index(col);
            if (ci < 0) {
                ctx_.report("E0093", ctx_.at(*cn), "column `" + col + "` not found in `" + disp + "`",
                            suggestion_hint(col, t.names, "columns"), ctx_.value_len(*cn));
                continue;
            }
            if (std::string(col_key) == "x" && ci != 0) check_increasing(t, ci, disp, ctx_.diags);
            if (auto u = c.get(unit_key); u && u->is_string()) {
                const std::string us = u->as_string()->get();
                if (is_known_unit(us) && us != t.units[static_cast<std::size_t>(ci)])
                    ctx_.report("E0031", ctx_.at(*u),
                                "`" + std::string(unit_key) + "` is `" + us + "`, but column `" + col + "` of `" + disp +
                                    "` is in `" + t.units[static_cast<std::size_t>(ci)] + "`",
                                "make the curve declaration and the unit line of the file agree", ctx_.value_len(*u));
            }
        }
    }
}

// ---------------------------------------------------------------- model

void PartChecker::check_model(const toml::table& m) {
    auto kind = ctx_.choice(m, "kind", kModelKinds, "[model]");
    if (!m.contains("kind")) {
        ctx_.report("E0005", ctx_.at(m), "missing required key `kind` in [model]",
                    "add kind = \"primitive\", \"subcircuit\", \"behavioral\", \"load\" or \"noise_source\"");
        return;
    }
    if (!kind) return;

    const std::vector<std::string> all = {"element", "nodes",   "params",  "polarity", "elements",      "internal_nodes",
                                          "signals", "outputs", "branches", "noise",   "state_machine"};
    std::vector<KeySpec> spec = {{"kind", VT::String, true}, {"description", VT::String}};
    if (*kind == "primitive") {
        spec.insert(spec.end(), {{"element", VT::String, true}, {"nodes", VT::Array, true}, {"params", VT::Table}, {"polarity", VT::String}});
    } else if (*kind == "subcircuit") {
        spec.insert(spec.end(), {{"elements", VT::ArrayOfTables, true}, {"internal_nodes", VT::ArrayOfStrings}, {"signals", VT::ArrayOfTables}});
    } else if (*kind == "behavioral") {
        spec.insert(spec.end(), {{"outputs", VT::ArrayOfTables, true}, {"internal_nodes", VT::ArrayOfStrings},
                                 {"elements", VT::ArrayOfTables}, {"signals", VT::ArrayOfTables}});
    } else if (*kind == "load") {
        spec.insert(spec.end(), {{"branches", VT::ArrayOfTables, true}, {"internal_nodes", VT::ArrayOfStrings},
                                 {"elements", VT::ArrayOfTables}, {"signals", VT::ArrayOfTables},
                                 {"outputs", VT::ArrayOfTables}, {"state_machine", VT::Table}});
    } else {
        spec.insert(spec.end(), {{"noise", VT::Table, true}});
    }
    std::vector<Misplaced> misplaced;
    for (const auto& k : all) {
        if (std::any_of(spec.begin(), spec.end(), [&](const KeySpec& s) { return k == s.name; })) continue;
        std::vector<std::string> kinds;
        if (k == "element" || k == "nodes" || k == "params" || k == "polarity") kinds = {"primitive"};
        else if (k == "elements") kinds = {"subcircuit", "behavioral", "load"};
        else if (k == "internal_nodes" || k == "signals") kinds = {"subcircuit", "behavioral", "load"};
        else if (k == "outputs") kinds = {"behavioral", "load"};
        else if (k == "branches" || k == "state_machine") kinds = {"load"};
        else if (k == "noise") kinds = {"noise_source"};
        misplaced.push_back({k, "`" + k + "` is only used with model kind " + join_quoted(kinds) +
                                    "; this model has kind `" + *kind + "`"});
    }
    ctx_.keys(m, "[model] of kind `" + *kind + "`", spec, misplaced);

    if (*kind == "primitive") {
        check_primitive(m);
        return;
    }
    if (*kind == "noise_source") {
        if (auto n = m["noise"].as_table()) check_noise(*n);
        return;
    }
    // internal nodes
    if (auto a = m["internal_nodes"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfStrings)) {
        for (auto&& e : *a) {
            const std::string n = e.as_string()->get();
            if (!is_pin_name(n) || n == "0")
                ctx_.report("E0012", ctx_.at(e), "internal node name `" + n + "` is not valid",
                            "use letters, digits and `_`; `0` is the global ground and cannot be declared", ctx_.value_len(e));
            else if (pin_set_.count(n))
                ctx_.report("E0013", ctx_.at(e), "internal node `" + n + "` has the name of a pin",
                            "pins are nodes already; remove it from internal_nodes or rename it", ctx_.value_len(e));
            else if (!internal_nodes_.insert(n).second)
                ctx_.report("E0013", ctx_.at(e), "internal node `" + n + "` is listed twice", "", ctx_.value_len(e));
        }
    }
    has_sm_ = m.contains("state_machine") && m["state_machine"].is_table();
    if (auto a = m["signals"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) check_signals(*a);
    if (has_sm_) check_state_machine(*m["state_machine"].as_table());
    if (auto a = m["elements"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) {
        int idx = 0;
        for (auto&& e : *a) check_element(*e.as_table(), "[[model.elements]] entry " + std::to_string(++idx), false, nullptr, components_);
    }
    if (auto a = m["outputs"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) check_outputs(*a);
    if (auto a = m["branches"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) check_branches(*a, has_sm_);
    // state values refer to branches; checked after branches are known
    if (has_sm_) {
        const toml::table& sm = *m["state_machine"].as_table();
        if (auto states = sm["states"].as_array()) {
            for (auto&& st : *states) {
                auto stt = st.as_table();
                if (!stt) continue;
                auto vals = (*stt)["values"].as_table();
                if (!vals) continue;
                const std::string sname = ctx_.str(*stt, "name").value_or("?");
                for (auto&& [k, v] : *vals) {
                    const std::string b(k.str());
                    auto it = branch_mode_.find(b);
                    if (it == branch_mode_.end()) {
                        std::vector<std::string> c;
                        for (const auto& [bn, mode] : branch_mode_)
                            if (mode == "state_machine") c.push_back(bn);
                        ctx_.report("E0103", ctx_.at(k.source()), "state `" + sname + "` sets unknown branch `" + b + "`",
                                    suggestion_hint(b, c, "branches with mode state_machine"), static_cast<int>(b.size()));
                        continue;
                    }
                    if (it->second != "state_machine") {
                        ctx_.report("E0103", ctx_.at(k.source()),
                                    "state `" + sname + "` sets branch `" + b + "`, which has mode `" + it->second + "`",
                                    "only branches with mode = \"state_machine\" take their value from states",
                                    static_cast<int>(b.size()));
                        continue;
                    }
                    if (!TomlCtx::matches(v, VT::Value)) {
                        ctx_.report("E0006", ctx_.at(v), "value of `" + b + "` in state `" + sname + "` must be a number or string, found " + TomlCtx::type_name(v),
                                    "", ctx_.value_len(v));
                        continue;
                    }
                    value(*vals, b, branch_unit_[b], true, "state `" + sname + "`");
                }
            }
        }
    }
}

void PartChecker::check_primitive(const toml::table& m) {
    auto en = m.get("element");
    if (!en || !en->is_string()) return;
    const std::string type = en->as_string()->get();
    const PrimSpec* ps = find_prim(type);
    if (!ps || type == "B") {
        auto types = prim_types(false);
        std::string s = suggest(type, types);
        ctx_.report("E0007", ctx_.at(*en),
                    "`element` in [model] must be one of " + join_quoted(types) + ", found `" + type + "`",
                    type == "B" ? "a behavioral source needs kind = \"behavioral\" or a `B` element in a subcircuit"
                                : (s.empty() ? "see SPEC.md section 9.2" : "did you mean `" + s + "`?"),
                    ctx_.value_len(*en));
        return;
    }
    const std::string where = "[model] (`" + type + "`, " + ps->what + ")";
    check_nodes_array(m, "nodes", where, ps->nmin, ps->nmax, ps->node_names);
    check_prim_params(*ps, m.get("params"), where);
    if (!ps->polarities.empty()) {
        if (!m.contains("polarity"))
            ctx_.report("E0005", ctx_.at(m), "missing required key `polarity` in " + where,
                        "add polarity = " + join_quoted(ps->polarities) + " (one of them, in double quotes)");
        else
            ctx_.choice(m, "polarity", ps->polarities, where);
    } else if (m.contains("polarity")) {
        ctx_.report("E0004", ctx_.key_at(m, "polarity"), "key `polarity` is not allowed in " + where,
                    "`polarity` is only used with element Q or M", 8);
    }
}

void PartChecker::check_prim_params(const PrimSpec& ps, const toml::node* params, const std::string& where) {
    std::vector<std::string> names;
    for (const auto& p : ps.params) names.emplace_back(p.name);
    const toml::table* t = params ? params->as_table() : nullptr;
    if (t) {
        for (auto&& [k, v] : *t) {
            const std::string key(k.str());
            auto it = std::find_if(ps.params.begin(), ps.params.end(), [&](const PrimParam& p) { return key == p.name; });
            if (it == ps.params.end()) {
                std::string hint = suggestion_hint(key, names, "parameters");
                if (names.empty()) hint = "element `" + std::string(ps.type) + "` takes no parameters";
                ctx_.report("E0070", ctx_.at(k.source()),
                            "unknown parameter `" + key + "` for element `" + ps.type + "` (" + ps.what + ")", hint,
                            static_cast<int>(key.size()));
                continue;
            }
            if (!TomlCtx::matches(v, VT::Value)) {
                ctx_.report("E0006", ctx_.at(v), "`" + key + "` in " + where + " must be " + TomlCtx::vt_name(VT::Value) + ", found " + TomlCtx::type_name(v),
                            "", ctx_.value_len(v));
                continue;
            }
            value(*t, key, it->unit, true, where);
        }
    }
    for (const auto& p : ps.params) {
        if (p.required && !(t && t->contains(p.name))) {
            ctx_.report("E0071", params ? ctx_.at(*params) : Loc{ctx_.file, 0, 0},
                        "element `" + std::string(ps.type) + "` (" + ps.what + ") needs parameter `" + p.name + "` in " + where,
                        std::string("add ") + p.name + " = <value in " + p.unit + "> to `params`, e.g. params = { " + p.name + " = \"{my_parameter}\" }");
        }
    }
}

void PartChecker::check_element(const toml::table& e, const std::string& where_in, bool in_test,
                                std::set<std::string>* test_nodes, std::set<std::string>& names) {
    ctx_.keys(e, where_in,
              {{"name", VT::String, true},
               {"type", VT::String},
               {"part", VT::String},
               {"nodes", VT::Any, true},
               {"params", VT::Table},
               {"features", VT::Table},
               {"polarity", VT::String},
               {"source", VT::String},
               {"expr", VT::String},
               {"feature", VT::String},
               {"when_disabled", VT::String},
               {"description", VT::String}});
    std::string where = where_in;
    std::string name;
    if (auto n = e.get("name"); n && n->is_string()) {
        name = n->as_string()->get();
        if (!is_element_name(name))
            ctx_.report("E0012", ctx_.at(*n), "element name `" + name + "` is not valid",
                        "element names start with a letter, then letters, digits and `_`, e.g. `R1`", ctx_.value_len(*n));
        if (!names.insert(name).second)
            ctx_.report("E0013", ctx_.at(*n), "name `" + name + "` is used twice",
                        in_test ? "names of test elements and stimuli must be unique within a test"
                                : "elements, outputs and branches share one namespace",
                        ctx_.value_len(*n));
        where = "element `" + name + "`";
    }
    const bool has_type = e.contains("type"), has_part = e.contains("part");
    if (has_type == has_part) {
        ctx_.report("E0072", ctx_.at(e),
                    where + (has_type ? " sets both `type` and `part`" : " needs `type` or `part`"),
                    "use `type` for a primitive element (R, C, L, ...) or `part` for another library part, not both");
        return;
    }
    feature_ref(e, "feature");
    if (auto wd = ctx_.choice(e, "when_disabled", {"open", "short"}, where)) {
        if (!e.contains("feature"))
            ctx_.report("E0005", ctx_.key_at(e, "when_disabled"), where + " sets `when_disabled` but has no `feature`",
                        "add feature = \"...\" or remove when_disabled", 13);
        else if (*wd == "short")
            if (auto na = e["nodes"].as_array(); na && na->size() != 2)
                ctx_.report("E0042", ctx_.at(*na), where + " uses when_disabled = \"short\" but has " + std::to_string(na->size()) + " nodes",
                            "only two-terminal elements can be replaced by a short", ctx_.value_len(*na));
    }
    auto nodes = e.get("nodes");
    auto register_node = [&](const toml::node& n, const std::string& nm) {
        if (in_test) {
            if (!is_pin_name(nm))
                ctx_.report("E0012", ctx_.at(n), "node name `" + nm + "` is not valid", "use letters, digits and `_`",
                            ctx_.value_len(n));
            else if (test_nodes)
                test_nodes->insert(nm);
        } else {
            node_ref(n, nm, where);
        }
    };

    if (has_part) {
        auto pn = e.get("part");
        if (!pn->is_string()) return;
        PartRef ref;
        ref.element = name;
        ref.target = pn->as_string()->get();
        ref.loc = ctx_.at(*pn);
        ref.len = ctx_.value_len(*pn);
        for (const char* k : {"polarity", "source", "expr"})
            if (e.contains(k))
                ctx_.report("E0004", ctx_.key_at(e, k), "key `" + std::string(k) + "` is not allowed for " + where + ", which uses `part`",
                            "parameters of a part instance go into `params`", static_cast<int>(std::string(k).size()));
        if (nodes) {
            ref.nodes_loc = ctx_.at(*nodes);
            if (auto na = nodes->as_array()) {
                ref.node_count = static_cast<int>(na->size());
                for (auto&& x : *na) {
                    if (auto s = x.as_string()) register_node(x, s->get());
                    else ctx_.report("E0006", ctx_.at(x), "node names must be strings", "", ctx_.value_len(x));
                }
            } else if (auto nt = nodes->as_table()) {
                ref.nodes_is_table = true;
                for (auto&& [k, v] : *nt) {
                    ref.node_pins.push_back({std::string(k.str()), ctx_.at(k.source()), static_cast<int>(k.str().size())});
                    if (auto s = v.as_string()) register_node(v, s->get());
                    else ctx_.report("E0006", ctx_.at(v), "node names must be strings", "", ctx_.value_len(v));
                }
            } else {
                ctx_.report("E0006", ctx_.at(*nodes), "`nodes` of " + where + " must be an array or a table, found " + TomlCtx::type_name(*nodes),
                            "e.g. nodes = [\"a\", \"k\"] or nodes = { a = \"n1\", k = \"0\" }", ctx_.value_len(*nodes));
            }
        }
        if (auto pt = e["params"].as_table()) {
            for (auto&& [k, v] : *pt) {
                ref.params.push_back({std::string(k.str()), ctx_.at(k.source()), static_cast<int>(k.str().size())});
                if (TomlCtx::matches(v, VT::Value)) value(*pt, k.str(), "*", true, where);
            }
        }
        if (auto ft = e["features"].as_table()) {
            for (auto&& [k, v] : *ft) {
                ref.features.push_back({std::string(k.str()), ctx_.at(k.source()), static_cast<int>(k.str().size())});
                if (!v.is_boolean())
                    ctx_.report("E0006", ctx_.at(v), "feature switch `" + std::string(k.str()) + "` must be true or false",
                                "", ctx_.value_len(v));
            }
        }
        info_.refs.push_back(std::move(ref));
        return;
    }

    auto tn = e.get("type");
    if (!tn->is_string()) return;
    const std::string type = tn->as_string()->get();
    const PrimSpec* ps = find_prim(type);
    if (!ps) {
        auto types = prim_types(true);
        std::string s = suggest(type, types);
        ctx_.report("E0007", ctx_.at(*tn), "`type` of " + where + " must be one of " + join_quoted(types) + ", found `" + type + "`",
                    s.empty() ? "to use another library part, write part = \"generic_...\" instead of type"
                              : "did you mean `" + s + "`?",
                    ctx_.value_len(*tn));
        return;
    }
    where = where + " (`" + type + "`)";
    if (e.contains("features"))
        ctx_.report("E0004", ctx_.key_at(e, "features"), "key `features` is not allowed for " + where,
                    "feature switches are only used with `part` instances; use `feature` to make this element optional", 8);
    if (nodes) {
        if (auto na = nodes->as_array()) {
            for (auto&& x : *na) {
                if (auto s = x.as_string()) register_node(x, s->get());
                else ctx_.report("E0006", ctx_.at(x), "node names must be strings", "write node names in quotes", ctx_.value_len(x));
            }
            const int cnt = static_cast<int>(na->size());
            if (cnt < ps->nmin || cnt > ps->nmax) {
                std::string expect = ps->nmin == ps->nmax ? std::to_string(ps->nmin)
                                                          : std::to_string(ps->nmin) + " or " + std::to_string(ps->nmax);
                ctx_.report("E0042", ctx_.at(*nodes), where + " needs " + expect + " nodes, found " + std::to_string(cnt),
                            std::string("node order: ") + ps->node_names, ctx_.value_len(*nodes));
            }
        } else {
            ctx_.report("E0006", ctx_.at(*nodes), "`nodes` of " + where + " must be an array, found " + TomlCtx::type_name(*nodes),
                        "the table form { pin = node } is only for `part` instances", ctx_.value_len(*nodes));
        }
    }
    if (type == "B") {
        if (e.contains("params"))
            ctx_.report("E0004", ctx_.key_at(e, "params"), "key `params` is not allowed for " + where,
                        "a B element is defined by `source` and `expr`", 6);
        for (const char* k : {"source", "expr"})
            if (!e.contains(k))
                ctx_.report("E0005", ctx_.at(e), "missing required key `" + std::string(k) + "` in " + where,
                            "B elements need source = \"voltage\" or \"current\" and expr = \"...\"");
        ctx_.choice(e, "source", {"voltage", "current"}, where);
        if (!in_test) expression(e, "expr", where);
    } else {
        for (const char* k : {"source", "expr"})
            if (e.contains(k))
                ctx_.report("E0004", ctx_.key_at(e, k), "key `" + std::string(k) + "` is not allowed for " + where,
                            "`source` and `expr` are only used with type = \"B\"", static_cast<int>(std::string(k).size()));
        if (in_test) {
            // Test bench element values must be fixed numbers.
            std::vector<std::string> pnames;
            for (const auto& p : ps->params) pnames.emplace_back(p.name);
            if (auto pt = e["params"].as_table()) {
                for (auto&& [k, v] : *pt) {
                    auto it = std::find_if(ps->params.begin(), ps->params.end(), [&](const PrimParam& p) { return k.str() == p.name; });
                    if (it == ps->params.end()) {
                        ctx_.report("E0070", ctx_.at(k.source()), "unknown parameter `" + std::string(k.str()) + "` for element `" + type + "`",
                                    suggestion_hint(k.str(), pnames, "parameters"), static_cast<int>(k.str().size()));
                        continue;
                    }
                    if (TomlCtx::matches(v, VT::Value)) value(*pt, k.str(), it->unit, false, where);
                }
            }
            for (const auto& p : ps->params)
                if (p.required && !(e["params"].as_table() && e["params"].as_table()->contains(p.name)))
                    ctx_.report("E0071", ctx_.at(e), "element `" + type + "` needs parameter `" + p.name + "` in " + where,
                                std::string("add params = { ") + p.name + " = ... }");
        } else {
            check_prim_params(*ps, e.get("params"), where);
        }
    }
    if (!ps->polarities.empty()) {
        if (!e.contains("polarity"))
            ctx_.report("E0005", ctx_.at(e), "missing required key `polarity` in " + where,
                        "add polarity = " + join_quoted(ps->polarities) + " (one of them, in double quotes)");
        else
            ctx_.choice(e, "polarity", ps->polarities, where);
    } else if (e.contains("polarity")) {
        ctx_.report("E0004", ctx_.key_at(e, "polarity"), "key `polarity` is not allowed for " + where,
                    "`polarity` is only used with type Q or M", 8);
    }
}

void PartChecker::check_signals(const toml::array& a) {
    int idx = 0;
    for (auto&& node : a) {
        const toml::table& s = *node.as_table();
        std::string where = "[[model.signals]] entry " + std::to_string(++idx);
        ctx_.keys(s, where, {{"name", VT::String, true}, {"expr", VT::String, true}, {"tau", VT::Value}, {"unit", VT::String}, {"description", VT::String}});
        std::string name;
        if (check_lower_name(s, where, name, true)) {
            where = "signal `" + name + "`";
            define_value_name(name, "signal", s);
        }
        if (auto u = s.get("unit"); u && u->is_string() && !is_known_unit(u->as_string()->get()))
            ctx_.report("E0030", ctx_.at(*u), "unknown unit `" + u->as_string()->get() + "` in " + where,
                        suggestion_hint(u->as_string()->get(), known_units(), "units", 10), ctx_.value_len(*u));
        Val tau = value(s, "tau", "s", true, where);
        if (tau.ok && !tau.is_expr && tau.num <= 0)
            ctx_.report("E0033", ctx_.at(*s.get("tau")), "`tau` of " + where + " must be positive",
                        "remove `tau` for an unfiltered signal", ctx_.value_len(*s.get("tau")));
        expression(s, "expr", where);
    }
}

void PartChecker::check_outputs(const toml::array& a) {
    int idx = 0;
    for (auto&& node : a) {
        const toml::table& o = *node.as_table();
        std::string where = "[[model.outputs]] entry " + std::to_string(++idx);
        ctx_.keys(o, where,
                  {{"name", VT::String, true}, {"source", VT::String, true}, {"nodes", VT::Array, true}, {"expr", VT::String, true},
                   {"r_out", VT::Value}, {"ron", VT::Value}, {"roff", VT::Value}, {"feature", VT::String}, {"description", VT::String}});
        std::string name;
        if (check_lower_name(o, where, name, false)) {
            where = "output `" + name + "`";
            if (!components_.insert(name).second)
                ctx_.report("E0013", ctx_.at(*o.get("name")), "name `" + name + "` is used twice",
                            "elements, outputs and branches share one namespace", ctx_.value_len(*o.get("name")));
        }
        auto src = ctx_.choice(o, "source", {"voltage", "current", "switch"}, where);
        check_nodes_array(o, "nodes", where, 2, 2, "first second");
        feature_ref(o, "feature");
        if (src) {
            if (*src != "voltage" && o.contains("r_out"))
                ctx_.report("E0004", ctx_.key_at(o, "r_out"), "key `r_out` is not allowed for " + where + " with source `" + *src + "`",
                            "`r_out` is only used with source = \"voltage\"", 5);
            for (const char* k : {"ron", "roff"})
                if (*src != "switch" && o.contains(k))
                    ctx_.report("E0004", ctx_.key_at(o, k), "key `" + std::string(k) + "` is not allowed for " + where + " with source `" + *src + "`",
                                "`ron` and `roff` are only used with source = \"switch\"", static_cast<int>(std::string(k).size()));
        }
        for (const char* k : {"r_out", "ron", "roff"}) {
            Val v = value(o, k, "Ohm", true, where);
            if (v.ok && !v.is_expr && v.num < 0)
                ctx_.report("E0033", ctx_.at(*o.get(k)), "`" + std::string(k) + "` of " + where + " is negative", "",
                            ctx_.value_len(*o.get(k)));
        }
        expression(o, "expr", where);
    }
}

void PartChecker::check_branches(const toml::array& a, bool has_sm) {
    int idx = 0;
    for (auto&& node : a) {
        const toml::table& b = *node.as_table();
        std::string where = "[[model.branches]] entry " + std::to_string(++idx);
        ctx_.keys(b, where,
                  {{"name", VT::String, true}, {"nodes", VT::Array, true}, {"quantity", VT::String}, {"mode", VT::String, true},
                   {"value", VT::Value}, {"curve", VT::String}, {"input", VT::String}, {"expr", VT::String},
                   {"v_min", VT::Value}, {"feature", VT::String}, {"description", VT::String}});
        std::string name;
        if (check_lower_name(b, where, name, false)) {
            where = "branch `" + name + "`";
            if (!components_.insert(name).second)
                ctx_.report("E0013", ctx_.at(*b.get("name")), "name `" + name + "` is used twice",
                            "elements, outputs and branches share one namespace", ctx_.value_len(*b.get("name")));
        }
        check_nodes_array(b, "nodes", where, 2, 2, "from to (current flows from the first node through the load to the second)");
        feature_ref(b, "feature");
        auto quantity = ctx_.choice(b, "quantity", {"current", "resistance", "power"}, where);
        const std::string q = b.contains("quantity") ? quantity.value_or("") : "current";
        const std::string unit = q == "current" ? "A" : q == "resistance" ? "Ohm" : q == "power" ? "W" : "*";
        auto mode = ctx_.choice(b, "mode", {"fixed", "curve", "formula", "state_machine"}, where);
        if (!name.empty()) {
            branch_mode_[name] = mode.value_or("?");
            branch_unit_[name] = unit;
        }
        if (b.contains("v_min") && q != "power" && !q.empty())
            ctx_.report("E0004", ctx_.key_at(b, "v_min"), "key `v_min` is not allowed for " + where + " with quantity `" + q + "`",
                        "`v_min` is only used with quantity = \"power\"", 5);
        Val vmin = value(b, "v_min", "V", false, where);
        if (vmin.ok && vmin.num <= 0)
            ctx_.report("E0033", ctx_.at(*b.get("v_min")), "`v_min` of " + where + " must be positive", "",
                        ctx_.value_len(*b.get("v_min")));
        if (!mode) continue;
        std::vector<std::string> needed, allowed;
        if (*mode == "fixed") needed = {"value"};
        else if (*mode == "curve") needed = {"curve", "input"};
        else if (*mode == "formula") needed = {"expr"};
        allowed = needed;
        if (*mode == "state_machine") allowed = {"value"};
        for (const char* k : {"value", "curve", "input", "expr"}) {
            if (b.contains(k) && std::find(allowed.begin(), allowed.end(), k) == allowed.end()) {
                std::string owner = std::string(k) == "value" ? "fixed or state_machine"
                                  : std::string(k) == "expr" ? "formula" : "curve";
                ctx_.report("E0004", ctx_.key_at(b, k), "key `" + std::string(k) + "` is not used with mode `" + *mode + "` in " + where,
                            "`" + std::string(k) + "` belongs to mode " + owner + "; remove it or change `mode`",
                            static_cast<int>(std::string(k).size()));
            }
        }
        for (const auto& k : needed)
            if (!b.contains(k))
                ctx_.report("E0005", ctx_.at(b), "missing required key `" + k + "` in " + where + " (mode `" + *mode + "`)",
                            "see SPEC.md section 10 for the keys of each mode");
        if (*mode == "state_machine" && !has_sm)
            ctx_.report("E0005", ctx_.at(*b.get("mode")), where + " uses mode `state_machine`, but [model.state_machine] is missing",
                        "add a [model.state_machine] table (SPEC.md section 11)", ctx_.value_len(*b.get("mode")));
        if (*mode == "fixed" || *mode == "state_machine") value(b, "value", unit, true, where);
        if (*mode == "formula") expression(b, "expr", where);
        if (*mode == "curve") {
            expression(b, "input", where);
            if (auto c = b.get("curve"); c && c->is_string() && !curves_.count(c->as_string()->get())) {
                std::vector<std::string> cs(curves_.begin(), curves_.end());
                ctx_.report("E0094", ctx_.at(*c), "unknown curve `" + c->as_string()->get() + "` in " + where,
                            cs.empty() ? "declare the curve with [[curves]] (SPEC.md section 13)"
                                       : suggestion_hint(c->as_string()->get(), cs, "curves"),
                            ctx_.value_len(*c));
            }
        }
    }
}

void PartChecker::check_state_machine(const toml::table& sm) {
    ctx_.keys(sm, "[model.state_machine]",
              {{"initial", VT::String, true}, {"variables", VT::ArrayOfTables}, {"inputs", VT::ArrayOfTables},
               {"states", VT::ArrayOfTables, true}, {"transitions", VT::ArrayOfTables}, {"description", VT::String}});
    int idx = 0;
    if (auto a = sm["variables"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) {
        for (auto&& node : *a) {
            const toml::table& v = *node.as_table();
            std::string where = "[[model.state_machine.variables]] entry " + std::to_string(++idx);
            ctx_.keys(v, where, {{"name", VT::String, true}, {"init", VT::Number, true}, {"description", VT::String}});
            std::string name;
            if (check_lower_name(v, where, name, true)) define_value_name(name, "variable", v);
        }
    }
    idx = 0;
    if (auto a = sm["inputs"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) {
        for (auto&& node : *a) {
            const toml::table& in = *node.as_table();
            std::string where = "[[model.state_machine.inputs]] entry " + std::to_string(++idx);
            ctx_.keys(in, where, {{"name", VT::String, true}, {"expr", VT::String, true}, {"high", VT::Value, true},
                                  {"low", VT::Value, true}, {"description", VT::String}});
            std::string name;
            if (check_lower_name(in, where, name, true)) {
                where = "input `" + name + "`";
                if (define_value_name(name, "input", in)) inputs_.insert(name);
            }
            expression(in, "expr", where);
            Val hi = value(in, "high", "*", true, where);
            Val lo = value(in, "low", "*", true, where);
            if (hi.ok && lo.ok && !hi.is_expr && !lo.is_expr && lo.num > hi.num)
                ctx_.report("E0034", ctx_.at(*in.get("low")), "`low` threshold (" + fmt(lo.num) + ") of " + where + " is above `high` (" + fmt(hi.num) + ")",
                            "the falling threshold must not be above the rising threshold", ctx_.value_len(*in.get("low")));
        }
    }
    // states
    std::vector<std::pair<std::string, Loc>> state_list;
    std::map<std::string, int> state_len;
    std::set<std::string> final_states;
    idx = 0;
    if (auto a = sm["states"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) {
        for (auto&& node : *a) {
            const toml::table& st = *node.as_table();
            std::string where = "[[model.state_machine.states]] entry " + std::to_string(++idx);
            ctx_.keys(st, where, {{"name", VT::String, true}, {"values", VT::Table}, {"on_enter", VT::ArrayOfStrings},
                                  {"on_exit", VT::ArrayOfStrings}, {"final", VT::Bool}, {"description", VT::String}});
            std::string name;
            if (!check_lower_name(st, where, name, false)) continue;
            where = "state `" + name + "`";
            if (!states_.insert(name).second) {
                ctx_.report("E0013", ctx_.at(*st.get("name")), "state `" + name + "` is defined twice",
                            "state names must be unique", ctx_.value_len(*st.get("name")));
                continue;
            }
            state_list.push_back({name, ctx_.at(*st.get("name"))});
            state_len[name] = ctx_.value_len(*st.get("name"));
            if (ctx_.boolean(st, "final").value_or(false)) final_states.insert(name);
            for (const char* k : {"on_enter", "on_exit"})
                if (auto acts = st[k].as_array())
                    for (auto&& act : *acts)
                        if (auto s = act.as_string()) add_expr(act, s->get(), 0, ExprKind::Action, "`" + std::string(k) + "` of " + where);
        }
    }
    bool bad_state_ref = false;
    auto check_state_name = [&](const toml::node& n, const std::string& s, bool allow_any, const std::string& where) {
        if (allow_any && s == "*") return true;
        if (states_.count(s)) return true;
        bad_state_ref = true;
        std::vector<std::string> c(states_.begin(), states_.end());
        ctx_.report("E0100", ctx_.at(n), "unknown state `" + s + "` in " + where, suggestion_hint(s, c, "states"),
                    ctx_.value_len(n));
        return false;
    };
    std::string initial;
    if (auto n = sm.get("initial"); n && n->is_string()) {
        initial = n->as_string()->get();
        if (!check_state_name(*n, initial, false, "`initial`")) initial.clear();
    }
    // transitions
    std::map<std::string, std::set<std::string>> edges;
    std::set<std::string> from_any_targets;
    idx = 0;
    if (auto a = sm["transitions"].as_array(); a && TomlCtx::matches(*a, VT::ArrayOfTables)) {
        for (auto&& node : *a) {
            const toml::table& tr = *node.as_table();
            std::string where = "[[model.state_machine.transitions]] entry " + std::to_string(++idx);
            ctx_.keys(tr, where,
                      {{"from", VT::String, true}, {"to", VT::String, true}, {"on", VT::String}, {"after", VT::Value},
                       {"when", VT::String}, {"guard", VT::String}, {"actions", VT::ArrayOfStrings}, {"priority", VT::Int},
                       {"description", VT::String}});
            std::string from, to;
            bool ok_from = false, ok_to = false;
            if (auto n = tr.get("from"); n && n->is_string()) {
                from = n->as_string()->get();
                ok_from = check_state_name(*n, from, true, "`from` of " + where);
            }
            if (auto n = tr.get("to"); n && n->is_string()) {
                to = n->as_string()->get();
                ok_to = check_state_name(*n, to, false, "`to` of " + where);
            }
            if (ok_from && ok_to) {
                if (from == "*") from_any_targets.insert(to);
                else edges[from].insert(to);
            }
            if (!tr.contains("on") && !tr.contains("after") && !tr.contains("when"))
                ctx_.report("E0072", ctx_.at(tr), where + " has no trigger",
                            "add `on = \"rising(input)\"`, `after = \"10 us\"` or `when = \"condition\"`");
            if (auto on = tr.get("on"); on && on->is_string()) {
                static const std::regex re(R"(^\s*(rising|falling)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*$)");
                const std::string s = on->as_string()->get();
                if (!std::regex_match(s, re))
                    ctx_.report("E0080", ctx_.at(*on), "`on` of " + where + " must be `rising(input)` or `falling(input)`, found `" + s + "`",
                                "use `when` for conditions and `after` for timeouts", ctx_.value_len(*on));
                else
                    expression(tr, "on", where, ExprKind::Edge);
            }
            Val after = value(tr, "after", "s", true, where);
            if (after.ok && !after.is_expr && after.num < 0)
                ctx_.report("E0033", ctx_.at(*tr.get("after")), "`after` of " + where + " is negative",
                            "a timeout is a time >= 0", ctx_.value_len(*tr.get("after")));
            expression(tr, "when", where);
            expression(tr, "guard", where);
            if (auto acts = tr["actions"].as_array())
                for (auto&& act : *acts)
                    if (auto s = act.as_string()) add_expr(act, s->get(), 0, ExprKind::Action, "`actions` of " + where);
        }
    }
    // reachability (skipped after unknown state names, which would cascade)
    if (!initial.empty() && !bad_state_ref) {
        std::set<std::string> seen{initial};
        std::vector<std::string> todo{initial};
        for (const auto& t : from_any_targets)
            if (seen.insert(t).second) todo.push_back(t);
        while (!todo.empty()) {
            std::string s = todo.back();
            todo.pop_back();
            for (const auto& t : edges[s])
                if (seen.insert(t).second) todo.push_back(t);
        }
        for (const auto& [s, loc] : state_list)
            if (!seen.count(s))
                ctx_.report("E0101", loc, "state `" + s + "` cannot be reached from the initial state `" + initial + "`",
                            "add a transition into it, or remove the state", state_len[s]);
        for (const auto& [s, loc] : state_list)
            if (seen.count(s) && edges[s].empty() && from_any_targets.empty() && !final_states.count(s))
                ctx_.report("W0102", loc, "state `" + s + "` has no outgoing transition",
                            "add a transition out of it, or mark it with final = true if the machine is meant to stop there",
                            state_len[s]);
    }
}

void PartChecker::check_noise(const toml::table& n) {
    struct NKey {
        const char* name;
        const char* unit;  // "VA" = V or A depending on output, "VAD" = density
    };
    const std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>> types = {
        {"white", {{"density", "bandwidth"}, {"seed"}}},
        {"pink", {{"density", "f_min", "f_max"}, {"seed"}}},
        {"burst", {{"amplitude", "rate", "duration"}, {"seed", "rise_time"}}},
        {"ripple", {{"amplitude", "frequency"}, {"shape", "offset", "phase"}}},
        {"esd", {{"peak", "rise_time", "decay_time"}, {"delay", "interval", "count"}}},
    };
    std::vector<KeySpec> spec = {{"type", VT::String, true}, {"output", VT::String, true}, {"nodes", VT::Array, true}};
    const std::vector<std::string> all = {"density", "bandwidth", "f_min", "f_max", "amplitude", "rate", "duration",
                                          "rise_time", "decay_time", "frequency", "shape", "offset", "phase", "peak",
                                          "delay", "interval", "count", "seed"};
    for (const auto& k : all) {
        VT vt = (k == "seed" || k == "count") ? VT::Int : (k == "shape") ? VT::String : VT::Value;
        spec.push_back({k.c_str(), vt, false});
    }
    // keys() needs stable c_str pointers: `all` outlives the call.
    ctx_.keys(n, "[model.noise]", spec);
    auto type = ctx_.choice(n, "type", {"white", "pink", "burst", "ripple", "esd"}, "[model.noise]");
    auto output = ctx_.choice(n, "output", {"voltage", "current"}, "[model.noise]");
    check_nodes_array(n, "nodes", "[model.noise]", 2, 2, "p n");
    if (!type) return;
    const auto& [req, opt] = types.at(*type);
    for (const auto& k : all) {
        if (!n.contains(k)) continue;
        bool known = std::find(req.begin(), req.end(), k) != req.end() || std::find(opt.begin(), opt.end(), k) != opt.end();
        if (!known)
            ctx_.report("E0004", ctx_.key_at(n, k), "key `" + k + "` is not used by noise type `" + *type + "`",
                        "keys of type " + *type + ": " + join_quoted(req) + (opt.empty() ? "" : ", optional " + join_quoted(opt)),
                        static_cast<int>(k.size()));
    }
    for (const auto& k : req)
        if (!n.contains(k))
            ctx_.report("E0005", ctx_.at(n), "noise type `" + *type + "` needs `" + k + "` in [model.noise]",
                        "required keys for " + *type + ": " + join_quoted(req));
    const std::string va = output.value_or("voltage") == "current" ? "A" : "V";
    const std::map<std::string, std::string> units = {
        {"density", va + "/sqrt(Hz)"}, {"bandwidth", "Hz"}, {"f_min", "Hz"}, {"f_max", "Hz"}, {"amplitude", va},
        {"rate", "Hz"}, {"duration", "s"}, {"rise_time", "s"}, {"decay_time", "s"}, {"frequency", "Hz"},
        {"offset", va}, {"phase", "rad"}, {"peak", va}, {"delay", "s"}, {"interval", "s"}};
    for (const auto& [k, u] : units) {
        Val v = value(n, k, u, true, "[model.noise]");
        const bool may_be_negative = k == "offset" || k == "phase" || k == "peak" || k == "amplitude";
        if (v.ok && !v.is_expr && !may_be_negative && v.num < 0)
            ctx_.report("E0033", ctx_.at(*n.get(k)), "`" + k + "` in [model.noise] is negative", "",
                        ctx_.value_len(*n.get(k)));
    }
    if (n.contains("shape")) ctx_.choice(n, "shape", {"sine", "triangle", "sawtooth", "full_wave", "half_wave"}, "[model.noise]");
    if (auto c = n.get("count"); c && c->is_integer() && c->value<int64_t>().value_or(0) < 1)
        ctx_.report("E0033", ctx_.at(*c), "`count` in [model.noise] must be at least 1", "", ctx_.value_len(*c));
}

// ---------------------------------------------------------------- expressions

void PartChecker::resolve(const PendingExpr& p, const Scope& s) {
    ExprResult r;
    std::string target;
    int target_pos = 0;
    if (p.kind == ExprKind::Action) r = parse_assignment(p.text, target, target_pos);
    else r = parse_expression(p.text);
    auto at = [&](int pos) {
        Loc l = p.loc;
        l.col += pos;
        return l;
    };
    if (!r.ok) {
        ctx_.report("E0080", at(r.err_pos), "syntax error in " + p.what + ": " + r.error,
                    "expression syntax: SPEC.md section 15", 1);
        return;
    }
    for (int pos : r.mega_warnings)
        ctx_.report("W0083", at(pos), "suffix `M` in " + p.what + " means milli (1e-3) in SPICE expressions",
                    "write `meg` or `e6` for mega, or `m` for milli", 1);
    if (p.kind == ExprKind::Action) {
        auto it = s.values.find(target);
        if (it == s.values.end() || it->second != "variable") {
            std::vector<std::string> vars;
            for (const auto& [n, k] : s.values)
                if (k == "variable") vars.push_back(n);
            ctx_.report("E0081", at(target_pos), "`" + target + "` in " + p.what + " is not a state machine variable",
                        vars.empty() ? "declare it with [[model.state_machine.variables]]" : suggestion_hint(target, vars, "variables"),
                        static_cast<int>(target.size()));
        }
    }
    std::vector<std::string> value_names;
    for (const auto& [n, k] : s.values) value_names.push_back(n);
    for (const auto& b : builtin_names())
        if (b != "time_in_state" || s.has_sm) value_names.push_back(b);
    for (const auto& u : r.uses) {
        const int len = static_cast<int>(u.name.size());
        switch (u.kind) {
            case ExprUse::Kind::Name: {
                if (std::find(value_names.begin(), value_names.end(), u.name) != value_names.end()) break;
                if (u.name == "time_in_state") {
                    ctx_.report("E0081", at(u.pos), "`time_in_state` in " + p.what + " needs a state machine",
                                "time_in_state is only defined in parts with [model.state_machine]", len);
                    break;
                }
                std::string hint = suggestion_hint(u.name, value_names, "names", 10);
                if (s.curves.count(u.name)) hint = "`" + u.name + "` is a curve; call it as `" + u.name + "(x)`";
                ctx_.report("E0081", at(u.pos), "unknown name `" + u.name + "` in " + p.what, hint, len);
                break;
            }
            case ExprUse::Kind::Call: {
                if (s.curves.count(u.name)) {
                    if (u.argc != 1)
                        ctx_.report("E0082", at(u.pos), "curve `" + u.name + "` takes 1 argument, found " + std::to_string(u.argc),
                                    "call it as `" + u.name + "(x)`", len);
                    break;
                }
                const FunctionInfo* f = find_function(u.name);
                if (!f) {
                    std::vector<std::string> fn;
                    for (const auto& x : builtin_functions()) fn.emplace_back(x.name);
                    fn.insert(fn.end(), s.curves.begin(), s.curves.end());
                    std::string hint = suggestion_hint(u.name, fn, "functions", 10);
                    if (s.values.count(u.name)) hint = "`" + u.name + "` is a " + s.values.at(u.name) + ", not a function";
                    ctx_.report("E0082", at(u.pos), "unknown function `" + u.name + "` in " + p.what, hint, len);
                    break;
                }
                const std::string fname = f->name;
                if (u.argc < f->min_args || (f->max_args >= 0 && u.argc > f->max_args)) {
                    std::string expect = f->max_args < 0 ? "at least " + std::to_string(f->min_args)
                                       : f->min_args == f->max_args ? std::to_string(f->min_args)
                                                                     : std::to_string(f->min_args) + " or " + std::to_string(f->max_args);
                    ctx_.report("E0082", at(u.pos), "function `" + u.name + "` takes " + expect + " argument(s), found " + std::to_string(u.argc),
                                "SPEC.md section 15.3 lists all functions", len);
                    break;
                }
                if ((fname == "rising" || fname == "falling") && p.kind != ExprKind::Edge)
                    ctx_.report("E0082", at(u.pos), "`" + fname + "()` can only be used in `on` of a transition",
                                "use the input name for its logic level, e.g. `din == 1`", len);
                if (fname == "in_state" && !s.has_sm)
                    ctx_.report("E0082", at(u.pos), "`in_state()` in " + p.what + " needs a state machine",
                                "add [model.state_machine]", len);
                break;
            }
            case ExprUse::Kind::Node: {
                if (s.nodes.count(u.name)) {
                    nodes_used_.insert(u.name);
                    break;
                }
                std::vector<std::string> c(s.nodes.begin(), s.nodes.end());
                ctx_.report("E0040", at(u.pos), "unknown node `" + u.name + "` in V() in " + p.what,
                            suggestion_hint(u.name, c, "nodes"), len);
                break;
            }
            case ExprUse::Kind::Current: {
                if (s.currents.count(u.name)) {
                    nodes_used_.insert(u.name);
                    break;
                }
                std::vector<std::string> c(s.currents.begin(), s.currents.end());
                ctx_.report("E0040", at(u.pos), "unknown pin or element `" + u.name + "` in I() in " + p.what,
                            suggestion_hint(u.name, c, "pins and elements"), len);
                break;
            }
            case ExprUse::Kind::EdgeArg: {
                if (s.inputs.count(u.name)) break;
                std::vector<std::string> c(s.inputs.begin(), s.inputs.end());
                ctx_.report("E0081", at(u.pos), "`" + u.name + "` in " + p.what + " is not a state machine input",
                            c.empty() ? "declare it with [[model.state_machine.inputs]]" : suggestion_hint(u.name, c, "inputs"), len);
                break;
            }
            case ExprUse::Kind::StateArg: {
                if (!s.has_sm || s.states.count(u.name)) break;
                std::vector<std::string> c(s.states.begin(), s.states.end());
                ctx_.report("E0100", at(u.pos), "unknown state `" + u.name + "` in " + p.what,
                            suggestion_hint(u.name, c, "states"), len);
                break;
            }
        }
    }
}

void PartChecker::resolve_all_model() {
    Scope s;
    s.values = value_names_;
    for (const auto& c : curves_) s.values.erase(c);
    s.curves = curves_;
    s.nodes = pin_set_;
    s.nodes.insert(internal_nodes_.begin(), internal_nodes_.end());
    s.nodes.insert("0");
    s.currents = pin_set_;
    s.currents.insert(components_.begin(), components_.end());
    s.states = states_;
    s.inputs = inputs_;
    s.has_sm = has_sm_;
    for (const auto& p : model_exprs_) resolve(p, s);
    model_exprs_.clear();
}

// ---------------------------------------------------------------- tests

void PartChecker::check_tests(const toml::array& a) {
    std::set<std::string> test_names;
    int idx = 0;
    for (auto&& node : a) {
        const toml::table& t = *node.as_table();
        ++test_count_;
        std::string where = "[[tests]] entry " + std::to_string(++idx);
        ctx_.keys(t, where,
                  {{"name", VT::String, true}, {"description", VT::String}, {"analysis", VT::String, true},
                   {"stop", VT::Value}, {"step", VT::Value}, {"sweep", VT::Table}, {"params", VT::Table},
                   {"features", VT::Table}, {"stimulus", VT::ArrayOfTables}, {"elements", VT::ArrayOfTables},
                   {"expect", VT::ArrayOfTables, true}});
        std::string name;
        if (check_lower_name(t, where, name, false)) {
            where = "test `" + name + "`";
            if (!test_names.insert(name).second)
                ctx_.report("E0013", ctx_.at(*t.get("name")), "test `" + name + "` is defined twice",
                            "test names must be unique", ctx_.value_len(*t.get("name")));
        }
        auto analysis = ctx_.choice(t, "analysis", {"op", "dc", "tran"}, where);
        double t_stop = -1, sw_from = 0, sw_to = 0;
        bool have_range = false;
        if (analysis) {
            if (*analysis == "tran" && !t.contains("stop"))
                ctx_.report("E0005", ctx_.at(t), "missing required key `stop` in " + where + " (analysis `tran`)",
                            "add stop = \"1 ms\" (end time of the transient analysis)");
            if (*analysis == "dc" && !t.contains("sweep"))
                ctx_.report("E0005", ctx_.at(t), "missing required key `sweep` in " + where + " (analysis `dc`)",
                            "add sweep = { stimulus = \"v1\", from = 0, to = 5, step = 0.1 }");
            for (const char* k : {"stop", "step"})
                if (*analysis != "tran" && t.contains(k))
                    ctx_.report("E0004", ctx_.key_at(t, k), "key `" + std::string(k) + "` is not used with analysis `" + *analysis + "` in " + where,
                                "`stop` and `step` belong to analysis = \"tran\"", static_cast<int>(std::string(k).size()));
            if (*analysis != "dc" && t.contains("sweep"))
                ctx_.report("E0004", ctx_.key_at(t, "sweep"), "key `sweep` is not used with analysis `" + *analysis + "` in " + where,
                            "`sweep` belongs to analysis = \"dc\"", 5);
        }
        Val stop = value(t, "stop", "s", false, where);
        Val step = value(t, "step", "s", false, where);
        if (stop.ok) {
            if (stop.num <= 0)
                ctx_.report("E0033", ctx_.at(*t.get("stop")), "`stop` of " + where + " must be positive", "",
                            ctx_.value_len(*t.get("stop")));
            else {
                t_stop = stop.num;
                have_range = true;
            }
        }
        if (step.ok && step.num <= 0)
            ctx_.report("E0033", ctx_.at(*t.get("step")), "`step` of " + where + " must be positive", "",
                        ctx_.value_len(*t.get("step")));

        // instance parameters and features
        if (auto pt = t["params"].as_table()) {
            std::vector<std::string> pnames;
            for (const auto& [n, _] : info_.params) pnames.push_back(n);
            for (auto&& [k, v] : *pt) {
                const std::string key(k.str());
                auto it = info_.params.find(key);
                if (it == info_.params.end()) {
                    ctx_.report("E0075", ctx_.at(k.source()), "unknown parameter `" + key + "` in `params` of " + where,
                                suggestion_hint(key, pnames, "parameters"), static_cast<int>(key.size()));
                    continue;
                }
                if (!it->second.adjustable) {
                    ctx_.report("E0050", ctx_.at(k.source()), "parameter `" + key + "` is not adjustable and cannot be set in " + where,
                                "remove it, or set adjustable = true in the parameter if instances may change it",
                                static_cast<int>(key.size()));
                    continue;
                }
                const std::string& ty = it->second.type;
                if (ty == "real" || ty == "integer") {
                    if (TomlCtx::matches(v, VT::Value)) value(*pt, key, it->second.unit.empty() ? "*" : it->second.unit, false, where);
                    else ctx_.report("E0006", ctx_.at(v), "`" + key + "` must be a number or quantity, found " + TomlCtx::type_name(v), "", ctx_.value_len(v));
                } else if (ty == "boolean" && !v.is_boolean()) {
                    ctx_.report("E0006", ctx_.at(v), "`" + key + "` must be true or false, found " + TomlCtx::type_name(v), "", ctx_.value_len(v));
                } else if ((ty == "string" || ty == "enum") && !v.is_string()) {
                    ctx_.report("E0006", ctx_.at(v), "`" + key + "` must be a string, found " + TomlCtx::type_name(v), "", ctx_.value_len(v));
                } else if (ty == "enum") {
                    ctx_.choice(*pt, key, it->second.choices, where);
                }
            }
        }
        if (auto ft = t["features"].as_table()) {
            for (auto&& [k, v] : *ft) {
                const std::string key(k.str());
                if (!features_.count(key)) {
                    std::vector<std::string> c(features_.begin(), features_.end());
                    ctx_.report("E0060", ctx_.at(k.source()), "unknown feature `" + key + "` in " + where,
                                suggestion_hint(key, c, "features"), static_cast<int>(key.size()));
                } else if (!v.is_boolean()) {
                    ctx_.report("E0006", ctx_.at(v), "feature switch `" + key + "` must be true or false", "", ctx_.value_len(v));
                }
            }
        }

        // test bench: elements first (they define extra nodes), then stimuli
        std::set<std::string> tnodes(pin_set_.begin(), pin_set_.end());
        tnodes.insert("0");
        std::set<std::string> tnames;
        std::set<std::string> elem_nodes;
        if (auto ea = t["elements"].as_array(); ea && TomlCtx::matches(*ea, VT::ArrayOfTables)) {
            int ei = 0;
            for (auto&& e : *ea)
                check_element(*e.as_table(), "[[tests.elements]] entry " + std::to_string(++ei) + " of " + where, true, &elem_nodes, tnames);
        }
        tnodes.insert(elem_nodes.begin(), elem_nodes.end());
        std::set<std::string> stim_names;
        std::map<std::string, std::string> stim_source;
        if (auto sa = t["stimulus"].as_array(); sa && TomlCtx::matches(*sa, VT::ArrayOfTables)) {
            int si = 0;
            for (auto&& sn : *sa) {
                const toml::table& s = *sn.as_table();
                std::string sw = "[[tests.stimulus]] entry " + std::to_string(++si) + " of " + where;
                ctx_.keys(s, sw,
                          {{"name", VT::String, true}, {"nodes", VT::Array, true}, {"source", VT::String, true},
                           {"waveform", VT::String, true}, {"value", VT::Value}, {"v1", VT::Value}, {"v2", VT::Value},
                           {"delay", VT::Value}, {"rise", VT::Value}, {"fall", VT::Value}, {"width", VT::Value},
                           {"period", VT::Value}, {"offset", VT::Value}, {"amplitude", VT::Value},
                           {"frequency", VT::Value}, {"points", VT::Array}});
                std::string sname;
                if (auto n = s.get("name"); n && n->is_string()) {
                    sname = n->as_string()->get();
                    if (!is_element_name(sname))
                        ctx_.report("E0012", ctx_.at(*n), "stimulus name `" + sname + "` is not valid",
                                    "start with a letter, then letters, digits and `_`, e.g. `v1`", ctx_.value_len(*n));
                    if (!tnames.insert(sname).second)
                        ctx_.report("E0013", ctx_.at(*n), "name `" + sname + "` is used twice in " + where,
                                    "names of test elements and stimuli must be unique within a test", ctx_.value_len(*n));
                    stim_names.insert(sname);
                    sw = "stimulus `" + sname + "` of " + where;
                }
                if (auto na = s["nodes"].as_array()) {
                    for (auto&& x : *na) {
                        auto xs = x.as_string();
                        if (!xs) {
                            ctx_.report("E0006", ctx_.at(x), "node names must be strings", "", ctx_.value_len(x));
                            continue;
                        }
                        if (!tnodes.count(xs->get())) {
                            std::vector<std::string> c(tnodes.begin(), tnodes.end());
                            ctx_.report("E0040", ctx_.at(x), "unknown node `" + xs->get() + "` in " + sw,
                                        suggestion_hint(xs->get(), c, "nodes (pins, test element nodes, `0`)"), ctx_.value_len(x));
                        }
                    }
                    if (na->size() != 2)
                        ctx_.report("E0042", ctx_.at(*na), sw + " needs 2 nodes, found " + std::to_string(na->size()),
                                    "node order: positive negative", ctx_.value_len(*na));
                }
                auto src = ctx_.choice(s, "source", {"voltage", "current"}, sw);
                if (src && !sname.empty()) stim_source[sname] = *src;
                const std::string lvl = src ? (*src == "voltage" ? "V" : "A") : "*";
                auto wf = ctx_.choice(s, "waveform", {"dc", "pulse", "sin", "pwl"}, sw);
                if (!wf) continue;
                std::vector<std::string> req, opt;
                if (*wf == "dc") req = {"value"};
                else if (*wf == "pulse") req = {"v1", "v2", "delay", "rise", "fall", "width", "period"};
                else if (*wf == "sin") { req = {"offset", "amplitude", "frequency"}; opt = {"delay"}; }
                else req = {"points"};
                for (const char* k : {"value", "v1", "v2", "delay", "rise", "fall", "width", "period", "offset", "amplitude", "frequency", "points"}) {
                    if (!s.contains(k)) continue;
                    if (std::find(req.begin(), req.end(), k) == req.end() && std::find(opt.begin(), opt.end(), k) == opt.end())
                        ctx_.report("E0004", ctx_.key_at(s, k), "key `" + std::string(k) + "` is not used with waveform `" + *wf + "` in " + sw,
                                    "keys of waveform " + *wf + ": " + join_quoted(req), static_cast<int>(std::string(k).size()));
                }
                for (const auto& k : req)
                    if (!s.contains(k))
                        ctx_.report("E0005", ctx_.at(s), "missing required key `" + k + "` in " + sw + " (waveform `" + *wf + "`)",
                                    "keys of waveform " + *wf + ": " + join_quoted(req));
                for (const char* k : {"value", "v1", "v2", "offset", "amplitude"}) value(s, k, lvl, false, sw);
                for (const char* k : {"delay", "rise", "fall", "width", "period"}) {
                    Val v = value(s, k, "s", false, sw);
                    if (v.ok && v.num < 0)
                        ctx_.report("E0033", ctx_.at(*s.get(k)), "`" + std::string(k) + "` in " + sw + " is negative", "", ctx_.value_len(*s.get(k)));
                }
                Val fq = value(s, "frequency", "Hz", false, sw);
                if (fq.ok && fq.num <= 0)
                    ctx_.report("E0033", ctx_.at(*s.get("frequency")), "`frequency` in " + sw + " must be positive", "", ctx_.value_len(*s.get("frequency")));
                if (auto pts = s["points"].as_array()) {
                    double last = -1e300;
                    for (auto&& pt : *pts) {
                        auto pa = pt.as_array();
                        bool good = pa && pa->size() == 2 && TomlCtx::matches((*pa)[0], VT::Number) && TomlCtx::matches((*pa)[1], VT::Number);
                        if (!good) {
                            ctx_.report("E0006", ctx_.at(pt), "each entry of `points` in " + sw + " must be [time, value] with two numbers",
                                        "e.g. points = [[0, 0], [1e-3, 5]]", ctx_.value_len(pt));
                            break;
                        }
                        double tv = (*pa)[0].value<double>().value_or(0);
                        if (!(tv > last)) {
                            ctx_.report("E0092", ctx_.at(pt), "time points of `points` in " + sw + " are not strictly increasing",
                                        "sort the points by time and remove duplicates", ctx_.value_len(pt));
                            break;
                        }
                        last = tv;
                    }
                }
            }
        }
        if (auto sw = t["sweep"].as_table()) {
            const std::string sww = "`sweep` of " + where;
            ctx_.keys(*sw, sww, {{"stimulus", VT::String, true}, {"from", VT::Value, true}, {"to", VT::Value, true}, {"step", VT::Value, true}});
            std::string unit = "*";
            if (auto sn = sw->get("stimulus"); sn && sn->is_string()) {
                const std::string st = sn->as_string()->get();
                if (!stim_names.count(st)) {
                    std::vector<std::string> c(stim_names.begin(), stim_names.end());
                    ctx_.report("E0113", ctx_.at(*sn), "unknown stimulus `" + st + "` in " + sww,
                                c.empty() ? "add a [[tests.stimulus]] with this name" : suggestion_hint(st, c, "stimuli"), ctx_.value_len(*sn));
                } else {
                    unit = stim_source[st] == "voltage" ? "V" : stim_source[st] == "current" ? "A" : "*";
                }
            }
            Val f = value(*sw, "from", unit, false, sww);
            Val to = value(*sw, "to", unit, false, sww);
            Val st = value(*sw, "step", unit, false, sww);
            if (st.ok && st.num <= 0)
                ctx_.report("E0033", ctx_.at(*sw->get("step")), "`step` of " + sww + " must be positive", "", ctx_.value_len(*sw->get("step")));
            if (f.ok && to.ok) {
                sw_from = std::min(f.num, to.num);
                sw_to = std::max(f.num, to.num);
                have_range = true;
            }
        }

        // expectations
        Scope scope;
        for (const auto& [n, p] : info_.params) scope.values[n] = "parameter";
        scope.nodes = tnodes;
        scope.currents = pin_set_;
        scope.currents.insert(tnames.begin(), tnames.end());
        scope.test = true;
        if (auto xa = t["expect"].as_array(); xa && TomlCtx::matches(*xa, VT::ArrayOfTables)) {
            int xi = 0;
            for (auto&& xn : *xa) {
                const toml::table& x = *xn.as_table();
                const std::string xw = "[[tests.expect]] entry " + std::to_string(++xi) + " of " + where;
                ctx_.keys(x, xw,
                          {{"probe", VT::String, true}, {"value", VT::Value}, {"at", VT::Value}, {"min", VT::Value},
                           {"max", VT::Value}, {"file", VT::String}, {"column", VT::String}, {"abs_tol", VT::Value},
                           {"rel_tol", VT::Number}, {"description", VT::String}});
                if (auto pn = x.get("probe"); pn && pn->is_string()) {
                    PendingExpr pe{pn->as_string()->get(), ctx_.at(*pn), ExprKind::Normal, "`probe` of " + xw};
                    pe.loc.col += 1;
                    resolve(pe, scope);
                }
                const int forms = (x.contains("value") ? 1 : 0) + ((x.contains("min") || x.contains("max")) ? 1 : 0) +
                                  (x.contains("file") ? 1 : 0);
                if (forms != 1) {
                    ctx_.report("E0072", ctx_.at(x), xw + (forms == 0 ? " has no expected result" : " mixes `value`, `min`/`max` and `file`"),
                                "use exactly one of: value (with optional at), min and/or max, or file with column");
                    continue;
                }
                if (x.contains("at") && !x.contains("value"))
                    ctx_.report("E0004", ctx_.key_at(x, "at"), "key `at` is only used together with `value` in " + xw, "", 2);
                if (x.contains("column") && !x.contains("file"))
                    ctx_.report("E0004", ctx_.key_at(x, "column"), "key `column` is only used together with `file` in " + xw, "", 6);
                for (const char* k : {"value", "min", "max"}) value(x, k, "*", false, xw);
                Val abs_tol = value(x, "abs_tol", "*", false, xw);
                if (abs_tol.ok && abs_tol.num < 0)
                    ctx_.report("E0033", ctx_.at(*x.get("abs_tol")), "`abs_tol` in " + xw + " is negative", "", ctx_.value_len(*x.get("abs_tol")));
                if (auto rt = x.get("rel_tol"); rt && rt->value<double>().value_or(0) < 0)
                    ctx_.report("E0033", ctx_.at(*rt), "`rel_tol` in " + xw + " is negative", "", ctx_.value_len(*rt));
                if ((x.contains("value") || x.contains("file")) && !x.contains("abs_tol") && !x.contains("rel_tol"))
                    ctx_.report("E0111", ctx_.at(x.contains("value") ? *x.get("value") : *x.get("file")),
                                xw + " has no tolerance",
                                "add abs_tol (in the unit of the probe) and/or rel_tol (fraction, 0.01 = 1 %)",
                                ctx_.value_len(x.contains("value") ? *x.get("value") : *x.get("file")));
                const std::string probe_unit = "*";
                Val atv = value(x, "at", analysis.value_or("") == "tran" ? "s" : probe_unit, false, xw);
                if (atv.ok && analysis) {
                    if (*analysis == "op")
                        ctx_.report("E0112", ctx_.at(*x.get("at")), "`at` in " + xw + " has no meaning for analysis `op`",
                                    "remove `at`; an operating point has a single value", ctx_.value_len(*x.get("at")));
                    else if (have_range && *analysis == "tran" && (atv.num < 0 || atv.num > t_stop))
                        ctx_.report("E0112", ctx_.at(*x.get("at")), "`at` (" + fmt(atv.num) + " s) in " + xw + " is outside the analysis 0 .. " + fmt(t_stop) + " s",
                                    "use a time between 0 and `stop`, or increase `stop`", ctx_.value_len(*x.get("at")));
                    else if (have_range && *analysis == "dc" && (atv.num < sw_from || atv.num > sw_to))
                        ctx_.report("E0112", ctx_.at(*x.get("at")), "`at` (" + fmt(atv.num) + ") in " + xw + " is outside the sweep " + fmt(sw_from) + " .. " + fmt(sw_to),
                                    "use a sweep value between `from` and `to`", ctx_.value_len(*x.get("at")));
                }
                std::string full, disp;
                if (x.contains("file")) {
                    if (!x.contains("column"))
                        ctx_.report("E0005", ctx_.at(x), "missing required key `column` in " + xw, "name the column of the reference file to compare");
                    if (data_file(x, "file", full, disp)) {
                        const CsvTable& ct = load_csv(full, disp, ctx_.diags);
                        if (ct.ok)
                            if (auto cn = x.get("column"); cn && cn->is_string() && ct.column_index(cn->as_string()->get()) < 0)
                                ctx_.report("E0093", ctx_.at(*cn), "column `" + cn->as_string()->get() + "` not found in `" + disp + "`",
                                            suggestion_hint(cn->as_string()->get(), ct.names, "columns"), ctx_.value_len(*cn));
                    }
                }
            }
        }
    }
}

}  // namespace

PartInfo check_part_file(const std::string& path, const std::string& display, Diagnostics& diags) {
    PartChecker c(path, display, diags);
    return c.run();
}

// ---------------------------------------------------------------- part sets

void check_part_set(const std::vector<const PartInfo*>& parts, Diagnostics& diags, bool check_duplicates,
                    bool check_refs) {
    std::map<std::string, const PartInfo*> by_id;
    for (const PartInfo* p : parts) {
        if (!p->loaded || p->id.empty()) continue;
        auto it = by_id.find(p->id);
        if (it != by_id.end()) {
            if (check_duplicates)
                diags.report("E0011", p->id_loc, "part id `" + p->id + "` is already defined in `" + it->second->display + "`",
                             "part ids must be unique in a library; rename one of the parts", p->id_len);
            continue;
        }
        by_id[p->id] = p;
    }
    if (!check_refs) return;
    std::vector<std::string> ids;
    for (const auto& [id, _] : by_id) ids.push_back(id);

    for (const PartInfo* p : parts) {
        for (const auto& r : p->refs) {
            auto it = by_id.find(r.target);
            if (it == by_id.end()) {
                std::string hint = suggestion_hint(r.target, ids, "parts", 8);
                hint += "; when checking a single file, check the library directory instead so that other parts are loaded";
                diags.report("E0073", r.loc, "unknown part `" + r.target + "` used by element `" + r.element + "`", hint, r.len);
                continue;
            }
            const PartInfo& t = *it->second;
            std::vector<std::string> pnames;
            for (const auto& [n, _] : t.params) pnames.push_back(n);
            for (const auto& prm : r.params) {
                auto pi = t.params.find(prm.name);
                if (pi == t.params.end())
                    diags.report("E0075", prm.loc, "part `" + t.id + "` has no parameter `" + prm.name + "`",
                                 suggestion_hint(prm.name, pnames, "parameters"), prm.len);
                else if (!pi->second.adjustable)
                    diags.report("E0050", prm.loc, "parameter `" + prm.name + "` of part `" + t.id + "` is not adjustable",
                                 "remove it from `params`; the part fixes this value", prm.len);
            }
            for (const auto& f : r.features)
                if (std::find(t.features.begin(), t.features.end(), f.name) == t.features.end())
                    diags.report("E0060", f.loc, "part `" + t.id + "` has no feature `" + f.name + "`",
                                 suggestion_hint(f.name, t.features, "features"), f.len);
            if (r.nodes_is_table) {
                std::set<std::string> given;
                for (const auto& np : r.node_pins) {
                    given.insert(np.name);
                    if (std::find(t.pins.begin(), t.pins.end(), np.name) == t.pins.end())
                        diags.report("E0040", np.loc, "part `" + t.id + "` has no pin `" + np.name + "`",
                                     suggestion_hint(np.name, t.pins, "pins"), np.len);
                }
                for (const auto& pin : t.pins)
                    if (!given.count(pin))
                        diags.report("E0042", r.nodes_loc, "pin `" + pin + "` of part `" + t.id + "` is not connected by element `" + r.element + "`",
                                     "connect every pin; use \"0\" for ground", 1);
            } else if (r.node_count != static_cast<int>(t.pins.size()) && !t.pins.empty()) {
                diags.report("E0042", r.nodes_loc,
                             "element `" + r.element + "` connects " + std::to_string(r.node_count) + " nodes, part `" + t.id + "` has " +
                                 std::to_string(t.pins.size()) + " pins",
                             "pin order of `" + t.id + "`: " + join_quoted(t.pins), 1);
            }
        }
    }

    // recursion
    std::map<std::string, int> state;
    std::function<void(const PartInfo*, std::vector<std::string>&)> dfs = [&](const PartInfo* p, std::vector<std::string>& stack) {
        state[p->id] = 1;
        stack.push_back(p->id);
        for (const auto& r : p->refs) {
            auto it = by_id.find(r.target);
            if (it == by_id.end()) continue;
            if (state[r.target] == 1) {
                std::string chain;
                auto s = std::find(stack.begin(), stack.end(), r.target);
                for (; s != stack.end(); ++s) chain += "`" + *s + "` -> ";
                chain += "`" + r.target + "`";
                diags.report("E0074", r.loc, "part `" + r.target + "` contains itself: " + chain,
                             "a part cannot use itself, directly or through other parts", r.len);
            } else if (state[r.target] == 0) {
                dfs(it->second, stack);
            }
        }
        stack.pop_back();
        state[p->id] = 2;
    };
    for (const auto& [id, p] : by_id) {
        std::vector<std::string> stack;
        if (state[id] == 0) dfs(p, stack);
    }
}

}  // namespace libcheck
