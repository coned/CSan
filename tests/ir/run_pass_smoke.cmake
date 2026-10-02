# SPDX-License-Identifier: MIT
# pass_inserts_hooks test: runs the plugin over pass_smoke.ll with opt and
# requires a call to every hook family. Expects -DOPT=, -DPLUGIN=, -DINPUT=.

foreach(var OPT PLUGIN INPUT)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "pass_smoke: -D${var}= is required")
  endif()
endforeach()
if(NOT EXISTS "${INPUT}")
  message(FATAL_ERROR "pass_smoke: no input module at ${INPUT}")
endif()
if(NOT EXISTS "${PLUGIN}")
  message(FATAL_ERROR "pass_smoke: no pass plugin at ${PLUGIN}")
endif()

execute_process(
  COMMAND "${OPT}" "-load-pass-plugin=${PLUGIN}" -passes=csan -S "${INPUT}"
  OUTPUT_VARIABLE instrumented
  ERROR_VARIABLE opt_errors
  RESULT_VARIABLE opt_status)

if(NOT opt_status EQUAL 0)
  message(FATAL_ERROR "pass_smoke: opt failed (${opt_status})\n${opt_errors}")
endif()

set(required
    __csan_init          # inserted at main() entry
    __csan_read          # plain load
    __csan_write         # plain store
    __csan_atomic_load   # load atomic
    __csan_atomic_store  # store atomic
    __csan_atomic_rmw    # atomicrmw
    __csan_atomic_cas    # cmpxchg
    __csan_fence_acquire # fence acquire
    __csan_fence_release # fence release
    __csan_sfence        # llvm.x86.sse.sfence
    __csan_mfence        # llvm.x86.sse2.mfence
    __csan_wb            # llvm.x86.clwb
    __csan_flush         # llvm.x86.clflushopt
    __csan_memcpy)       # llvm.memcpy

set(missing "")
foreach(hook IN LISTS required)
  # Only a call site counts: the pass declares every hook unconditionally.
  if(NOT instrumented MATCHES "call[^\n]* @${hook}\\(")
    list(APPEND missing ${hook})
  endif()
endforeach()

if(missing)
  string(REPLACE ";" "\n  " pretty "${missing}")
  message(FATAL_ERROR
          "pass_smoke: the pass inserted no call to:\n  ${pretty}\n"
          "The plugin loaded and the module verified, so this is the pass "
          "declining to instrument, not a build failure.")
endif()

message(STATUS "pass_smoke: all ${CMAKE_MATCH_COUNT} hook families present")
