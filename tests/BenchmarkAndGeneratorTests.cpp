#include "benchmarking/BenchmarkRunner.h"
#include "common/Errors.h"
#include "generation/SampleLogGenerator.h"
#include "parser/LogParser.h"
#include "processing/LogProcessor.h"

#include "TestHelpers.h"

#include <gtest/gtest.h>

#include <sstream>

using namespace loganalyzer;

// ---------------------------------------------------------------------------
// SampleLogGenerator
// ---------------------------------------------------------------------------

namespace {

std::string generate(const GeneratorOptions& options) {
    std::ostringstream out;
    SampleLogGenerator(options).generate(out);
    return out.str();
}

} // namespace

TEST(SampleLogGeneratorTest, ProducesRequestedNumberOfLines) {
    const std::string log = generate({.lines = 1234});
    EXPECT_EQ(std::count(log.begin(), log.end(), '\n'), 1234);
}

TEST(SampleLogGeneratorTest, SameSeedSameOutputDifferentSeedDifferentOutput) {
    EXPECT_EQ(generate({.lines = 500, .seed = 1}), generate({.lines = 500, .seed = 1}));
    EXPECT_NE(generate({.lines = 500, .seed = 1}), generate({.lines = 500, .seed = 2}));
}

TEST(SampleLogGeneratorTest, ZeroMalformedRateGivesOnlyValidLines) {
    const std::string log = generate({.lines = 2000, .malformedRate = 0.0});
    const LogParser parser;
    std::istringstream in(log);
    std::string line;
    while (std::getline(in, line)) {
        EXPECT_TRUE(parser.parseLine(line).ok()) << line;
    }
}

TEST(SampleLogGeneratorTest, MalformedRateIsRoughlyHonoured) {
    const LogProcessor processor({}, {.threadCount = 1});
    const auto result =
        processor.process(std::make_unique<std::istringstream>(generate({.lines = 20'000, .malformedRate = 0.1})));
    const double rate = static_cast<double>(result.summary.invalidEntries) / 20'000.0;
    EXPECT_GT(rate, 0.08);
    EXPECT_LT(rate, 0.12);
}

TEST(SampleLogGeneratorTest, RejectsInvalidOptions) {
    EXPECT_THROW(SampleLogGenerator({.lines = 0}), ConfigurationError);
    EXPECT_THROW(SampleLogGenerator({.malformedRate = -0.1}), ConfigurationError);
    EXPECT_THROW(SampleLogGenerator({.malformedRate = 1.5}), ConfigurationError);
}

TEST(SampleLogGeneratorTest, WritesFileAndReportsUnwritablePath) {
    const test::TempDirectory dir;
    const auto file = dir.path() / "gen.log";
    SampleLogGenerator({.lines = 10}).generateToFile(file);
    EXPECT_EQ(test::readFile(file), generate({.lines = 10}));
    EXPECT_THROW(SampleLogGenerator({.lines = 10}).generateToFile(dir.path() / "missing" / "x.log"), FileError);
}

// ---------------------------------------------------------------------------
// BenchmarkRunner
// ---------------------------------------------------------------------------

TEST(BenchmarkRunnerTest, AlwaysIncludesSingleThreadedBaseline) {
    const BenchmarkRunner runner({.threadCounts = {4, 2, 4}});
    EXPECT_EQ(runner.options().threadCounts, (std::vector<unsigned>{1, 2, 4})); // sorted, unique, with 1
}

TEST(BenchmarkRunnerTest, RejectsInvalidOptions) {
    EXPECT_THROW(BenchmarkRunner({.iterations = 0}), ConfigurationError);
    EXPECT_THROW(BenchmarkRunner({.threadCounts = {0}}), ConfigurationError);
    FilterOptions inverted;
    inverted.from = util::parseTimestamp("2026-10-21 00:00:00");
    inverted.to = util::parseTimestamp("2026-10-20 00:00:00");
    EXPECT_THROW(BenchmarkRunner({.filter = inverted}), ConfigurationError);
}

TEST(BenchmarkRunnerTest, RunsEveryConfigurationAndResultsMatch) {
    const test::TempDirectory dir;
    const auto file = dir.path() / "bench.log";
    SampleLogGenerator({.lines = 30'000, .seed = 3, .malformedRate = 0.01}).generateToFile(file);

    std::vector<std::string> progress;
    const BenchmarkRunner runner({.threadCounts = {2, 4},
                                  .iterations = 2,
                                  .chunkSizeBytes = 16 * 1024,
                                  .analysis = {.detectDuplicates = true}});
    const BenchmarkReport report = runner.run(file, [&](const std::string& step) { progress.push_back(step); });

    ASSERT_EQ(report.runs.size(), 3u);
    EXPECT_EQ(report.runs[0].threads, 1u);
    EXPECT_DOUBLE_EQ(report.runs[0].speedup, 1.0);
    EXPECT_EQ(report.totalLines, 30'000u);
    EXPECT_TRUE(report.allResultsMatch);
    for (const BenchmarkRun& run : report.runs) {
        EXPECT_EQ(run.samples.size(), 2u);
        EXPECT_TRUE(run.matchesBaseline);
        EXPECT_GT(run.median.count(), 0);
        EXPECT_GT(run.linesPerSecond, 0.0);
        EXPECT_NEAR(run.efficiency, run.speedup / run.threads, 1e-12);
    }
    EXPECT_EQ(progress.size(), 1u + 3u * 2u); // warm-up + 3 configurations x 2 iterations

    std::ostringstream table;
    BenchmarkRunner::print(report, table);
    EXPECT_NE(table.str().find("baseline"), std::string::npos);
    EXPECT_NE(table.str().find("identical"), std::string::npos);
    EXPECT_NE(table.str().find("All configurations produced identical analysis results"), std::string::npos);
}

TEST(BenchmarkRunnerTest, MissingFileThrows) {
    const BenchmarkRunner runner({.iterations = 1, .warmup = false});
    EXPECT_THROW((void)runner.run("no/such/file.log"), FileError);
}
