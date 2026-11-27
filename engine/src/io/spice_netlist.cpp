// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file spice_netlist.cpp
 * @brief Definitions (two passes), parameter scopes and subcircuit expansion.
 */
#include <mspice/io/spice_netlist.hpp>

#include <cctype>
#include <cstdio>
#include <set>

namespace mspice::io {

namespace {

/** @brief Trim ASCII whitespace. */
std::string trim(const std::string &s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

/** @brief Whitespace-separated words of @p s. */
std::vector<std::string> words(const std::string &s) {
    std::vector<std::string> r;
    std::string cur;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty())
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

/** @brief Offset in @p s after skipping @p n whitespace-separated words. */
std::size_t skip_words(const std::string &s, int n) {
    std::size_t i = 0;
    for (int k = 0; k < n; ++k) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
        while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])))
            ++i;
    }
    return i;
}

/** @brief Offset of the first top-level assignment `ident=` or `params:` in @p s, npos if
 *  none. */
std::size_t first_assignment(const std::string &s) {
    const std::size_t pp = s.find("params:");
    std::size_t best = pp;
    int depth = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '(' || c == '{')
            ++depth;
        else if ((c == ')' || c == '}') && depth > 0)
            --depth;
        if (depth || !(i == 0 || std::isspace(static_cast<unsigned char>(s[i - 1]))))
            continue;
        if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_'))
            continue;
        std::size_t j = i;
        while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_'))
            ++j;
        while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j])))
            ++j;
        if (j < s.size() && s[j] == '=' && (j + 1 >= s.size() || s[j + 1] != '=')) {
            if (best == std::string::npos || i < best)
                best = i;
            break;
        }
    }
    return best;
}

/** @brief Format a value with full precision (substituted parameter values). */
std::string fmt(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

/** @brief Parse a .model line into a ModelDef; false on syntax errors. */
bool parse_model(const Line &l, ModelDef &m, std::string &err) {
    std::size_t i = skip_words(l.text, 1);
    const std::string rest = trim(l.text.substr(i));
    std::size_t k = 0;
    while (k < rest.size() && !std::isspace(static_cast<unsigned char>(rest[k])))
        ++k;
    m.name = rest.substr(0, k);
    std::string tail = trim(rest.substr(k));
    std::size_t t = 0;
    while (t < tail.size() && (std::isalnum(static_cast<unsigned char>(tail[t])) || tail[t] == '_'))
        ++t;
    m.type = tail.substr(0, t);
    tail = trim(tail.substr(t));
    if (m.name.empty() || m.type.empty()) {
        err = "model name or type missing";
        return false;
    }
    if (!tail.empty() && tail.front() == '(') {
        if (tail.back() != ')') {
            err = "missing ')' in .model";
            return false;
        }
        tail = trim(tail.substr(1, tail.size() - 2));
    }
    bool ok = true;
    auto pairs = split_assignments(tail, &ok);
    // Flags (nchan, pchan, ...) before the first assignment or after a value.
    const std::size_t first = first_assignment(tail);
    for (const std::string &w :
         words(tail.substr(0, first == std::string::npos ? tail.size() : first)))
        m.params.emplace_back(w, "1");
    for (auto &p : pairs) {
        const std::vector<std::string> ws = words(p.second);
        if (ws.size() > 1 && parse_spice_number(ws[0])) {
            m.params.emplace_back(p.first, ws[0]);
            for (std::size_t n = 1; n < ws.size(); ++n)
                m.params.emplace_back(ws[n], "1");
        } else {
            m.params.push_back(p);
        }
    }
    return true;
}

/** @brief Supported model types (others are reported as `unsupported` when used). */
bool model_type_supported(const std::string &t) {
    static const std::set<std::string> ok = {"d", "npn", "pnp", "nmos", "pmos", "vdmos", "sw"};
    return ok.count(t) > 0;
}

/** @brief Directives that the import evaluates or passes on. */
bool directive_known(const std::string &d) {
    static const std::set<std::string> ok = {
        ".tran", ".options", ".option", ".opt",  ".ic",      ".save",  ".print", ".plot",
        ".temp", ".nodeset", ".op",     ".meas", ".measure", ".probe", ".width", ".title"};
    return ok.count(d) > 0;
}

/** @brief Parameter values of one instance level (dynamic scope chain). */
struct ParamFrame {
    const ParamFrame *parent = nullptr;
    std::map<std::string, double> values;
    /** @brief Look up @p name here or in outer levels. */
    std::optional<double> lookup(const std::string &name) const {
        for (const ParamFrame *f = this; f; f = f->parent) {
            auto it = f->values.find(name);
            if (it != f->values.end())
                return it->second;
        }
        return std::nullopt;
    }
};

/** @brief One instance level while expanding. */
struct Frame {
    const Scope *scope = nullptr;
    std::string path;                         ///< instance path, empty at top
    std::map<std::string, std::string> ports; ///< port -> connected net (flattened)
    const ParamFrame *params = nullptr;
    const Frame *caller = nullptr;
    int depth = 0;
};

/** @brief Flattening state. */
class Flattener {
  public:
    /** @brief Prepare with deck and options. */
    Flattener(const Deck &deck, const FlattenOptions &opt) : deck_(deck), opt_(opt) {}

    /** @brief Run all passes. */
    FlatNetlist run() {
        out_.title = deck_.title;
        out_.files = deck_.files;
        out_.control = deck_.control;
        out_.report = deck_.report;
        top_ = collect_definitions(deck_, out_.report);
        eval_params(top_->params, top_->param_locs, global_, "");
        out_.global_params = global_.values;
        Frame top;
        top.scope = top_.get();
        top.params = &global_;
        expand(top);
        for (const Line &l : top_->directives)
            directive(l);
        return std::move(out_);
    }

  private:
    const Deck &deck_;
    const FlattenOptions &opt_;
    FlatNetlist out_;
    std::unique_ptr<Scope> top_;
    ParamFrame global_;
    std::set<std::string> names_;
    std::set<std::string> reported_models_;

    /** @brief Add a report entry. */
    void note(const SourceLoc &loc, const std::string &elem, const std::string &feature,
              const std::string &kind, const std::string &reason) {
        out_.report.push_back({loc.file, loc.line, elem, feature, kind, reason});
    }

    /** @brief Evaluate .param assignments in order into @p into (repeat until no progress,
     *  so global parameters may be used before their definition). */
    void eval_params(const std::vector<std::pair<std::string, std::string>> &ps,
                     const std::vector<SourceLoc> &locs, ParamFrame &into,
                     const std::string &where) {
        std::vector<bool> done(ps.size(), false);
        std::vector<std::string> err(ps.size());
        bool progress = true;
        while (progress) {
            progress = false;
            for (std::size_t k = 0; k < ps.size(); ++k) {
                if (done[k])
                    continue;
                try {
                    into.values[ps[k].first] = evaluate_parameter_expression(
                        ps[k].second, [&](const std::string &n) { return into.lookup(n); });
                    done[k] = true;
                    progress = true;
                } catch (const ExprError &e) {
                    err[k] = e.what();
                }
            }
        }
        for (std::size_t k = 0; k < ps.size(); ++k)
            if (!done[k])
                note(locs[k], where, ".param " + ps[k].first, "unsupported", err[k]);
    }

    /** @brief Flattened node name for local node @p n in frame @p f. */
    static std::string node_name(const Frame &f, const std::string &n) {
        if (n == "0")
            return n;
        auto it = f.ports.find(n);
        if (it != f.ports.end())
            return it->second;
        return f.path.empty() ? n : f.path + "." + n;
    }
    /** @brief Flattened device name for local device @p n in frame @p f (ngspice form). */
    static std::string device_name(const Frame &f, const std::string &n) {
        if (f.path.empty() || n.empty())
            return n;
        return std::string(1, n[0]) + "." + f.path + "." + n;
    }

    /** @brief Evaluate a value token (`{...}`, `'...'` or plain); plain non-numbers stay. */
    std::string value_token(const Frame &f, const std::string &tok, const Line &l,
                            const std::string &elem) {
        if (tok.empty() || (tok[0] != '{' && tok[0] != '\''))
            return tok;
        try {
            return fmt(evaluate_parameter_expression(
                tok, [&](const std::string &n) { return f.params->lookup(n); }));
        } catch (const ExprError &e) {
            note(l.loc, elem, "parameter expression " + tok, "unsupported", e.what());
            return tok;
        }
    }

    /** @brief Find a subcircuit definition visible from @p s (lexical). */
    static const SubcktDef *find_subckt(const Scope *s, const std::string &name) {
        for (; s; s = s->parent) {
            auto it = s->subckts.find(name);
            if (it != s->subckts.end())
                return it->second.get();
        }
        return nullptr;
    }
    /** @brief True when a model @p name is visible from frame @p f. */
    static bool model_visible(const Frame &f, const std::string &name) {
        for (const Scope *s = f.scope; s; s = s->parent)
            if (s->models.count(name))
                return true;
        return false;
    }

    /** @brief Resolve a model reference to its FlatNetlist key (evaluated once per instance
     *  of the defining scope); empty when not found (reported). */
    std::string model_ref(const Frame &f, const std::string &name, const Line &l,
                          const std::string &elem) {
        const Scope *owner = nullptr;
        for (const Scope *s = f.scope; s && !owner; s = s->parent)
            if (s->models.count(name))
                owner = s;
        if (!owner) {
            note(l.loc, elem, "model " + name, "unsupported", "model not defined");
            return {};
        }
        const Frame *def = &f;
        while (def && def->scope != owner)
            def = def->caller;
        const ParamFrame *pf = def ? def->params : &global_;
        const std::string key = (def && !def->path.empty()) ? def->path + ":" + name : name;
        if (!out_.models.count(key)) {
            const ModelDef &md = owner->models.at(name);
            FlatModel fm;
            fm.name = key;
            fm.type = md.type;
            fm.loc = md.loc;
            for (const auto &p : md.params) {
                try {
                    fm.params.emplace_back(
                        p.first, evaluate_parameter_expression(p.second, [&](const std::string &n) {
                            return pf->lookup(n);
                        }));
                } catch (const ExprError &e) {
                    note(md.loc, key, "model parameter " + p.first, "unsupported", e.what());
                }
            }
            out_.models[key] = fm;
        }
        if (!model_type_supported(out_.models[key].type) && reported_models_.insert(key).second)
            note(l.loc, elem, "model type " + out_.models[key].type, "unsupported",
                 "model type not supported (model " + key + ")");
        return key;
    }

    /** @brief Parse and resolve an expression of element @p e; errors go to the report. */
    void expression(const Frame &f, FlatElement &e, const std::string &text, const Line &l) {
        e.expr_text = text;
        try {
            Resolver r;
            r.param = [&](const std::string &n) { return f.params->lookup(n); };
            r.node = [&](const std::string &n) { return node_name(f, n); };
            r.device = [&](const std::string &n) { return device_name(f, n); };
            e.expr = fold(resolve(parse_expression(text), r));
        } catch (const ExprError &err) {
            const std::string msg = err.what();
            std::string feature = "expression";
            const std::size_t q = msg.find("unknown function '");
            if (q != std::string::npos) {
                const std::size_t a = q + 18;
                feature = "function " + msg.substr(a, msg.find('\'', a) - a);
            }
            note(l.loc, e.name, feature, "unsupported", msg);
        }
    }

    /** @brief Split `key = expr` after @p offset in the line text into key and expression. */
    static bool keyed_expression(const std::string &text, std::size_t offset, std::string &key,
                                 std::string &expr) {
        std::size_t i = offset;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        std::size_t j = i;
        while (j < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_'))
            ++j;
        key = text.substr(i, j - i);
        while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j])))
            ++j;
        if (key.empty() || j >= text.size() || text[j] != '=')
            return false;
        expr = trim(text.substr(j + 1));
        return !expr.empty();
    }

    /** @brief Substitute braces in the trailing tokens and store them as args. */
    void store_args(const Frame &f, FlatElement &e, const std::vector<std::string> &tok,
                    std::size_t from, const Line &l) {
        for (std::size_t k = from; k < tok.size(); ++k)
            e.args.push_back(value_token(f, tok[k], l, e.name));
    }

    /** @brief Expand all elements of frame @p f. */
    void expand(const Frame &f) {
        if (f.scope != top_.get() && !f.scope->directives.empty())
            for (const Line &l : f.scope->directives)
                note(l.loc, f.path, "directive " + words(l.text)[0] + " in .subckt", "unsupported",
                     "directives inside subcircuits are not evaluated");
        for (const Line &l : f.scope->elements)
            element(f, l);
    }

    /** @brief Flatten one element line. */
    void element(const Frame &f, const Line &l) {
        const std::vector<std::string> tok = tokenize_line(l.text);
        if (tok.empty())
            return;
        const std::string &local = tok[0];
        const char type = local[0];
        if (type == 'x') {
            instance(f, l, tok);
            return;
        }
        FlatElement e;
        e.type = type;
        e.local_name = local;
        e.path = f.path;
        e.name = device_name(f, local);
        e.loc = l.loc;
        if (!names_.insert(e.name).second)
            note(l.loc, e.name, "name", "unsupported", "duplicate element name after flattening");
        auto nodes = [&](std::size_t n) {
            for (std::size_t k = 1; k <= n && k < tok.size(); ++k)
                e.nodes.push_back(node_name(f, tok[k]));
            if (tok.size() <= n)
                note(l.loc, e.name, "nodes", "unsupported",
                     "too few nodes (" + std::to_string(n) + " expected)");
        };
        auto keyed = [&](int skip) {
            std::string key, ex;
            if (!keyed_expression(l.text, skip_words(l.text, skip), key, ex)) {
                note(l.loc, e.name, "value", "unsupported", "expression of the form key=... expected");
                return;
            }
            e.expr_key = key;
            expression(f, e, ex, l);
        };
        switch (type) {
        case 'r':
        case 'c':
        case 'l':
        case 'v':
        case 'i':
            nodes(2);
            store_args(f, e, tok, 3, l);
            break;
        case 'b':
            nodes(2);
            keyed(3);
            break;
        case 'e':
        case 'g': {
            const std::string kw = tok.size() > 3 ? tok[3] : std::string();
            if (kw == "value" || kw == "vol" || kw == "cur") {
                nodes(2);
                keyed(3);
            } else if (kw == "poly" || kw == "table" || kw == "laplace" || kw == "freq") {
                nodes(2);
                store_args(f, e, tok, 3, l);
                note(l.loc, e.name, std::string(1, type) + " " + kw, "unsupported",
                     "form not supported");
            } else {
                nodes(4);
                store_args(f, e, tok, 5, l);
            }
            break;
        }
        case 'f':
        case 'h':
            nodes(2);
            if (tok.size() > 3 && tok[3] == "poly") {
                store_args(f, e, tok, 3, l);
                note(l.loc, e.name, std::string(1, type) + " poly", "unsupported",
                     "form not supported");
            } else if (tok.size() > 3) {
                e.refs.push_back(device_name(f, tok[3]));
                store_args(f, e, tok, 4, l);
            }
            break;
        case 'd':
            nodes(2);
            if (tok.size() > 3)
                e.model = model_ref(f, tok[3], l, e.name);
            store_args(f, e, tok, 4, l);
            break;
        case 'q':
        case 'm': {
            // Node count from the position of the model name (Q: 3-4, M: 3 (VDMOS) or 4).
            std::size_t mi = 0;
            for (std::size_t k = 4; k <= 5 && k < tok.size(); ++k) {
                if (model_visible(f, tok[k])) {
                    mi = k;
                    break;
                }
            }
            if (!mi) {
                const std::size_t guess = type == 'q' ? 4 : 5;
                nodes(guess - 1);
                note(l.loc, e.name, "model", "unsupported",
                     "no defined model found at position 4 or 5");
                store_args(f, e, tok, guess, l);
                break;
            }
            nodes(mi - 1);
            e.model = model_ref(f, tok[mi], l, e.name);
            store_args(f, e, tok, mi + 1, l);
            break;
        }
        case 's':
            nodes(4);
            if (tok.size() > 5)
                e.model = model_ref(f, tok[5], l, e.name);
            store_args(f, e, tok, 6, l);
            break;
        case 'w':
            nodes(2);
            if (tok.size() > 3)
                e.refs.push_back(device_name(f, tok[3]));
            if (tok.size() > 4)
                e.model = model_ref(f, tok[4], l, e.name);
            store_args(f, e, tok, 5, l);
            note(l.loc, e.name, "W", "unsupported", "current-controlled switch is not supported");
            break;
        case 'k':
            for (std::size_t k = 1; k < tok.size() && k <= 2; ++k)
                e.refs.push_back(device_name(f, tok[k]));
            store_args(f, e, tok, 3, l);
            note(l.loc, e.name, "K", "unsupported", "coupled inductors are not supported");
            break;
        default: {
            // J 3, T 4, O 4, U 3, Z 3 nodes; others unknown.
            static const std::map<char, std::size_t> counts = {
                {'j', 3}, {'t', 4}, {'o', 4}, {'u', 3}, {'z', 3}};
            auto it = counts.find(type);
            if (it != counts.end()) {
                nodes(it->second);
                store_args(f, e, tok, it->second + 1, l);
            } else {
                store_args(f, e, tok, 1, l);
            }
            note(l.loc, e.name, std::string(1, static_cast<char>(std::toupper(type))), "unsupported",
                 "element type not supported");
        }
        }
        out_.elements.push_back(std::move(e));
    }

    /** @brief Expand a subcircuit instance X. */
    void instance(const Frame &f, const Line &l, const std::vector<std::string> &tok) {
        const std::string &local = tok[0];
        const std::string iname = f.path.empty() ? local : f.path + "." + local;
        std::size_t a = first_assignment(l.text);
        const std::string head = l.text.substr(0, a == std::string::npos ? l.text.size() : a);
        std::string tail = a == std::string::npos ? std::string() : l.text.substr(a);
        if (tail.rfind("params:", 0) == 0)
            tail = tail.substr(7);
        const std::vector<std::string> w = words(head);
        if (w.size() < 2) {
            note(l.loc, iname, "X", "unsupported", "subcircuit name missing");
            return;
        }
        const std::string sub = w.back();
        const SubcktDef *def = find_subckt(f.scope, sub);
        if (!def) {
            note(l.loc, iname, "subcircuit " + sub, "unsupported", "subcircuit not defined");
            return;
        }
        const std::size_t nconn = w.size() - 2;
        if (nconn != def->ports.size()) {
            note(l.loc, iname, "subcircuit " + sub, "unsupported",
                 "connection count " + std::to_string(nconn) + " instead of " +
                     std::to_string(def->ports.size()));
            return;
        }
        if (f.depth + 1 > opt_.max_depth) {
            note(l.loc, iname, "subcircuit " + sub, "unsupported", "nesting depth exceeded");
            return;
        }
        if (!names_.insert("x:" + iname).second)
            note(l.loc, iname, "name", "unsupported", "duplicate instance name after flattening");
        Frame nf;
        nf.scope = &def->body;
        nf.path = iname;
        nf.caller = &f;
        nf.depth = f.depth + 1;
        for (std::size_t k = 0; k < def->ports.size(); ++k)
            nf.ports[def->ports[k]] = node_name(f, w[k + 1]);
        ParamFrame pf;
        pf.parent = f.params;
        bool ok = true;
        const auto inst = split_assignments(tail, &ok);
        if (!ok)
            note(l.loc, iname, "instance parameters", "unsupported", "unreadable parameter list: " + tail);
        std::map<std::string, std::string> given(inst.begin(), inst.end());
        for (const auto &d : def->defaults) {
            auto it = given.find(d.first);
            try {
                if (it != given.end())
                    pf.values[d.first] = evaluate_parameter_expression(
                        it->second, [&](const std::string &n) { return f.params->lookup(n); });
                else
                    pf.values[d.first] = evaluate_parameter_expression(
                        d.second, [&](const std::string &n) { return pf.lookup(n); });
            } catch (const ExprError &e) {
                note(l.loc, iname, "parameter " + d.first, "unsupported", e.what());
            }
        }
        for (const auto &g : inst) {
            bool declared = false;
            for (const auto &d : def->defaults)
                declared = declared || d.first == g.first;
            if (declared)
                continue;
            if (g.first == "m")
                note(l.loc, iname, "instance multiplier m", "unsupported",
                     "m is not declared as a parameter of " + sub);
            else
                note(l.loc, iname, "parameter " + g.first, "unsupported",
                     "not declared in .subckt " + sub);
        }
        eval_params(def->body.params, def->body.param_locs, pf, iname);
        nf.params = &pf;
        expand(nf);
    }

    /** @brief Store a top-level directive (braces evaluated with global parameters). */
    void directive(const Line &l) {
        Directive d;
        d.text = l.text;
        d.loc = l.loc;
        d.tokens = tokenize_line(l.text);
        d.name = d.tokens.empty() ? std::string() : d.tokens[0];
        Frame top;
        top.scope = top_.get();
        top.params = &global_;
        for (std::size_t k = 1; k < d.tokens.size(); ++k)
            d.tokens[k] = value_token(top, d.tokens[k], l, d.name);
        if (!directive_known(d.name))
            note(l.loc, d.name, "directive " + d.name, "unsupported",
                 "directive not supported");
        out_.directives.push_back(std::move(d));
    }
};

} // namespace

std::unique_ptr<Scope> collect_definitions(const Deck &deck, std::vector<ReportItem> &report) {
    auto top = std::make_unique<Scope>();
    std::vector<Scope *> stack{top.get()};
    std::vector<SubcktDef *> defs;
    auto note = [&](const SourceLoc &loc, const std::string &elem, const std::string &feature,
                    const std::string &kind, const std::string &reason) {
        report.push_back({loc.file, loc.line, elem, feature, kind, reason});
    };
    for (const Line &l : deck.lines) {
        const std::vector<std::string> w = words(l.text);
        if (w.empty())
            continue;
        const std::string &cmd = w[0];
        Scope *cur = stack.back();
        if (cmd[0] != '.') {
            cur->elements.push_back(l);
            continue;
        }
        if (cmd == ".subckt") {
            const std::size_t body = skip_words(l.text, 1);
            const std::string rest = l.text.substr(body);
            const std::size_t a = first_assignment(rest);
            const std::vector<std::string> names =
                words(rest.substr(0, a == std::string::npos ? rest.size() : a));
            if (names.empty()) {
                note(l.loc, "", ".subckt", "unsupported", "subcircuit name missing");
                continue;
            }
            auto def = std::make_shared<SubcktDef>();
            def->name = names[0];
            def->ports.assign(names.begin() + 1, names.end());
            def->loc = l.loc;
            def->body.parent = cur;
            if (a != std::string::npos) {
                std::string tail = rest.substr(a);
                if (tail.rfind("params:", 0) == 0)
                    tail = tail.substr(7);
                bool ok = true;
                def->defaults = split_assignments(tail, &ok);
                if (!ok)
                    note(l.loc, def->name, ".subckt params", "unsupported",
                         "unreadable parameter list: " + tail);
            }
            if (cur->subckts.count(def->name))
                note(l.loc, def->name, ".subckt " + def->name, "note",
                     "subcircuit defined twice, the last definition applies");
            cur->subckts[def->name] = def;
            defs.push_back(def.get());
            stack.push_back(&def->body);
        } else if (cmd == ".ends") {
            if (defs.empty()) {
                note(l.loc, "", ".ends", "unsupported", ".ends without .subckt");
                continue;
            }
            if (w.size() > 1 && w[1] != defs.back()->name)
                note(l.loc, defs.back()->name, ".ends " + w[1], "note",
                     "name does not match .subckt " + defs.back()->name);
            defs.pop_back();
            stack.pop_back();
        } else if (cmd == ".model") {
            ModelDef m;
            m.loc = l.loc;
            std::string err;
            if (!parse_model(l, m, err)) {
                note(l.loc, "", ".model", "unsupported", err);
                continue;
            }
            if (cur->models.count(m.name))
                note(l.loc, m.name, ".model " + m.name, "note",
                     "duplicate model name (" + cur->models[m.name].loc.file + ":" +
                         std::to_string(cur->models[m.name].loc.line) +
                         "), the last definition applies");
            cur->models[m.name] = m;
        } else if (cmd == ".param") {
            bool ok = true;
            const auto ps = split_assignments(l.text.substr(skip_words(l.text, 1)), &ok);
            if (!ok || ps.empty())
                note(l.loc, "", ".param", "unsupported", "unreadable parameter line");
            for (const auto &p : ps) {
                cur->params.push_back(p);
                cur->param_locs.push_back(l.loc);
            }
        } else if (cmd == ".func" || cmd == ".global" || cmd == ".csparam") {
            note(l.loc, "", cmd, "unsupported", "directive not supported");
        } else {
            cur->directives.push_back(l);
        }
    }
    for (const SubcktDef *d : defs)
        note(d->loc, d->name, ".subckt " + d->name, "unsupported", ".ends missing");
    return top;
}

FlatNetlist flatten(const Deck &deck, const FlattenOptions &opt) {
    return Flattener(deck, opt).run();
}

FlatNetlist load_spice_file(const std::string &path, const ReadOptions &ropt,
                            const FlattenOptions &fopt) {
    return flatten(read_spice_file(path, ropt), fopt);
}

std::map<char, int> FlatNetlist::count_by_type() const {
    std::map<char, int> r;
    for (const FlatElement &e : elements)
        ++r[e.type];
    return r;
}

std::vector<ReportItem> FlatNetlist::missing() const {
    std::vector<ReportItem> r;
    for (const ReportItem &i : report)
        if (i.kind == "unsupported")
            r.push_back(i);
    return r;
}

const FlatElement *FlatNetlist::find(const std::string &name) const {
    for (const FlatElement &e : elements)
        if (e.name == name)
            return &e;
    return nullptr;
}

} // namespace mspice::io
