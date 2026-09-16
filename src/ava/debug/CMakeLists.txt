# Build-time debug printer generation only runs when libcwd debug support is
# effectively enabled, i.e. when OptionEnableLibcwd is ON.
#
# That generation requires ctags with JSON output, a Universal Ctags feature.
#
# The whole detection is therefore gated on OptionEnableLibcwd, and a JSON-
# capable ctags is then a hard requirement. Configure with -DEnableLibcwd=OFF
# to skip this entirely.
#

# CTAGS_EXECUTABLE is the cache variable holding the path to the binary, or
# CTAGS_EXECUTABLE-NOTFOUND when no ctags is on the PATH. CTAGS_FOUND is the
# normalized boolean recomputed every configure from CTAGS_EXECUTABLE.
find_program(CTAGS_EXECUTABLE ctags
  DOC "Path to the ctags executable; used by build-time code generation targets."
)
if (CTAGS_EXECUTABLE)
  set(CTAGS_FOUND TRUE)
else()
  set(CTAGS_FOUND FALSE)
endif ()

set(CTAGS_HAS_JSON FALSE)
if (CTAGS_FOUND)
  include("${CMAKE_SOURCE_DIR}/cmake/DetectCtagsJson.cmake")
  ava_detect_ctags_json("${CTAGS_EXECUTABLE}" CTAGS_HAS_JSON)
endif ()

# Print find result(s).
if (CTAGS_FOUND)
  message(STATUS "Found ctags: ${CTAGS_EXECUTABLE}")
  if (CTAGS_HAS_JSON)
    message(STATUS "ctags supports JSON output (Universal Ctags)")
  else()
    message(STATUS "ctags does not support JSON output")
  endif ()
else()
  message(STATUS "ctags not found.")
endif ()

# Require a JSON-capable ctags (Universal Ctags) when libcwd is enabled.
if (NOT CTAGS_HAS_JSON)
  message(FATAL_ERROR
    "EnableLibcwd is ON, which requires a ctags with JSON output support "
    "(Universal Ctags with the 'json' output format available) for "
    "build-time debug printer generation, but no such ctags was found. "
    "Install Universal Ctags with JSON support, or work around this by "
    "configuring with -DEnableLibcwd=OFF.")
endif ()

find_program(PYTHON3_EXECUTABLE python3
  DOC "Path to the python3 executable; used to generate print_members definitions."
  REQUIRED
)

#------------------------------------------------------------------------------
# Create the pseudo target `ctags-json` that generates tags.json
#
set(_ava_ctags_json_out "${CMAKE_BINARY_DIR}/generated/ctags/tags.json")

# Ninja decides whether to rerun the print_members generator by comparing the
# build-log-RECORDED mtime of the generated .cpp outputs against the CURRENT
# mtime of the edge's inputs (it does not re-stat the outputs for this check).
# If the .cpp edge depended on tags.json directly, then every configure that
# refreshes tags.json -- which configure always does when it has to regenerate
# the printers -- would leave tags.json newer than the .cpp edge's recorded
# output mtime, so the very next build would needlessly rerun the generator even
# though configure already regenerated the .cpp.
#
# The stamp breaks that coupling: the .cpp edge depends on the stamp, and ONLY
# `ctags-json` advances the stamp. A manual `ctags-json` refresh is therefore
# picked up by the next build (the intended behavior), while a configure-time or
# `generate-print-members`-time regeneration -- which already refreshed the .cpp
# themselves -- deliberately leaves the stamp untouched, so no follow-up build
# reruns the generator.
set(_ava_ctags_json_stamp "${CMAKE_BINARY_DIR}/generated/ctags/tags.json.stamp")

# Ensure the stamp exists before the build-time edge can reference it. It is
# created only when absent; its mtime is never bumped here (only `ctags-json`
# does that), see the rationale above.
if (NOT EXISTS "${_ava_ctags_json_stamp}")
  file(WRITE "${_ava_ctags_json_stamp}" "")
endif ()

add_custom_target(ctags-json
  COMMAND "${CMAKE_SOURCE_DIR}/cmake/scripts/generate_ctags_json.sh"
          "${_ava_ctags_json_out}"
          "${CTAGS_EXECUTABLE}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_ava_ctags_json_stamp}"
  DEPENDS "${CMAKE_SOURCE_DIR}/cmake/scripts/generate_ctags_json.sh"
          "${CMAKE_SOURCE_DIR}/cmake/scripts/list_ava_sources.sh"
  COMMENT "Generating JSON tags for debug printer generation: ${_ava_ctags_json_out}"
  VERBATIM
)

# The print_members.cpp files live under here. The build-time rule that
# (re)generates them and the `generate-print-members` pseudo-target are declared
# further down, once the set of output files is known.
#
set(_ava_print_members_dir "${CMAKE_BINARY_DIR}/generated/print_members")

#------------------------------------------------------------------------------
# Create the `source_files` list, containing all required print_members.cpp files.
#
set(_ava_source_list_file "${_ava_print_members_dir}/source_files")

# Determine if we need to run the generator at configure time.
#
# This configure-time generation only exists so CMake can discover the set of
# generated print_members.cpp files (read from source_files) and add them to the
# ava_debug library below. It therefore runs only when source_files or any listed
# cpp is absent (e.g. a fresh checkout). Staleness against tags.json -- e.g.
# after a manual `ctags-json` refresh -- is handled at BUILD time by the
# add_custom_command(OUTPUT ...) declared further below, so it is intentionally
# NOT checked here.
set(NEED_GENERATION FALSE)

if (NOT EXISTS "${_ava_source_list_file}")
  message(STATUS "Need generation of print_members.cpp files because ${_ava_source_list_file} doesn't exist.")
  set(NEED_GENERATION TRUE)
else()
  # File exists, read it.
  file(STRINGS "${_ava_source_list_file}" REL_FILES)
  foreach(REL_FILE IN LISTS REL_FILES)
    if (NOT EXISTS "${_ava_print_members_dir}/${REL_FILE}")
      set(NEED_GENERATION TRUE)
      message(STATUS "Need (re)generation of print_members.cpp files because \"${_ava_print_members_dir}/${REL_FILE}\" doesn't exist.")
      break() # Stop checking as soon as we find a missing file.
    endif ()
  endforeach()
endif ()

# Run the generator if needed.
if (NOT NEED_GENERATION)
  message(STATUS "Not regenerating print_members.cpp files")
else ()
  # Manually run whatever ctags-json does here.
  message(STATUS "Generating ${_ava_ctags_json_out}")
  execute_process(
    COMMAND "${CMAKE_SOURCE_DIR}/cmake/scripts/generate_ctags_json.sh"
            "${_ava_ctags_json_out}"
            "${CTAGS_EXECUTABLE}"
    RESULT_VARIABLE GEN_RESULT
    OUTPUT_VARIABLE GEN_OUTPUT
    ERROR_VARIABLE GEN_ERROR
  )

  if (GEN_OUTPUT)
    message(STATUS "${GEN_OUTPUT}")
  endif ()
  if (GEN_ERROR)
    message(STATUS "${GEN_ERROR}")
  endif ()

  if (NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to successfully generate the \"${_ava_ctags_json_out}\" file.")
  endif ()

  # Manually run the python script.
  message(STATUS "Generating print_members.cpp files in ${_ava_print_members_dir}")
  file(MAKE_DIRECTORY "${_ava_print_members_dir}")
  execute_process(
    COMMAND "${PYTHON3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/cmake/scripts/generate_print_members.py"
            "${_ava_ctags_json_out}"
            "${_ava_print_members_dir}"
    RESULT_VARIABLE GEN_RESULT
    OUTPUT_VARIABLE GEN_OUTPUT
    ERROR_VARIABLE GEN_ERROR
  )

  if (GEN_OUTPUT)
    message(STATUS "${GEN_OUTPUT}")
  endif ()
  if (GEN_ERROR)
    message(STATUS "${GEN_ERROR}")
  endif ()

  if (NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to successfully generate print_members.cpp files.")
  endif ()
endif ()

#------------------------------------------------------------------------------
# Construct a list with all generated source files.
#
# Watch source_files for content changes so that when a build-time regeneration
# adds or removes a print_members.cpp (because a class toggled opt-in/opt-out),
# the next build re-runs cmake configure and _ava_generated_source_files is
# adjusted accordingly. Without this, the configure-time list would go stale
# whenever the set of generated files changes during a build.
set(_ava_generated_source_files "")
file(STRINGS "${_ava_source_list_file}" REL_FILES)
foreach(REL_FILE IN LISTS REL_FILES)
  list(APPEND _ava_generated_source_files "${_ava_print_members_dir}/${REL_FILE}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ava_source_list_file}")

#------------------------------------------------------------------------------
# Build-time generation rule for the print_members.cpp files.
#
# The edge depends on the stamp (NOT on tags.json directly) for the reason
# documented where the stamp is declared: Ninja reruns this command whenever the
# stamp's current mtime is newer than the RECORDED mtime of these outputs, and
# only `ctags-json` advances the stamp. So a manual `ctags-json` refresh is
# picked up by the next normal build, while a configure-time or
# `generate-print-members`-time regeneration (which already wrote the .cpp) does
# not cause a follow-up build to rerun the generator. The .py script itself is
# also a dependency so editing it forces a regeneration. The generator reads
# tags.json at run time; tags.json is intentionally NOT listed here so that a
# configure-time refresh of tags.json cannot trigger this edge.
#
# The guard handles the degenerate case where no type opted into print_members:
# add_custom_command requires at least one OUTPUT, and there is nothing to drive.
if (_ava_generated_source_files)
add_custom_command(
  OUTPUT ${_ava_generated_source_files}
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ava_print_members_dir}"
  COMMAND "${PYTHON3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/cmake/scripts/generate_print_members.py"
          "${_ava_ctags_json_out}"
          "${_ava_print_members_dir}"
  DEPENDS "${_ava_ctags_json_stamp}"
          "${CMAKE_SOURCE_DIR}/cmake/scripts/generate_print_members.py"
  COMMENT "Generating print_members definitions into ${_ava_print_members_dir}"
  VERBATIM
)

# Manual pseudo-target that forces a full, unconditional refresh of both
# tags.json and the print_members.cpp files.
#
# It runs the two scripts directly as its own COMMANDS rather than depending on
# `ctags-json` or on the .cpp edge. Two reasons:
#
#  - It must ALWAYS run generate_print_members.py, but the .cpp edge is planned
#    from the mtimes at build start, so it would only fire when already stale --
#    a clean-state `generate-print-members` would otherwise be a no-op for the
#    printers. Running the .py as a COMMAND of this always-out-of-date target
#    guarantees it.
#
#  - It must NOT advance the stamp. Depending on `ctags-json` would touch the
#    stamp, and since this target refreshes the .cpp as a side effect (not via
#    the Ninja-tracked .cpp edge), Ninja's recorded output mtime for the .cpp
#    would stay behind the stamp, making the very next build rerun the generator
#    (the "python runs again" symptom). Running the scripts directly leaves the
#    stamp -- and thus the .cpp edge -- untouched, so the next plain build only
#    recompiles the freshly-written .cpp without regenerating them.
#
# The .sh runs to completion before the .py starts (COMMANDS of one edge run
# sequentially), so the .py always reads a fully-written tags.json.
add_custom_target(generate-print-members
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_ava_print_members_dir}"
  COMMAND "${CMAKE_SOURCE_DIR}/cmake/scripts/generate_ctags_json.sh"
          "${_ava_ctags_json_out}"
          "${CTAGS_EXECUTABLE}"
  COMMAND "${PYTHON3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/cmake/scripts/generate_print_members.py"
          "${_ava_ctags_json_out}"
          "${_ava_print_members_dir}"
  DEPENDS "${CMAKE_SOURCE_DIR}/cmake/scripts/generate_ctags_json.sh"
          "${CMAKE_SOURCE_DIR}/cmake/scripts/list_ava_sources.sh"
          "${CMAKE_SOURCE_DIR}/cmake/scripts/generate_print_members.py"
  COMMENT "Forcing regeneration of tags.json and print_members.cpp files"
  VERBATIM
)
endif ()

# Tell CMake that these files are generated so IDEs treat them nicely.
set_source_files_properties(${_ava_generated_source_files} PROPERTIES GENERATED TRUE)

#------------------------------------------------------------------------------
# Combine everything into a library.
#
add_library(ava_debug
  "debug_channels.cpp"
  "maxlen.cpp"
  "debug_ostream_operators.cpp"
  "libcwd_output_sink.cpp"
  "${CMAKE_SOURCE_DIR}/src/ava/tui/terminal/print_members.cpp"  # Manually written print_members / print_on methods.
  ${_ava_generated_source_files}
)

target_include_directories(ava_debug
  PUBLIC
    "${CMAKE_BINARY_DIR}/src"   # Needed for ava/
    "${CMAKE_SOURCE_DIR}/src"
)

# The generated print_members.cpp files #include headers from ava_terminal
# (e.g. ComplexChar.h, which pulls in GraphemeCluster.h). GraphemeCluster and
# Border use AVA_PRINT_ON_MEMBERS — a hand-written print_members — so the
# generated .o files reference but do not define their print_members.
# Other dependencies might exist if, for example, a generated print_members
# uses a `to_string`. These should be added when encountered.
target_link_libraries(ava_debug
  PUBLIC
    AVA::agent          # ava::agent::to_string
    AVA::app            # ava::app::runtime::to_string
    AVA::lsp            # ava::lsp::to_string
    AVA::tui            # ava::tui::to_string
)

ava_apply_debugging(ava_debug)
ava_apply_warnings(ava_debug)
ava_apply_sanitizers(ava_debug)

# Create an ALIAS target.
add_library(AVA::debug ALIAS ava_debug)
