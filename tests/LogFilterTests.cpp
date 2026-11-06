#include "common/Errors.h"
#include "filtering/LogFilter.h"
#include "processing/LogProcessor.h"
#include "util/TimeUtils.h"

#include "TestHelpers.h"

#include <gtest/gtest.h>

using namespace loganalyzer;

namespace {

Timestamp ts(std::string_view text) {
    return util::parseTimestamp(text).value();
}

LogEntry makeEntry(LogLevel level, std::string_view time, std::string message) {
    LogEntry entry;
    entry.level = level;
    entry.timestamp = ts(time);
    entry.message = std::move(message);
    return entry;
}

class LogFilterTest : public ::testing::Test {
protected:
    const LogEntry info = makeEntry(LogLevel::Info, "2026-10-20 09:00:00", "Application started");
    const LogEntry warning = makeEntry(LogLevel::Warning, "2026-10-20 12:00:00", "High memory usage detected");
    const LogEntry error = makeEntry(LogLevel::Error, "2026-10-20 14:30:30", "Database connection failed");
    const LogEntry critical = makeEntry(LogLevel::Critical, "2026-10-20 18:00:00", "Database unavailable: TIMEOUT");
};

} // namespace

TEST_F(LogFilterTest, EmptyFilterMatchesEverything) {
    const LogFilter filter;
    EXPECT_TRUE(filter.empty());
    EXPECT_TRUE(filter.matches(info));
    EXPECT_TRUE(filter.matches(critical));
    EXPECT_TRUE(LogFilter::fromOptions({}).empty());
}

TEST_F(LogFilterTest, SingleLevel) {
    const auto filter = LogFilter::fromOptions({.levels = {LogLevel::Error}});
    EXPECT_FALSE(filter.matches(info));
    EXPECT_FALSE(filter.matches(warning));
    EXPECT_TRUE(filter.matches(error));
    EXPECT_FALSE(filter.matches(critical));
}

TEST_F(LogFilterTest, MultipleLevels) {
    const auto filter =
        LogFilter::fromOptions({.levels = {LogLevel::Warning, LogLevel::Error, LogLevel::Critical}});
    EXPECT_FALSE(filter.matches(info));
    EXPECT_TRUE(filter.matches(warning));
    EXPECT_TRUE(filter.matches(error));
    EXPECT_TRUE(filter.matches(critical));
    EXPECT_EQ(filter.describe().front(), "level in {WARNING, ERROR, CRITICAL}");
}

TEST_F(LogFilterTest, TimeRangeIsInclusive) {
    const auto filter =
        LogFilter::fromOptions({.from = ts("2026-10-20 12:00:00"), .to = ts("2026-10-20 14:30:30")});
    EXPECT_FALSE(filter.matches(info));
    EXPECT_TRUE(filter.matches(warning)); // == from
    EXPECT_TRUE(filter.matches(error));   // == to
    EXPECT_FALSE(filter.matches(critical));
}

TEST_F(LogFilterTest, OpenEndedTimeRanges) {
    const auto fromOnly = LogFilter::fromOptions({.from = ts("2026-10-20 14:00:00")});
    EXPECT_FALSE(fromOnly.matches(warning));
    EXPECT_TRUE(fromOnly.matches(critical));

    const auto toOnly = LogFilter::fromOptions({.to = ts("2026-10-20 10:00:00")});
    EXPECT_TRUE(toOnly.matches(info));
    EXPECT_FALSE(toOnly.matches(warning));
}

TEST_F(LogFilterTest, InvertedTimeRangeIsRejected) {
    EXPECT_THROW(
        (void)LogFilter::fromOptions({.from = ts("2026-10-21 00:00:00"), .to = ts("2026-10-20 00:00:00")}),
        ConfigurationError);
}

TEST_F(LogFilterTest, KeywordIsCaseInsensitiveByDefault) {
    const auto filter = LogFilter::fromOptions({.keywords = {"DATABASE"}});
    EXPECT_TRUE(filter.matches(error));
    EXPECT_TRUE(filter.matches(critical));
    EXPECT_FALSE(filter.matches(info));
}

TEST_F(LogFilterTest, KeywordCaseSensitive) {
    const auto filter = LogFilter::fromOptions({.keywords = {"timeout"}, .caseSensitive = true});
    EXPECT_FALSE(filter.matches(critical)); // message has "TIMEOUT"
    const auto upper = LogFilter::fromOptions({.keywords = {"TIMEOUT"}, .caseSensitive = true});
    EXPECT_TRUE(upper.matches(critical));
}

TEST_F(LogFilterTest, MultipleKeywordsAny) {
    const auto filter = LogFilter::fromOptions({.keywords = {"memory", "started"}});
    EXPECT_TRUE(filter.matches(info));
    EXPECT_TRUE(filter.matches(warning));
    EXPECT_FALSE(filter.matches(error));
}

TEST_F(LogFilterTest, MultipleKeywordsAll) {
    const auto filter =
        LogFilter::fromOptions({.keywords = {"database", "timeout"}, .keywordMatch = KeywordMatch::All});
    EXPECT_FALSE(filter.matches(error)); // has "database" only
    EXPECT_TRUE(filter.matches(critical));
}

TEST_F(LogFilterTest, EmptyKeywordIsRejected) {
    EXPECT_THROW((void)LogFilter::fromOptions({.keywords = {"ok", ""}}), ConfigurationError);
}

TEST_F(LogFilterTest, CombinedFiltersAreAndedTogether) {
    const auto filter = LogFilter::fromOptions({
        .levels = {LogLevel::Error, LogLevel::Critical},
        .from = ts("2026-10-20 14:00:00"),
        .to = ts("2026-10-20 17:00:00"),
        .keywords = {"database"},
    });
    EXPECT_EQ(filter.size(), 3u);
    EXPECT_TRUE(filter.matches(error));     // all three conditions hold
    EXPECT_FALSE(filter.matches(critical)); // outside the time range
    EXPECT_FALSE(filter.matches(warning));  // wrong level
}

TEST_F(LogFilterTest, CriteriaAreOrderedCheapestFirst) {
    const auto filter = LogFilter::fromOptions(
        {.levels = {LogLevel::Error}, .from = ts("2026-10-20 00:00:00"), .keywords = {"a", "b"}});
    const auto descriptions = filter.describe();
    ASSERT_EQ(descriptions.size(), 3u);
    EXPECT_EQ(descriptions[0].rfind("level", 0), 0u);
    EXPECT_EQ(descriptions[1].rfind("time", 0), 0u);
    EXPECT_EQ(descriptions[2].rfind("message contains any of", 0), 0u);
}

/// A user-defined criterion plugs in without modifying LogFilter (Open/Closed).
TEST_F(LogFilterTest, CustomCriterionCanBeAdded) {
    class SourceFileCriterion final : public FilterCriterion {
    public:
        bool matches(const LogEntryView& entry) const override { return entry.sourceFile == "db.cpp"; }
        std::string describe() const override { return "source file is db.cpp"; }
    };
    LogFilter filter;
    filter.add(std::make_unique<SourceFileCriterion>());
    LogEntry withSource = error;
    withSource.sourceFile = "db.cpp";
    withSource.sourceLine = 3;
    EXPECT_TRUE(filter.matches(withSource));
    EXPECT_FALSE(filter.matches(error));
}

TEST(LogFilterProcessingTest, ProcessorCountsOnlyMatchingEntries) {
    LogFilter filter = LogFilter::fromOptions({.levels = {LogLevel::Error, LogLevel::Critical}});
    const LogProcessor processor(std::move(filter));
    const ProcessingResult result = processor.process(test::samplePath("application.log"));
    EXPECT_EQ(result.summary.validEntries, 80u);
    EXPECT_EQ(result.summary.matchedEntries, 12u);
    ASSERT_FALSE(result.matchedSamples.empty());
    for (const auto& entry : result.matchedSamples) {
        EXPECT_TRUE(isErrorLevel(entry.level));
    }
}
