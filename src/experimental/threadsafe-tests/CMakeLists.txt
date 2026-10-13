cmake_minimum_required(VERSION 3.10...4.4.2)

project(ava-threadsafe-tests
        VERSION 0.0
        DESCRIPTION "The test from threadsafe-testsuite."
        LANGUAGES CXX C
)

add_executable(threadsafe00 threadsafe00-test.cpp)
target_link_libraries(threadsafe00 PRIVATE ${AICXX_OBJECTS_LIST})
