# Finite CTest per-test timeout policy for the AVA test directory.
#
# ava_apply_test_timeout_policy(<test>...) applies two layers, in order, to
# the given registered test names:
#
# 1. Finite default: every test that did not set an explicit TIMEOUT property
#    receives AVA_DEFAULT_TEST_TIMEOUT_SECONDS (60s), so no suite can hang
#    CTest forever. Explicit narrower or longer limits are preserved.
# 2. Debug override: when AVA_DEBUG_NO_TIMEOUT is present in the environment
#    at *configure* time (e.g. `AVA_DEBUG_NO_TIMEOUT=1 aap-configure`), every
#    test's TIMEOUT is raised so ctest no longer SIGTERMs a hung or
#    step-debugged test while a debugger is attached. Optional
#    AVA_DEBUG_NO_TIMEOUT_SECONDS overrides the one-hour default, which is the
#    literal floor requested by the knob ("at least one hour, or effectively
#    never"); a non-positive-integer value falls back to 3600.
#
# The override is configure-time only because CTest bakes TIMEOUT into
# CTestTestfile.cmake at generate time; toggle the variable and re-run
# aap-configure (aap-test alone cannot change it).
#
# Only the ctest kill-timer is touched. In-test deadlines (condition_variable
# waits, execute_process TIMEOUTs, futures) are intentionally left as-authored:
# many of them encode negative assertions ("this must not happen") and
# inflating them blindly would turn green tests into multi-hour hangs.
set(AVA_DEFAULT_TEST_TIMEOUT_SECONDS 60)

function(ava_apply_test_timeout_policy)
  # Fill only unset per-test TIMEOUT properties with the finite default.
  foreach(_ava_test IN LISTS ARGV)
    get_property(_ava_timeout_set TEST ${_ava_test} PROPERTY TIMEOUT SET)
    if(NOT _ava_timeout_set)
      set_property(TEST ${_ava_test} PROPERTY TIMEOUT ${AVA_DEFAULT_TEST_TIMEOUT_SECONDS})
    endif()
  endforeach()

  # Debug override: presence of AVA_DEBUG_NO_TIMEOUT enables it; a positive
  # integer AVA_DEBUG_NO_TIMEOUT_SECONDS overrides the 3600s default.
  if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT})
    set(AVA_DEBUG_NO_TIMEOUT_SECONDS "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}")
    if(AVA_DEBUG_NO_TIMEOUT_SECONDS STREQUAL "" OR NOT AVA_DEBUG_NO_TIMEOUT_SECONDS MATCHES "^[1-9][0-9]*$")
      set(AVA_DEBUG_NO_TIMEOUT_SECONDS 3600)
    endif()
    if(ARGV)
      list(LENGTH ARGV AVA_DEBUG_NO_TIMEOUT_COUNT)
      set_property(TEST ${ARGV} PROPERTY TIMEOUT ${AVA_DEBUG_NO_TIMEOUT_SECONDS})
      message(STATUS
        "${Red}AVA_DEBUG_NO_TIMEOUT: raised CTest TIMEOUT to ${AVA_DEBUG_NO_TIMEOUT_SECONDS}s for "
        "${AVA_DEBUG_NO_TIMEOUT_COUNT} test(s); rerun configure without it to restore normal timeouts.${ColourReset}")
    endif()
  endif()
endfunction()
