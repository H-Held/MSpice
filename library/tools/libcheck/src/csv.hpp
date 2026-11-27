// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
//
// CSV data files (SPEC section 13.3).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "diag.hpp"

namespace libcheck {

struct CsvTable {
    bool ok = false;                 // file readable and well-formed
    std::vector<std::string> names;  // column names
    std::vector<std::string> units;  // column units
    std::vector<std::vector<double>> columns;
    std::vector<int> row_lines;      // source line of each data row
    std::vector<int> name_cols;      // source column of each header cell
    int header_line = 0;
    int unit_line = 0;

    int column_index(const std::string& name) const;
};

// Loads and checks a CSV file once; later calls return the cached result
// without reporting again. `display` is the path used in diagnostics.
// The caller must check existence before (E0090).
const CsvTable& load_csv(const std::string& path, const std::string& display, Diagnostics& diags);

// Checks that the given column is strictly increasing; reports E0092 once
// per file and column.
bool check_increasing(const CsvTable& t, int col, const std::string& display, Diagnostics& diags);

}  // namespace libcheck
