// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
#pragma once

#include <string_view>

namespace mspice::core {
/** @brief Version string of the engine, e.g. "0.1.0-dev". */
[[nodiscard]] std::string_view version() noexcept;
} // namespace mspice::core
