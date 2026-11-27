// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file spice_import.cpp
 * @brief Flattened SPICE netlist to sim::Circuit and sim::Settings.
 */
#include <mspice/io/spice_import.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>

namespace mspice::io {

std::string Diagnostic::format(const char *severity) const {
    std::string s;
    if (!file.empty()) {
        s = file;
        if (line > 0)
            s += ":" + std::to_string(line);
        s += ": ";
    }
    return s + severity + ": " + message;
}

namespace {

/** @brief what() text of an ImportError: the formatted diagnostics, one per line. */
std::string join(const std::vector<Diagnostic> &d) {
    std::string s;
    for (const Diagnostic &x : d)
        s += (s.empty() ? "" : "\n") + x.format();
    return s;
}

} // namespace

ImportError::ImportError(std::vector<Diagnostic> diagnostics)
    : std::runtime_error(join(diagnostics)), diagnostics_(std::move(diagnostics)) {}

namespace {

/** @brief Thermal voltage at 300 K used by the diode and BJT stamps. */
constexpr double thermal_voltage = 0.02585;

/** @brief Throw an ImportError for one location. */
[[noreturn]] void fail(const SourceLoc &loc, const std::string &msg) {
    throw ImportError({Diagnostic{loc.file, loc.line, msg}});
}

/** @brief Number token or ImportError "<what>: invalid number '<tok>'". */
double number(const SourceLoc &loc, const std::string &what, const std::string &tok) {
    const std::optional<double> v = parse_spice_number(tok);
    if (!v || !std::isfinite(*v))
        fail(loc, what + ": invalid number '" + tok + "'");
    return *v;
}

/** @brief Upper-case element letter for messages. */
std::string letter(char type) {
    return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(type))));
}

/** @brief Probe name of a branch current, as ngspice prints it. */
std::string current_name(const std::string &device) { return "i(" + device + ")"; }

/** @brief Values of a .tran directive. */
struct Tran {
    double step = 0, stop = 0, start = 0, max_step = 0;
    bool uic = false;
    bool present = false;
};

/** @brief Parser and mapper state for one netlist. */
class Importer {
  public:
    Importer(const FlatNetlist &n, const ImportOptions &opt) : n_(n), opt_(opt) {}

    Simulation run() {
        check_report();
        directives();
        for (const FlatElement &e : n_.elements) {
            element(e);
            connect(e);
        }
        check_floating();
        apply_initial_conditions();
        sim_.start = tran_.start;
        return std::move(sim_);
    }

  private:
    const FlatNetlist &n_;
    const ImportOptions &opt_;
    Simulation sim_;
    Tran tran_;
    std::vector<std::pair<std::string, std::pair<double, SourceLoc>>> ic_;
    std::map<std::string, std::string> parent_; ///< Union-find over nets joined by a DC path.
    std::map<std::string, SourceLoc> first_use_; ///< First element line that uses a net.
    std::vector<std::string> net_order_;         ///< Nets in order of first use.

    /** @brief Ground names as the circuit treats them. */
    static bool is_ground(const std::string &n) { return n == "0" || n == "gnd"; }
    /** @brief Union-find root of net @p n (ground nets map to "0"). */
    std::string root(const std::string &n) {
        std::string r = is_ground(n) ? std::string("0") : n;
        while (parent_.count(r) && parent_[r] != r)
            r = parent_[r];
        return r;
    }
    /** @brief Join the nets @p a and @p b. */
    void join(const std::string &a, const std::string &b) {
        const std::string ra = root(a), rb = root(b);
        if (ra == rb)
            return;
        // Keep ground as the root of its set.
        if (rb == "0")
            parent_[ra] = rb;
        else
            parent_[rb] = ra;
    }

    /** @brief Record the nets of element @p e and the DC paths it provides. Current sources,
     *  the MOSFET gate and the control inputs of E/G/S provide none; everything else is
     *  treated as connecting its terminals (capacitors included: the engine solves the
     *  operating point with them). */
    void connect(const FlatElement &e) {
        for (const std::string &n : e.nodes) {
            if (is_ground(n))
                continue;
            if (!parent_.count(n)) {
                parent_[n] = n;
                net_order_.push_back(n);
                first_use_[n] = e.loc;
            }
        }
        switch (e.type) {
        case 'i':
            return;
        case 'm':
            join(e.nodes[0], e.nodes[2]); // drain-source; gate and bulk carry no DC current
            return;
        case 'q':
            join(e.nodes[0], e.nodes[1]);
            join(e.nodes[1], e.nodes[2]);
            return;
        default:
            // Two-terminal elements and the outputs of S, E, G, B (controls are not paths).
            if (e.nodes.size() >= 2)
                join(e.nodes[0], e.nodes[1]);
        }
    }

    /** @brief Nets without any DC path to ground make the MNA matrix singular: name them. */
    void check_floating() {
        std::map<std::string, std::vector<std::string>> groups;
        std::vector<std::string> order;
        for (const std::string &n : net_order_) {
            const std::string r = root(n);
            if (r == "0")
                continue;
            if (!groups.count(r))
                order.push_back(r);
            groups[r].push_back(n);
        }
        std::vector<Diagnostic> errors;
        for (const std::string &r : order) {
            const std::vector<std::string> &g = groups[r];
            std::string names;
            for (const std::string &n : g)
                names += (names.empty() ? "'" : ", '") + n + "'";
            const SourceLoc &loc = first_use_[g.front()];
            errors.push_back({loc.file, loc.line,
                              (g.size() == 1 ? "node " + names + " has" : "nodes " + names + " have") +
                                  " no DC path to ground (floating)"});
        }
        if (!errors.empty())
            throw ImportError(std::move(errors));
    }

    /** @brief Every `unsupported` report entry becomes one diagnostic. */
    void check_report() {
        std::vector<Diagnostic> errors;
        for (const ReportItem &r : n_.report) {
            const std::string who = !r.element.empty() ? r.element : r.feature;
            const std::string msg = who.empty() ? r.reason : who + ": " + r.reason;
            if (r.kind == "unsupported")
                errors.push_back({r.file, r.line, msg});
            else
                sim_.warnings.push_back({r.file, r.line, msg});
        }
        if (!errors.empty())
            throw ImportError(std::move(errors));
    }

    // ---- directives --------------------------------------------------------------------

    void directives() {
        sim::Settings &s = sim_.settings;
        std::optional<sim::Method> method;
        for (const Directive &d : n_.directives) {
            const std::vector<std::string> &t = d.tokens;
            if (d.name == ".tran") {
                tran(d);
            } else if (d.name == ".options" || d.name == ".option" || d.name == ".opt") {
                // key = value pairs; tokenize_line() makes '=' a token of its own.
                for (std::size_t k = 1; k < t.size(); ++k) {
                    const std::string &key = t[k];
                    if (k + 2 >= t.size() || t[k + 1] != "=")
                        fail(d.loc, ".options: option '" + key + "' needs a value (key=value)");
                    const std::string &val = t[k + 2];
                    if (key == "reltol") {
                        s.rel_tol = number(d.loc, ".options reltol", val);
                        if (!(s.rel_tol > 0))
                            fail(d.loc, ".options reltol: must be positive");
                    } else if (key == "abstol") {
                        s.abs_tol = number(d.loc, ".options abstol", val);
                        if (!(s.abs_tol > 0))
                            fail(d.loc, ".options abstol: must be positive");
                    } else if (key == "method") {
                        if (val == "trap" || val == "trapezoidal")
                            method = sim::Method::Trapezoid;
                        else if (val == "gear")
                            method = sim::Method::BDF2;
                        else if (val == "euler" || val == "be")
                            method = sim::Method::BackwardEuler;
                        else
                            fail(d.loc, ".options method: expected trap, gear or euler, got '" +
                                            val + "'");
                    } else {
                        fail(d.loc, ".options: option '" + key + "' is not supported");
                    }
                    k += 2;
                }
            } else if (d.name == ".ic") {
                initial_conditions(d);
            } else {
                fail(d.loc, d.name + ": directive not supported by mspice run");
            }
        }
        if (!tran_.present)
            throw ImportError({Diagnostic{n_.files.empty() ? std::string() : n_.files.front(), 0,
                                          "no .tran directive; mspice run needs a transient "
                                          "analysis (.tran tstep tstop)"}});
        if (method)
            s.method = *method;
        s.stop = tran_.stop;
        s.max_step = tran_.max_step;
        s.operating_point = !tran_.uic;
        s.adaptive = opt_.adaptive;
        if (opt_.adaptive) {
            // SPICE-like step and Newton control (see Settings): one solve per step with a
            // predictor error estimate, at most 10 Newton iterations per attempt (ITL4), an
            // eightfold step cut after a failed attempt and an extrapolated Newton start.
            s.lte_method = sim::LteMethod::Predictor;
            s.newton_max = 10;
            s.newton_cut = 8;
            s.predict_guess = true;
        }
    }

    /** @brief .tran tstep tstop [tstart [tmax]] [uic] */
    void tran(const Directive &d) {
        if (tran_.present)
            fail(d.loc, ".tran: only one .tran directive is supported");
        std::vector<double> v;
        for (std::size_t k = 1; k < d.tokens.size(); ++k) {
            if (d.tokens[k] == "uic") {
                tran_.uic = true;
                continue;
            }
            v.push_back(number(d.loc, ".tran", d.tokens[k]));
        }
        if (v.size() < 2 || v.size() > 4)
            fail(d.loc, ".tran: expected tstep tstop [tstart [tmax]] [uic]");
        tran_.step = v[0];
        tran_.stop = v[1];
        tran_.start = v.size() > 2 ? v[2] : 0.0;
        if (!(tran_.step > 0) || !(tran_.stop > 0))
            fail(d.loc, ".tran: tstep and tstop must be positive");
        if (tran_.start < 0 || tran_.start >= tran_.stop)
            fail(d.loc, ".tran: tstart must be at least 0 and below tstop");
        // ngspice default for the largest step: min(tstep, (tstop - tstart) / 50).
        tran_.max_step = v.size() > 3 ? v[3] : std::min(tran_.step, (tran_.stop - tran_.start) / 50);
        if (!(tran_.max_step > 0))
            fail(d.loc, ".tran: tmax must be positive");
        tran_.present = true;
    }

    /** @brief .ic v(net)=value ... */
    void initial_conditions(const Directive &d) {
        const std::vector<std::string> &t = d.tokens;
        for (std::size_t k = 1; k < t.size();) {
            if (k + 5 >= t.size() || t[k] != "v" || t[k + 1] != "(" || t[k + 3] != ")" ||
                t[k + 4] != "=")
                fail(d.loc, ".ic: expected v(net)=value");
            ic_.push_back({t[k + 2], {number(d.loc, ".ic v(" + t[k + 2] + ")", t[k + 5]), d.loc}});
            k += 6;
        }
    }

    void apply_initial_conditions() {
        for (const auto &[net, value] : ic_) {
            if (!is_ground(net) && !parent_.count(net))
                fail(value.second, ".ic: unknown node '" + net + "'");
            try {
                sim_.circuit.initial_voltage(net, value.first);
            } catch (const std::invalid_argument &e) {
                fail(value.second, ".ic v(" + net + "): " + e.what());
            }
        }
    }

    // ---- elements ----------------------------------------------------------------------

    /** @brief Model of element @p e with the expected types, or an error. */
    const FlatModel &model(const FlatElement &e, std::initializer_list<const char *> types) {
        auto it = n_.models.find(e.model);
        if (e.model.empty() || it == n_.models.end())
            fail(e.loc, e.name + ": model missing");
        for (const char *t : types)
            if (it->second.type == t)
                return it->second;
        fail(e.loc, e.name + ": model '" + e.model + "' has type '" + it->second.type +
                        "', not usable for element " + letter(e.type));
    }

    /** @brief Model parameters as a map; any key outside @p allowed is an error. */
    std::map<std::string, double> params(const FlatElement &e, const FlatModel &m,
                                         const std::set<std::string> &allowed) {
        std::map<std::string, double> r;
        for (const auto &[k, v] : m.params) {
            if (!allowed.count(k))
                fail(e.loc, e.name + ": model parameter '" + k + "' of model '" + m.name +
                                "' is not supported");
            r[k] = v;
        }
        return r;
    }

    /** @brief Error when element @p e has trailing arguments (from index @p from). */
    void no_extra(const FlatElement &e, std::size_t from = 0) {
        if (e.args.size() > from)
            fail(e.loc, e.name + ": unexpected argument '" + e.args[from] + "'");
    }

    /** @brief The single value argument of an R, C or L element. */
    double value(const FlatElement &e) {
        if (e.args.empty())
            fail(e.loc, e.name + ": value missing");
        const double v = number(e.loc, e.name, e.args[0]);
        no_extra(e, 1);
        return v;
    }

    /** @brief Numbers of a source function from args[i] (after the keyword): either in
     *  parentheses or bare until the next keyword. Advances @p i past them. */
    std::vector<double> function_args(const FlatElement &e, const std::string &fn,
                                      std::size_t &i) {
        std::vector<double> v;
        if (i < e.args.size() && e.args[i] == "(") {
            ++i;
            while (i < e.args.size() && e.args[i] != ")")
                v.push_back(number(e.loc, e.name + " " + fn, e.args[i++]));
            if (i >= e.args.size())
                fail(e.loc, e.name + ": missing ')' after " + fn + "(");
            ++i;
        } else {
            while (i < e.args.size() && parse_spice_number(e.args[i]))
                v.push_back(number(e.loc, e.name + " " + fn, e.args[i++]));
        }
        return v;
    }

    /** @brief Waveform of an independent V or I source. */
    sim::Waveform waveform(const FlatElement &e) {
        sim::Waveform w;
        std::optional<double> dc;
        bool shaped = false;
        std::size_t i = 0;
        if (e.args.empty())
            fail(e.loc, e.name + ": value missing");
        while (i < e.args.size()) {
            const std::string &tok = e.args[i];
            if (tok == "dc") {
                ++i;
                if (i >= e.args.size())
                    fail(e.loc, e.name + ": value missing after dc");
                dc = number(e.loc, e.name, e.args[i++]);
            } else if (tok == "ac") {
                ++i;
                for (int k = 0; k < 2 && i < e.args.size() && parse_spice_number(e.args[i]); ++k)
                    ++i;
                sim_.warnings.push_back(
                    {e.loc.file, e.loc.line, e.name + ": ac specification ignored (transient only)"});
            } else if (tok == "pulse" || tok == "sin" || tok == "pwl") {
                if (shaped)
                    fail(e.loc, e.name + ": more than one source function");
                shaped = true;
                ++i;
                const std::vector<double> a = function_args(e, tok, i);
                if (tok == "pulse")
                    pulse(e, a, w);
                else if (tok == "sin")
                    sine(e, a, w);
                else
                    pwl(e, a, w);
            } else if (tok == "exp" || tok == "sffm" || tok == "am" || tok == "trnoise" ||
                       tok == "trrandom") {
                fail(e.loc, e.name + ": source function " + tok + " is not supported");
            } else if (!dc && !shaped && parse_spice_number(tok)) {
                dc = number(e.loc, e.name, tok);
                ++i;
            } else {
                if (tok == "(" || tok == ")")
                    fail(e.loc, e.name + ": unexpected '" + tok + "'");
                if (!parse_spice_number(tok))
                    fail(e.loc, e.name + ": invalid number '" + tok + "'");
                fail(e.loc, e.name + ": unexpected argument '" + tok + "'");
            }
        }
        if (!shaped) {
            if (!dc)
                fail(e.loc, e.name + ": value missing");
            w.high = *dc;
        }
        return w;
    }

    /** @brief PULSE(v1 v2 [td [tr [tf [pw [per [np]]]]]]) with ngspice defaults. */
    void pulse(const FlatElement &e, const std::vector<double> &a, sim::Waveform &w) {
        if (a.size() < 2 || a.size() > 8)
            fail(e.loc, e.name + ": pulse expects 2 to 8 values (v1 v2 td tr tf pw per np)");
        w.pulse = true;
        w.low = a[0];
        w.high = a[1];
        w.delay = a.size() > 2 ? a[2] : 0.0;
        w.rise = a.size() > 3 && a[3] > 0 ? a[3] : tran_.step;
        w.fall = a.size() > 4 && a[4] > 0 ? a[4] : tran_.step;
        w.on = a.size() > 5 ? a[5] : tran_.stop;
        w.period = a.size() > 6 && a[6] > 0 ? a[6] : std::max(tran_.stop, w.rise + w.on + w.fall);
        if (a.size() > 7) {
            if (a[7] < 0 || a[7] != std::floor(a[7]))
                fail(e.loc, e.name + ": pulse count must be a non-negative integer");
            w.cycles = static_cast<int>(a[7]);
        }
        if (w.delay < 0 || w.on < 0 || w.rise + w.on + w.fall > w.period)
            fail(e.loc, e.name + ": invalid pulse timing (tr + pw + tf must not exceed per)");
    }

    /** @brief SIN(vo va [freq [td [theta [phase]]]]) with ngspice defaults. */
    void sine(const FlatElement &e, const std::vector<double> &a, sim::Waveform &w) {
        if (a.size() < 2 || a.size() > 6)
            fail(e.loc, e.name + ": sin expects 2 to 6 values (vo va freq td theta phase)");
        w.sine = true;
        w.low = a[0];
        w.high = a[1];
        w.freq = a.size() > 2 ? a[2] : 1.0 / tran_.stop;
        w.delay = a.size() > 3 ? a[3] : 0.0;
        w.damping = a.size() > 4 ? a[4] : 0.0;
        w.phase = a.size() > 5 ? a[5] : 0.0;
        if (w.delay < 0 || w.freq < 0)
            fail(e.loc, e.name + ": sin frequency and delay must not be negative");
    }

    /** @brief PWL(t1 v1 t2 v2 ...). */
    void pwl(const FlatElement &e, const std::vector<double> &a, sim::Waveform &w) {
        if (a.size() < 2 || a.size() % 2 != 0)
            fail(e.loc, e.name + ": pwl expects pairs of time and value");
        for (std::size_t k = 0; k < a.size(); k += 2) {
            if (a[k] < 0 || (k > 0 && a[k] < a[k - 2]))
                fail(e.loc, e.name + ": pwl times must be non-negative and non-decreasing");
            w.trace.push_back({a[k], a[k + 1]});
        }
    }

    /** @brief Key=value arguments (w=1u l=2u) of a MOSFET instance. */
    std::map<std::string, double> keyed_args(const FlatElement &e,
                                             const std::set<std::string> &allowed) {
        std::map<std::string, double> r;
        for (std::size_t k = 0; k < e.args.size(); k += 3) {
            if (k + 2 >= e.args.size() || e.args[k + 1] != "=")
                fail(e.loc, e.name + ": unexpected argument '" + e.args[k] + "'");
            if (!allowed.count(e.args[k]))
                fail(e.loc, e.name + ": instance parameter '" + e.args[k] + "' is not supported");
            r[e.args[k]] = number(e.loc, e.name + " " + e.args[k], e.args[k + 2]);
        }
        return r;
    }

    /** @brief Behavioural expression of B/E/G: register it as a voltage or current source. */
    void expression_source(const FlatElement &e, bool voltage) {
        if (!e.expr)
            fail(e.loc, e.name + ": expression missing");
        Tape tape;
        try {
            tape = compile(e.expr);
        } catch (const ExprError &err) {
            fail(e.loc, e.name + ": " + err.what());
        }
        for (const ControlRef &c : tape.controls)
            if (c.kind == ControlRef::Kind::Current)
                fail(e.loc, e.name + ": current control i(" + c.name +
                                ") is not supported by mspice run");
        const std::vector<std::string> controls = tape.control_names();
        for (double t : tape.breakpoints)
            sim_.circuit.breakpoint(t);
        auto f = [tape](const auto &v) { return evaluate(tape, v); };
        if (voltage)
            sim_.circuit.behavioural_voltage(e.nodes[0], e.nodes[1], controls, f);
        else
            sim_.circuit.behavioural_current(e.nodes[0], e.nodes[1], controls, f);
    }

    void element(const FlatElement &e) {
        sim::Circuit &c = sim_.circuit;
        try {
            switch (e.type) {
            case 'r':
                c.resistor(e.nodes[0], e.nodes[1], value(e));
                break;
            case 'c':
                c.capacitor(e.nodes[0], e.nodes[1], value(e));
                break;
            case 'l':
                c.inductor(e.nodes[0], e.nodes[1], value(e), current_name(e.name));
                break;
            case 'v':
                c.voltage(e.nodes[0], e.nodes[1], waveform(e), current_name(e.name));
                break;
            case 'i': {
                sim::Waveform w = waveform(e);
                if (!w.pulse && !w.sine && w.trace.empty())
                    c.current(e.nodes[0], e.nodes[1], w.high);
                else
                    c.current(e.nodes[0], e.nodes[1], w);
                break;
            }
            case 'd':
                diode(e);
                break;
            case 'q':
                bjt(e);
                break;
            case 'm':
                mosfet(e);
                break;
            case 's':
                voltage_switch(e);
                break;
            case 'b':
                if (e.expr_key != "v" && e.expr_key != "i")
                    fail(e.loc, e.name + ": expected v=... or i=...");
                expression_source(e, e.expr_key == "v");
                break;
            case 'e':
            case 'g':
                controlled_source(e);
                break;
            default:
                fail(e.loc, e.name + ": element type " + letter(e.type) +
                                " is not supported by mspice run");
            }
        } catch (const std::invalid_argument &err) {
            fail(e.loc, e.name + ": " + err.what());
        }
    }

    void diode(const FlatElement &e) {
        const FlatModel &m = model(e, {"d"});
        no_extra(e);
        auto p = params(e, m, {"is", "n", "bv", "ibv", "nbv", "cjo", "cj0", "m", "vj", "fc", "tt"});
        sim::DiodeParams d;
        auto get = [&](const char *k, double def) { return p.count(k) ? p[k] : def; };
        d.is = get("is", 1e-14);
        d.vt = get("n", 1.0) * thermal_voltage;
        d.bv = get("bv", 0.0);
        d.ibv = get("ibv", 1e-3);
        d.nbv = get("nbv", 1.0);
        d.cjo = get("cjo", get("cj0", 0.0));
        d.m = get("m", 0.5);
        d.vj = get("vj", 1.0);
        d.fc = get("fc", 0.5);
        d.tt = get("tt", 0.0);
        sim_.circuit.diode(e.nodes[0], e.nodes[1], d);
    }

    void bjt(const FlatElement &e) {
        if (e.nodes.size() != 3)
            fail(e.loc, e.name + ": substrate node is not supported");
        const FlatModel &m = model(e, {"npn", "pnp"});
        no_extra(e);
        auto p = params(e, m, {"is", "bf", "br", "nf", "cje", "vje", "mje", "cjc", "vjc", "mjc",
                               "fc", "tf", "tr"});
        auto get = [&](const char *k, double def) { return p.count(k) ? p[k] : def; };
        sim::BjtParams b;
        b.is = get("is", 1e-16);
        b.bf = get("bf", 100);
        b.br = get("br", 1);
        b.vt = get("nf", 1.0) * thermal_voltage;
        b.cje = get("cje", 0);
        b.vje = get("vje", 0.75);
        b.mje = get("mje", 0.33);
        b.cjc = get("cjc", 0);
        b.vjc = get("vjc", 0.75);
        b.mjc = get("mjc", 0.33);
        b.fc = get("fc", 0.5);
        b.tf = get("tf", 0);
        b.tr = get("tr", 0);
        if (m.type == "npn")
            sim_.circuit.npn(e.nodes[0], e.nodes[1], e.nodes[2], b);
        else
            sim_.circuit.pnp(e.nodes[0], e.nodes[1], e.nodes[2], b);
    }

    void mosfet(const FlatElement &e) {
        if (e.nodes.size() != 4)
            fail(e.loc, e.name + ": expected drain, gate, source and bulk nodes");
        if (e.nodes[3] != e.nodes[2])
            fail(e.loc, e.name + ": bulk must be connected to source (body effect is not modelled)");
        const FlatModel &m = model(e, {"nmos", "pmos"});
        auto p = params(e, m, {"level", "kp", "vto", "lambda"});
        if (p.count("level") && p["level"] != 1)
            fail(e.loc, e.name + ": only level 1 MOSFET models are supported");
        auto inst = keyed_args(e, {"w", "l"});
        const double w = inst.count("w") ? inst["w"] : 1.0;
        const double l = inst.count("l") ? inst["l"] : 1.0;
        if (!(w > 0) || !(l > 0))
            fail(e.loc, e.name + ": w and l must be positive");
        const double kp = p.count("kp") ? p["kp"] : 2e-5;
        const double vto = p.count("vto") ? p["vto"] : 0.0;
        const double lambda = p.count("lambda") ? p["lambda"] : 0.0;
        if (m.type == "nmos")
            sim_.circuit.nmos(e.nodes[0], e.nodes[1], e.nodes[2], kp * w / l, vto, lambda);
        else
            sim_.circuit.pmos(e.nodes[0], e.nodes[1], e.nodes[2], kp * w / l, vto, lambda);
    }

    void voltage_switch(const FlatElement &e) {
        const FlatModel &m = model(e, {"sw"});
        auto p = params(e, m, {"vt", "vh", "ron", "roff"});
        bool on = false;
        if (!e.args.empty()) {
            if (e.args[0] == "on")
                on = true;
            else if (e.args[0] != "off")
                fail(e.loc, e.name + ": unexpected argument '" + e.args[0] + "'");
            no_extra(e, 1);
        }
        auto get = [&](const char *k, double def) { return p.count(k) ? p[k] : def; };
        sim_.circuit.voltage_switch(e.nodes[0], e.nodes[1], e.nodes[2], e.nodes[3],
                                    get("ron", 1.0), get("roff", 1e12), get("vt", 0.0),
                                    std::abs(get("vh", 0.0)), on);
    }

    /** @brief E (VCVS) and G (VCCS): linear gain or value expression. */
    void controlled_source(const FlatElement &e) {
        const bool voltage = e.type == 'e';
        if (!e.expr_key.empty()) {
            expression_source(e, voltage);
            return;
        }
        if (e.nodes.size() != 4)
            fail(e.loc, e.name + ": expected four nodes and a gain");
        if (e.args.empty())
            fail(e.loc, e.name + ": gain missing");
        const double gain = number(e.loc, e.name, e.args[0]);
        no_extra(e, 1);
        auto f = [gain](const auto &v) { return gain * (v[0] - v[1]); };
        if (voltage)
            sim_.circuit.behavioural_voltage(e.nodes[0], e.nodes[1], {e.nodes[2], e.nodes[3]}, f);
        else
            sim_.circuit.behavioural_current(e.nodes[0], e.nodes[1], {e.nodes[2], e.nodes[3]}, f);
    }
};

} // namespace

Simulation build_simulation(const FlatNetlist &netlist, const ImportOptions &opt) {
    return Importer(netlist, opt).run();
}

} // namespace mspice::io
