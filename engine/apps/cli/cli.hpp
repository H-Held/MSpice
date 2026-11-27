// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/**
 * @file cli.hpp
 * @brief Command line front end of `mspice`, callable in-process for tests.
 */
#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace mspice::cli {

/** @brief Exit codes of the command line tool. */
enum ExitCode : int {
    exit_ok = 0,       ///< Success.
    exit_input = 1,    ///< Netlist error or failed simulation.
    exit_usage = 2,    ///< Invalid command line.
};

/** @brief Run the command line tool.
 *  @param args arguments without the program name
 *  @param out standard output (CSV results, help, version)
 *  @param err standard error (diagnostics)
 *  @return exit code (see ExitCode)
 */
int run(const std::vector<std::string> &args, std::ostream &out, std::ostream &err);

} // namespace mspice::cli
