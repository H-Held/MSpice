// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file cli.cpp
 * @brief `mspice run`: read a SPICE netlist, run the transient, print CSV.
 */
#include "cli.hpp"

#include <cstdio>
#include <exception>
#include <fstream>
#include <mspice/core/version.hpp>
#include <mspice/io/spice_import.hpp>
#include <mspice/io/spice_netlist.hpp>

namespace mspice::cli {
namespace {

constexpr const char *usage_text =
    "Usage:\n"
    "  mspice run [options] <netlist>   simulate a SPICE netlist (.tran) and print CSV\n"
    "  mspice --version                 print the version\n"
    "  mspice --help                    print this help\n"
    "\n"
    "Options of run:\n"
    "  -o, --output <file>   write the CSV to <file> instead of standard output\n"
    "  --fixed-step          fixed steps of the .tran maximum step instead of adaptive steps\n"
    "\n"
    "Exit codes: 0 success, 1 netlist or simulation error, 2 invalid command line.\n";

/** @brief Shortest text that reads back to the same double. */
std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

/** @brief Write the result as CSV: time, node voltages (without ground), probed currents. */
void write_csv(const sim::Result &r, double start, std::ostream &out) {
    out << "time";
    for (std::size_t k = 1; k < r.nodes.size(); ++k)
        out << ",v(" << r.nodes[k] << ")";
    for (const std::string &c : r.currents)
        out << "," << c;
    out << "\n";
    for (const sim::Sample &s : r.samples) {
        if (s.time < start)
            continue;
        out << num(s.time);
        for (std::size_t k = 1; k < s.volts.size(); ++k)
            out << "," << num(s.volts[k]);
        for (double a : s.amps)
            out << "," << num(a);
        out << "\n";
    }
}

/** @brief Usage error: message plus a hint, exit code 2. */
int usage_error(std::ostream &err, const std::string &msg) {
    err << "mspice: error: " << msg << "\nRun 'mspice --help' for usage.\n";
    return exit_usage;
}

/** @brief The run command. */
int run_command(const std::vector<std::string> &args, std::ostream &out, std::ostream &err) {
    std::string netlist, output;
    io::ImportOptions opt;
    for (std::size_t k = 0; k < args.size(); ++k) {
        const std::string &a = args[k];
        if (a == "-o" || a == "--output") {
            if (k + 1 >= args.size())
                return usage_error(err, "option " + a + " needs a file name");
            output = args[++k];
        } else if (a == "--fixed-step") {
            opt.adaptive = false;
        } else if (!a.empty() && a[0] == '-') {
            return usage_error(err, "unknown option '" + a + "'");
        } else if (netlist.empty()) {
            netlist = a;
        } else {
            return usage_error(err, "more than one netlist given");
        }
    }
    if (netlist.empty())
        return usage_error(err, "run needs a netlist file");

    if (!std::ifstream(netlist, std::ios::binary)) {
        err << "mspice: error: cannot read '" << netlist << "'\n";
        return exit_input;
    }
    io::Simulation simulation;
    try {
        const io::FlatNetlist flat = io::load_spice_file(netlist);
        simulation = io::build_simulation(flat, opt);
    } catch (const io::ImportError &e) {
        for (const io::Diagnostic &d : e.diagnostics())
            err << d.format() << "\n";
        return exit_input;
    }
    for (const io::Diagnostic &w : simulation.warnings)
        err << w.format("warning") << "\n";

    sim::Result result;
    try {
        result = simulation.circuit.run(simulation.settings);
    } catch (const std::exception &e) {
        err << netlist << ": error: simulation failed: " << e.what() << "\n";
        return exit_input;
    }
    if (output.empty()) {
        write_csv(result, simulation.start, out);
        return exit_ok;
    }
    std::ofstream file(output, std::ios::binary);
    if (!file) {
        err << "mspice: error: cannot write '" << output << "'\n";
        return exit_input;
    }
    write_csv(result, simulation.start, file);
    if (!file) {
        err << "mspice: error: cannot write '" << output << "'\n";
        return exit_input;
    }
    return exit_ok;
}

} // namespace

int run(const std::vector<std::string> &args, std::ostream &out, std::ostream &err) {
    if (args.empty() || (args.size() == 1 && (args[0] == "--help" || args[0] == "-h"))) {
        out << "MSpice " << core::version() << " - transient circuit simulator\n\n"
            << usage_text;
        return exit_ok;
    }
    if (args.size() == 1 && args[0] == "--version") {
        out << "MSpice " << core::version() << "\n";
        return exit_ok;
    }
    if (args[0] == "run")
        return run_command({args.begin() + 1, args.end()}, out, err);
    return usage_error(err, "unknown command '" + args[0] + "'");
}

} // namespace mspice::cli
