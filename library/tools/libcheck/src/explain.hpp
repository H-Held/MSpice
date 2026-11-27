// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
//
// Long explanations of diagnostic codes. The texts live in
// library/docs/errors/<CODE>.md and are embedded at build time.
#pragma once

#include <cstddef>

namespace libcheck {

struct ExplainDoc {
    const char* code;
    const char* text;
};

extern const ExplainDoc kExplainDocs[];
extern const std::size_t kExplainDocCount;

}  // namespace libcheck
