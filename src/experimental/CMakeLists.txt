# Compile all subdirectories with the same standard.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(ncurses-tests)
add_subdirectory(json-tests)
add_subdirectory(signals-tests)
