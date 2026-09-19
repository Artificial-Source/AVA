# Create the `terminal` object library.
add_library(ava_terminal)

# Declare source files.
target_sources(ava_terminal
  PRIVATE
    "BasicScreen.cxx"
    "BasicWindow.cxx"
    "CIEDE2000.cxx"
    "ComplexChar.cxx"
    "ColorPalette.cxx"
    "Context.cxx"
    "Cursor.cxx"
    "GraphemeRun.cxx"
    "GraphemeSpan.cxx"
    "HorizontalLayout.cxx"
    "KeyboardInputMode.cxx"
    "LayoutItem.cxx"
    "MouseInputMode.cxx"
    "Spacer.cxx"
    "Pad.cxx"
    "Paragraph.cxx"
    "TextSpan.cxx"
    "Window.cxx"
    "WindowBorder.cxx"
    "WindowPad.cxx"
    "private_convert.cxx"
)

if (OptionEnableLibcwd)
target_sources(ava_terminal
  PRIVATE
    "print_members.cpp"
)
endif ()

# Set required include directories.
get_target_property(CWDS_INTERFACE_INCLUDE_DIRECTORIES AICxx::cwds INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(ava_terminal
  PUBLIC
    "${CWDS_INTERFACE_INCLUDE_DIRECTORIES}"     # For sys.h and debug.h.
    "${CMAKE_SOURCE_DIR}/src"                   # Because headers of terminal/ use #include "ava/...".
  INTERFACE
    "${CMAKE_SOURCE_DIR}/src/ava/tui"
  PRIVATE
    ${CURSES_INCLUDE_DIRS}
)

# Set link dependencies.
target_link_libraries(ava_terminal
  PUBLIC
    AVA::core           # ava::core::Application::instance()
    ${CURSES_LIBRARIES}
    ${AICXX_OBJECTS_LIST}
)

if (OptionEnableLibcwd)
  target_link_libraries(ava_terminal
    PUBLIC
      AVA::debug
  )
endif ()

ava_apply_warnings(ava_terminal)
ava_apply_sanitizers(ava_terminal)

# Create an ALIAS target.
add_library(AVA::terminal ALIAS ava_terminal)
