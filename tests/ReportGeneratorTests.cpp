#include "analysis/LogAnalyzer.h"
#include "common/Errors.h"
#include "processing/LogProcessor.h"
#include "reporting/ReportGenerator.h"

#include "../src/reporting/JsonWriter.h"
#include "TestHelpers.h"

#include <gtest/gtest.h>

using namespace loganalyzer;

namespace {

/// Report built from a real sample file through the real pipeline.
AnalysisReport sampleReport(bool withFilter, bool duplicates) {
    const AnalysisOptions options{.topCount = 3, .detectDuplicates = duplicates};
    FilterOptions filterOptions;
    if (withFilter) {
        filterOptions.levels = {LogLevel::Error, LogLevel::Critical};
        filterOptions.keywords = {"database"};
    }
    LogFilter filter = LogFilter::fromOptions(filterOptions);
    auto descriptions = filter.describe();
    const LogProcessor processor(std::move(filter), {.threadCount = 1}, options);
    return LogAnalyzer{options}.analyze(processor.process(test::samplePath("application.log")), "application.log",
                                        std::move(descriptions));
}

/// A report containing hostile text: quotes, commas, newlines, control and
/// non-UTF-8 bytes, and a spreadsheet formula.
AnalysisReport hostileReport() {
    AnalysisReport report;
    report.inputName = "C:\\logs\\app \"prod\".log";
    report.summary = {.totalLines = 2, .validEntries = 1, .invalidEntries = 1, .matchedEntries = 1};
    report.filterDescriptions = {"message contains \"x\""};
    LogEntry entry;
    entry.lineNumber = 1;
    entry.level = LogLevel::Error;
    entry.message = "=HYPERLINK(\"http://evil\"),\ttab \x01 bad\xff utf8";
    report.matchedSamples.push_back(entry);
    report.topErrors.push_back({entry.message, MessageStats{.count = 1}});
    report.invalidSamples.push_back({2, ParseErrorCode::MissingTimestamp, "line, with \"quotes\""});
    for (const LogLevel level : kAllLogLevels) {
        report.levelDistribution.push_back({level, level == LogLevel::Error ? 1u : 0u, 0.0});
    }
    return report;
}

/// Minimal structural JSON check: balanced brackets outside strings, legal
/// escapes, no raw control characters inside strings.
bool looksLikeValidJson(const std::string& json) {
    int depth = 0;
    bool inString = false;
    for (std::size_t i = 0; i < json.size(); ++i) {
        const auto c = static_cast<unsigned char>(json[i]);
        if (inString) {
            if (c < 0x20) {
                return false;
            }
            if (c == '\\') {
                const char next = json[++i];
                if (std::string_view("\"\\/bfnrtu").find(next) == std::string_view::npos) {
                    return false;
                }
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (--depth < 0) {
                return false;
            }
        }
    }
    return depth == 0 && !inString;
}

} // namespace

TEST(ReportFormatTest, ParsesFormatNames) {
    EXPECT_EQ(parseReportFormat("txt"), ReportFormat::Text);
    EXPECT_EQ(parseReportFormat("TEXT"), ReportFormat::Text);
    EXPECT_EQ(parseReportFormat("Json"), ReportFormat::Json);
    EXPECT_EQ(parseReportFormat("csv"), ReportFormat::Csv);
    EXPECT_FALSE(parseReportFormat("xml"));
}

TEST(ReportGeneratorTest, FactoryCreatesMatchingFormatter) {
    for (const ReportFormat format : {ReportFormat::Text, ReportFormat::Csv, ReportFormat::Json}) {
        EXPECT_EQ(ReportGenerator(format).format(), format);
    }
}

TEST(ReportGeneratorTest, TextReportContainsAllSections) {
    const std::string text = ReportGenerator(ReportFormat::Text).toString(sampleReport(true, true));
    for (const char* section : {"PROCESSING SUMMARY", "LOG LEVEL DISTRIBUTION", "TOP ERRORS", "MOST ACTIVE PERIODS",
                                "DUPLICATE MESSAGES", "MATCHING ENTRIES", "PERFORMANCE"}) {
        EXPECT_NE(text.find(section), std::string::npos) << section;
    }
    EXPECT_NE(text.find("Database connection failed"), std::string::npos);
    EXPECT_NE(text.find("level in {ERROR, CRITICAL}"), std::string::npos);
}

TEST(ReportGeneratorTest, TextReportOmitsFilterAndDuplicateSectionsWhenUnused) {
    const std::string text = ReportGenerator(ReportFormat::Text).toString(sampleReport(false, false));
    EXPECT_EQ(text.find("MATCHING ENTRIES"), std::string::npos);
    EXPECT_EQ(text.find("DUPLICATE MESSAGES"), std::string::npos);
    EXPECT_EQ(text.find("INVALID ENTRIES"), std::string::npos); // sample has no invalid lines
}

TEST(ReportGeneratorTest, JsonReportIsWellFormedAndComplete) {
    const std::string json = ReportGenerator(ReportFormat::Json).toString(sampleReport(true, true));
    EXPECT_TRUE(looksLikeValidJson(json)) << json;
    for (const char* key : {"\"summary\"", "\"total_lines\": 80", "\"level_distribution\"", "\"top_errors\"",
                            "\"most_active_periods\"", "\"duplicates\"", "\"matched_entries_sample\"",
                            "\"performance\"", "\"lines_per_second\""}) {
        EXPECT_NE(json.find(key), std::string::npos) << key;
    }
}

TEST(ReportGeneratorTest, JsonDuplicatesAreNullWhenDisabled) {
    const std::string json = ReportGenerator(ReportFormat::Json).toString(sampleReport(false, false));
    EXPECT_NE(json.find("\"duplicates\": null"), std::string::npos);
}

TEST(ReportGeneratorTest, JsonEscapesHostileText) {
    const std::string json = ReportGenerator(ReportFormat::Json).toString(hostileReport());
    EXPECT_TRUE(looksLikeValidJson(json)) << json;
    EXPECT_NE(json.find(R"(C:\\logs\\app \"prod\".log)"), std::string::npos);
    EXPECT_NE(json.find(R"(\ttab \u0001 bad)" "\xEF\xBF\xBD"), std::string::npos); // 0xFF -> U+FFFD
}

TEST(JsonWriterTest, EscapeRules) {
    using detail::escapeJson;
    EXPECT_EQ(escapeJson("plain"), "plain");
    EXPECT_EQ(escapeJson("a\"b\\c"), "a\\\"b\\\\c");
    EXPECT_EQ(escapeJson("\n\r\t"), "\\n\\r\\t");
    EXPECT_EQ(escapeJson(std::string_view("\0", 1)), "\\u0000");
    EXPECT_EQ(escapeJson("caf\xC3\xA9"), "caf\xC3\xA9");             // valid UTF-8 kept
    EXPECT_EQ(escapeJson("\xC3"), "\xEF\xBF\xBD");                   // truncated sequence
    EXPECT_EQ(escapeJson("\xC0\xAF"), "\xEF\xBF\xBD\xEF\xBF\xBD");   // overlong encoding
    EXPECT_EQ(escapeJson("\xED\xA0\x80"), "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"); // surrogate
}

TEST(ReportGeneratorTest, CsvQuotesAndNeutralisesFormulas) {
    const std::string csv = ReportGenerator(ReportFormat::Csv).toString(hostileReport());
    // The formula gets a leading apostrophe; embedded quotes are doubled.
    EXPECT_NE(csv.find(R"("'=HYPERLINK(""http://evil""))"), std::string::npos) << csv;
    EXPECT_NE(csv.find(R"("line, with ""quotes""")"), std::string::npos) << csv;
}

TEST(ReportGeneratorTest, CsvHasOneHeaderPerSection) {
    const std::string csv = ReportGenerator(ReportFormat::Csv).toString(sampleReport(true, true));
    EXPECT_EQ(csv.rfind("section,metric,value\n", 0), 0u);
    for (const char* header : {"section,level,count,percentage", "section,rank,message,count",
                               "section,granularity,period_start,count", "section,line,timestamp,level"}) {
        EXPECT_NE(csv.find(header), std::string::npos) << header;
    }
    EXPECT_NE(csv.find("summary,total_lines,80"), std::string::npos);
    EXPECT_NE(csv.find("level_distribution,ERROR,"), std::string::npos);
}

TEST(ReportGeneratorTest, WritesReportToFile) {
    const test::TempDirectory dir;
    const auto path = dir.path() / "report.json";
    const ReportGenerator generator(ReportFormat::Json);
    const AnalysisReport report = sampleReport(false, false);
    generator.writeToFile(report, path);
    EXPECT_EQ(test::readFile(path), generator.toString(report));
}

TEST(ReportGeneratorTest, RejectsInvalidOutputPaths) {
    const test::TempDirectory dir;
    const ReportGenerator generator(ReportFormat::Text);
    const AnalysisReport report;
    EXPECT_THROW(generator.writeToFile(report, dir.path() / "missing-dir" / "r.txt"), ReportError);
    EXPECT_THROW(generator.writeToFile(report, dir.path()), ReportError); // a directory
    EXPECT_THROW(ReportGenerator::validateOutputPath(""), ReportError);
}

TEST(ReportGeneratorTest, EmptyReportRendersInEveryFormat) {
    AnalysisReport report;
    report.inputName = "empty.log";
    for (const ReportFormat format : {ReportFormat::Text, ReportFormat::Csv, ReportFormat::Json}) {
        std::string output;
        ASSERT_NO_THROW(output = ReportGenerator(format).toString(report));
        EXPECT_FALSE(output.empty());
    }
    EXPECT_TRUE(looksLikeValidJson(ReportGenerator(ReportFormat::Json).toString(report)));
}
