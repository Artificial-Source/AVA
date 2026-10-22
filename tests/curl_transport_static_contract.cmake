if(NOT DEFINED AVA_CURL_TRANSPORT_SOURCE OR NOT DEFINED AVA_CURL_TRANSPORT_HEADER)
  message(FATAL_ERROR "curl transport static contract requires source and header paths")
endif()

file(READ "${AVA_CURL_TRANSPORT_SOURCE}" curl_source)
file(READ "${AVA_CURL_TRANSPORT_HEADER}" curl_header)

function(_curl_static_violation OUT SOURCE)
  string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" scan_source "${SOURCE}")
  string(REGEX REPLACE "//[^\r\n]*" "" scan_source "${scan_source}")
  set(violation "")
  foreach(api IN ITEMS fork vfork posix_spawn posix_spawnp system popen kill waitpid waitid poll)
    if(scan_source MATCHES "(^|[^A-Za-z0-9_:])(::)?${api}[ \t\r\n]*\\(")
      set(violation "${api}")
      break()
    endif()
  endforeach()
  if(violation STREQUAL "" AND scan_source MATCHES "(^|[^A-Za-z0-9_])pollfd([^A-Za-z0-9_]|$)")
    set(violation "pollfd")
  endif()
  if(violation STREQUAL "")
    foreach(header IN ITEMS poll\\.h signal\\.h sys/wait\\.h)
      if(scan_source MATCHES "(^|\n)[ \t]*#[ \t]*include[ \t]*[<\"]${header}[>\"][ \t]*(\n|$)")
        set(violation "${header}")
        break()
      endif()
    endforeach()
  endif()
  set(${OUT} "${violation}" PARENT_SCOPE)
endfunction()

foreach(api IN ITEMS fork vfork posix_spawn posix_spawnp system popen kill waitpid waitid poll)
  foreach(form IN ITEMS "${api}()" "::${api}()")
    _curl_static_violation(found "${form}")
    if(found STREQUAL "")
      message(FATAL_ERROR "curl static scanner failed to detect ${form}")
    endif()
  endforeach()
endforeach()
# Exercise the same scanner on mutations without shared scratch files.
foreach(banned IN ITEMS "struct pollfd value" "#include <poll.h>" "# include <signal.h>" "#include <sys/wait.h>")
  _curl_static_violation(found "${banned}")
  if(found STREQUAL "")
    message(FATAL_ERROR "curl static scanner failed to detect ${banned}")
  endif()
endforeach()
foreach(clean IN ITEMS "request_kill()" "skill()" "foo::fork()" "foofork()" "killjoy()" "waitpid2()" "pollfd_cache"
    "// fork() kill()" "/* poll()\n#include <poll.h>\n*/" "// #include <signal.h>" "#include <pollXh>")
  _curl_static_violation(found "${clean}")
  if(NOT found STREQUAL "")
    message(FATAL_ERROR "curl static scanner falsely detected ${clean}: ${found}")
  endif()
endforeach()

_curl_static_violation(curl_violation "${curl_source}")
if(NOT curl_violation STREQUAL "")
  message(FATAL_ERROR "curl transport regained legacy process ownership: ${curl_violation}")
endif()

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
