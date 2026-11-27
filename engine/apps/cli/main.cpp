// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/** @file main.cpp
 *  @brief Entry point of the `mspice` command line tool. */
#include "cli.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char *argv[]) {
    std::ios::sync_with_stdio(false);
    const std::vector<std::string> args(argv + 1, argv + argc);
    return mspice::cli::run(args, std::cout, std::cerr);
}
