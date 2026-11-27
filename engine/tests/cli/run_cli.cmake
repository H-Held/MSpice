# SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
# Copyright 2026 The MSpice Authors
#
# Run the mspice binary on one netlist and check exit code and standard error exactly.
#   cmake -DCLI=<mspice> -DNETLIST=<file> -DEXPECT_CODE=<n> [-DEXPECT_STDERR=<text>] -P run_cli.cmake
# With EXPECT_CODE 0 the standard error must be empty and the CSV must start with "time,".
execute_process(COMMAND "${CLI}" run "${NETLIST}"
                RESULT_VARIABLE code
                OUTPUT_VARIABLE out
                ERROR_VARIABLE err)
string(REPLACE "\r\n" "\n" err "${err}")
string(STRIP "${err}" err)
if(NOT code STREQUAL EXPECT_CODE)
  message(FATAL_ERROR "exit code ${code}, expected ${EXPECT_CODE}\nstderr: ${err}")
endif()
if(NOT err STREQUAL "${EXPECT_STDERR}")
  message(FATAL_ERROR "stderr differs\n  got:      [${err}]\n  expected: [${EXPECT_STDERR}]")
endif()
if(EXPECT_CODE STREQUAL "0" AND NOT out MATCHES "^time,")
  message(FATAL_ERROR "CSV output does not start with 'time,'")
endif()
message(STATUS "ok: exit code ${code}, stderr [${err}]")
