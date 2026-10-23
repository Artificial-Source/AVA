add_library(ava_observability "run_observer.cpp")
target_include_directories(ava_observability PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(ava_observability
  PUBLIC
    AVA::core
)
ava_apply_debugging(ava_observability)
ava_apply_warnings(ava_observability)
ava_apply_sanitizers(ava_observability)

# Create an ALIAS target.
add_library(AVA::observability ALIAS ava_observability)
