cmake_minimum_required(VERSION 3.21)

project(cpress
    VERSION 1.0.0
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
    # /wd5030: an attribute for other compilers, such as [[gnu::noinline]],
    # is ignored, as the standard says, without a warning. GCC and Clang
    # still warn about one that none of them knows.
    target_compile_options(cpress_warnings INTERFACE /W4 /permissive- /utf-8 /wd5030)
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
    # A build type that does not optimize (Debug, or none) is raised to -O1.
    # At -O0 the stage 5 tests run sixty to seventy times slower than in a
    # release build, and the suite is far over its budget of five minutes;
    # at -O1 the asserts of a Debug build are still on. (Stage 5 brief, test
    # 23.) The optimized build types keep their own level, so a Release
    # build is checked at the level it ships at.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
        -fno-omit-frame-pointer
        $<$<NOT:$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:-O1>)
    add_link_options(-fsanitize=address,undefined)
endif()

# Core library: container format, codec registry, checksum, file I/O, and
# the memory behind the models' large tables.
add_library(cpress_core STATIC
    src/codecs.cpp
    src/container.cpp
    src/crc32.cpp
    src/io.cpp
    src/stream.cpp
    src/table_memory.cpp
    src/text/word_transform.cpp)
target_include_directories(cpress_core PUBLIC include PRIVATE src)
target_link_libraries(cpress_core PRIVATE cpress_warnings)

# Command-line tool.
add_executable(cpress src/cli/main.cpp)
target_compile_definitions(cpress PRIVATE CPRESS_VERSION="${PROJECT_VERSION}")
target_link_libraries(cpress PRIVATE cpress_core cpress_warnings)

# Benchmark runner.
add_executable(cpress-bench bench/bench.cpp bench/platform.cpp)
# src/ for model/table_allocator.hpp, how much memory went on large pages, and
# for message_file.hpp's window and file source.
target_include_directories(cpress-bench PRIVATE src)
target_link_libraries(cpress-bench PRIVATE cpress_core cpress_warnings)
if(WIN32)
    target_link_libraries(cpress-bench PRIVATE psapi)
endif()

# The fair baseline's column splitter (bench/baselines.sh).
add_executable(cpress-columns bench/columns.cpp bench/platform.cpp)
# src/ for itch/schema.hpp, the message layouts, and for message_file.hpp's
# window and file source.
target_include_directories(cpress-columns PRIVATE src)
target_link_libraries(cpress-columns PRIVATE cpress_core cpress_warnings)
if(WIN32)
    target_link_libraries(cpress-columns PRIVATE psapi)
endif()

# Tests.
enable_testing()
add_executable(cpress-tests
    tests/main.cpp
    tests/apm_test.cpp
    tests/coder_test.cpp
    tests/context_table_test.cpp
    tests/counter_test.cpp
    tests/field_coder_test.cpp
    tests/golden_test.cpp
    tests/history_table_test.cpp
    tests/itch_sample.cpp
    tests/itch_schema_test.cpp
    tests/logistic_test.cpp
    tests/match_model_test.cpp
    tests/message_file_test.cpp
    tests/message_model_test.cpp
    tests/mix6_test.cpp
    tests/mix9_test.cpp
    tests/mixer_test.cpp
    tests/order0_test.cpp
    tests/order_table_test.cpp
    tests/probability_map_test.cpp
    tests/residual_test.cpp
    tests/roundtrip.cpp
    tests/state_table_test.cpp
    tests/stream_test.cpp
    tests/table_allocator_test.cpp
    tests/word_transform_test.cpp)
# bench/ for message_file.hpp, the benchmark tools' reader.
target_include_directories(cpress-tests PRIVATE src bench)
target_link_libraries(cpress-tests PRIVATE cpress_core cpress_warnings)
add_test(NAME cpress-tests COMMAND cpress-tests)
