include(FetchContent)
include(GoogleTest)

# GoogleTest: use an installed copy if one is available (FIND_PACKAGE_ARGS),
# otherwise download a pinned, hash-verified release.
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    URL_HASH SHA256=f179ec217f9b3b3f3c6e8b02d3e7eda997b49e4ce26d6b235c9053bec9c0bf9f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM  # treat GoogleTest headers as system headers: no warnings from them
    FIND_PACKAGE_ARGS NAMES GTest
)

# MSVC: link GoogleTest against the same (dynamic) C runtime as our code.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(loganalyzer_tests
    TestHelpers.h
    BenchmarkAndGeneratorTests.cpp
    CommandLineTests.cpp
    LogAnalyzerTests.cpp
    LogFileReaderTests.cpp
    LogFilterTests.cpp
    LogParserTests.cpp
    LogProcessorTests.cpp
    ReportGeneratorTests.cpp
    StringUtilsTests.cpp
    ThreadPoolTests.cpp
)

target_link_libraries(loganalyzer_tests
    PRIVATE
        loganalyzer_core
        loganalyzer_warnings
        GTest::gtest_main
)

# Lets tests locate the sample logs regardless of the working directory.
target_compile_definitions(loganalyzer_tests
    PRIVATE LOGANALYZER_SAMPLES_DIR="${PROJECT_SOURCE_DIR}/samples")

# Registers every TEST() with CTest so `ctest` runs them individually.
gtest_discover_tests(loganalyzer_tests DISCOVERY_MODE PRE_TEST)
