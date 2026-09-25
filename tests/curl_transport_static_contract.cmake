if(NOT DEFINED AVA_CURL_TRANSPORT_SOURCE OR NOT DEFINED AVA_CURL_TRANSPORT_HEADER)
  message(FATAL_ERROR "curl transport static contract requires source and header paths")
endif()

file(READ "${AVA_CURL_TRANSPORT_SOURCE}" curl_source)
file(READ "${AVA_CURL_TRANSPORT_HEADER}" curl_header)

foreach(forbidden IN ITEMS
    "fork[ \t\r\n]*\\("
    "kill[ \t\r\n]*\\("
    "waitpid[ \t\r\n]*\\("
    "waitid[ \t\r\n]*\\("
    "poll[ \t\r\n]*\\("
    "pollfd"
    "<poll.h>"
    "<signal.h>"
    "<sys/wait.h>")
  if(curl_source MATCHES "${forbidden}")
    message(FATAL_ERROR "curl transport regained legacy process ownership: ${forbidden}")
  endif()
endforeach()

foreach(required IN ITEMS
    [=[dump-header = \"/dev/stderr\"]=]
    "suppress-connect-headers"
    "parent_scope_.operation()"
    "make_curl_environment_v1"
    "supervisor.reserve"
    "supervisor.spawn"
    "supervisor.request_stop"
    "supervisor.wait("
    "supervisor.try_wait"
    "supervisor.wait_for_activity"
    "supervisor.account_output")
  string(FIND "${curl_source}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "curl transport static contract is missing: ${required}")
  endif()
endforeach()

string(FIND "${curl_header}" "CurlCliTransport() = delete" deleted_default)
string(FIND "${curl_header}" "explicit CurlCliTransport(ava::process::ProcessScopeV1 parent_scope)" scoped_constructor)
if(deleted_default EQUAL -1 OR scoped_constructor EQUAL -1)
  message(FATAL_ERROR "curl transport constructor is not explicitly process-scoped")
endif()
