#include "benchmarking/BenchmarkRunner.h"

#include "analysis/LogAnalyzer.h"
#include "common/Errors.h"
#include "processing/LogProcessor.h"
#include "threading/ThreadPool.h"
#include "util/StringUtils.h"
#include "util/TimeUtils.h"

#include <algorithm>
#include <format>
#include <optional>

namespace loganalyzer {

namespace {

struct Measurement {
    std::chrono::nanoseconds duration;
    AnalysisReport report;
};

std::chrono::nanoseconds median(std::vector<std::chrono::nanoseconds> values) {
    std::sort(values.begin(), values.end());
    const std::size_t mid = values.size() / 2;
    return values.size() % 2 == 1 ? values[mid] : (values[mid - 1] + values[mid]) / 2;
}

} // namespace

BenchmarkRunner::BenchmarkRunner(BenchmarkOptions options) : options_(std::move(options)) {
    if (options_.iterations == 0) {
        throw ConfigurationError("benchmark needs at least one iteration");
    }
    if (std::any_of(options_.threadCounts.begin(), options_.threadCounts.end(), [](unsigned t) { return t == 0; })) {
        throw ConfigurationError("benchmark thread counts must be at least 1");
    }
    // The single-threaded run is the baseline for speedup: make sure it exists,
    // then sort and remove duplicates.
    options_.threadCounts.push_back(1);
    std::sort(options_.threadCounts.begin(), options_.threadCounts.end());
    options_.threadCounts.erase(std::unique(options_.threadCounts.begin(), options_.threadCounts.end()),
                                options_.threadCounts.end());
    // Validate the filter once, up front, rather than in the middle of a run.
    (void)LogFilter::fromOptions(options_.filter);
}

BenchmarkReport BenchmarkRunner::run(const std::filesystem::path& input, const ProgressCallback& progress) const {
    const auto measure = [&](unsigned threads) {
        // Every run gets a fresh filter and processor: identical configuration
        // except for the thread count.
        LogFilter filter = LogFilter::fromOptions(options_.filter);
        auto descriptions = filter.describe();
        const LogProcessor processor(
            std::move(filter), {.threadCount = threads, .chunkSizeBytes = options_.chunkSizeBytes}, options_.analysis);
        const ProcessingResult result = processor.process(input);
        return Measurement{result.performance.duration,
                           LogAnalyzer(options_.analysis).analyze(result, input.string(), std::move(descriptions))};
    };
    const auto report = [&](const std::string& message) {
        if (progress) {
            progress(message);
        }
    };

    if (options_.warmup) {
        report("warm-up run (not timed)");
        (void)measure(1);
    }

    BenchmarkReport result;
    result.inputName = input.string();
    result.hardwareThreads = static_cast<unsigned>(ThreadPool::hardwareThreads());
    result.iterations = options_.iterations;
    result.chunkSizeBytes = options_.chunkSizeBytes;

    std::optional<AnalysisReport> baseline;
    for (const unsigned threads : options_.threadCounts) {
        BenchmarkRun run;
        run.threads = threads;
        for (unsigned i = 1; i <= options_.iterations; ++i) {
            report(std::format("{} thread(s), iteration {}/{}", threads, i, options_.iterations));
            Measurement m = measure(threads);
            run.samples.push_back(m.duration);
            if (!baseline) {
                // First run ever (threads == 1): it defines the expected results.
                result.totalLines = m.report.summary.totalLines;
                result.bytes = m.report.performance.bytesProcessed;
                baseline = std::move(m.report);
            } else if (!haveSameResults(*baseline, m.report)) {
                run.matchesBaseline = false;
                result.allResultsMatch = false;
            }
        }
        run.median = median(run.samples);
        run.best = *std::min_element(run.samples.begin(), run.samples.end());
        const double seconds = std::chrono::duration<double>(run.median).count();
        run.linesPerSecond = seconds > 0.0 ? static_cast<double>(result.totalLines) / seconds : 0.0;
        result.runs.push_back(std::move(run));
    }

    const double baselineSeconds = std::chrono::duration<double>(result.runs.front().median).count();
    for (BenchmarkRun& run : result.runs) {
        const double seconds = std::chrono::duration<double>(run.median).count();
        run.speedup = seconds > 0.0 ? baselineSeconds / seconds : 0.0;
        run.efficiency = run.speedup / run.threads;
    }
    return result;
}

void BenchmarkRunner::print(const BenchmarkReport& report, std::ostream& out) {
    const std::string rule(76, '-');
    out << "Benchmark: " << report.inputName << '\n'
        << std::format("  Input        : {} lines, {:.1f} MiB\n", util::formatWithSeparators(report.totalLines),
                       static_cast<double>(report.bytes) / (1024.0 * 1024.0))
        << std::format("  Machine      : {} hardware threads\n", report.hardwareThreads)
        << std::format("  Method       : median of {} timed run(s) per configuration, {} KiB chunks\n",
                       report.iterations, report.chunkSizeBytes / 1024)
        << '\n'
        << std::format("  {:>7}  {:>12}  {:>12}  {:>14}  {:>8}  {:>10}  {}\n", "Threads", "Median", "Best",
                       "Lines/s", "Speedup", "Efficiency", "Results")
        << "  " << rule << '\n';
    for (const BenchmarkRun& run : report.runs) {
        const std::string verdict = run.threads == 1 ? "baseline" : (run.matchesBaseline ? "identical" : "DIFFERENT");
        out << std::format("  {:>7}  {:>12}  {:>12}  {:>14}  {:>7.2f}x  {:>9.1f}%  {}\n", run.threads,
                           util::formatDuration(run.median), util::formatDuration(run.best),
                           util::formatWithSeparators(static_cast<std::uint64_t>(run.linesPerSecond)), run.speedup,
                           run.efficiency * 100.0, verdict);
    }
    out << '\n'
        << (report.allResultsMatch ? "All configurations produced identical analysis results.\n"
                                   : "WARNING: some configurations produced different results!\n");
}

} // namespace loganalyzer
