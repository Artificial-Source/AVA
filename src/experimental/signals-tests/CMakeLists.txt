cmake_minimum_required(VERSION 3.10...4.4.2)

project(ava-signal-tests
        VERSION 0.0
        DESCRIPTION "The test from utils-testsuite that tests the Signals class."
        LANGUAGES CXX C
)

add_executable(signals00 signals00-basic.cpp)
target_link_libraries(signals00 PRIVATE ${AICXX_OBJECTS_LIST})
