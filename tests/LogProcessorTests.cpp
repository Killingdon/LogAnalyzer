#include "analysis/LogAnalyzer.h"
#include "common/Errors.h"
#include "processing/LogProcessor.h"

#include "TestHelpers.h"

#include <gtest/gtest.h>

#include <random>
#include <sstream>

using namespace loganalyzer;

namespace {

/// Builds a deterministic log with every kind of line: all levels, repeated
/// messages, source locations, out-of-order timestamps, malformed and blank lines.
std::string buildMixedLog(std::size_t lines, std::uint32_t seed) {
    std::mt19937 rng(seed);
    static constexpr const char* kLevels[] = {"TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"};
    static constexpr const char* kMessages[] = {
        "Database connection failed", "Cache miss", "User login successful", "Request timed out",
        "Disk usage high", "Payment declined", "Retrying connection", "Configuration reloaded",
    };
    std::string out;
    for (std::size_t i = 0; i < lines; ++i) {
        const auto r = rng();
        if (r % 97 == 0) {
            out += "corrupted line without structure\n";
            continue;
        }
        if (r % 89 == 0) {
            out += "\n";
            continue;
        }
        const unsigned minute = static_cast<unsigned>((i / 7 + r % 5) % 60);
        const unsigned hour = static_cast<unsigned>((i / 400) % 24);
        out += "[2026-10-20 ";
        out += (hour < 10 ? "0" : "") + std::to_string(hour) + ":" + (minute < 10 ? "0" : "") +
               std::to_string(minute) + ":00] [";
        out += kLevels[(r >> 8) % 6];
        out += "] ";
        if ((r >> 4) % 3 == 0) {
            out += "[Module" + std::to_string((r >> 12) % 4) + ".cpp:" + std::to_string((r >> 16) % 500) + "] ";
        }
        out += kMessages[(r >> 20) % 8];
        out += "\n";
    }
    return out;
}

AnalysisReport analyzeString(const std::string& content, unsigned threads, std::size_t chunkSize,
                             const FilterOptions& filterOptions, const AnalysisOptions& analysisOptions) {
    LogFilter filter = LogFilter::fromOptions(filterOptions);
    auto descriptions = filter.describe();
    const LogProcessor processor(std::move(filter), {.threadCount = threads, .chunkSizeBytes = chunkSize},
                                 analysisOptions);
    const ProcessingResult result = processor.process(std::make_unique<std::istringstream>(content));
    EXPECT_EQ(result.performance.threadCount, threads);
    return LogAnalyzer{analysisOptions}.analyze(result, "generated", std::move(descriptions));
}

} // namespace

/// Core correctness guarantee of the project: for any thread count and any
/// chunk size, the analysis is identical to the single-threaded one.
class ConcurrencyEquivalenceTest : public ::testing::TestWithParam<std::tuple<unsigned, std::size_t>> {};

TEST_P(ConcurrencyEquivalenceTest, MultithreadedMatchesSingleThreaded) {
    const auto [threads, chunkSize] = GetParam();
    const std::string log = buildMixedLog(20'000, 1234);
    const AnalysisOptions analysisOptions{.topCount = 10, .bucket = TimeBucket::Minute, .detectDuplicates = true};

    const AnalysisReport baseline = analyzeString(log, 1, 1 << 20, {}, analysisOptions);
    const AnalysisReport candidate = analyzeString(log, threads, chunkSize, {}, analysisOptions);

    EXPECT_TRUE(haveSameResults(baseline, candidate));
    EXPECT_EQ(candidate.summary, baseline.summary);
    EXPECT_EQ(candidate.topErrors, baseline.topErrors);
    EXPECT_EQ(candidate.duplicates, baseline.duplicates);
    EXPECT_EQ(candidate.mostActivePeriods, baseline.mostActivePeriods);
    EXPECT_EQ(candidate.matchedSamples, baseline.matchedSamples);
    EXPECT_EQ(candidate.invalidSamples, baseline.invalidSamples);
    EXPECT_GT(baseline.summary.invalidEntries, 0u);
    EXPECT_GT(baseline.summary.emptyLines, 0u);
}

INSTANTIATE_TEST_SUITE_P(ThreadsAndChunkSizes, ConcurrencyEquivalenceTest,
                         ::testing::Combine(::testing::Values(1u, 2u, 3u, 4u, 8u, 16u),
                                            ::testing::Values(std::size_t{256}, std::size_t{4096},
                                                              std::size_t{65536})));

TEST(LogProcessorTest, FilteredMultithreadedMatchesSingleThreaded) {
    const std::string log = buildMixedLog(10'000, 99);
    const FilterOptions filter{
        .levels = {LogLevel::Error, LogLevel::Critical},
        .from = util::parseTimestamp("2026-10-20 03:00:00"),
        .to = util::parseTimestamp("2026-10-20 15:59:59"),
        .keywords = {"database", "payment"},
    };
    const AnalysisReport baseline = analyzeString(log, 1, 1 << 20, filter, {});
    const AnalysisReport candidate = analyzeString(log, 8, 512, filter, {});
    EXPECT_TRUE(haveSameResults(baseline, candidate));
    EXPECT_GT(baseline.summary.matchedEntries, 0u);
    EXPECT_LT(baseline.summary.matchedEntries, baseline.summary.validEntries);
}

TEST(LogProcessorTest, ManyChunksAreAllProcessed) {
    const std::string log = buildMixedLog(5'000, 7);
    const LogProcessor processor({}, {.threadCount = 4, .chunkSizeBytes = 128});
    const ProcessingResult result = processor.process(std::make_unique<std::istringstream>(log));
    EXPECT_EQ(result.summary.totalLines, 5'000u);
    EXPECT_GT(result.performance.chunkCount, 100u);
    EXPECT_EQ(result.performance.bytesProcessed, log.size());
}

TEST(LogProcessorTest, EmptyInputInBothModes) {
    for (const unsigned threads : {1u, 4u}) {
        const LogProcessor processor({}, {.threadCount = threads});
        const ProcessingResult result = processor.process(std::make_unique<std::istringstream>(""));
        EXPECT_EQ(result.summary, ProcessingSummary{});
        EXPECT_EQ(result.performance.chunkCount, 0u);
    }
}

TEST(LogProcessorTest, MissingFileThrowsInBothModes) {
    for (const unsigned threads : {1u, 4u}) {
        const LogProcessor processor({}, {.threadCount = threads});
        EXPECT_THROW((void)processor.process(std::filesystem::path("no/such/file.log")), FileError);
    }
}

TEST(LogProcessorTest, AutoThreadCountUsesHardware) {
    const LogProcessor processor;
    EXPECT_GE(processor.effectiveThreadCount(), 1u);
}

TEST(LogProcessorTest, SampleFilesGiveSameResultsInBothModes) {
    for (const char* name : {"application.log", "server.log", "malformed.log"}) {
        const AnalysisOptions options{.detectDuplicates = true};
        const auto run = [&](unsigned threads) {
            const LogProcessor processor({}, {.threadCount = threads, .chunkSizeBytes = 64}, options);
            return LogAnalyzer{options}.analyze(processor.process(test::samplePath(name)), name);
        };
        EXPECT_TRUE(haveSameResults(run(1), run(4))) << name;
    }
}

TEST(LogProcessorTest, AutoModeUsesNoMoreThreadsThanChunks) {
    // application.log is ~5 KB: a single 1 MiB chunk, so auto mode needs 1 thread.
    const ProcessingResult automatic = LogProcessor{}.process(test::samplePath("application.log"));
    EXPECT_EQ(automatic.performance.threadCount, 1u);

    // An explicit thread count is honoured even for a tiny file.
    const LogProcessor explicitFour({}, {.threadCount = 4});
    EXPECT_EQ(explicitFour.process(test::samplePath("application.log")).performance.threadCount, 4u);

    // 64-byte chunks: ~76 chunks, so auto mode uses min(hardware threads, chunks).
    const LogProcessor smallChunks({}, {.threadCount = 0, .chunkSizeBytes = 64});
    const ProcessingResult result = smallChunks.process(test::samplePath("application.log"));
    EXPECT_EQ(result.performance.threadCount,
              std::min<unsigned>(smallChunks.effectiveThreadCount(),
                                 static_cast<unsigned>(result.performance.chunkCount)));
}

namespace {

/// Simulates a bug in user-supplied code running on worker threads.
class ThrowingCriterion final : public FilterCriterion {
public:
    bool matches(const LogEntryView& entry) const override {
        if (entry.message.find("Database connection failed") != std::string_view::npos) {
            throw std::runtime_error("simulated failure in a filter");
        }
        return true;
    }
    std::string describe() const override { return "throws on database errors"; }
};

} // namespace

TEST(LogProcessorTest, WorkerExceptionBecomesProcessingErrorInBothModes) {
    for (const unsigned threads : {1u, 4u}) {
        LogFilter filter;
        filter.add(std::make_unique<ThrowingCriterion>());
        // Small chunks: many tasks are in flight when the failure happens.
        const LogProcessor processor(std::move(filter), {.threadCount = threads, .chunkSizeBytes = 64});
        try {
            (void)processor.process(test::samplePath("application.log"));
            FAIL() << "expected ProcessingError with " << threads << " thread(s)";
        } catch (const ProcessingError& e) {
            EXPECT_NE(std::string(e.what()).find("simulated failure"), std::string::npos);
        }
    }
}

TEST(LogProcessorTest, ProcessorIsReusableAfterAFailure) {
    const LogProcessor processor({}, {.threadCount = 4});
    EXPECT_THROW((void)processor.process(std::filesystem::path("missing.log")), FileError);
    const ProcessingResult result = processor.process(test::samplePath("application.log"));
    EXPECT_EQ(result.summary.totalLines, 80u);
}
