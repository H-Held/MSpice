# SPDX-License-Identifier: LicenseRef-Apache-2.0-with-Commons-Clause
# Copyright 2026 The MSpice Authors
cmake_minimum_required(VERSION 3.21)
project(MSpice VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MSPICE_BUILD_TESTS "Build the test suite" ON)
option(MSPICE_BUILD_BENCHMARKS "Build the batched-LU benchmark (mspice_batch_bench)" ON)
# The batched LU kernel (batched_plan.cpp) is only used by the benchmark and its test. With
# this option its SoA loops are compiled for the build machine (e.g. AVX-512); the resulting
# binaries do not run on older CPUs. Off by default for portable binaries.
option(MSPICE_NATIVE_ARCH "Compile batched_plan.cpp with -O3 -march=native (not portable)" OFF)
# SuiteSparse KLU is LGPL-2.1: optional, off by default, linked dynamically only. The
# built-in dense and fixed-structure sparse LU are the default and fully tested.
option(MSPICE_USE_KLU "Use SuiteSparse KLU (LGPL, shared libraries only) for sparse MNA" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_library(mspice_core STATIC
  src/core/version.cpp
  src/sim/circuit.cpp
  src/sim/linear_solver.cpp
  src/sim/linear_exact.cpp
  src/sim/sparse_plan.cpp
  src/sim/batched_plan.cpp
  src/firmware/pin_bridge.cpp
  src/io/spice_lexer.cpp
  src/io/spice_netlist.cpp
  src/io/spice_expr.cpp
  src/io/spice_import.cpp)
target_include_directories(mspice_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(mspice_core PUBLIC cxx_std_20)

if(MSPICE_NATIVE_ARCH AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/sim/batched_plan.cpp PROPERTIES COMPILE_OPTIONS "-O3;-march=native")
endif()

if(MSPICE_USE_KLU)
  find_path(KLU_INCLUDE_DIR klu.h PATH_SUFFIXES suitesparse)
  find_library(KLU_LIBRARY NAMES klu)
  find_library(AMD_LIBRARY NAMES amd)
  find_library(COLAMD_LIBRARY NAMES colamd)
  find_library(BTF_LIBRARY NAMES btf)
  find_library(SUITESPARSECONFIG_LIBRARY NAMES suitesparseconfig)
  set(_klu_libs ${KLU_LIBRARY} ${AMD_LIBRARY} ${COLAMD_LIBRARY} ${BTF_LIBRARY}
                ${SUITESPARSECONFIG_LIBRARY})
  if(NOT (KLU_INCLUDE_DIR AND KLU_LIBRARY AND AMD_LIBRARY AND COLAMD_LIBRARY AND BTF_LIBRARY
          AND SUITESPARSECONFIG_LIBRARY))
    message(FATAL_ERROR "MSPICE_USE_KLU=ON, but SuiteSparse KLU was not found "
                        "(klu.h and the libraries klu, amd, colamd, btf, suitesparseconfig).")
  endif()
  # LGPL: never link a static archive into MSpice. MinGW import libraries (.dll.a) are fine.
  foreach(_lib IN LISTS _klu_libs)
    if(_lib MATCHES "\\.a$" AND NOT _lib MATCHES "\\.dll\\.a$")
      message(FATAL_ERROR "MSPICE_USE_KLU needs shared SuiteSparse libraries, found ${_lib}")
    endif()
  endforeach()
  target_compile_definitions(mspice_core PRIVATE MSPICE_WITH_KLU=1)
  target_include_directories(mspice_core PRIVATE ${KLU_INCLUDE_DIR})
  target_link_libraries(mspice_core PUBLIC ${_klu_libs})
  message(STATUS "SuiteSparse KLU: ON (${KLU_LIBRARY})")
else()
  message(STATUS "SuiteSparse KLU: OFF (built-in LU)")
endif()

# A foreign libstdc++-6.dll earlier in PATH (for example the one shipped with Git for
# Windows) crashes MinGW binaries at start-up; link the C++ runtime statically there.
function(mspice_executable target)
  if(MINGW)
    target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
  endif()
endfunction()

# Command line tool `mspice`. The front end is a small library so tests can call it in-process.
add_library(mspice_cli_lib STATIC apps/cli/cli.cpp)
target_include_directories(mspice_cli_lib PUBLIC ${PROJECT_SOURCE_DIR}/apps/cli)
target_link_libraries(mspice_cli_lib PUBLIC mspice_core)
add_executable(mspice_cli apps/cli/main.cpp)
target_link_libraries(mspice_cli PRIVATE mspice_cli_lib)
set_target_properties(mspice_cli PROPERTIES OUTPUT_NAME mspice)
mspice_executable(mspice_cli)

if(MSPICE_BUILD_BENCHMARKS)
  # Feasibility benchmark (not a test): batched LU, optional OpenMP over variant chunks.
  add_executable(mspice_batch_bench apps/batch_bench/main.cpp)
  target_include_directories(mspice_batch_bench PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  target_link_libraries(mspice_batch_bench PRIVATE mspice_core)
  find_package(OpenMP COMPONENTS CXX QUIET)
  if(OpenMP_CXX_FOUND)
    target_link_libraries(mspice_batch_bench PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(mspice_batch_bench PRIVATE MSPICE_HAVE_OPENMP=1)
  endif()
  mspice_executable(mspice_batch_bench)
endif()

if(MSPICE_BUILD_TESTS)
  enable_testing()

  # mspice_test(<name> <source>): one test executable linked against the core library.
  function(mspice_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE mspice_core ${ARGN})
    mspice_executable(${target})
  endfunction()

  mspice_test(mspice_version_test tests/core_version_test.cpp)
  add_test(NAME core_version COMMAND mspice_version_test)

  mspice_test(mspice_sim_test tests/sim_test.cpp)
  add_test(NAME transient_rc_rlc_diode COMMAND mspice_sim_test)
  mspice_test(mspice_solver_fix_test tests/solver_fix_test.cpp)
  add_test(NAME solver_regressions COMMAND mspice_solver_fix_test)
  mspice_test(mspice_trace_test tests/trace_test.cpp)
  add_test(NAME analog_pin_trace_breakpoints COMMAND mspice_trace_test)
  mspice_test(mspice_linear_solver_test tests/linear_solver_test.cpp)
  add_test(NAME pivoted_lu_source_constraints COMMAND mspice_linear_solver_test)
  mspice_test(mspice_linear_exact_test tests/linear_exact_test.cpp)
  add_test(NAME linear_exact COMMAND mspice_linear_exact_test)
  mspice_test(mspice_sparse_plan_test tests/sparse_plan_test.cpp)
  add_test(NAME sparse_plan COMMAND mspice_sparse_plan_test)
  mspice_test(mspice_batched_plan_test tests/batched_plan_test.cpp)
  add_test(NAME batched_plan COMMAND mspice_batched_plan_test)
  mspice_test(mspice_pin_bridge_test tests/pin_bridge_test.cpp)
  add_test(NAME pin_bridge_events COMMAND mspice_pin_bridge_test)

  mspice_test(mspice_correctness_test tests/correctness_test.cpp)
  foreach(mode bdf2 gnd nmos led)
    add_test(NAME correctness_${mode} COMMAND mspice_correctness_test ${mode})
  endforeach()

  mspice_test(mspice_models_test tests/models_test.cpp)
  foreach(mode pmos switch switch_threshold gate_driver led_curve bjt operating_point
               switch_timing time_breakpoint)
    add_test(NAME models_${mode} COMMAND mspice_models_test ${mode})
  endforeach()

  mspice_test(mspice_dual_test tests/dual_test.cpp)
  add_test(NAME dual_numbers COMMAND mspice_dual_test)
  mspice_test(mspice_behaviour_grad_test tests/behaviour_grad_test.cpp)
  add_test(NAME behaviour_gradients COMMAND mspice_behaviour_grad_test)
  mspice_test(mspice_describe_test tests/describe_test.cpp)
  add_test(NAME netlist_describe_json COMMAND mspice_describe_test)

  # SPICE import: expressions, netlist reading and flattening, mapping to a circuit.
  mspice_test(mspice_spice_expr_test tests/spice_expr_test.cpp)
  foreach(mode parse semantics breakpoints cse circuit bench)
    add_test(NAME spice_expr_${mode} COMMAND mspice_spice_expr_test ${mode})
  endforeach()
  add_test(NAME spice_expr_expressions COMMAND mspice_spice_expr_test expressions
           "${PROJECT_SOURCE_DIR}/tests/spice/expressions.txt")
  mspice_test(mspice_spice_netlist_test tests/spice_netlist_test.cpp)
  add_test(NAME spice_netlist_basic COMMAND mspice_spice_netlist_test basic
           "${PROJECT_SOURCE_DIR}/tests/spice")
  add_test(NAME spice_netlist_errors COMMAND mspice_spice_netlist_test errors)

  # Command line: in-process tests (exact messages, analytic results) ...
  mspice_test(mspice_cli_test tests/cli_test.cpp mspice_cli_lib)
  add_test(NAME cli_in_process COMMAND mspice_cli_test "${PROJECT_SOURCE_DIR}/tests/cli")
  # ... and the real binary: exit code, exact standard error, CSV header.
  set(_cli_cases
      "rc_ok|0|"
      "malformed_subckt|1|malformed_subckt.cir:3: error: s: .ends missing"
      "malformed_paren|1|malformed_paren.cir:2: error: v1: missing ')' after pulse("
      "unknown_element|1|unknown_element.cir:3: error: z1: element type not supported"
      "missing_node|1|missing_node.cir:3: error: r1: too few nodes (2 expected)"
      "invalid_number|1|invalid_number.cir:3: error: r1: invalid number '1x2k'")
  foreach(_case IN LISTS _cli_cases)
    string(REPLACE "|" ";" _parts "${_case}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _code)
    list(GET _parts 2 _stderr)
    add_test(NAME cli_binary_${_name}
             COMMAND ${CMAKE_COMMAND}
                     -DCLI=$<TARGET_FILE:mspice_cli>
                     -DNETLIST=${_name}.cir
                     -DEXPECT_CODE=${_code}
                     "-DEXPECT_STDERR=${_stderr}"
                     -P "${PROJECT_SOURCE_DIR}/tests/cli/run_cli.cmake"
             WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}/tests/cli")
  endforeach()
endif()
