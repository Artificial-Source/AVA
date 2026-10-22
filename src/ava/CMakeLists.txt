set(AVA_BACKEND_LIBRARIES
  ava_agent
  ava_config
  ava_context
  ava_core
  ava_event
  ava_frontend
  ava_diagnostics
  ava_http
  ava_command
  ava_containment
  ava_lsp
  ava_mcp
  ava_observability
  ava_permissions
  ava_plugin
  ava_process
  ava_provider
  ava_session
  ava_tools

  CACHE INTERNAL "AVA backend libraries"
)

add_subdirectory(agent)
add_subdirectory(config)
add_subdirectory(context)
add_subdirectory(core)
add_subdirectory(process)
add_subdirectory(event)
add_subdirectory(frontend)
add_subdirectory(http)
add_subdirectory(diagnostics)
add_subdirectory(command)
add_subdirectory(containment)
add_subdirectory(lsp)
add_subdirectory(mcp)
add_subdirectory(observability)
add_subdirectory(permissions)
add_subdirectory(plugin)
add_subdirectory(provider)
add_subdirectory(session)
add_subdirectory(tools)

set(AVA_FRONTEND_LIBRARIES
  AVA::app
  AVA::tui
  AVA::terminal

  CACHE INTERNAL "AVA frontend libraries"
)

add_subdirectory(app)
add_subdirectory(tui)

if (OptionEnableLibcwd)
  add_subdirectory(debug)
  list(APPEND AVA_BACKEND_LIBRARIES ava_debug)
  set(AVA_BACKEND_LIBRARIES ${AVA_BACKEND_LIBRARIES} CACHE INTERNAL "AVA backend libraries (with debugging enabled)")
endif ()
