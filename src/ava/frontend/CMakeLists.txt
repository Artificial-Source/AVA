add_library(ava_frontend
  "AgentSelection.cxx"
  "Data.cxx"
  "Directories.cxx"
  "FeedbackKey.cxx"
  "FrontEnd.cxx"
  "GitBranch.cxx"
  "ModelSelection.cxx"
  "PromptModeAndContextSources.cxx"
  "SessionIdentity.cxx"
  "SessionStats.cxx"
)

target_link_libraries(ava_frontend
  PUBLIC
    AVA::core
  PRIVATE
    # Model projections format committed model and reasoning selections using config helpers.
    AVA::config
)

ava_apply_debugging(ava_frontend)
ava_apply_warnings(ava_frontend)
ava_apply_sanitizers(ava_frontend)

# Create an ALIAS target.
add_library(AVA::frontend ALIAS ava_frontend)
