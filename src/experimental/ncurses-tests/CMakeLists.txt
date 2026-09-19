# CURSES_NEED_WIDE was added to 3.10
cmake_minimum_required(VERSION 3.10...4.3.2)

project(ava-ncurses-tests
        VERSION 0.0
        DESCRIPTION "Tests to see if the fundamentals required for a TUI work on all platforms."
        LANGUAGES CXX C
)

set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)

find_library(CURSES_PANELW_LIBRARY
  NAMES panelw
  REQUIRED
)

add_executable(linux02 linux02-screen-resizing.cpp Application.cpp)
target_include_directories(linux02 PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(linux02 PRIVATE AVA::terminal)

add_executable(linux04 linux04-attributes.cpp Application.cpp)
target_include_directories(linux04 PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(linux04 PRIVATE AVA::terminal)

add_executable(linux05 linux05-colors.cpp Application.cpp)
target_link_libraries(linux05 PRIVATE AVA::terminal)

add_executable(linux06 linux06-wrapping.cpp Application.cpp)
target_link_libraries(linux06 PRIVATE AVA::terminal)

add_executable(linux07 linux07-full-grapheme-cluster.cpp Application.cpp)
target_link_libraries(linux07 PRIVATE AVA::terminal)

add_executable(linux08 linux08-pad.cpp lorem_ipsum_paragraphs.cpp Application.cpp)
target_link_libraries(linux08 PRIVATE AVA::terminal)

add_executable(linux09 linux09-layout.cpp Application.cpp)
target_link_libraries(linux09 PRIVATE AVA::terminal)

add_executable(linux10 linux10-composer-area.cpp lorem_ipsum_paragraphs.cpp Application.cpp)
target_link_libraries(linux10 PRIVATE AVA::terminal)
