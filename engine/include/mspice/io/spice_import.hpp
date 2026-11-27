// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file spice_import.hpp
 * @brief Map a flattened SPICE netlist onto a sim::Circuit and transient settings.
 *
 * Supported elements: R, C, L, V, I (DC, PULSE, SIN, PWL), D, Q (NPN/PNP), M (level-1
 * NMOS/PMOS), S (voltage-controlled switch), B (V= or I= expression), E and G (linear gain or
 * value expression). Supported directives: .tran, .options (reltol, abstol, method), .ic.
 * Everything else is rejected with an ImportError that names file, line and element:
 * unsupported input is never dropped silently. Nets without any DC path to ground (only
 * current sources, MOSFET gates or controlled-source inputs attached) are reported by
 * name before the run, instead of a singular matrix.
 */
#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <mspice/io/spice_netlist.hpp>
#include <mspice/sim/circuit.hpp>

namespace mspice::io {

/** @brief One problem found in a netlist. */
struct Diagnostic {
    std::string file;    ///< File of the offending line, empty when not tied to a line.
    int line = 0;        ///< 1-based line, 0 when unknown.
    std::string message; ///< One-line message, e.g. "r1: invalid number '1x'".
    /** @brief "file:line: error: message", or "error: message" without a location. */
    std::string format(const char *severity = "error") const;
};

/** @brief Netlist that cannot be simulated; holds one diagnostic per problem. */
class ImportError : public std::runtime_error {
  public:
    /** @brief Create an error from at least one diagnostic. */
    explicit ImportError(std::vector<Diagnostic> diagnostics);
    /** @brief All problems in netlist order. */
    const std::vector<Diagnostic> &diagnostics() const { return diagnostics_; }

  private:
    std::vector<Diagnostic> diagnostics_;
};

/** @brief Options of build_simulation(). */
struct ImportOptions {
    /** Adaptive time steps (default). False: fixed steps of the .tran maximum step. */
    bool adaptive = true;
};

/** @brief Circuit plus settings ready for sim::Circuit::run(). */
struct Simulation {
    sim::Circuit circuit;
    sim::Settings settings;
    double start = 0;                 ///< .tran tstart: earlier samples are not reported.
    std::vector<Diagnostic> warnings; ///< Non-fatal notes (e.g. an ignored AC specification).
};

/** @brief Map a flattened netlist onto a circuit.
 *  @param netlist result of flatten()/load_spice_file()
 *  @param opt import options
 *  @return circuit and transient settings
 *  @throws ImportError listing every report entry of kind `unsupported`, or the first problem
 *          found while mapping elements and directives
 */
Simulation build_simulation(const FlatNetlist &netlist, const ImportOptions &opt = {});

} // namespace mspice::io
