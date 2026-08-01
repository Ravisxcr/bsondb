cmake_minimum_required(VERSION 3.15)
project(custom_bson_engine C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# This target builds only the Python-agnostic BSON engine (no Python.h
# dependency) as a standalone static library -- a smoke check that
# src/c_engine/bson_engine.c compiles cleanly outside the Python
# extension pipeline, and the future home of the CMocka/Unity C test
# suite. It is NOT invoked by `pip install -e .`; that path uses
# setup.py's setuptools Extension() directly, which lists its own
# sources and does not require cmake to be installed.
add_library(bson_engine_static STATIC
    src/c_engine/bson_engine.c
    src/c_engine/platform_mmap.c
    src/c_engine/storage.c
    src/c_engine/scanner.c
    src/c_engine/btree.c
)
target_include_directories(bson_engine_static PUBLIC include)

if(MSVC)
    target_compile_options(bson_engine_static PRIVATE /W4)
else()
    target_compile_options(bson_engine_static PRIVATE -Wall -Wextra)
endif()

# Plain assert()-based smoke test executables (no CMocka/Unity wiring --
# consistent with the rest of this project's C test convention). Not
# part of `pip install -e .`.
option(BUILD_TESTING "Build the native C smoke test executables" OFF)
if(BUILD_TESTING)
    enable_testing()

    add_executable(test_mmap tests/c_tests/test_mmap.c)
    target_link_libraries(test_mmap PRIVATE bson_engine_static)
    add_test(NAME test_mmap COMMAND test_mmap)

    add_executable(test_storage tests/c_tests/test_storage.c)
    target_link_libraries(test_storage PRIVATE bson_engine_static)
    add_test(NAME test_storage COMMAND test_storage)

    add_executable(test_btree tests/c_tests/test_btree.c)
    target_link_libraries(test_btree PRIVATE bson_engine_static)
    add_test(NAME test_btree COMMAND test_btree)
endif()
