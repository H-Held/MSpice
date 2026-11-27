// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#include <mspice/core/version.hpp>

#include <string_view>

/** @brief Check library linkage and exported version. */
int main() { return mspice::core::version() == std::string_view{"0.1.0-dev"} ? 0 : 1; }
