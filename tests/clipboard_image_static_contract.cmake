if(NOT DEFINED AVA_CLIPBOARD_IMAGE_SOURCE OR NOT DEFINED AVA_CLIPBOARD_IMAGE_HEADER OR NOT DEFINED AVA_INTERACTIVE_TUI_SOURCE)
  message(FATAL_ERROR "clipboard image static contract requires source, header, and TUI paths")
endif()

file(READ "${AVA_CLIPBOARD_IMAGE_SOURCE}" clipboard_source)
file(READ "${AVA_CLIPBOARD_IMAGE_HEADER}" clipboard_header)
get_filename_component(tui_source_name "${AVA_INTERACTIVE_TUI_SOURCE}" NAME)
if(tui_source_name STREQUAL "interactive_tui.cpp")
  get_filename_component(tui_source_directory "${AVA_INTERACTIVE_TUI_SOURCE}" DIRECTORY)
  set(tui_authority_source_path "${tui_source_directory}/TuiFrontEnd.cpp")
else()
  set(tui_authority_source_path "${AVA_INTERACTIVE_TUI_SOURCE}")
endif()
if(NOT EXISTS "${tui_authority_source_path}")
  message(FATAL_ERROR "clipboard image static contract cannot find TUI authority owner: ${tui_authority_source_path}")
endif()
file(READ "${tui_authority_source_path}" tui_source)

function(_clipboard_static_violation OUT SOURCE)
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
    foreach(header IN ITEMS poll\\.h signal\\.h csignal sys/wait\\.h sys/types\\.h unistd\\.h fcntl\\.h)
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
    _clipboard_static_violation(found "${form}")
    if(found STREQUAL "")
      message(FATAL_ERROR "clipboard static scanner failed to detect ${form}")
    endif()
  endforeach()
endforeach()
# Exercise the same scanner on mutations without shared scratch files.
foreach(banned IN ITEMS "struct pollfd value" "#include <poll.h>" "# include <signal.h>" "#include <csignal>"
    "#include <sys/wait.h>" "#include <sys/types.h>" "#include <unistd.h>" "#include <fcntl.h>")
  _clipboard_static_violation(found "${banned}")
  if(found STREQUAL "")
    message(FATAL_ERROR "clipboard static scanner failed to detect ${banned}")
  endif()
endforeach()
foreach(clean IN ITEMS "request_kill()" "skill()" "foo::fork()" "foofork()" "killjoy()" "waitpid2()" "pollfd_cache"
    "// fork() kill()" "/* poll()\n#include <poll.h>\n*/" "// #include <signal.h>" "#include <pollXh>")
  _clipboard_static_violation(found "${clean}")
  if(NOT found STREQUAL "")
    message(FATAL_ERROR "clipboard static scanner falsely detected ${clean}: ${found}")
  endif()
endforeach()

_clipboard_static_violation(clipboard_violation "${clipboard_source}")
if(NOT clipboard_violation STREQUAL "")
  message(FATAL_ERROR "clipboard image helper regained legacy process ownership: ${clipboard_violation}")
endif()
foreach(required IN ITEMS
    "session_process_scope.operation()"
    "make_clipboard_desktop_environment_v1"
    "if (Clock::now() >= helper_deadline)"
    "supervisor.reserve"
    "ProcessRoleV1::ClipboardHelper"
    ".termination_grace = 0ms"
    ".execution_deadline = helper_deadline"
    "supervisor.spawn"
    ".cwd = \"/\""
    "StreamModeV1::Discard"
    "StreamModeV1::Capture"
    "supervisor.request_stop"
    "supervisor.wait("
    "supervisor.try_wait"
    "supervisor.wait_for_activity"
    "supervisor.account_output"
    "status.cleanup != ava::process::CleanupStateV1::Complete"
    "clipboard_helper_lifecycle_error()"
    "AVA_ASSERT_NO_SESSION_LOCK_HELD")
  string(FIND "${clipboard_source}" "${required}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "clipboard image static contract is missing: ${required}")
  endif()
endforeach()

string(FIND "${clipboard_source}" "if (Clock::now() >= helper_deadline)" deadline_check_index)
string(FIND "${clipboard_source}" "auto reservation = supervisor.reserve" reservation_index)
if(deadline_check_index EQUAL -1 OR reservation_index EQUAL -1 OR deadline_check_index GREATER reservation_index)
  message(FATAL_ERROR "clipboard image helper must reject an exhausted deadline before reservation")
endif()

foreach(required_header IN ITEMS
    "std::optional<ava::process::ProcessScopeV1> const& session_process_scope"
    "import_clipboard_image_attachment")
  string(FIND "${clipboard_header}" "${required_header}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "clipboard image API is not session-process-scoped: ${required_header}")
  endif()
endforeach()

foreach(required_tui IN ITEMS
    "TuiFrontEnd::on_paste_clipboard_image() const"
    "std::optional<std::pair<session::SessionStore, std::optional<process::ProcessScopeV1>>> authority"
    "SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_)"
    "authority.emplace(session_r->store, session_r->session_process_scope())"
    "import_clipboard_image_attachment(authority->first, authority->second)")
  string(FIND "${tui_source}" "${required_tui}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "TUI frontend does not snapshot and pass the current session scope: ${required_tui}")
  endif()
endforeach()

string(CONCAT tui_authority_snapshot
    "  std::optional<std::pair<session::SessionStore, std::optional<process::ProcessScopeV1>>> authority;\n"
    "  {\n"
    "    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session_);\n"
    "    authority.emplace(session_r->store, session_r->session_process_scope());\n"
    "  }\n"
    "  return import_clipboard_image_attachment(authority->first, authority->second);")
string(FIND "${tui_source}" "${tui_authority_snapshot}" snapshot_found)
if(snapshot_found EQUAL -1)
  message(FATAL_ERROR "TUI frontend must copy clipboard store/process authority inside one session read guard before import")
endif()
