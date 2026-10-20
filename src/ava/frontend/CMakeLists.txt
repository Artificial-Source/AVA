add_library(ava_frontend
  "Data.cxx"
  "Directories.cxx"
  "FeedbackKey.cxx"
  "FrontEnd.cxx"
  "GitBranch.cxx"
  "ModelSelection.cxx"
  "PromptMetadata.cxx"
  "SessionStats.cxx"
)

target_link_libraries(ava_frontend
  PUBLIC
    AVA::core
)

ava_apply_debugging(ava_frontend)
ava_apply_warnings(ava_frontend)
ava_apply_sanitizers(ava_frontend)

# Create an ALIAS target.
add_library(AVA::frontend ALIAS ava_frontend)
