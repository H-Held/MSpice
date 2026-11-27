# SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
# Copyright 2026 The MSpice Authors
#
# Runs mspice-libcheck with --format=json and checks that it reports exactly
# one diagnostic, as described by an .expected file:
#
#   code: E0004
#   message: unknown key `nmae` in [part]
#   file: tests/invalid/E0004_unknown_key.toml     (optional)
#   line: 7                                         (optional)
#
# Usage: cmake -DTOOL=<exe> -DARGS=<arg|arg|...> -DEXPECTED=<file> -P run_expect.cmake

string(REPLACE "|" ";" args "${ARGS}")
list(INSERT args 1 "--format=json")
execute_process(COMMAND "${TOOL}" ${args}
                OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)

file(READ "${EXPECTED}" exp)
foreach(key code message file line)
  set(want_${key} "")
  if(exp MATCHES "(^|\n)${key}: ([^\n]*)")
    string(STRIP "${CMAKE_MATCH_2}" want_${key})
  endif()
endforeach()
if(want_code STREQUAL "" OR want_message STREQUAL "")
  message(FATAL_ERROR "${EXPECTED}: needs `code:` and `message:` lines")
endif()

string(JSON count ERROR_VARIABLE jerr LENGTH "${out}" diagnostics)
if(jerr)
  message(FATAL_ERROR "output is not valid JSON (${jerr}):\n${out}\n${err}")
endif()
if(NOT count EQUAL 1)
  message(FATAL_ERROR "expected exactly 1 diagnostic, got ${count}:\n${out}")
endif()
string(JSON got_code GET "${out}" diagnostics 0 code)
string(JSON got_message GET "${out}" diagnostics 0 message)
string(JSON got_file GET "${out}" diagnostics 0 file)
string(JSON got_line GET "${out}" diagnostics 0 line)

set(fail "")
if(NOT got_code STREQUAL want_code)
  string(APPEND fail "code: expected ${want_code}, got ${got_code}\n")
endif()
if(NOT got_message STREQUAL want_message)
  string(APPEND fail "message:\n  expected: ${want_message}\n  got:      ${got_message}\n")
endif()
if(NOT want_file STREQUAL "" AND NOT got_file STREQUAL want_file)
  string(APPEND fail "file: expected ${want_file}, got ${got_file}\n")
endif()
if(NOT want_line STREQUAL "" AND NOT got_line STREQUAL want_line)
  string(APPEND fail "line: expected ${want_line}, got ${got_line}\n")
endif()
if(want_code MATCHES "^E")
  set(want_rc 1)
else()
  set(want_rc 0)
endif()
if(NOT rc EQUAL want_rc)
  string(APPEND fail "exit code: expected ${want_rc}, got ${rc}\n")
endif()
if(fail)
  message(FATAL_ERROR "${fail}\nfull output:\n${out}\n${err}")
endif()
message(STATUS "ok: ${got_code} ${got_message}")
