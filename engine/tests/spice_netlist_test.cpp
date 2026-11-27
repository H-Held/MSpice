// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file spice_netlist_test.cpp
 * @brief SPICE reading, parameter scopes, subcircuit flattening with ngspice names, report.
 *
 * Modes (first argument):
 *  - `basic <dir>`: tests/spice/basic.cir with include placeholder, .lib section, nested
 *    subcircuits, params:, numparam scopes, K/J placeholders and report entries.
 *  - `errors`: small inline netlists for each report entry and node-count detection.
 */
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <mspice/io/spice_netlist.hpp>

using namespace mspice::io;

namespace {

int failures = 0;
int checks = 0;

/** @brief Count a check; print @p msg when @p ok is false. */
void expect(bool ok, const std::string &msg) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", msg.c_str());
    }
}

/** @brief Joined node list for messages. */
std::string join(const std::vector<std::string> &v) {
    std::string s;
    for (const auto &x : v)
        s += (s.empty() ? "" : " ") + x;
    return s;
}

/** @brief True when the report has an entry of @p kind whose feature contains @p feature and
 *  whose element equals @p element (empty = any). */
bool has_report(const FlatNetlist &n, const std::string &kind, const std::string &feature,
                const std::string &element = "") {
    for (const ReportItem &r : n.report)
        if (r.kind == kind && r.feature.find(feature) != std::string::npos &&
            (element.empty() || r.element == element))
            return true;
    return false;
}

/** @brief Print the report (for diagnosis). */
void dump_report(const FlatNetlist &n) {
    for (const ReportItem &r : n.report)
        std::printf("  [%s] %s:%d %s | %s | %s\n", r.kind.c_str(), r.file.c_str(), r.line,
                    r.element.c_str(), r.feature.c_str(), r.reason.c_str());
}

/** @brief Check element @p name: nodes and (optionally) first arg / model. */
void check_element(const FlatNetlist &n, const std::string &name,
                   const std::vector<std::string> &nodes, const std::string &arg0 = "",
                   const std::string &model = "") {
    const FlatElement *e = n.find(name);
    expect(e != nullptr, "element " + name + " missing");
    if (!e)
        return;
    expect(e->nodes == nodes, name + " nodes [" + join(e->nodes) + "] != [" + join(nodes) + "]");
    if (!arg0.empty())
        expect(!e->args.empty() && e->args[0] == arg0,
               name + " arg0 " + (e->args.empty() ? "<none>" : e->args[0]) + " != " + arg0);
    if (!model.empty())
        expect(e->model == model, name + " model " + e->model + " != " + model);
}

/** @brief Mode basic. */
void test_basic(const std::string &dir) {
    ReadOptions ro;
    ro.path_vars["inc"] = dir + "/inc";
    const FlatNetlist n = load_spice_file(dir + "/basic.cir", ro);
    expect(n.title.find("Basic test") != std::string::npos, "title: " + n.title);
    check_element(n, "v1", {"in", "0"}, "dc");
    check_element(n, "r1", {"in", "mid"}, "2000");
    // XA: gain=3 -> g2 = 6; ports in->mid, gnd->0, y->out.
    check_element(n, "e.xa.e1", {"out", "0", "mid", "0"}, "6");
    check_element(n, "e.xb.e1", {"o2", "0", "out", "0"}, "4");
    check_element(n, "r.xa.rin", {"mid", "0"}, "10k");
    check_element(n, "v.xa.vm", {"out", "xa.ym"}, "0");
    check_element(n, "r.xa.rloc", {"xa.loc", "0"}, "1");
    check_element(n, "r.xa.rg", {"0", "0"}, "1");
    check_element(n, "d.xa.xinner.dd", {"mid", "xa.loc"}, "", "dx");
    check_element(n, "r.xd.rp", {"mid", "0"}, "1");
    {
        const FlatElement *b = n.find("b.xa.bi");
        expect(b && b->expr && b->expr_key == "i", "b.xa.bi expression");
        if (b && b->expr)
            expect(to_string(b->expr) == "(((1 * v(mid,0)) / 1000) + (i(v.xa.vm) * 0))",
                   "b.xa.bi: " + to_string(b->expr));
        const FlatElement *c = n.find("b.xc.bi");
        expect(c && c->expr && to_string(c->expr).find("(2 * v(mid,0))") != std::string::npos,
               "b.xc.bi uses m=2");
        const FlatElement *o = n.find("bout");
        expect(o && o->expr && to_string(o->expr) == "(((v(out) * 1000) / 1000) + time)",
               "bout: " + (o && o->expr ? to_string(o->expr) : std::string("<none>")));
    }
    {
        const FlatElement *k = n.find("k1");
        expect(k && k->refs == std::vector<std::string>{"l1", "l2"} && k->args.size() == 1,
               "k1 refs");
    }
    expect(n.find("rinc") != nullptr, "include with @INC@ placeholder");
    expect(n.find("rlib") != nullptr, ".lib section typ included");
    expect(n.find("rworst") == nullptr, ".lib section worst excluded");
    expect(n.find("r_after_end") == nullptr, "line after .end ignored");
    expect(has_report(n, "unsupported", "K", "k1"), "K reported");
    expect(has_report(n, "unsupported", "J", "j1"), "J reported");
    expect(has_report(n, "unsupported", "parameter unknown", "xc"), "undeclared instance parameter");
    expect(has_report(n, "unsupported", "instance multiplier m", "xd"), "instance multiplier m");
    expect(n.missing().size() == 4,
           "exactly 4 unsupported entries, got " + std::to_string(n.missing().size()));
    expect(n.directives.size() == 2 && n.directives[0].name == ".tran" &&
               n.directives[1].name == ".options",
           "directives");
    expect(n.control.size() == 2 && n.control[1].text == "meas tran x avg v(out)", "control block");
    expect(n.global_params.at("rload") == 2000, "forward reference in global .param");
    expect(n.models.count("dx") == 1 && n.models.at("dx").params.size() == 2, "model dx");
    if (failures)
        dump_report(n);
}

/** @brief Flatten inline text (title line added). */
FlatNetlist flat(const std::string &body) {
    return flatten(read_spice_text("* test\n" + body, ".", "<text>"));
}

/** @brief Mode errors. */
void test_errors() {
    {
        const FlatNetlist n = flat(".include missing.inc\nR1 a 0 1\n");
        expect(has_report(n, "unsupported", ".include"), "missing include");
    }
    {
        const FlatNetlist n = flat("X1 a b NIX\n");
        expect(has_report(n, "unsupported", "subcircuit nix", "x1"), "undefined subckt");
    }
    {
        const FlatNetlist n = flat(".subckt S a b\nR1 a b 1\n.ends T\nX1 n1 S\n");
        expect(has_report(n, "note", ".ends t"), ".ends name mismatch");
        expect(has_report(n, "unsupported", "subcircuit s", "x1"), "port count mismatch");
    }
    {
        const FlatNetlist n = flat("B1 a 0 V=foo(v(b))\n");
        expect(has_report(n, "unsupported", "function foo", "b1"), "unknown function");
    }
    {
        const FlatNetlist n = flat("R1 a 0 1\nr1 b 0 2\n");
        expect(has_report(n, "unsupported", "name", "r1"), "duplicate name");
    }
    {
        const FlatNetlist n = flat("R1 gnd a 1\n.subckt S p\nR1 p gnd 1\n.ends\nX1 q S\n");
        const FlatElement *r = n.find("r1");
        expect(r && r->nodes[0] == "gnd", "top-level gnd kept");
        const FlatElement *s = n.find("r.x1.r1");
        expect(s && s->nodes == std::vector<std::string>{"q", "x1.gnd"}, "local gnd scoped");
    }
    {
        const FlatNetlist n = flat(".model NM VDMOS(nchan Vto=1 Kp=2)\n.model N1 NMOS(level=1)\n"
                                   ".model QN NPN(Is=1f)\nM1 d g s NM\nM2 d g s b N1 w=1u\n"
                                   "Q1 c b e QN\nQ2 c b e sub QN 2\n.model DZ ZENER(bv=5)\n"
                                   "D1 a b DZ\nS1 a b c d SW1 on\n.model SW1 SW(vt=1 vh=0.1)\n"
                                   "Hm a b Vx 2\nVx a c 0\n");
        expect(n.find("m1") && n.find("m1")->nodes.size() == 3, "VDMOS 3 nodes");
        expect(n.find("m2") && n.find("m2")->nodes.size() == 4 && n.find("m2")->args.size() == 3,
               "MOS 4 nodes + w=1u");
        expect(n.find("q1") && n.find("q1")->nodes.size() == 3, "BJT 3 nodes");
        expect(n.find("q2") && n.find("q2")->nodes.size() == 4 && n.find("q2")->args[0] == "2",
               "BJT 4 nodes + area");
        expect(has_report(n, "unsupported", "model type zener", "d1"), "unsupported model type");
        expect(n.models.at("nm").params[0].first == "nchan", "VDMOS flag nchan");
        expect(n.find("s1") && n.find("s1")->model == "sw1" && n.find("s1")->args[0] == "on",
               "switch");
        expect(n.find("hm") && n.find("hm")->refs[0] == "vx", "H refs");
        expect(n.missing().size() == 1, "only the zener model is missing");
    }
    {
        // Subckt-local model with parameter, nested instance path and device reference.
        const FlatNetlist n =
            flat(".subckt A x params: is0=1f\n.model DL D(Is={is0})\nD1 x 0 DL\nXb x B\n"
                 ".subckt B y\nF1 y 0 V1 3\nV1 y z 0\nRz z 0 1\n.ends\n.ends\n"
                 "X1 n A is0=2f\n");
        const FlatElement *d = n.find("d.x1.d1");
        expect(d && d->model == "x1:dl" && n.models.count("x1:dl") &&
                   std::abs(n.models.at("x1:dl").params[0].second - 2e-15) < 1e-27,
               "local model with instance parameter");
        const FlatElement *f = n.find("f.x1.xb.f1");
        expect(f && f->refs[0] == "v.x1.xb.v1" && f->nodes[0] == "n", "nested F reference");
        expect(n.find("r.x1.xb.rz") && n.find("r.x1.xb.rz")->nodes[0] == "x1.xb.z",
               "nested node names");
    }
    {
        // .param inside a subckt sees caller parameters (dynamic scope like numparam).
        const FlatNetlist n = flat(".param k=3\n.subckt S a params: p=2\n.param q={p*k}\n"
                                   "R1 a 0 {q}\n.ends\nX1 n S p=5\n");
        expect(n.find("r.x1.r1") && n.find("r.x1.r1")->args[0] == "15", "param scope q=p*k");
    }
}

} // namespace

/** @brief Run the selected mode; exit code 0 when all checks pass. */
int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "errors";
    const std::string arg = argc > 2 ? argv[2] : ".";
    if (mode == "basic")
        test_basic(arg);
    else if (mode == "errors")
        test_errors();
    else {
        std::printf("unknown mode %s\n", mode.c_str());
        return 2;
    }
    std::printf("%s: %d checks, %d failures\n", mode.c_str(), checks, failures);
    return failures == 0 ? 0 : 1;
}
