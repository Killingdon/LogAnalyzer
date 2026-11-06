#include "analysis/LogAnalyzer.h"
#include "analysis/StatisticsCollector.h"
#include "parser/LogParser.h"
#include "processing/LogProcessor.h"

#include "TestHelpers.h"

#include <gtest/gtest.h>

using namespace loganalyzer;

namespace {

Timestamp ts(std::string_view text) {
    return util::parseTimestamp(text).value();
}

/// Parses the given lines (numbered from 1) into owning entries.
std::vector<LogEntry> parseAll(const std::vector<std::string>& lines) {
    const LogParser parser;
    std::vector<LogEntry> entries;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        auto entry = parser.parse(lines[i], i + 1);
        EXPECT_TRUE(entry.has_value()) << lines[i];
        if (entry) {
            entries.push_back(std::move(*entry));
        }
    }
    return entries;
}

const std::vector<std::string> kLines = {
    "[2026-10-20 10:00:00] [INFO] Application started",
    "[2026-10-20 10:05:00] [ERROR] Database connection failed",
    "[2026-10-20 10:06:00] [DEBUG] Retrying connection",
    "[2026-10-20 10:07:00] [ERROR] Database connection failed",
    "[2026-10-20 11:00:00] [WARNING] High memory usage detected",
    "[2026-10-20 11:30:00] [CRITICAL] Disk full",
    "[2026-10-20 11:31:00] [ERROR] Database connection failed",
    "[2026-10-20 11:45:00] [INFO] Application started",
    "[2026-10-20 09:00:00] [TRACE] Out-of-order early entry",
    "[2026-10-20 12:00:00] [ERROR] Payment timeout",
};

ProcessingResult resultFor(const std::vector<LogEntry>& entries, const AnalysisOptions& options) {
    ProcessingResult result{.statistics = StatisticsCollector(options)};
    for (const auto& entry : entries) {
        result.statistics.add(entry);
    }
    result.summary.totalLines = result.summary.validEntries = result.summary.matchedEntries = entries.size();
    return result;
}

} // namespace

TEST(StatisticsCollectorTest, CountsLevelsAndTimeRange) {
    StatisticsCollector stats;
    for (const auto& entry : parseAll(kLines)) {
        stats.add(entry);
    }
    EXPECT_EQ(stats.entryCount(), 10u);
    EXPECT_EQ(stats.count(LogLevel::Trace), 1u);
    EXPECT_EQ(stats.count(LogLevel::Debug), 1u);
    EXPECT_EQ(stats.count(LogLevel::Info), 2u);
    EXPECT_EQ(stats.count(LogLevel::Warning), 1u);
    EXPECT_EQ(stats.count(LogLevel::Error), 4u);
    EXPECT_EQ(stats.count(LogLevel::Critical), 1u);
    // Earliest is by time, not file position: line 9 is out of order.
    EXPECT_EQ(stats.earliest(), ts("2026-10-20 09:00:00"));
    EXPECT_EQ(stats.latest(), ts("2026-10-20 12:00:00"));
}

TEST(StatisticsCollectorTest, TracksErrorMessagesWithOccurrences) {
    StatisticsCollector stats;
    for (const auto& entry : parseAll(kLines)) {
        stats.add(entry);
    }
    const auto& errors = stats.errorMessages();
    ASSERT_EQ(errors.size(), 3u); // "Database...", "Disk full", "Payment timeout"
    const MessageStats& db = errors.find("Database connection failed")->second;
    EXPECT_EQ(db.count, 3u);
    EXPECT_EQ(db.first, (Occurrence{2, ts("2026-10-20 10:05:00")}));
    EXPECT_EQ(db.last, (Occurrence{7, ts("2026-10-20 11:31:00")}));
    EXPECT_TRUE(stats.allMessages().empty()); // duplicate detection is off
}

TEST(StatisticsCollectorTest, MergeEqualsSequentialForEverySplitPoint) {
    const auto entries = parseAll(kLines);
    const AnalysisOptions options{.detectDuplicates = true};

    StatisticsCollector sequential(options);
    for (const auto& e : entries) {
        sequential.add(e);
    }

    for (std::size_t split = 0; split <= entries.size(); ++split) {
        StatisticsCollector left(options);
        StatisticsCollector right(options);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            (i < split ? left : right).add(entries[i]);
        }
        // Merge in the "wrong" order on purpose: merging must be commutative.
        right.merge(std::move(left));
        EXPECT_EQ(right.entryCount(), sequential.entryCount());
        EXPECT_EQ(right.levelCounts(), sequential.levelCounts());
        EXPECT_EQ(right.earliest(), sequential.earliest());
        EXPECT_EQ(right.latest(), sequential.latest());
        EXPECT_EQ(right.errorMessages(), sequential.errorMessages());
        EXPECT_EQ(right.allMessages(), sequential.allMessages());
        EXPECT_EQ(right.activity(), sequential.activity());
        EXPECT_EQ(left.entryCount(), 0u); // source is emptied
    }
}

TEST(StatisticsCollectorTest, BucketStartTruncates) {
    const Timestamp t = ts("2026-10-20 14:37:12");
    EXPECT_EQ(bucketStart(t, TimeBucket::Minute), ts("2026-10-20 14:37:00"));
    EXPECT_EQ(bucketStart(t, TimeBucket::Hour), ts("2026-10-20 14:00:00"));
    EXPECT_EQ(bucketStart(t, TimeBucket::Day), ts("2026-10-20 00:00:00"));
    EXPECT_EQ(parseTimeBucket("HOUR"), TimeBucket::Hour);
    EXPECT_FALSE(parseTimeBucket("week"));
}

TEST(LogAnalyzerTest, ComputesDistributionAndPercentages) {
    const auto result = resultFor(parseAll(kLines), {});
    const AnalysisReport report = LogAnalyzer{}.analyze(result, "test.log");

    ASSERT_EQ(report.levelDistribution.size(), kLogLevelCount);
    EXPECT_EQ(report.levelDistribution[toIndex(LogLevel::Error)].count, 4u);
    EXPECT_DOUBLE_EQ(report.levelDistribution[toIndex(LogLevel::Error)].percentage, 40.0);
    EXPECT_EQ(report.errorCount, 5u); // 4 ERROR + 1 CRITICAL
    EXPECT_DOUBLE_EQ(report.errorPercentage, 50.0);
    EXPECT_EQ(report.inputName, "test.log");
}

TEST(LogAnalyzerTest, TopErrorsSortedByFrequencyThenFirstOccurrence) {
    const auto result = resultFor(parseAll(kLines), {});
    const AnalysisReport report = LogAnalyzer{}.analyze(result, "x");
    ASSERT_EQ(report.topErrors.size(), 3u);
    EXPECT_EQ(report.topErrors[0].message, "Database connection failed");
    EXPECT_EQ(report.topErrors[0].stats.count, 3u);
    // "Disk full" (line 6) and "Payment timeout" (line 10) tie at 1: earlier first.
    EXPECT_EQ(report.topErrors[1].message, "Disk full");
    EXPECT_EQ(report.topErrors[2].message, "Payment timeout");
}

TEST(LogAnalyzerTest, TopCountLimitsLists) {
    const AnalysisOptions options{.topCount = 1};
    const auto result = resultFor(parseAll(kLines), options);
    const AnalysisReport report = LogAnalyzer{options}.analyze(result, "x");
    EXPECT_EQ(report.topErrors.size(), 1u);
    EXPECT_EQ(report.mostActivePeriods.size(), 1u);
    EXPECT_EQ(report.distinctErrorMessages, 3u);
}

TEST(LogAnalyzerTest, MostActivePeriods) {
    const auto result = resultFor(parseAll(kLines), {});
    const AnalysisReport report = LogAnalyzer{}.analyze(result, "x");
    ASSERT_EQ(report.mostActivePeriods.size(), 4u); // hours 09, 10, 11, 12
    EXPECT_EQ(report.mostActivePeriods[0], (ActivityPeriod{ts("2026-10-20 10:00:00"), 4}));
    EXPECT_EQ(report.mostActivePeriods[1], (ActivityPeriod{ts("2026-10-20 11:00:00"), 4}));
    EXPECT_EQ(report.mostActivePeriods[2].count, 1u);
}

TEST(LogAnalyzerTest, DuplicateDetectionIgnoresTimestamps) {
    const AnalysisOptions options{.detectDuplicates = true};
    const auto result = resultFor(parseAll(kLines), options);
    const AnalysisReport report = LogAnalyzer{options}.analyze(result, "x");

    ASSERT_TRUE(report.duplicatesEnabled);
    EXPECT_EQ(report.duplicateMessageCount, 2u);
    ASSERT_EQ(report.duplicates.size(), 2u);
    EXPECT_EQ(report.duplicates[0].message, "Database connection failed");
    EXPECT_EQ(report.duplicates[0].stats.count, 3u);
    EXPECT_EQ(report.duplicates[1].message, "Application started");
    EXPECT_EQ(report.duplicates[1].stats.count, 2u);
    EXPECT_EQ(report.duplicates[1].stats.first, (Occurrence{1, ts("2026-10-20 10:00:00")}));
    EXPECT_EQ(report.duplicates[1].stats.last, (Occurrence{8, ts("2026-10-20 11:45:00")}));
}

TEST(LogAnalyzerTest, EmptyInputProducesZeroesNotNaN) {
    const ProcessingResult empty;
    const AnalysisReport report = LogAnalyzer{}.analyze(empty, "empty.log");
    EXPECT_EQ(report.errorPercentage, 0.0);
    EXPECT_FALSE(report.firstTimestamp.has_value());
    EXPECT_TRUE(report.topErrors.empty());
    EXPECT_TRUE(report.mostActivePeriods.empty());
    for (const auto& level : report.levelDistribution) {
        EXPECT_EQ(level.percentage, 0.0);
    }
}

TEST(LogAnalyzerTest, EndToEndOnSampleFile) {
    const AnalysisOptions options{.topCount = 3, .detectDuplicates = true};
    const LogProcessor processor({}, {}, options);
    const ProcessingResult result = processor.process(test::samplePath("application.log"));
    const AnalysisReport report = LogAnalyzer{options}.analyze(result, "application.log");

    EXPECT_EQ(report.summary.totalLines, 80u);
    EXPECT_EQ(report.errorCount, 12u);
    EXPECT_EQ(report.firstTimestamp, ts("2026-10-20 08:00:01"));
    EXPECT_EQ(report.lastTimestamp, ts("2026-10-20 18:00:00"));
    ASSERT_FALSE(report.topErrors.empty());
    EXPECT_EQ(report.topErrors[0].message, "Database connection failed");
    EXPECT_EQ(report.topErrors[0].stats.count, 4u);
    ASSERT_FALSE(report.duplicates.empty());
    EXPECT_EQ(report.duplicates[0].message, "Health check passed");
}
