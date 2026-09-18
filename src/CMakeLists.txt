add_subdirectory(ava)
add_subdirectory(experimental)

if(AVA_BUILD_DESKTOP_QML)
  add_subdirectory(ava/desktop)
endif()

add_executable(ava
  "main.cpp"
)

# Prevent multi-config generators from trying to write the executable to CMAKE_CURRENT_BINARY_DIR:
# that is obviously not possible since we already have a directory called `ava` here.
set_target_properties(ava PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}"
)

target_link_libraries(ava
  PRIVATE
    ${AVA_FRONTEND_LIBRARIES}
    ${AVA_BACKEND_LIBRARIES}
)
if (OptionEnableLibcwd)
  # Frontend lists AVA::terminal before ava_debug, so CMake's static-cycle
  # expansion ends on debug. GNU ld will not extract TextSpan typeinfo for
  # print_members.cpp.o pulled from that last debug visit unless the pair is
  # rescanned.
  target_link_libraries(ava PRIVATE "$<LINK_GROUP:RESCAN,AVA::debug,AVA::terminal>")
endif()

install(TARGETS ava RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ava)

ava_apply_debugging(ava)
ava_apply_warnings(ava)
ava_apply_sanitizers(ava)
