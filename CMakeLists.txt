cmake_minimum_required(VERSION 3.25)

project(LogAnalyzer
    VERSION 1.0.0
    DESCRIPTION "Multithreaded log analysis tool"
    LANGUAGES CXX)

# -----------------------------------------------------------------------------
# Global settings
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)            # -std=c++20, not -std=gnu++20
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)    # compile_commands.json for clangd / IDEs

# Single-config generators (Ninja, Makefiles) default to an optimised build.
# Multi-config generators (Visual Studio) choose the configuration at build time.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOGANALYZER_BUILD_TESTS "Build the GoogleTest unit tests" ON)
option(LOGANALYZER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------
# Warning flags. Kept in an INTERFACE target so they apply to our own code only
# (third-party code such as GoogleTest is compiled with its own settings).
# -----------------------------------------------------------------------------
add_library(loganalyzer_warnings INTERFACE)
if(MSVC)
    target_compile_options(loganalyzer_warnings INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(LOGANALYZER_WARNINGS_AS_ERRORS)
        target_compile_options(loganalyzer_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(loganalyzer_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
        -Wold-style-cast -Woverloaded-virtual)
    if(LOGANALYZER_WARNINGS_AS_ERRORS)
        target_compile_options(loganalyzer_warnings INTERFACE -Werror)
    endif()
    # Clang 19+ warns when a designated initializer omits members, but omitting
    # members that have default initializers is the point of the C++20 feature.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wno-missing-designated-field-initializers
                            LOGANALYZER_HAS_WNO_MISSING_DESIGNATED)
    if(LOGANALYZER_HAS_WNO_MISSING_DESIGNATED)
        target_compile_options(loganalyzer_warnings INTERFACE
            -Wno-missing-designated-field-initializers)
    endif()
endif()

# -----------------------------------------------------------------------------
# Generated version header (values come from project() above)
# -----------------------------------------------------------------------------
set(LOGANALYZER_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated")
configure_file(cmake/Version.h.in "${LOGANALYZER_GENERATED_DIR}/common/Version.h" @ONLY)

# -----------------------------------------------------------------------------
# Core library: all application logic. The executable and the unit tests both
# link against it, so the tests exercise exactly the code that ships.
# -----------------------------------------------------------------------------
add_library(loganalyzer_core STATIC
    src/analysis/LogAnalyzer.cpp
    src/analysis/StatisticsCollector.cpp
    src/benchmarking/BenchmarkRunner.cpp
    src/cli/CommandLineInterface.cpp
    src/cli/CommandLineParser.cpp
    src/filtering/LogFilter.cpp
    src/generation/SampleLogGenerator.cpp
    src/models/LogEntry.cpp
    src/parser/LogParser.cpp
    src/processing/LogProcessor.cpp
    src/reader/LogFileReader.cpp
    src/reporting/CsvReportFormatter.cpp
    src/reporting/JsonReportFormatter.cpp
    src/reporting/JsonWriter.cpp
    src/reporting/ReportGenerator.cpp
    src/reporting/TextReportFormatter.cpp
    src/threading/ThreadPool.cpp
    src/util/StringUtils.cpp
    src/util/TimeUtils.cpp
)

target_include_directories(loganalyzer_core
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
        "${LOGANALYZER_GENERATED_DIR}"
)

target_compile_features(loganalyzer_core PUBLIC cxx_std_20)
target_link_libraries(loganalyzer_core
    PUBLIC  Threads::Threads
    PRIVATE loganalyzer_warnings
)

# -----------------------------------------------------------------------------
# Command-line executable
# -----------------------------------------------------------------------------
add_executable(loganalyzer src/main.cpp)
target_link_libraries(loganalyzer PRIVATE loganalyzer_core loganalyzer_warnings)

# `cmake --install <build-dir> --prefix <dir>` copies the executable to <dir>/bin.
install(TARGETS loganalyzer RUNTIME DESTINATION bin)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
if(LOGANALYZER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
