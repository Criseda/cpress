cmake_minimum_required(VERSION 3.21)

project(cpress
    VERSION 0.1.0
    DESCRIPTION "Context-mixing lossless compressor"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CPRESS_WERROR "Treat compiler warnings as errors" OFF)
option(CPRESS_SANITIZE "Build with ASan and UBSan (GCC and Clang only)" OFF)

# For clang-tidy and editors (ignored by the Visual Studio generators).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warning flags shared by every target.
add_library(cpress_warnings INTERFACE)
if(MSVC)
    target_compile_options(cpress_warnings INTERFACE /W4 /permissive- /utf-8)
    if(CPRESS_WERROR)
        target_compile_options(cpress_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(cpress_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(CPRESS_WERROR)
        target_compile_options(cpress_warnings INTERFACE -Werror)
    endif()
endif()

if(CPRESS_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "CPRESS_SANITIZE supports GCC and Clang only")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Core library: container format, codec registry, checksum, file I/O.
add_library(cpress_core STATIC
    src/codecs.cpp
    src/container.cpp
    src/crc32.cpp
    src/io.cpp)
target_include_directories(cpress_core PUBLIC include)
target_link_libraries(cpress_core PRIVATE cpress_warnings)

# Command-line tool.
add_executable(cpress src/cli/main.cpp)
target_compile_definitions(cpress PRIVATE CPRESS_VERSION="${PROJECT_VERSION}")
target_link_libraries(cpress PRIVATE cpress_core cpress_warnings)

# Benchmark runner.
add_executable(cpress-bench bench/bench.cpp bench/platform.cpp)
target_link_libraries(cpress-bench PRIVATE cpress_core cpress_warnings)
if(WIN32)
    target_link_libraries(cpress-bench PRIVATE psapi)
endif()

# Tests.
enable_testing()
add_executable(cpress-tests tests/roundtrip.cpp)
target_link_libraries(cpress-tests PRIVATE cpress_core cpress_warnings)
add_test(NAME roundtrip COMMAND cpress-tests)
