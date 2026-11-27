// SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
// Copyright 2026 The MSpice Authors
/** @file describe_test.cpp
 *  @brief Netlist export: Circuit::describe_json with instance scopes and escaping.
 */
#include <iostream>
#include <mspice/sim/circuit.hpp>
#include <string>

namespace {
/** @brief Report a missing substring; returns true when @p json contains @p needle. */
bool has(const std::string &json, const std::string &needle) {
    if (json.find(needle) != std::string::npos)
        return true;
    std::cerr << "missing: " << needle << '\n';
    return false;
}
} // namespace

/** @brief Build a small circuit with a nested instance scope and check the JSON. */
int main() {
    using namespace mspice::sim;
    Circuit c;
    Waveform dc;
    dc.high = 5;
    c.voltage("vin", "GND", dc);
    {
        const Circuit::InstanceScope scope = c.instance("U1", "regulator", {{"in", "vin"}});
        c.resistor("vin", "U1.p", 2.5);
        {
            const Circuit::InstanceScope inner = c.instance("U1.Q", "inner");
            c.capacitor("U1.p", "0", 1e-9);
        }
        c.behavioural_current("U1.p", "0", {"vin", Circuit::time_control},
                              [](const std::vector<double> &v) { return v[0] * 1e-3; });
    }
    c.resistor("U1.p", "out \"q\"", 1e3);
    {
        const Circuit::InstanceScope scope =
            c.instance("U5", "shunt_ref", {{"a", "0"}, {"k", "ref"}, {"ref", "ref"}});
        c.resistor("ref", "0", 1e3);
    }
    const std::string j = c.describe_json();
    const bool ok =
        has(j, "{\"nets\":[\"0\",\"vin\",\"U1.p\",") &&
        has(j, "{\"id\":\"V0\",\"type\":\"V\",\"nodes\":{\"+\":\"vin\",\"-\":\"0\"},\"value\":5,"
               "\"wave\":\"dc\",\"instance\":null,\"model\":null}") &&
        has(j, "{\"id\":\"R1\",\"type\":\"R\",\"nodes\":{\"1\":\"vin\",\"2\":\"U1.p\"},"
               "\"value\":2.5,\"instance\":\"U1\",\"model\":\"regulator\"}") &&
        has(j, "\"id\":\"C2\",\"type\":\"C\",\"nodes\":{\"1\":\"U1.p\",\"2\":\"0\"},"
               "\"value\":1.0000000000000001e-09,\"instance\":\"U1.Q\",\"model\":\"inner\"}") &&
        has(j, "\"id\":\"BI3\",\"type\":\"BI\"") &&
        has(j, "\"controls\":[\"vin\",\"#T\"],\"instance\":\"U1\",\"model\":\"regulator\"}") &&
        has(j, "\"id\":\"R4\",\"type\":\"R\",\"nodes\":{\"1\":\"U1.p\",\"2\":\"out \\\"q\\\"\"},"
               "\"value\":1000,\"instance\":null,\"model\":null}") &&
        has(j, "\"instance\":\"U5\",\"model\":\"shunt_ref\"}") &&
        has(j, "{\"id\":\"U1\",\"type\":\"X\",\"nodes\":{\"in\":\"vin\"},\"value\":null,"
               "\"instance\":\"U1\",\"model\":\"regulator\"}") &&
        has(j, "{\"id\":\"U5\",\"type\":\"X\",\"nodes\":{\"a\":\"0\",\"k\":\"ref\","
               "\"ref\":\"ref\"}") &&
        j.find("U1.Q\",\"type\":\"X\"") == std::string::npos && j.substr(j.size() - 2) == "]}";
    if (!ok) {
        std::cerr << j << '\n';
        return 1;
    }
    return 0;
}
