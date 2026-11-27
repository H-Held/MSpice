# SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
# Copyright 2026 The MSpice Authors
#
# Fails if a diagnostic code has no negative test in tests/invalid.
# Usage: cmake -DTOOL=<exe> -DINVALID_DIR=<dir> -P check_coverage.cmake

execute_process(COMMAND "${TOOL}" codes OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "`codes` failed")
endif()
string(REGEX MATCHALL "(^|\n)[EW][0-9][0-9][0-9][0-9]" codes "${out}")

file(GLOB expected_files "${INVALID_DIR}/*.expected")
set(tested "")
foreach(f IN LISTS expected_files)
  file(READ "${f}" text)
  if(text MATCHES "(^|\n)code: ([EW][0-9][0-9][0-9][0-9])")
    list(APPEND tested "${CMAKE_MATCH_2}")
  endif()
endforeach()

set(missing "")
foreach(c IN LISTS codes)
  string(STRIP "${c}" c)
  list(FIND tested "${c}" idx)
  if(idx EQUAL -1)
    list(APPEND missing "${c}")
  endif()
endforeach()
if(missing)
  message(FATAL_ERROR "codes without a negative test in tests/invalid: ${missing}")
endif()
list(LENGTH codes n)
message(STATUS "all ${n} codes have a negative test")
