#pragma once

#include "analysis/StatisticsCollector.h"
#include "filtering/LogFilter.h"
#include "reader/LogFileReader.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace loganalyzer {

struct BenchmarkOptions {
    std::vector<unsigned> threadCounts{1, 2, 4, 8}; ///< 1 (the baseline) is always added
    unsigned iterations = 3;                        ///< timed runs per configuration
    bool warmup = true;                             ///< one untimed run first (warms the OS file cache)
    std::size_t chunkSizeBytes = LogFileReader::kDefaultChunkSize;
    FilterOptions filter;
    AnalysisOptions analysis;
};

/// Measurements for one thread count.
struct BenchmarkRun {
    unsigned threads = 1;
    std::vector<std::chrono::nanoseconds> samples; ///< one per iteration
    std::chrono::nanoseconds median{0};
    std::chrono::nanoseconds best{0};
    double linesPerSecond = 0.0; ///< based on the median
    double speedup = 1.0;        ///< baseline median / this median
    double efficiency = 1.0;     ///< speedup / threads
    bool matchesBaseline = true; ///< every iteration produced the baseline's analysis
};

struct BenchmarkReport {
    std::string inputName;
    std::uint64_t totalLines = 0;
    std::uint64_t bytes = 0;
    unsigned hardwareThreads = 1;
    unsigned iterations = 0;
    std::size_t chunkSizeBytes = 0;
    std::vector<BenchmarkRun> runs; ///< sorted by thread count; runs[0] is the baseline
    bool allResultsMatch = true;
};

/// Measures the processing pipeline with different thread counts on the same
/// input and verifies that every configuration produces the same analysis.
///
/// The median of several iterations is reported because it is robust against
/// outliers (a background process, a cold cache) that would skew a mean.
class BenchmarkRunner {
public:
    using ProgressCallback = std::function<void(const std::string&)>;

    /// Throws ConfigurationError for invalid options.
    explicit BenchmarkRunner(BenchmarkOptions options);

    [[nodiscard]] BenchmarkReport run(const std::filesystem::path& input,
                                      const ProgressCallback& progress = {}) const;

    /// Prints the report as a table.
    static void print(const BenchmarkReport& report, std::ostream& out);

    [[nodiscard]] const BenchmarkOptions& options() const noexcept { return options_; }

private:
    BenchmarkOptions options_;
};

} // namespace loganalyzer
