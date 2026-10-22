include_guard(GLOBAL)

# Capture literal quoted-string comparisons in these functions without changing
# the including script's policy state.
cmake_policy(PUSH)
cmake_policy(SET CMP0054 NEW)
cmake_policy(SET CMP0007 NEW)

# ava_cli_timeouts(SECONDS...) exports AVA_TIMEOUT_<n> and AVA_POLL_<n> to
# the caller. Polls assume the existing 0.05-second driver loops. Debug timeout
# stretching is evaluated at cmake -P runtime, not during project configuration.
function(ava_cli_timeouts)
  foreach(SECONDS IN LISTS ARGN)
    if(NOT "${SECONDS}" MATCHES "^[1-9][0-9]*$")
      message(FATAL_ERROR "ava_cli_timeouts requires positive integer seconds: ${SECONDS}")
    endif()
    set(EFFECTIVE "${SECONDS}")
    if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT})
      set(EFFECTIVE 3600)
      if(DEFINED ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS} AND "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}" MATCHES "^[1-9][0-9]*$")
        set(EFFECTIVE "$ENV{AVA_DEBUG_NO_TIMEOUT_SECONDS}")
      endif()
    endif()
    math(EXPR POLLS "${EFFECTIVE} * 20")
    set(AVA_TIMEOUT_${SECONDS} "${EFFECTIVE}" PARENT_SCOPE)
    set(AVA_POLL_${SECONDS} "${POLLS}" PARENT_SCOPE)
  endforeach()
endfunction()

# ava_cli_isolated_env(OUT ROOT) returns --unset options followed by assignments
# accepted by cmake -E env and GNU env. Keep only loader, sanitizer, locale and
# explicit test-debug settings; credentials, endpoints and proxies never leak in.
function(ava_cli_isolated_env OUT ROOT)
  get_filename_component(ROOT "${ROOT}" ABSOLUTE)
  file(MAKE_DIRECTORY "${ROOT}/home" "${ROOT}/config" "${ROOT}/state" "${ROOT}/data" "${ROOT}/cache" "${ROOT}/runtime")
  file(CHMOD "${ROOT}" "${ROOT}/home" "${ROOT}/config" "${ROOT}/state" "${ROOT}/data" "${ROOT}/cache" "${ROOT}/runtime"
       PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
  set(ASSIGNMENTS
    "HOME=${ROOT}/home" "XDG_CONFIG_HOME=${ROOT}/config"
    "XDG_STATE_HOME=${ROOT}/state" "XDG_DATA_HOME=${ROOT}/data"
    "XDG_CACHE_HOME=${ROOT}/cache" "XDG_RUNTIME_DIR=${ROOT}/runtime" "NO_COLOR=1"
    "http_proxy=" "https_proxy=" "all_proxy=" "ftp_proxy="
    "HTTP_PROXY=" "HTTPS_PROXY=" "ALL_PROXY=" "FTP_PROXY="
    "no_proxy=*" "NO_PROXY=*")
  set(ALLOWLIST PATH LANG LC_ALL LC_CTYPE LD_LIBRARY_PATH LD_PRELOAD
      DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH ASAN_OPTIONS UBSAN_OPTIONS
      LSAN_OPTIONS TSAN_OPTIONS MSAN_OPTIONS ASAN_SYMBOLIZER_PATH LLVM_SYMBOLIZER_PATH
      TMPDIR TZ AVA_DEBUG_NO_TIMEOUT AVA_DEBUG_NO_TIMEOUT_SECONDS
      LIBCWD_NO_STARTUP_MSGS AVA_NO_DEBUG_OUTPUT AVA_TEST_NAME AVA_DEBUG_OUTPUT_DIR
      LIBCWD_RCFILE_NAME LIBCWD_RCFILE_OVERRIDE_NAME)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E environment
                  OUTPUT_VARIABLE INHERITED ERROR_VARIABLE ENV_ERROR RESULT_VARIABLE ENV_RESULT)
  if(NOT ENV_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot isolate the CLI fixture environment: ${ENV_ERROR}")
  endif()
  string(REPLACE "\n" ";" INHERITED "${INHERITED}")
  set(UNSETS "")
  foreach(VARIABLE IN LISTS INHERITED)
    if(VARIABLE MATCHES "^([^=]+)=")
      set(NAME "${CMAKE_MATCH_1}")
      list(FIND ALLOWLIST "${NAME}" ALLOWED_INDEX)
      if(ALLOWED_INDEX EQUAL -1)
        list(APPEND UNSETS "--unset=${NAME}")
      endif()
    endif()
  endforeach()
  list(APPEND UNSETS ${ASSIGNMENTS})
  set(${OUT} "${UNSETS}" PARENT_SCOPE)
endfunction()

# Internal framing helper: avoid CMake list splitting, whose semicolon/bracket
# rules would corrupt valid JSON string contents.
function(_ava_rpc_take_line OUT REST JSONL)
  string(FIND "${JSONL}" "\n" END)
  if(END EQUAL -1)
    set(LINE "${JSONL}")
    set(REMAINING "")
  else()
    string(SUBSTRING "${JSONL}" 0 ${END} LINE)
    math(EXPR START "${END} + 1")
    string(SUBSTRING "${JSONL}" ${START} -1 REMAINING)
  endif()
  set(${OUT} "${LINE}" PARENT_SCOPE)
  set(${REST} "${REMAINING}" PARENT_SCOPE)
endfunction()

# ava_rpc_response_line(OUT JSONL ID) selects exactly one top-level response with
# string id ID. Missing/duplicate matches and malformed nonempty records fail with
# the entire output attached; events and nested ids cannot satisfy the match.
function(ava_rpc_response_line OUT JSONL ID)
  set(REMAINING "${JSONL}")
  set(FOUND FALSE)
  while(NOT REMAINING STREQUAL "")
    _ava_rpc_take_line(LINE REMAINING "${REMAINING}")
    string(STRIP "${LINE}" LINE)
    if(LINE STREQUAL "")
      continue()
    endif()
    string(JSON ROOT_TYPE ERROR_VARIABLE PARSE_ERROR TYPE "${LINE}")
    if(NOT PARSE_ERROR STREQUAL "NOTFOUND" OR NOT ROOT_TYPE STREQUAL "OBJECT")
      message(FATAL_ERROR "RPC output contains a non-object/invalid record: ${LINE}\nstdout:\n${JSONL}")
    endif()
    string(JSON TYPE ERROR_VARIABLE TYPE_ERROR GET "${LINE}" type)
    if(NOT TYPE_ERROR STREQUAL "NOTFOUND" OR NOT TYPE STREQUAL "response")
      continue()
    endif()
    string(JSON ID_TYPE ERROR_VARIABLE ID_ERROR TYPE "${LINE}" id)
    if(NOT ID_ERROR STREQUAL "NOTFOUND" OR NOT ID_TYPE STREQUAL "STRING")
      continue()
    endif()
    string(JSON LINE_ID GET "${LINE}" id)
    if(LINE_ID STREQUAL "${ID}")
      if(FOUND)
        message(FATAL_ERROR "RPC request '${ID}' has duplicate terminal responses\nstdout:\n${JSONL}")
      endif()
      set(FOUND TRUE)
      set(RESPONSE "${LINE}")
    endif()
  endwhile()
  if(NOT FOUND)
    message(FATAL_ERROR "RPC request '${ID}' has no terminal response\nstdout:\n${JSONL}")
  endif()
  set(${OUT} "${RESPONSE}" PARENT_SCOPE)
endfunction()

# ava_rpc_expect_in_line(JSONL ID FIELD EXPECTED_JSON) compares a typed JSON value
# on that request's response only. FIELD is a dot-separated member/index path;
# e.g. (output stats result.entry_count 1), (output p1 success true), or
# (output model result.provider "\"openai\""). Types, numbers, and whole values
# are compared, never substring prefixes. Keys containing dots need a local check.
function(ava_cli_expect_json LINE FIELD EXPECTED_JSON)
  string(REPLACE "." ";" MEMBERS "${FIELD}")
  string(JSON ACTUAL ERROR_VARIABLE FIELD_ERROR GET "${LINE}" ${MEMBERS})
  if(NOT FIELD_ERROR STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Record is missing '${FIELD}': ${FIELD_ERROR}\nrecord: ${LINE}")
  endif()
  string(JSON ACTUAL_TYPE TYPE "${LINE}" ${MEMBERS})
  string(JSON EXPECTED ERROR_VARIABLE EXPECTED_ERROR GET "[${EXPECTED_JSON}]" 0)
  if(NOT EXPECTED_ERROR STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Expected value for '${FIELD}' is not JSON: ${EXPECTED_JSON}")
  endif()
  string(JSON EXPECTED_COUNT LENGTH "[${EXPECTED_JSON}]")
  if(NOT EXPECTED_COUNT EQUAL 1)
    message(FATAL_ERROR "Expected exactly one JSON value for '${FIELD}': ${EXPECTED_JSON}")
  endif()
  string(JSON EXPECTED_TYPE TYPE "[${EXPECTED_JSON}]" 0)
  if(NOT "${ACTUAL_TYPE}" STREQUAL "${EXPECTED_TYPE}" OR NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR "Field '${FIELD}' expected ${EXPECTED_JSON}, got ${ACTUAL_TYPE} '${ACTUAL}'\nrecord: ${LINE}")
  endif()
endfunction()

function(ava_cli_expect_type RECORD FIELD EXPECTED)
  string(REPLACE "." ";" MEMBERS "${FIELD}")
  string(JSON ACTUAL ERROR_VARIABLE FIELD_ERROR TYPE "${RECORD}" ${MEMBERS})
  if(NOT FIELD_ERROR STREQUAL "NOTFOUND" OR NOT ACTUAL STREQUAL "${EXPECTED}")
    message(FATAL_ERROR "Field '${FIELD}' expected JSON type ${EXPECTED}, got '${ACTUAL}': ${FIELD_ERROR}\nrecord: ${RECORD}")
  endif()
endfunction()

function(ava_rpc_expect_in_line JSONL ID FIELD EXPECTED_JSON)
  ava_rpc_response_line(LINE "${JSONL}" "${ID}")
  ava_cli_expect_json("${LINE}" "${FIELD}" "${EXPECTED_JSON}")
endfunction()

# Select catalog entries by identity rather than by unstable array position.
function(ava_cli_array_item OUT RECORD FIELD KEY VALUE)
  string(REPLACE "." ";" MEMBERS "${FIELD}")
  string(JSON ARRAY GET "${RECORD}" ${MEMBERS})
  string(JSON COUNT LENGTH "${ARRAY}")
  set(FOUND FALSE)
  if(COUNT GREATER 0)
    math(EXPR LAST "${COUNT} - 1")
    foreach(INDEX RANGE 0 ${LAST})
      string(JSON ITEM GET "${ARRAY}" ${INDEX})
      string(JSON IDENTITY ERROR_VARIABLE ID_ERROR GET "${ITEM}" "${KEY}")
      if(ID_ERROR STREQUAL "NOTFOUND" AND IDENTITY STREQUAL "${VALUE}")
        if(FOUND)
          message(FATAL_ERROR "Duplicate ${FIELD} entry '${KEY}=${VALUE}': ${RECORD}")
        endif()
        set(FOUND TRUE)
        set(SELECTED "${ITEM}")
      endif()
    endforeach()
  endif()
  if(NOT FOUND)
    message(FATAL_ERROR "Missing ${FIELD} entry '${KEY}=${VALUE}': ${RECORD}")
  endif()
  set(${OUT} "${SELECTED}" PARENT_SCOPE)
endfunction()

# ava_rpc_forbid(JSONL [EXPECTED_FAILURE_ID...]) forbids every success:false
# response except explicitly named error requests. Every response must contain a
# boolean success and a string id; missing or mistyped success cannot silently pass.
function(ava_rpc_forbid JSONL)
  set(REMAINING "${JSONL}")
  set(ALLOWED_FAILURE_IDS "${ARGN}")
  while(NOT REMAINING STREQUAL "")
    _ava_rpc_take_line(LINE REMAINING "${REMAINING}")
    string(STRIP "${LINE}" LINE)
    if(LINE STREQUAL "")
      continue()
    endif()
    string(JSON ROOT_TYPE ERROR_VARIABLE PARSE_ERROR TYPE "${LINE}")
    if(NOT PARSE_ERROR STREQUAL "NOTFOUND" OR NOT ROOT_TYPE STREQUAL "OBJECT")
      message(FATAL_ERROR "RPC output contains a non-object/invalid record: ${LINE}\nstdout:\n${JSONL}")
    endif()
    string(JSON TYPE ERROR_VARIABLE TYPE_ERROR GET "${LINE}" type)
    if(NOT TYPE_ERROR STREQUAL "NOTFOUND" OR NOT TYPE STREQUAL "response")
      continue()
    endif()
    string(JSON SUCCESS_TYPE ERROR_VARIABLE SUCCESS_ERROR TYPE "${LINE}" success)
    string(JSON ID_TYPE ERROR_VARIABLE ID_ERROR TYPE "${LINE}" id)
    if(NOT SUCCESS_ERROR STREQUAL "NOTFOUND" OR NOT SUCCESS_TYPE STREQUAL "BOOLEAN" OR
       NOT ID_ERROR STREQUAL "NOTFOUND" OR NOT ID_TYPE STREQUAL "STRING")
      message(FATAL_ERROR "Malformed RPC response: ${LINE}")
    endif()
    string(JSON SUCCESS GET "${LINE}" success)
    string(JSON ID GET "${LINE}" id)
    list(FIND ALLOWED_FAILURE_IDS "${ID}" FAILURE_INDEX)
    if(NOT SUCCESS AND FAILURE_INDEX EQUAL -1)
      message(FATAL_ERROR "Unexpected RPC failure for '${ID}': ${LINE}\nstdout:\n${JSONL}")
    endif()
  endwhile()
endfunction()

# Plain text fixtures (provider request logs, command-rendered text) deliberately
# use substring checks; structured RPC values use ava_rpc_expect_in_line instead.
function(ava_cli_expect_contains TEXT)
  foreach(EXPECTED IN LISTS ARGN)
    string(FIND "${TEXT}" "${EXPECTED}" POSITION)
    if(POSITION EQUAL -1)
      message(FATAL_ERROR "Expected text '${EXPECTED}' in:\n${TEXT}")
    endif()
  endforeach()
endfunction()

# Bound fake-provider log checks to one request, not its entire following tail.
function(ava_cli_provider_request OUT LOG INDEX)
  set(MARKER "--- request ${INDEX} ---\n")
  string(FIND "${LOG}" "${MARKER}" START)
  if(START EQUAL -1)
    message(FATAL_ERROR "Missing provider request ${INDEX}:\n${LOG}")
  endif()
  string(LENGTH "${MARKER}" MARKER_LENGTH)
  math(EXPR START "${START} + ${MARKER_LENGTH}")
  string(SUBSTRING "${LOG}" ${START} -1 SECTION)
  string(FIND "${SECTION}" "\n--- request " END)
  if(NOT END EQUAL -1)
    string(SUBSTRING "${SECTION}" 0 ${END} SECTION)
  endif()
  set(${OUT} "${SECTION}" PARENT_SCOPE)
endfunction()

# Select one event by name, optionally a payload call id or FIELD VALUE pair.
# Another event/response cannot satisfy assertions on the selected payload.
function(ava_cli_event_line OUT JSONL NAME)
  set(REMAINING "${JSONL}")
  set(FOUND FALSE)
  while(NOT REMAINING STREQUAL "")
    _ava_rpc_take_line(LINE REMAINING "${REMAINING}")
    string(STRIP "${LINE}" LINE)
    if(LINE STREQUAL "")
      continue()
    endif()
    string(JSON EVENT_NAME ERROR_VARIABLE EVENT_ERROR GET "${LINE}" name)
    if(NOT EVENT_ERROR STREQUAL "NOTFOUND" OR NOT EVENT_NAME STREQUAL "${NAME}")
      continue()
    endif()
    if(ARGC GREATER 3)
      if(ARGC EQUAL 5)
        string(REPLACE "." ";" SELECTOR "${ARGV3}")
        set(EXPECTED "${ARGV4}")
      else()
        set(SELECTOR payload call_id)
        set(EXPECTED "${ARGV3}")
      endif()
      string(JSON VALUE ERROR_VARIABLE SELECT_ERROR GET "${LINE}" ${SELECTOR})
      if(NOT SELECT_ERROR STREQUAL "NOTFOUND" OR NOT VALUE STREQUAL "${EXPECTED}")
        continue()
      endif()
    endif()
    if(FOUND)
      message(FATAL_ERROR "Duplicate '${NAME}' event (${ARGN}) in:\n${JSONL}")
    endif()
    set(FOUND TRUE)
    set(EVENT "${LINE}")
  endwhile()
  if(NOT FOUND)
    message(FATAL_ERROR "Missing '${NAME}' event (${ARGN}) in:\n${JSONL}")
  endif()
  ava_cli_expect_json("${EVENT}" schema_version 1)
  ava_cli_expect_json("${EVENT}" type "\"${NAME}\"")
  set(${OUT} "${EVENT}" PARENT_SCOPE)
endfunction()

function(ava_cli_shell_env OUT)
  set(QUOTED "")
  foreach(ASSIGNMENT IN LISTS ARGN)
    string(REPLACE "'" "'\\''" ASSIGNMENT "${ASSIGNMENT}")
    string(APPEND QUOTED " '${ASSIGNMENT}'")
  endforeach()
  set(${OUT} "${QUOTED}" PARENT_SCOPE)
endfunction()

function(ava_cli_forbid_contains TEXT)
  foreach(FORBIDDEN IN LISTS ARGN)
    string(FIND "${TEXT}" "${FORBIDDEN}" POSITION)
    if(NOT POSITION EQUAL -1)
      message(FATAL_ERROR "Forbidden text '${FORBIDDEN}' in:\n${TEXT}")
    endif()
  endforeach()
endfunction()

# Run generated drivers under a dedicated Linux subreaper. Its custody includes
# AVA, providers, and descendants that deliberately establish separate groups.
# The driver's product-cleanup assertions run before this outer safety net.
function(ava_cli_driver_support OUT ROOT)
  find_program(AVA_CLI_PYTHON NAMES python3 REQUIRED)
  set(SUPPORT "${ROOT}/cli-owner.sh")
  file(WRITE "${SUPPORT}"
    "ava_stop_child() {\n"
    "  [ -n \"$1\" ] || return 0\n"
    "  \"${AVA_CLI_PYTHON}\" \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cli_test_support.py\" terminate-group \"$1\" || return 1\n"
    "  wait \"$1\" 2>/dev/null || true\n"
    "}\n"
    "ava_run() {\n"
    "  setsid \"$@\" <&0 &\n"
    "  ava_pid=$!\n"
    "  wait \"$ava_pid\"\n"
    "  ava_status=$?\n"
    "  ava_stop_child \"$ava_pid\" || ava_status=1\n"
    "  ava_pid=\n"
    "  return \"$ava_status\"\n"
    "}\n"
    "ava_group_live() {\n"
    "  \"${AVA_CLI_PYTHON}\" \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cli_test_support.py\" group-live \"$1\"\n"
    "  group_status=$?\n"
    "  case \"$group_status\" in 0|1) return \"$group_status\";; *) echo 'process-group liveness fixture failed' >&2; exit 1;; esac\n"
    "}\n"
    "ava_wait_response() {\n"
    "  \"${AVA_CLI_PYTHON}\" \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cli_test_support.py\" wait-response \"$1\" \"$2\" \"$3\"\n"
    "}\n"
    "ava_cleanup() {\n"
    "  cleanup_status=$?\n"
    "  trap - EXIT INT TERM\n"
    "  ava_stop_child \"$ava_pid\" || cleanup_status=1\n"
    "  if [ -n \"$shell_pgid\" ]; then ava_stop_child \"$shell_pgid\" || cleanup_status=1; fi\n"
    "  fake_provider_stop || cleanup_status=1\n"
    "  if [ -n \"$home_churn\" ]; then rmdir \"$home_churn\" || cleanup_status=1; fi\n"
    "  exit \"$cleanup_status\"\n"
    "}\n"
    "ava_pid=\n"
    "shell_pgid=\n"
    "home_churn=\n"
    "trap ava_cleanup EXIT\n"
    "trap 'exit 130' INT\n"
    "trap 'exit 143' TERM\n")
  set(${OUT} "${SUPPORT}" PARENT_SCOPE)
  set(AVA_CLI_DRIVER_COMMAND "${AVA_CLI_PYTHON}"
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cli_test_support.py" run PARENT_SCOPE)
endfunction()

# Provider fixture locations have one fallback policy for every CLI driver.
macro(ava_cli_provider_paths)
  if(NOT DEFINED AVA_FAKE_PROVIDER_PY OR AVA_FAKE_PROVIDER_PY STREQUAL "")
    set(AVA_FAKE_PROVIDER_PY "${CMAKE_CURRENT_LIST_DIR}/fake_provider.py")
  endif()
  if(NOT DEFINED AVA_FAKE_PROVIDER_SH OR AVA_FAKE_PROVIDER_SH STREQUAL "")
    set(AVA_FAKE_PROVIDER_SH "${CMAKE_CURRENT_LIST_DIR}/fake_provider_shell.sh")
  endif()
  if(NOT DEFINED AVA_PYTHON OR AVA_PYTHON STREQUAL "")
    find_program(AVA_PYTHON NAMES python3 REQUIRED)
  endif()
endmacro()

cmake_policy(POP)
