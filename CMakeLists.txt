cmake_minimum_required(VERSION 3.16)
project(lsmstore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -O2)

add_executable(lsmstore
    src/main.cpp
    src/wal.cpp
    src/sstable.cpp
    src/bloomfilter.cpp
    src/compaction.cpp
)

target_include_directories(lsmstore PRIVATE include)

# --- Tests (optional, requires GoogleTest installed) ---
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    find_package(GTest REQUIRED)
    add_executable(skiplist_test tests/skiplist_test.cpp)
    target_include_directories(skiplist_test PRIVATE include)
    target_link_libraries(skiplist_test GTest::gtest_main)
endif()
