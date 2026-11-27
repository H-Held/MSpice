# SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
# Copyright 2026 The MSpice Authors
#
# mspice-libcheck: standalone validator for the MSpice part library.
# Build from the repository root:
#   cmake -S library/tools/libcheck -B build/libcheck -G Ninja
#   cmake --build build/libcheck
#   ctest --test-dir build/libcheck --output-on-failure

cmake_minimum_required(VERSION 3.21)
project(mspice_libcheck VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

get_filename_component(MSPICE_LIBRARY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(MSPICE_ERROR_DOCS_DIR "${MSPICE_LIBRARY_DIR}/docs/errors")

# Long explanations (docs/errors/*.md) are embedded into the binary.
file(GLOB MSPICE_ERROR_DOCS CONFIGURE_DEPENDS "${MSPICE_ERROR_DOCS_DIR}/*.md")
set(EXPLAIN_DATA "${CMAKE_CURRENT_BINARY_DIR}/explain_data.cpp")
add_custom_command(
  OUTPUT "${EXPLAIN_DATA}"
  COMMAND "${CMAKE_COMMAND}" "-DDOCS_DIR=${MSPICE_ERROR_DOCS_DIR}" "-DOUT=${EXPLAIN_DATA}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_docs.cmake"
  DEPENDS ${MSPICE_ERROR_DOCS} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_docs.cmake"
  COMMENT "Embedding error explanations"
  VERBATIM)

add_executable(mspice-libcheck
  src/main.cpp
  src/check.hpp
  src/csv.cpp
  src/diag.cpp
  src/expr.cpp
  src/library.cpp
  src/part.cpp
  src/tomlctx.cpp
  src/toml_impl.cpp
  src/units.cpp
  src/util.cpp
  "${EXPLAIN_DATA}")
target_include_directories(mspice-libcheck PRIVATE src third_party/tomlplusplus)
target_compile_definitions(mspice-libcheck PRIVATE TOML_HEADER_ONLY=0 TOML_ENABLE_FORMATTERS=0)
if(MSVC)
  target_compile_options(mspice-libcheck PRIVATE /W4 /utf-8 /permissive-)
else()
  target_compile_options(mspice-libcheck PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(MINGW)
  # Static runtime: the tool runs without the compiler's DLLs on PATH.
  target_link_options(mspice-libcheck PRIVATE -static)
endif()

install(TARGETS mspice-libcheck RUNTIME DESTINATION bin)

# ------------------------------------------------------------------ tests
enable_testing()
set(TOOL "$<TARGET_FILE:mspice-libcheck>")
set(TESTS_DIR "${MSPICE_LIBRARY_DIR}/tests")
set(RUN_EXPECT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_expect.cmake")

add_test(NAME selftest COMMAND mspice-libcheck selftest)
add_test(NAME codes_listed COMMAND mspice-libcheck codes --format=json)
add_test(NAME explain_known_code COMMAND mspice-libcheck explain E0004)
set_tests_properties(explain_known_code PROPERTIES PASS_REGULAR_EXPRESSION "## Wrong")
add_test(NAME explain_unknown_code COMMAND mspice-libcheck explain E9999)
set_tests_properties(explain_unknown_code PROPERTIES WILL_FAIL TRUE)
add_test(NAME usage_unknown_command COMMAND mspice-libcheck chekc)
set_tests_properties(usage_unknown_command PROPERTIES WILL_FAIL TRUE)

# The generic parts of this repository must validate without warnings.
add_test(NAME library_parts COMMAND mspice-libcheck check --deny-warnings .
         WORKING_DIRECTORY "${MSPICE_LIBRARY_DIR}")

# Positive tests: every file or directory in tests/valid must pass.
file(GLOB VALID_ENTRIES CONFIGURE_DEPENDS RELATIVE "${TESTS_DIR}/valid" "${TESTS_DIR}/valid/*")
foreach(entry IN LISTS VALID_ENTRIES)
  if(entry MATCHES "\\.toml$" OR (IS_DIRECTORY "${TESTS_DIR}/valid/${entry}" AND NOT entry STREQUAL "data"))
    string(REGEX REPLACE "\\.toml$" "" name "${entry}")
    add_test(NAME "valid/${name}" COMMAND mspice-libcheck check --deny-warnings "tests/valid/${entry}"
             WORKING_DIRECTORY "${MSPICE_LIBRARY_DIR}")
  endif()
endforeach()

# Negative tests: tests/invalid/<name>.expected describes the one diagnostic
# that tests/invalid/<name>.toml (or the directory <name>/) must produce.
file(GLOB INVALID_EXPECTED CONFIGURE_DEPENDS RELATIVE "${TESTS_DIR}/invalid" "${TESTS_DIR}/invalid/*.expected")
foreach(exp IN LISTS INVALID_EXPECTED)
  string(REGEX REPLACE "\\.expected$" "" name "${exp}")
  if(EXISTS "${TESTS_DIR}/invalid/${name}.toml")
    set(target "tests/invalid/${name}.toml")
  elseif(IS_DIRECTORY "${TESTS_DIR}/invalid/${name}")
    set(target "tests/invalid/${name}")
  else()
    message(FATAL_ERROR "tests/invalid/${exp} has no matching ${name}.toml or directory ${name}/")
  endif()
  add_test(NAME "invalid/${name}"
           COMMAND "${CMAKE_COMMAND}" "-DTOOL=${TOOL}" "-DARGS=check|${target}"
                   "-DEXPECTED=${TESTS_DIR}/invalid/${exp}" -P "${RUN_EXPECT}"
           WORKING_DIRECTORY "${MSPICE_LIBRARY_DIR}")
endforeach()

# Every diagnostic code must have at least one negative test.
add_test(NAME every_code_has_negative_test
         COMMAND "${CMAKE_COMMAND}" "-DTOOL=${TOOL}" "-DINVALID_DIR=${TESTS_DIR}/invalid"
                 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_coverage.cmake")

# Merging: base + vendor with an override must merge, and the result must validate.
set(MERGE_OUT "${CMAKE_CURRENT_BINARY_DIR}/merge_out")
add_test(NAME merge/override_writes_library
         COMMAND mspice-libcheck merge --force -o "${MERGE_OUT}" tests/merge/base tests/merge/vendor
         WORKING_DIRECTORY "${MSPICE_LIBRARY_DIR}")
set_tests_properties(merge/override_writes_library PROPERTIES FIXTURES_SETUP merged_library)
add_test(NAME merge/result_validates COMMAND mspice-libcheck check --deny-warnings "${MERGE_OUT}")
set_tests_properties(merge/result_validates PROPERTIES FIXTURES_REQUIRED merged_library)
add_test(NAME merge/override_applied COMMAND "${CMAKE_COMMAND}" -E cat "${MERGE_OUT}/MERGE_REPORT.json")
set_tests_properties(merge/override_applied PROPERTIES FIXTURES_REQUIRED merged_library
                     PASS_REGULAR_EXPRESSION "\"kept\": \"merge-test-vendor\", \"dropped\": \"merge-test-base\"")
# Without an override, the same id with different content is a conflict.
add_test(NAME merge/conflict_is_error
         COMMAND "${CMAKE_COMMAND}" "-DTOOL=${TOOL}"
                 "-DARGS=merge|--force|-o|${CMAKE_CURRENT_BINARY_DIR}/merge_conflict_out|tests/merge/base|tests/merge/conflict"
                 "-DEXPECTED=${TESTS_DIR}/merge/conflict.expected" -P "${RUN_EXPECT}"
         WORKING_DIRECTORY "${MSPICE_LIBRARY_DIR}")
add_test(NAME merge/conflict_policy_last
         COMMAND mspice-libcheck merge --force --on-conflict=last -o "${CMAKE_CURRENT_BINARY_DIR}/merge_last_out"
                 tests/merge/base tests/merge/conflict
         WORKING_DIRECTORY "${MSPICE_LIBRARY_DIR}")
