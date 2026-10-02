# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

# The compiler must be chosen before project(), which is when CMake probes it.
# A clang can only load a plugin built against its own LLVM, so this and
# LLVM_DIR must name the same version (checked after find_package(LLVM)).
set(CSAN_CLANG_VERSION 23 CACHE STRING
    "Major version of the clang that compiles the tests")
# Versioned names only: a bare `clang` on PATH may be any version.
find_program(CSAN_CC clang-${CSAN_CLANG_VERSION})
find_program(CSAN_CXX clang++-${CSAN_CLANG_VERSION})
if(NOT CSAN_CC OR NOT CSAN_CXX)
  message(FATAL_ERROR
    "clang-${CSAN_CLANG_VERSION} / clang++-${CSAN_CLANG_VERSION} not on PATH. "
    "Set -DCSAN_CLANG_VERSION to one that is, or put that toolchain's bin on PATH.")
endif()
set(CMAKE_C_COMPILER ${CSAN_CC})
set(CMAKE_CXX_COMPILER ${CSAN_CXX})

project(csan C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are not supported. Configure with: cmake -S . -B build")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# runtime/ is a PRIVATE include path; include/ holds the one public header.
file(GLOB CSAN_RUNTIME_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/runtime/*/*.cpp)
add_library(csan_runtime STATIC ${CSAN_RUNTIME_SOURCES})
target_include_directories(csan_runtime
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
target_link_libraries(csan_runtime PUBLIC Threads::Threads)
# PIC so the runtime can also be linked into a shared object (a dlopen()ed
# plugin); otherwise its TLS is emitted local-exec and that link fails.
set_target_properties(csan_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The runtime issues its own clwb + sfence after every atomic store, so the
# value reaches CXL memory rather than staying in the storing machine's cache.
target_compile_options(csan_runtime PRIVATE -Wall -Wextra -O2 -fno-omit-frame-pointer
                                            -mclwb -mclflushopt)

find_package(LLVM REQUIRED CONFIG)
if(LLVM_VERSION_MAJOR LESS 15)
  message(FATAL_ERROR
    "LLVM ${LLVM_PACKAGE_VERSION} is too old; 15 or newer is required")
endif()
if(NOT CSAN_CLANG_VERSION STREQUAL LLVM_VERSION_MAJOR)
  message(FATAL_ERROR
    "clang-${CSAN_CLANG_VERSION} cannot load a plugin built against LLVM "
    "${LLVM_VERSION_MAJOR}; set -DCSAN_CLANG_VERSION=${LLVM_VERSION_MAJOR}")
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM include dirs: ${LLVM_INCLUDE_DIRS}")

add_library(CSanPass MODULE pass/CSanPass.cpp)
target_include_directories(CSanPass PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(CSanPass PRIVATE ${LLVM_DEFINITIONS})
target_compile_options(CSanPass PRIVATE -fno-rtti -fPIC -Wall -Wextra)
target_link_libraries(CSanPass PRIVATE ${LLVM_LIBS})

enable_testing()

# The pass's own smoke test: every scenario below asks what the runtime
# concluded, so a pass that stopped instrumenting would leave every no-race
# scenario green. This one reads the instrumented IR and requires every hook.
find_program(CSAN_OPT NAMES opt opt-15 HINTS ${LLVM_TOOLS_BINARY_DIR})
if(CSAN_OPT)
  add_test(NAME pass_inserts_hooks COMMAND ${CMAKE_COMMAND}
           -DOPT=${CSAN_OPT}
           -DPLUGIN=$<TARGET_FILE:CSanPass>
           -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/pass_smoke.ll
           -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/run_pass_smoke.cmake)
else()
  message(WARNING "opt not found: skipping the pass_inserts_hooks test")
endif()

# Each test runs its scenario this many times, one fresh child process per
# repetition (tests/csan_test.h), and fails on the first wrong verdict.
# Use -DCSAN_TEST_REPEATS=1 while iterating.
set(CSAN_TEST_REPEATS 200 CACHE STRING
    "How many times each scenario runs inside one ctest test")
if(NOT CSAN_TEST_REPEATS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR
    "CSAN_TEST_REPEATS must be a positive integer, got '${CSAN_TEST_REPEATS}'")
endif()
message(STATUS "Repetitions per test: ${CSAN_TEST_REPEATS}")

# Optional extra targets, built only when present.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CMakeLists.txt)
  add_subdirectory(benchmarks)
endif()

# Every tests/<scenario>/*.cpp is one self-checking test executable.
# Scenario folders are named m<machines>_t<threads>_<pattern>.
file(GLOB CSAN_PROGRAM_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/tests/*/*.cpp)

foreach(src ${CSAN_PROGRAM_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}/include
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(${name} PRIVATE csan_runtime)
  target_compile_definitions(${name} PRIVATE CSAN_TEST_REPEATS=${CSAN_TEST_REPEATS})
  # -g and frame pointers let CSAN_TRACE capture and symbolize call stacks.
  target_compile_options(${name} PRIVATE -fpass-plugin=$<TARGET_FILE:CSanPass>
                                            -mclflushopt -mclwb
                                            -g -fno-omit-frame-pointer)
  add_dependencies(${name} CSanPass)
  # A change to the pass must recompile every test (the plugin path is spelled
  # out: OBJECT_DEPENDS takes no generator expression under Makefiles).
  set_source_files_properties(${src} PROPERTIES OBJECT_DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_SHARED_MODULE_PREFIX}CSanPass${CMAKE_SHARED_MODULE_SUFFIX})
  add_test(NAME ${name} COMMAND $<TARGET_FILE:${name}>)
  # These use more lock addresses than the default clock-table capacity.
  if(name MATCHES "cas_lock_many")
    set_tests_properties(${name} PROPERTIES
      ENVIRONMENT "CSAN_CLOCK_ENTRIES=16384")
  endif()
  # More than 64 threads (m<m>_t<n>_ with n > 64) needs a wider clock.
  if(name MATCHES "^m[0-9]+_t([0-9]+)_" AND CMAKE_MATCH_1 GREATER 64)
    set_tests_properties(${name} PROPERTIES
      ENVIRONMENT "CSAN_THREADS=128")
  endif()
endforeach()

# A traced finding prints its chain at the scenario lines that produced it.
# A named segment makes the scenario run once.
function(csan_trace_test name target regex)
  add_test(NAME ${name}
           COMMAND ${CMAKE_COMMAND} -E env CSAN_TRACE=1 CSAN_SHM_NAME=/csan_${name}
                   $<TARGET_FILE:${target}>)
  set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${regex}")
endfunction()

# Both atomic stores: the reporting one and the line's last write.
csan_trace_test(trace_names_atomic_writers m2_t2_atomic_line_conflict_no_chain
  "WRITE of size 1 at [^\n]*machine M1.*no_chain\\.cpp:21:.*LAST WRITE of that line by [^\n]*machine M0.*no_chain\\.cpp:34:.*PUBLISHED line: none.*ACQUIRED line: none")
# The publication's clwb and sfence, and the earlier flush that acquired nothing.
csan_trace_test(trace_flush_acquired_nothing m2_t2_atomic_line_conflict_flush_too_early
  "PUBLISHED line by thread T0[^\n]*machine M0\\):\n *write back at:\n[^\n]*too_early\\.cpp:49:.*fenced at:\n[^\n]*too_early\\.cpp:50:.*ACQUIRED line: none.*ACQUIRED NOTHING[^\n]*\n[^\n]*too_early\\.cpp:22:")
# One sfence finishing two publications, each with its own clwb.
csan_trace_test(trace_one_fence_two_slots m2_t2_trace_chain_one_fence_two_lines
  "write back at:\n[^\n]*two_lines\\.cpp:35:.*fenced at:\n[^\n]*two_lines\\.cpp:37:.*write back at:\n[^\n]*two_lines\\.cpp:36:.*fenced at:\n[^\n]*two_lines\\.cpp:37:")
# Five unfenced flushes, read out of a promoted map.
csan_trace_test(trace_promoted_map m2_t2_trace_chain_promoted_acq
  "FLUSHED, NOT FENCED line by thread T[0-9]+[^\n]*not counted: no fence has finished it:\n *flush at:\n[^\n]*promoted_acq\\.cpp:22:.*FLUSHED, NOT FENCED line by thread T[0-9]+[^\n]*not counted: no fence has finished it:\n *flush at:\n[^\n]*promoted_acq\\.cpp:22:.*FLUSHED, NOT FENCED line by thread T[0-9]+[^\n]*not counted: no fence has finished it:\n *flush at:\n[^\n]*promoted_acq\\.cpp:22:.*FLUSHED, NOT FENCED line by thread T[0-9]+[^\n]*not counted: no fence has finished it:\n *flush at:\n[^\n]*promoted_acq\\.cpp:22:.*FLUSHED, NOT FENCED line by thread T[0-9]+[^\n]*not counted: no fence has finished it:\n *flush at:\n[^\n]*promoted_acq\\.cpp:22:")
