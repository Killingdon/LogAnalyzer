#include "parser/LogParser.h"
#include "util/TimeUtils.h"

#include <gtest/gtest.h>

#include <ostream>

using namespace loganalyzer;

namespace {

Timestamp ts(std::string_view text) {
    const auto parsed = util::parseTimestamp(text);
    EXPECT_TRUE(parsed.has_value()) << text;
    return parsed.value_or(Timestamp{});
}

} // namespace

// ---------------------------------------------------------------------------
// Timestamp parsing
// ---------------------------------------------------------------------------

TEST(TimeUtilsTest, ParsesAndFormatsRoundTrip) {
    const auto parsed = util::parseTimestamp("2026-10-20 14:30:22");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(util::formatTimestamp(*parsed), "2026-10-20 14:30:22");
}

TEST(TimeUtilsTest, AcceptsIsoSeparatorAndFraction) {
    EXPECT_EQ(util::parseTimestamp("2026-10-20T14:30:22"), ts("2026-10-20 14:30:22"));
    EXPECT_EQ(util::parseTimestamp("2026-10-20 14:30:22.123"), ts("2026-10-20 14:30:22"));
}

TEST(TimeUtilsTest, RejectsInvalidDatesAndTimes) {
    EXPECT_FALSE(util::parseTimestamp("2026-02-30 10:00:00")); // no Feb 30th
    EXPECT_FALSE(util::parseTimestamp("2026-13-01 10:00:00")); // month 13
    EXPECT_FALSE(util::parseTimestamp("2026-10-20 24:00:00")); // hour 24
    EXPECT_FALSE(util::parseTimestamp("2026-10-20 10:60:00")); // minute 60
    EXPECT_FALSE(util::parseTimestamp("2026-10-20 10:00:60")); // second 60
    EXPECT_FALSE(util::parseTimestamp("2026/10/20 10:00:00"));
    EXPECT_FALSE(util::parseTimestamp("2026-10-20"));
    EXPECT_FALSE(util::parseTimestamp("2026-10-20 10:00:00."));
    EXPECT_FALSE(util::parseTimestamp("2026-10-2a 10:00:00"));
    EXPECT_FALSE(util::parseTimestamp(""));
}

TEST(TimeUtilsTest, LeapYearsAreHandled) {
    EXPECT_TRUE(util::parseTimestamp("2028-02-29 00:00:00"));
    EXPECT_FALSE(util::parseTimestamp("2026-02-29 00:00:00"));
}

TEST(TimeUtilsTest, ParseDateReturnsMidnight) {
    EXPECT_EQ(util::parseDate("2026-10-20"), ts("2026-10-20 00:00:00"));
    EXPECT_FALSE(util::parseDate("2026-10-20 00:00:00"));
}

// ---------------------------------------------------------------------------
// Log levels
// ---------------------------------------------------------------------------

TEST(LogLevelTest, ParsesNamesCaseInsensitivelyWithAliases) {
    EXPECT_EQ(parseLogLevel("ERROR"), LogLevel::Error);
    EXPECT_EQ(parseLogLevel("error"), LogLevel::Error);
    EXPECT_EQ(parseLogLevel("Warn"), LogLevel::Warning);
    EXPECT_EQ(parseLogLevel("FATAL"), LogLevel::Critical);
    EXPECT_FALSE(parseLogLevel("NOTICE"));
    EXPECT_FALSE(parseLogLevel(""));
}

TEST(LogLevelTest, RoundTripsAllLevels) {
    for (const LogLevel level : kAllLogLevels) {
        EXPECT_EQ(parseLogLevel(toString(level)), level);
    }
}

// ---------------------------------------------------------------------------
// Valid lines
// ---------------------------------------------------------------------------

TEST(LogParserTest, ParsesBasicLine) {
    const LogParser parser;
    const auto result = parser.parseLine("[2026-10-20 14:30:30] [ERROR] Database connection failed", 7);
    ASSERT_TRUE(result.ok());
    const LogEntryView& entry = result.entry();
    EXPECT_EQ(entry.timestamp, ts("2026-10-20 14:30:30"));
    EXPECT_EQ(entry.level, LogLevel::Error);
    EXPECT_EQ(entry.message, "Database connection failed");
    EXPECT_FALSE(entry.hasSourceLocation());
    EXPECT_EQ(entry.lineNumber, 7u);
}

TEST(LogParserTest, ParsesAllSupportedLevels) {
    const LogParser parser;
    for (const LogLevel level : kAllLogLevels) {
        const std::string line = "[2026-10-20 14:30:30] [" + std::string(toString(level)) + "] message";
        const auto result = parser.parseLine(line);
        ASSERT_TRUE(result.ok()) << line;
        EXPECT_EQ(result.entry().level, level);
    }
}

TEST(LogParserTest, ParsesOptionalSourceLocation) {
    const LogParser parser;
    const auto result = parser.parseLine("[2026-10-20 14:30:30] [ERROR] [DatabasePool.cpp:142] Connection refused");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.entry().sourceFile, "DatabasePool.cpp");
    EXPECT_EQ(result.entry().sourceLine, 142u);
    EXPECT_EQ(result.entry().message, "Connection refused");
}

TEST(LogParserTest, NonLocationBracketStaysInMessage) {
    const LogParser parser;
    const auto result = parser.parseLine("[2026-10-20 14:30:30] [INFO] [cache] miss for key 42");
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(result.entry().hasSourceLocation());
    EXPECT_EQ(result.entry().message, "[cache] miss for key 42");
}

TEST(LogParserTest, ToleratesCarriageReturnAndExtraSpaces) {
    const LogParser parser;
    const auto result = parser.parseLine("[2026-10-20 14:30:30]   [ warning ]   Disk at 91%  \r");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.entry().level, LogLevel::Warning);
    EXPECT_EQ(result.entry().message, "Disk at 91%");
}

TEST(LogParserTest, MessageMayContainBrackets) {
    const LogParser parser;
    const auto result = parser.parseLine("[2026-10-20 14:30:30] [DEBUG] payload=[1,2,3] ok");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.entry().message, "payload=[1,2,3] ok");
}

TEST(LogParserTest, OwningParseCopiesData) {
    const LogParser parser;
    std::string line = "[2026-10-20 14:30:30] [INFO] [main.cpp:10] Started";
    const auto entry = parser.parse(line, 3);
    line.assign(line.size(), 'x'); // destroy the original text
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->message, "Started");
    EXPECT_EQ(entry->sourceFile, "main.cpp");
    EXPECT_EQ(entry->sourceLine, 10u);
    EXPECT_EQ(entry->lineNumber, 3u);
}

TEST(LogParserTest, ViewAndEntryRoundTrip) {
    const LogParser parser;
    const auto entry = parser.parse("[2026-10-20 14:30:30] [CRITICAL] [a.cpp:1] Out of memory", 9);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->view().toEntry(), *entry);
}

// ---------------------------------------------------------------------------
// Invalid lines
// ---------------------------------------------------------------------------

struct InvalidLineCase {
    std::string_view line;
    ParseErrorCode expected;
};

// Tells GoogleTest how to print a parameter (used in test names and messages).
void PrintTo(const InvalidLineCase& c, std::ostream* os) {
    *os << '"';
    for (const char ch : c.line) {
        if (ch == '\t') {
            *os << "\\t";
        } else if (ch == '\r') {
            *os << "\\r";
        } else {
            *os << ch;
        }
    }
    *os << "\" -> " << describe(c.expected);
}

class LogParserInvalidTest : public ::testing::TestWithParam<InvalidLineCase> {};

TEST_P(LogParserInvalidTest, ReportsExpectedError) {
    const LogParser parser;
    const auto result = parser.parseLine(GetParam().line);
    ASSERT_FALSE(result.ok()) << GetParam().line;
    EXPECT_EQ(result.error(), GetParam().expected) << GetParam().line;
    EXPECT_FALSE(describe(result.error()).empty());
}

INSTANTIATE_TEST_SUITE_P(
    MalformedLines, LogParserInvalidTest,
    ::testing::Values(
        InvalidLineCase{"", ParseErrorCode::EmptyLine},
        InvalidLineCase{"   \t\r", ParseErrorCode::EmptyLine},
        InvalidLineCase{"Application started", ParseErrorCode::MissingTimestamp},
        InvalidLineCase{"2026-10-20 14:30:22 [INFO] no brackets", ParseErrorCode::MissingTimestamp},
        InvalidLineCase{"[2026-10-20 14:30:22 [INFO] unterminated", ParseErrorCode::InvalidTimestamp},
        InvalidLineCase{"[2026-10-20 25:30:22] [INFO] bad hour", ParseErrorCode::InvalidTimestamp},
        InvalidLineCase{"[not a date] [INFO] text", ParseErrorCode::InvalidTimestamp},
        InvalidLineCase{"[2026-10-20 14:30:22] INFO no level brackets", ParseErrorCode::MissingLevel},
        InvalidLineCase{"[2026-10-20 14:30:22]", ParseErrorCode::MissingLevel},
        InvalidLineCase{"[2026-10-20 14:30:22] [VERBOSE] text", ParseErrorCode::UnknownLevel},
        InvalidLineCase{"[2026-10-20 14:30:22] [] text", ParseErrorCode::UnknownLevel},
        InvalidLineCase{"[2026-10-20 14:30:22] [INFO]", ParseErrorCode::MissingMessage},
        InvalidLineCase{"[2026-10-20 14:30:22] [INFO]    ", ParseErrorCode::MissingMessage},
        InvalidLineCase{"[2026-10-20 14:30:22] [INFO] [main.cpp:12]", ParseErrorCode::MissingMessage}));
