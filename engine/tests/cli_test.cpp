// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file cli_test.cpp
 * @brief In-process tests of the `mspice` command line front end.
 *
 * Positive tests compare simulated waveforms with analytic solutions; negative tests check
 * the exit code and the exact text on standard error. Argument: the directory with the test
 * netlists (tests/cli). Netlists written by this test go to the working directory.
 */
#include <cli.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

/** @brief Count a check; print @p what when @p ok is false. */
void expect(bool ok, const std::string &what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

/** @brief Output of one CLI call. */
struct Run {
    int code = -1;
    std::string out, err;
};

/** @brief Call the CLI in-process. */
Run cli(const std::vector<std::string> &args) {
    std::ostringstream out, err;
    Run r;
    r.code = mspice::cli::run(args, out, err);
    r.out = out.str();
    r.err = err.str();
    return r;
}

/** @brief Write @p text to @p path (working directory) and return the path. */
std::string write(const std::string &path, const std::string &text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
    return path;
}

/** @brief Parsed CSV: column name -> values. */
struct Table {
    std::vector<std::string> names;
    std::map<std::string, std::vector<double>> cols;
    /** @brief Linear interpolation of column @p name at time @p t. */
    double at(const std::string &name, double t) const {
        const std::vector<double> &time = cols.at("time");
        const std::vector<double> &y = cols.at(name);
        for (std::size_t i = 1; i < time.size(); ++i)
            if (time[i] >= t) {
                const double f = (t - time[i - 1]) / (time[i] - time[i - 1]);
                return y[i - 1] + f * (y[i] - y[i - 1]);
            }
        return y.back();
    }
};

/** @brief Parse CSV text with a header line. */
Table parse_csv(const std::string &text) {
    Table t;
    std::istringstream in(text);
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::string cell;
        std::size_t k = 0;
        while (std::getline(row, cell, ',')) {
            if (header) {
                t.names.push_back(cell);
                t.cols[cell];
            } else if (k < t.names.size()) {
                t.cols[t.names[k]].push_back(std::stod(cell));
            }
            ++k;
        }
        header = false;
    }
    return t;
}

/** @brief Check a failing run: exit code and exact standard error. */
void expect_error(const std::string &what, const Run &r, int code, const std::string &err) {
    expect(r.code == code, what + ": exit code " + std::to_string(r.code) + ", expected " +
                               std::to_string(code));
    expect(r.err == err, what + ": stderr\n  got:      [" + r.err + "]\n  expected: [" + err + "]");
    expect(r.out.empty(), what + ": no output expected on stdout");
}

void positive_rc(const std::string &dir) {
    // RC low-pass with tau = 1 ms driven by a 1 V step: v(out) = 1 - exp(-t / tau).
    for (const bool fixed : {false, true}) {
        std::vector<std::string> args = {"run", dir + "/rc_ok.cir"};
        if (fixed)
            args.push_back("--fixed-step");
        const Run r = cli(args);
        const std::string tag = fixed ? "rc fixed step" : "rc adaptive";
        expect(r.code == 0, tag + ": exit code " + std::to_string(r.code) + " " + r.err);
        expect(r.err.empty(), tag + ": unexpected stderr " + r.err);
        if (r.code != 0)
            continue;
        const Table t = parse_csv(r.out);
        expect(t.names == std::vector<std::string>{"time", "v(in)", "v(out)", "i(v1)"},
               tag + ": header");
        expect(std::abs(t.cols.at("time").back() - 5e-3) < 1e-12, tag + ": ends at tstop");
        double worst = 0;
        for (double time : {0.2e-3, 0.5e-3, 1e-3, 2e-3, 3e-3, 5e-3}) {
            const double v = t.at("v(out)", time);
            const double ref = 1 - std::exp(-time / 1e-3);
            worst = std::max(worst, std::abs(v - ref));
            // Source current: -(1 - v(out)) / R flows into the + node (ngspice sign).
            const double i = t.at("i(v1)", time);
            expect(std::abs(i + (1 - v) / 1e3) < 1e-6, tag + ": source current at " +
                                                           std::to_string(time));
        }
        std::printf("%s: %zu samples, worst |v(out) - analytic| = %.3g V\n", tag.c_str(),
                    t.cols.at("time").size(), worst);
        expect(worst < 1e-3, tag + ": v(out) within 1 mV of 1 - exp(-t/RC)");
    }
}

void positive_sources() {
    // Divider, VCVS gain, behavioural source and a sine source in one netlist.
    const std::string net = write("cli_sources.cir", "* sources\n"
                                                     "V1 a 0 DC 10\n"
                                                     "R1 a b 3k\n"
                                                     "R2 b 0 1k\n"
                                                     "E1 c 0 b 0 4\n"
                                                     "R3 c 0 1k\n"
                                                     "B1 d 0 V=v(b)*v(c) + 1\n"
                                                     "R4 d 0 1k\n"
                                                     "V2 s 0 SIN(0 2 1k)\n"
                                                     "R5 s 0 1k\n"
                                                     ".tran 10u 1m\n"
                                                     ".end\n");
    const Run r = cli({"run", net});
    expect(r.code == 0 && r.err.empty(), "sources: exit code 0, got " + std::to_string(r.code) +
                                             " " + r.err);
    if (r.code != 0)
        return;
    const Table t = parse_csv(r.out);
    expect(std::abs(t.at("v(b)", 0.5e-3) - 2.5) < 1e-9, "sources: divider 2.5 V");
    expect(std::abs(t.at("v(c)", 0.5e-3) - 10) < 1e-9, "sources: E gain 4");
    expect(std::abs(t.at("v(d)", 0.5e-3) - 26) < 1e-9, "sources: B expression 2.5*10+1");
    // Exact at the sample points; check the samples themselves.
    const std::vector<double> &time = t.cols.at("time");
    const std::vector<double> &s = t.cols.at("v(s)");
    double worst = 0;
    for (std::size_t i = 0; i < time.size(); ++i)
        worst = std::max(worst, std::abs(s[i] - 2 * std::sin(2 * 3.14159265358979323846 * 1e3 *
                                                             time[i])));
    expect(worst < 1e-9, "sources: sine source");
}

void negative_files(const std::string &dir) {
    // The netlist path is part of each message; call with the same relative form as the
    // binary tests by passing the full path and building the expected prefix from it.
    struct Case {
        const char *file;
        int line;
        const char *message;
    };
    const Case cases[] = {
        {"malformed_subckt.cir", 3, "s: .ends missing"},
        {"malformed_paren.cir", 2, "v1: missing ')' after pulse("},
        {"unknown_element.cir", 3, "z1: element type not supported"},
        {"missing_node.cir", 3, "r1: too few nodes (2 expected)"},
        {"invalid_number.cir", 3, "r1: invalid number '1x2k'"},
    };
    for (const Case &c : cases) {
        const std::string path = dir + "/" + c.file;
        expect_error(c.file, cli({"run", path}), 1,
                     path + ":" + std::to_string(c.line) + ": error: " + c.message + "\n");
    }
}

void negative_inline() {
    const auto net = [](const std::string &name, const std::string &body) {
        return write(name, "* test\n" + body);
    };
    expect_error("no .tran", cli({"run", net("cli_no_tran.cir", "R1 a 0 1k\nV1 a 0 1\n")}), 1,
                 "cli_no_tran.cir: error: no .tran directive; mspice run needs a transient "
                 "analysis (.tran tstep tstop)\n");
    expect_error(".tran number",
                 cli({"run", net("cli_tran_number.cir", "R1 a 0 1k\nV1 a 0 1\n.tran 1u fast\n")}), 1,
                 "cli_tran_number.cir:4: error: .tran: invalid number 'fast'\n");
    expect_error("unsupported directive",
                 cli({"run", net("cli_ac.cir", "R1 a 0 1k\nV1 a 0 1\n.ac dec 10 1 1k\n")}), 1,
                 "cli_ac.cir:4: error: .ac: directive not supported\n");
    expect_error("two errors",
                 cli({"run", net("cli_two.cir", "R1 a\nZ1 a 0 1\n.tran 1u 1m\n")}), 1,
                 "cli_two.cir:2: error: r1: too few nodes (2 expected)\n"
                 "cli_two.cir:3: error: z1: element type not supported\n");
    expect_error("missing model",
                 cli({"run", net("cli_model.cir", "V1 a 0 1\nD1 a 0 DX\n.tran 1u 1m\n")}), 1,
                 "cli_model.cir:3: error: d1: model not defined\n");
    expect_error("unsupported model parameter",
                 cli({"run", net("cli_param.cir",
                                 "V1 a 0 1\nR1 a b 1k\nD1 b 0 DX\n.model DX D(is=1e-14 rs=2)\n"
                                 ".tran 1u 1m\n")}),
                 1, "cli_param.cir:4: error: d1: model parameter 'rs' of model 'dx' is not supported\n");
    expect_error("negative resistor",
                 cli({"run", net("cli_neg.cir", "V1 a 0 1\nR1 a 0 -5\n.tran 1u 1m\n")}), 1,
                 "cli_neg.cir:3: error: r1: resistor must be positive finite\n");
    expect_error("missing value",
                 cli({"run", net("cli_value.cir", "V1 a 0 1\nC1 a 0\n.tran 1u 1m\n")}), 1,
                 "cli_value.cir:3: error: c1: value missing\n");
    expect_error("missing file", cli({"run", "cli_does_not_exist.cir"}), 1,
                 "mspice: error: cannot read 'cli_does_not_exist.cir'\n");
}

void usage() {
    const Run help = cli({"--help"});
    expect(help.code == 0 && help.out.find("Usage:") != std::string::npos, "--help");
    const Run version = cli({"--version"});
    expect(version.code == 0 && version.out.rfind("MSpice ", 0) == 0, "--version");
    expect_error("unknown command", cli({"simulate", "x.cir"}), 2,
                 "mspice: error: unknown command 'simulate'\nRun 'mspice --help' for usage.\n");
    expect_error("unknown option", cli({"run", "--fast", "x.cir"}), 2,
                 "mspice: error: unknown option '--fast'\nRun 'mspice --help' for usage.\n");
    expect_error("no netlist", cli({"run"}), 2,
                 "mspice: error: run needs a netlist file\nRun 'mspice --help' for usage.\n");
    expect_error("-o without file", cli({"run", "x.cir", "-o"}), 2,
                 "mspice: error: option -o needs a file name\nRun 'mspice --help' for usage.\n");
}

} // namespace

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/cli";
    positive_rc(dir);
    positive_sources();
    negative_files(dir);
    negative_inline();
    usage();
    std::printf("cli: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
