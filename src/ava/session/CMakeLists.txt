add_library(ava_session
  "assistant_output.cpp"
  "attachments.cpp"
  "compaction.cpp"
  "export.cpp"
  "logical_projection.cpp"
  "portable_sanitization.cpp"
  "record.cpp"
  "session_branch.cpp"
  "session_lease.cpp"
  "session_metadata.cpp"
  "session_store.cpp"
  "session_store_append.cpp"
  "session_store_authority.cpp"
  "session_store_catalog.cpp"
  "session_store_read.cpp"
  "session_store_recovery.cpp"
  "session_store_rollback.cpp"
  "session_tree.cpp"
  "stats.cpp"
  "subagent_job_history.cpp"
  "transcript.cpp"
  "validation.cpp"
  "validation_fields.cpp"
)

target_include_directories(ava_session
  PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(ava_session
  PUBLIC
    AVA::core
    AVA::observability
  PRIVATE
    AVA::diagnostics
)

ava_apply_debugging(ava_session)
ava_apply_warnings(ava_session)
ava_apply_sanitizers(ava_session)

# Create an ALIAS target.
add_library(AVA::session ALIAS ava_session)
