cmake_minimum_required(VERSION 3.27...4.3.4)

project(ava
  VERSION 1.0.0
  LANGUAGES CXX
  DESCRIPTION "OpenAI-first C++23 terminal coding agent with an ncursesw TUI"
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#==============================================================================
# Begin of gitache configuration.

include(FetchContent)
option(AVA_ENABLE_GITACHE "Fetch and enable gitache-managed developer dependencies" ON)
if(AVA_ENABLE_GITACHE)
  # If a local gitache directory is present then use that rather than downloading one.
  if (EXISTS ${CMAKE_CURRENT_LIST_DIR}/gitache/gateway.cmake)
    # This will disable the use of the GIT_REPOSITORY/GIT_TAG below, and disable the
    # FetchContent- download and update step. Instead, use the gitache submodule as-is.
    set(FETCHCONTENT_SOURCE_DIR_GITACHE "${CMAKE_CURRENT_LIST_DIR}/gitache" CACHE INTERNAL "" FORCE)
  endif ()

  set(GITACHE_PACKAGES libcwd)  # List of requested packages.

  FetchContent_Declare(
    gitache
    GIT_REPOSITORY "https://github.com/CarloWood/gitache.git"
    GIT_TAG        "c5320544dfb15cbc444c3ae260dba382ceff1251"
  )
  FetchContent_MakeAvailable(gitache)
else()
  message(STATUS "gitache disabled by AVA_ENABLE_GITACHE=OFF")
endif()
# End of gitache configuration.

#==============================================================================
# This project uses aicxx modules.
include(cmake/aicxx/Project NO_POLICY_SCOPE)

#==============================================================================
# OPTIONS

set(AVA_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(AVA_VERSION_DISPLAY "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
file(MAKE_DIRECTORY "${AVA_GENERATED_INCLUDE_DIR}/ava/core")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ava/core/version.h.in"
  "${AVA_GENERATED_INCLUDE_DIR}/ava/core/version.h"
  @ONLY
)

# Keep the long-standing AVA_* command-line variables as compatibility inputs.
# An explicitly set cw_option value wins; otherwise it follows the legacy cache
# variable so existing presets, CI jobs, and documented invocations still work.
option(AVA_BUILD_TESTS "Build AVA tests (compatibility alias for EnableAvaBuildTests)" ON)
option(AVA_ENABLE_SANITIZERS "Build AVA with sanitizers (compatibility alias for EnableAvaSanitizers)" OFF)
cw_option(EnableAvaBuildTests
    "Build AVA tests" "${AVA_BUILD_TESTS}"
    "" OFF)
cw_option(EnableAvaSanitizers
    "Build AVA with AddressSanitizer and UndefinedBehaviorSanitizer" "${AVA_ENABLE_SANITIZERS}"
    "" OFF)

# AVA_BUILD_DESKTOP_QML gates the experimental Qt Quick/QML desktop prototype
# under src/ava/desktop (added upstream). It is declared with plain option()
# because src/CMakeLists.txt references the AVA_BUILD_DESKTOP_QML variable name
# directly, unlike the cw_option-based toggles above.
option(AVA_BUILD_DESKTOP_QML "Build the experimental Qt Quick/QML desktop prototype" OFF)
option(AVA_ENABLE_TSAN "Build AVA with ThreadSanitizer (focused coordinator tests only)" OFF)
if(OptionEnableAvaSanitizers AND AVA_ENABLE_TSAN)
  message(FATAL_ERROR "EnableAvaSanitizers/AVA_ENABLE_SANITIZERS and AVA_ENABLE_TSAN are mutually exclusive")
endif()

# Default maximum string length for debug output truncation. When no maxlen()
# IO manipulator is active on the debug stream, strings longer than this are
# truncated in debug output. 0 means no limit. The value ends up in the
# generated ava/core/config.h as config::ava_debug_maxlen_c.
#
# This is a numeric config value, not a boolean, so it uses set(CACHE STRING)
# rather than cw_option (which is ON/OFF only: a value like 50 is neither "ON"
# nor "OFF" and would be silently replaced by the default).
set(AVA_DEBUG_MAXLEN 100 CACHE STRING "Default maximum string length for debug output truncation (0 = no limit).")
if(AVA_DEBUG_MAXLEN EQUAL 100)
  set(_ava_debug_maxlen_color "${OptionColorYay}")
  set(_ava_debug_maxlen_info " (default)")
else()
  set(_ava_debug_maxlen_color "${OptionColorAlert}")
  set(_ava_debug_maxlen_info "")
endif()
message(STATUS "${Option} ${OptionColor}AVA_DEBUG_MAXLEN${ColourReset} (Default maximum string length for debug output truncation) =\n\t${_ava_debug_maxlen_color}${AVA_DEBUG_MAXLEN}${ColourReset}${_ava_debug_maxlen_info}")
unset(_ava_debug_maxlen_color)
unset(_ava_debug_maxlen_info)

# ACP keeps JSON implementation details behind its codec. Prefer an installed
# package so configured offline environments do not need the network; the
# fallback is an immutable official release artifact.
find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
  set(JSON_BuildTests OFF CACHE INTERNAL "" FORCE)
  set(JSON_Install OFF CACHE INTERNAL "" FORCE)
  # The initialized, pinned gitlink is the release source dependency. Keep the
  # immutable upstream archive only as the existing non-qualified fallback for
  # incomplete developer checkouts; provenance rejects a missing gitlink.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/json/CMakeLists.txt")
    add_subdirectory(src/json EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(
      ava_nlohmann_json
      URL "https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz"
      URL_HASH "SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(ava_nlohmann_json)
  endif()
endif()

function(ava_apply_debugging target)
  target_link_libraries(${target} PRIVATE ${AICXX_OBJECTS_LIST})
endfunction()

function(ava_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Werror -Wextra -Wpedantic -Wundef)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
      # Keep GCC's optimizer-driven maybe-uninitialized diagnostics visible, but
      # do not promote them to errors for GCC 13.x through 16.x. Those releases can report false
      # positives through libstdc++ internals such as std::regex, std::optional,
      # and std::variant when Release optimization inlines those implementations.
      target_compile_options(${target} PRIVATE -Wno-error=maybe-uninitialized)
    endif()
    if (NOT "${CMAKE_CXX_FLAGS}" MATCHES "-f(error-limit|max-errors)=")
      # Limit number of errors to five (if this option isn't already set in CXXFLAGS).
      target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-fmax-errors=5>
        $<$<CXX_COMPILER_ID:Clang>:-ferror-limit=5>
        $<$<CXX_COMPILER_ID:AppleClang>:-ferror-limit=5>
      )
    endif()
  endif()
endfunction()

function(ava_apply_sanitizers target)
  if(OptionEnableAvaSanitizers AND NOT MSVC)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(${target} PUBLIC -fsanitize=address,undefined -fno-sanitize-recover=undefined)
  endif()
endfunction()

#==============================================================================
# CONFIG FILES

# Generate config.h from config.h.in.
configure_file(
  "${CMAKE_SOURCE_DIR}/config.h.in"
  "${CMAKE_BINARY_DIR}/config.h"
  @ONLY
)

# Tell cwds/sys.h that we have a config.h. Reject the two guarded non-MIT
# ai-utils headers if a future AVA source accidentally includes either one.
add_definitions(-DHAVE_CONFIG_H)
add_compile_definitions(MIT_LICENSE_ONLY)
include_directories(${top_objdir})

# End of CONFIG FILES section.

#==============================================================================

# ThreadSanitizer must instrument every project-built object that can be linked
# into an instrumented executable. Applying it only to AVA's final targets can
# select uninstrumented weak libstdc++ template definitions from AICxx object
# libraries and hide real synchronization from TSan.
if(AVA_ENABLE_TSAN AND NOT MSVC)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

# ai-utils probes x86 BMI2 by compiling and running GCC x86 builtins. Those
# builtins do not exist on non-x86 targets, where a failed probe must mean
# "unsupported" rather than aborting configuration.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _ava_system_processor)
if(NOT _ava_system_processor MATCHES "^(x86_64|amd64|i[3-6]86)$")
  set(CW_BMI2_SUPPORT 0 CACHE INTERNAL "BMI2 is unavailable on non-x86 targets" FORCE)
endif()
unset(_ava_system_processor)

# Declare aicxx submodules.
include(AICxxSubmodules)

# libcwd publishes libdw/libelf as interface dependencies rather than recording
# them as NEEDED entries. Linux toolchains that default to --as-needed can then
# discard both libraries and leave debug executables with unresolved symbols.
if(OptionEnableLibcwd AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(cwds_ObjLib INTERFACE "LINKER:--no-as-needed")
endif()

set(AICXX_OBJECTS_LIST ${AICXX_OBJECTS_LIST} enchantum::enchantum CACHE INTERNAL "List of OBJECT libaries that this project uses.")

add_subdirectory(src)
add_subdirectory(enchantum)

install(
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/operations/release-artifact-readme.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}"
  RENAME README.md
  COMPONENT ava
)
install(
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}"
  COMPONENT ava
)
install(
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/core/usage.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/core/configuration.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/core/context-resources.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/core/custom-providers.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/core/environment-variables.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/core/providers.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/core/subagents.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/core/thinking-modes.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/core/tools.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs/core"
  COMPONENT ava
)
install(
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/interfaces/themes-keybindings.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs/interfaces"
  COMPONENT ava
)
install(
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/operations/testing.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/operations/terminal-setup.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/operations/troubleshooting.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/operations/diagnostics.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/operations/release-checklist.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs/operations"
  COMPONENT ava
)
install(
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/extensions/lsp.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/extensions/mcp.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/extensions/plugin-system.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs/extensions"
  COMPONENT ava
)
install(
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/security/sandboxing.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/security/containment.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs/security"
  COMPONENT ava
)
install(
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/development/session-versioning.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/development/side-effect-safety-checklist.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs/development"
  COMPONENT ava
)
install(
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/headless-protocol.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/rpc-protocol.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/acp.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/acp-support.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/session-format.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/plugin-compatibility-policy.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs"
  COMPONENT ava
)
install(
  FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/interop/evidence/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/docs/interop/evidence/zed-1.9.0-2026-07-14.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs/interop/evidence"
  COMPONENT ava
)
install(
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/product/mvp-coverage-ledger.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs/product"
  COMPONENT ava
)
install(
  FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/schema/theme.schema.json"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/docs/schema"
  COMPONENT ava
)

if(OptionEnableAvaBuildTests)
  enable_testing()
  add_subdirectory(tests)
endif()
