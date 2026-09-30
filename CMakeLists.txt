cmake_minimum_required(VERSION 3.22)

project(OptiTrade VERSION 2.0.0 LANGUAGES CXX)

option(OPTITRADE_BUILD_TESTS "Build unit and integration tests" ON)
option(OPTITRADE_BUILD_APPS "Build command line tools and benchmarks" ON)
option(OPTITRADE_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)
set(OPTITRADE_SANITIZE "" CACHE STRING
    "Sanitizers to enable, e.g. \"address,undefined\" or \"thread\" (empty = none)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(OPTITRADE_SANITIZE)
    add_compile_options(-fsanitize=${OPTITRADE_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${OPTITRADE_SANITIZE})
endif()

find_package(Threads REQUIRED)

# The engine is header-only.
add_library(optitrade INTERFACE)
add_library(optitrade::optitrade ALIAS optitrade)
target_include_directories(optitrade INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(optitrade INTERFACE cxx_std_20)
target_link_libraries(optitrade INTERFACE Threads::Threads)

function(optitrade_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Werror)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # GCC 12+ reports -Warray-bounds on paths it cannot prove unreachable: the
        # decoders bounds-check every read at run time, but the tests hand them
        # constant-size buffers of every length, including deliberately short ones.
        # Real out-of-bounds accesses are caught by the ASan/UBSan test runs instead.
        target_compile_options(${target} PRIVATE -Wno-array-bounds)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Tests: every tests/test_*.cpp is its own executable and its own ctest entry.
# Assertions stay enabled in Release builds.
# ---------------------------------------------------------------------------
if(OPTITRADE_BUILD_TESTS)
    enable_testing()
    file(GLOB OPTITRADE_TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/test_*.cpp)
    foreach(src ${OPTITRADE_TEST_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE optitrade::optitrade)
        target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/fuzz)
        target_compile_definitions(${name} PRIVATE
            OPTITRADE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
        target_compile_options(${name} PRIVATE -UNDEBUG)
        optitrade_warnings(${name})
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Tools and benchmarks: every apps/ot_*.cpp becomes an executable of that name.
# ---------------------------------------------------------------------------
if(OPTITRADE_BUILD_APPS)
    file(GLOB OPTITRADE_APP_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/apps/ot_*.cpp)
    foreach(src ${OPTITRADE_APP_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE optitrade::optitrade)
        target_compile_options(${name} PRIVATE -O3)
        optitrade_warnings(${name})
    endforeach()
endif()

# ---------------------------------------------------------------------------
# libFuzzer targets: every fuzz/fuzz_*.cpp.
# ---------------------------------------------------------------------------
if(OPTITRADE_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "OPTITRADE_BUILD_FUZZERS requires clang (libFuzzer).")
    endif()
    file(GLOB OPTITRADE_FUZZ_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/fuzz/fuzz_*.cpp)
    foreach(src ${OPTITRADE_FUZZ_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE optitrade::optitrade)
        target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/fuzz)
        target_compile_options(${name} PRIVATE
            -fsanitize=fuzzer-no-link,address,undefined -UNDEBUG -O1 -g)
        target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
endif()
