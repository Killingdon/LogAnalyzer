#include "cli/CommandLineInterface.h"
#include "cli/CommandLineParser.h"
#include "common/Errors.h"

#include "TestHelpers.h"

#include <gtest/gtest.h>

#include <sstream>

using namespace loganalyzer;

namespace {

Command parse(std::initializer_list<std::string_view> args) {
    return CommandLineParser::parse(std::vector<std::string_view>(args));
}

AnalyzeCommand parseAnalyze(std::initializer_list<std::string_view> args) {
    const Command command = parse(args);
    EXPECT_TRUE(std::holds_alternative<AnalyzeCommand>(command));
    return std::get<AnalyzeCommand>(command);
}

/// Runs the CLI in-process and captures both output streams.
struct CliRun {
    int exitCode;
    std::string out;
    std::string err;
};

CliRun runCli(const std::vector<std::string>& args) {
    std::ostringstream out;
    std::ostringstream err;
    std::vector<std::string_view> views(args.begin(), args.end());
    CommandLineInterface cli(out, err);
    const int code = cli.run(views);
    return {code, out.str(), err.str()};
}

} // namespace

// ---------------------------------------------------------------------------
// Parser: commands
// ---------------------------------------------------------------------------

TEST(CommandLineParserTest, HelpAndVersion) {
    EXPECT_TRUE(std::holds_alternative<HelpCommand>(parse({"--help"})));
    EXPECT_TRUE(std::holds_alternative<HelpCommand>(parse({"-h"})));
    EXPECT_EQ(std::get<HelpCommand>(parse({"help", "analyze"})).topic, "analyze");
    EXPECT_EQ(std::get<HelpCommand>(parse({"analyze", "--help"})).topic, "analyze");
    EXPECT_EQ(std::get<HelpCommand>(parse({"benchmark", "-h"})).topic, "benchmark");
    EXPECT_TRUE(std::holds_alternative<VersionCommand>(parse({"--version"})));
    EXPECT_TRUE(std::holds_alternative<VersionCommand>(parse({"-v"})));
}

TEST(CommandLineParserTest, RejectsMissingOrUnknownCommands) {
    EXPECT_THROW(parse({}), CommandLineError);
    EXPECT_THROW(parse({"analyse"}), CommandLineError); // British spelling is not a command
    EXPECT_THROW(parse({"--frobnicate"}), CommandLineError);
    EXPECT_THROW(parse({"help", "nonsense"}), CommandLineError);
    EXPECT_THROW(parse({"--version", "extra"}), CommandLineError);
}

// ---------------------------------------------------------------------------
// Parser: analyze
// ---------------------------------------------------------------------------

TEST(CommandLineParserTest, AnalyzeDefaults) {
    const AnalyzeCommand cmd = parseAnalyze({"analyze", "app.log"});
    EXPECT_EQ(cmd.input, std::filesystem::path("app.log"));
    EXPECT_EQ(cmd.processing.threadCount, 0u); // auto
    EXPECT_EQ(cmd.format, ReportFormat::Text);
    EXPECT_FALSE(cmd.output.has_value());
    EXPECT_TRUE(cmd.filter.levels.empty());
    EXPECT_FALSE(cmd.analysis.detectDuplicates);
}

TEST(CommandLineParserTest, AnalyzeAllOptions) {
    const AnalyzeCommand cmd = parseAnalyze({
        "analyze", "app.log", "--threads", "4", "--level", "WARNING,ERROR", "-l", "critical", "--keyword", "db, timeout",
        "--match", "all", "--case-sensitive", "--from", "2026-10-20 10:00:00", "--to=2026-10-20", "--top", "5",
        "--period", "minute", "--duplicates", "--samples", "3", "--chunk-size", "64", "--format", "csv", "-o", "r.out",
    });
    EXPECT_EQ(cmd.processing.threadCount, 4u);
    EXPECT_EQ(cmd.filter.levels, (std::vector{LogLevel::Warning, LogLevel::Error, LogLevel::Critical}));
    EXPECT_EQ(cmd.filter.keywords, (std::vector<std::string>{"db", "timeout"}));
    EXPECT_EQ(cmd.filter.keywordMatch, KeywordMatch::All);
    EXPECT_TRUE(cmd.filter.caseSensitive);
    EXPECT_EQ(cmd.filter.from, util::parseTimestamp("2026-10-20 10:00:00"));
    EXPECT_EQ(cmd.filter.to, util::parseTimestamp("2026-10-20 23:59:59")); // date-only --to = end of day
    EXPECT_EQ(cmd.analysis.topCount, 5u);
    EXPECT_EQ(cmd.analysis.bucket, TimeBucket::Minute);
    EXPECT_TRUE(cmd.analysis.detectDuplicates);
    EXPECT_EQ(cmd.processing.maxMatchedSamples, 3u);
    EXPECT_EQ(cmd.processing.chunkSizeBytes, 64u * 1024u);
    EXPECT_EQ(cmd.format, ReportFormat::Csv);
    EXPECT_EQ(cmd.output, std::filesystem::path("r.out"));
}

TEST(CommandLineParserTest, FormatIsInferredFromOutputExtension) {
    EXPECT_EQ(parseAnalyze({"analyze", "a.log", "-o", "r.json"}).format, ReportFormat::Json);
    EXPECT_EQ(parseAnalyze({"analyze", "a.log", "-o", "r.CSV"}).format, ReportFormat::Csv);
    EXPECT_EQ(parseAnalyze({"analyze", "a.log", "-o", "r.txt"}).format, ReportFormat::Text);
    // An explicit --format wins over the extension.
    EXPECT_EQ(parseAnalyze({"analyze", "a.log", "-o", "r.json", "--format", "txt"}).format, ReportFormat::Text);
}

TEST(CommandLineParserTest, ThreadsAcceptsAuto) {
    EXPECT_EQ(parseAnalyze({"analyze", "a.log", "--threads=auto"}).processing.threadCount, 0u);
    EXPECT_EQ(parseAnalyze({"analyze", "a.log", "-t", "1"}).processing.threadCount, 1u);
}

struct InvalidArgsCase {
    std::vector<std::string_view> args;
    std::string_view expectedMessagePart;
};

class InvalidAnalyzeArgsTest : public ::testing::TestWithParam<InvalidArgsCase> {};

TEST_P(InvalidAnalyzeArgsTest, ThrowsHelpfulError) {
    try {
        (void)CommandLineParser::parse(GetParam().args);
        FAIL() << "expected CommandLineError";
    } catch (const CommandLineError& e) {
        EXPECT_NE(std::string(e.what()).find(GetParam().expectedMessagePart), std::string::npos) << e.what();
    }
}

INSTANTIATE_TEST_SUITE_P(
    Cases, InvalidAnalyzeArgsTest,
    ::testing::Values(InvalidArgsCase{{"analyze"}, "missing input file"},
                      InvalidArgsCase{{"analyze", "a.log", "b.log"}, "exactly one file"},
                      InvalidArgsCase{{"analyze", "a.log", "--threads"}, "requires a value"},
                      InvalidArgsCase{{"analyze", "a.log", "--threads", "-3"}, "between 0 and 256"},
                      InvalidArgsCase{{"analyze", "a.log", "--threads", "257"}, "between 0 and 256"},
                      InvalidArgsCase{{"analyze", "a.log", "--threads", "four"}, "between 0 and 256"},
                      InvalidArgsCase{{"analyze", "a.log", "--level", "LOUD"}, "unknown log level 'LOUD'"},
                      InvalidArgsCase{{"analyze", "a.log", "--level", "ERROR,"}, "unknown log level ''"},
                      InvalidArgsCase{{"analyze", "a.log", "--keyword", "a,,b"}, "empty keyword"},
                      InvalidArgsCase{{"analyze", "a.log", "--match", "some"}, "'any' or 'all'"},
                      InvalidArgsCase{{"analyze", "a.log", "--from", "2026-13-01"}, "invalid value"},
                      InvalidArgsCase{{"analyze", "a.log", "--from", "2026-10-21", "--to", "2026-10-20"},
                                      "is after --to"},
                      InvalidArgsCase{{"analyze", "a.log", "--format", "xml"}, "txt, csv or json"},
                      InvalidArgsCase{{"analyze", "a.log", "--top", "0"}, "between 1 and 1000"},
                      InvalidArgsCase{{"analyze", "a.log", "--period", "week"}, "minute, hour or day"},
                      InvalidArgsCase{{"analyze", "a.log", "--duplicates=yes"}, "does not take a value"},
                      InvalidArgsCase{{"analyze", "a.log", "--output="}, "non-empty path"},
                      InvalidArgsCase{{"analyze", "a.log", "--bogus"}, "unknown option '--bogus'"}));

// ---------------------------------------------------------------------------
// Parser: benchmark and generate
// ---------------------------------------------------------------------------

TEST(CommandLineParserTest, BenchmarkOptions) {
    const auto cmd = std::get<BenchmarkCommand>(
        parse({"benchmark", "big.log", "--threads", "2,4", "-n", "5", "--no-warmup", "--level", "ERROR"}));
    EXPECT_EQ(cmd.input, std::filesystem::path("big.log"));
    EXPECT_EQ(cmd.options.threadCounts, (std::vector<unsigned>{2, 4}));
    EXPECT_EQ(cmd.options.iterations, 5u);
    EXPECT_FALSE(cmd.options.warmup);
    EXPECT_EQ(cmd.options.filter.levels, std::vector{LogLevel::Error});

    const auto defaults = std::get<BenchmarkCommand>(parse({"benchmark", "big.log"}));
    EXPECT_EQ(defaults.options.threadCounts, (std::vector<unsigned>{1, 2, 4, 8}));

    EXPECT_THROW(parse({"benchmark", "big.log", "--threads", "0,2"}), CommandLineError);
    EXPECT_THROW(parse({"benchmark", "big.log", "--iterations", "0"}), CommandLineError);
    EXPECT_THROW(parse({"benchmark"}), CommandLineError);
}

TEST(CommandLineParserTest, GenerateOptions) {
    const auto cmd =
        std::get<GenerateCommand>(parse({"generate", "out.log", "--lines", "500", "--seed", "9", "--malformed-rate", "0.25"}));
    EXPECT_EQ(cmd.output, std::filesystem::path("out.log"));
    EXPECT_EQ(cmd.options.lines, 500u);
    EXPECT_EQ(cmd.options.seed, 9u);
    EXPECT_DOUBLE_EQ(cmd.options.malformedRate, 0.25);

    EXPECT_THROW(parse({"generate", "out.log", "--malformed-rate", "1.5"}), CommandLineError);
    EXPECT_THROW(parse({"generate", "out.log", "--malformed-rate", "abc"}), CommandLineError);
    EXPECT_THROW(parse({"generate", "out.log", "--lines", "0"}), CommandLineError);
    EXPECT_THROW(parse({"generate"}), CommandLineError);
}

TEST(CommandLineParserTest, EveryCommandHasHelp) {
    for (const char* topic : {"", "analyze", "benchmark", "generate"}) {
        EXPECT_NE(CommandLineParser::helpText(topic).find("Usage:"), std::string::npos) << topic;
    }
}

// ---------------------------------------------------------------------------
// CommandLineInterface: end-to-end, in-process
// ---------------------------------------------------------------------------

TEST(CommandLineInterfaceTest, ExitCodes) {
    EXPECT_EQ(runCli({"--version"}).exitCode, kExitSuccess);
    EXPECT_EQ(runCli({"--help"}).exitCode, kExitSuccess);
    EXPECT_EQ(runCli({}).exitCode, kExitUsage);
    EXPECT_EQ(runCli({"analyze", "x.log", "--threads", "x"}).exitCode, kExitUsage);
    EXPECT_EQ(runCli({"analyze", "does-not-exist.log"}).exitCode, kExitFailure);
}

TEST(CommandLineInterfaceTest, ErrorsGoToStderr) {
    const CliRun run = runCli({"analyze", "does-not-exist.log"});
    EXPECT_TRUE(run.out.empty());
    EXPECT_NE(run.err.find("file does not exist"), std::string::npos);
}

TEST(CommandLineInterfaceTest, AnalyzePrintsTextReport) {
    const CliRun run = runCli({"analyze", test::samplePath("application.log").string(), "--threads", "2"});
    EXPECT_EQ(run.exitCode, kExitSuccess) << run.err;
    EXPECT_NE(run.out.find("PROCESSING SUMMARY"), std::string::npos);
    EXPECT_NE(run.out.find("Database connection failed"), std::string::npos);
}

TEST(CommandLineInterfaceTest, AnalyzeWritesJsonFile) {
    const test::TempDirectory dir;
    const auto output = dir.path() / "report.json";
    const CliRun run =
        runCli({"analyze", test::samplePath("server.log").string(), "--level", "ERROR", "-o", output.string()});
    EXPECT_EQ(run.exitCode, kExitSuccess) << run.err;
    EXPECT_NE(run.out.find("JSON report written"), std::string::npos);
    const std::string json = test::readFile(output);
    EXPECT_EQ(json.front(), '{');
    EXPECT_NE(json.find("\"matched_entries\": 7"), std::string::npos) << json; // 7 ERROR lines in server.log
}

TEST(CommandLineInterfaceTest, InvalidOutputDirectoryFailsBeforeProcessing) {
    const CliRun run = runCli({"analyze", test::samplePath("application.log").string(), "-o", "no/such/dir/r.txt"});
    EXPECT_EQ(run.exitCode, kExitFailure);
    EXPECT_NE(run.err.find("does not exist"), std::string::npos);
}

TEST(CommandLineInterfaceTest, EmptyFileProducesWarning) {
    const test::TempDirectory dir;
    const auto file = dir.writeFile("empty.log", "");
    const CliRun run = runCli({"analyze", file.string()});
    EXPECT_EQ(run.exitCode, kExitSuccess);
    EXPECT_NE(run.err.find("is empty"), std::string::npos);
}

TEST(CommandLineInterfaceTest, GenerateThenAnalyze) {
    const test::TempDirectory dir;
    const auto file = dir.path() / "gen.log";
    ASSERT_EQ(runCli({"generate", file.string(), "--lines", "1000", "--malformed-rate", "0"}).exitCode, kExitSuccess);
    const CliRun run = runCli({"analyze", file.string(), "--format", "json"});
    EXPECT_EQ(run.exitCode, kExitSuccess);
    EXPECT_NE(run.out.find("\"total_lines\": 1000"), std::string::npos);
    EXPECT_NE(run.out.find("\"invalid_entries\": 0"), std::string::npos);
}
