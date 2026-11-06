#include "cli/CommandLineParser.h"

#include "common/Errors.h"
#include "common/Version.h"
#include "util/StringUtils.h"
#include "util/TimeUtils.h"

#include <cerrno>
#include <cstdlib>
#include <format>
#include <limits>

namespace loganalyzer {

namespace {

constexpr unsigned kMaxThreads = 256;
constexpr std::uint64_t kMaxTop = 1000;
constexpr std::uint64_t kMaxSamples = 100'000;
constexpr std::uint64_t kMaxChunkKiB = 1024 * 1024; // 1 GiB
constexpr std::uint64_t kMaxIterations = 100;
constexpr std::uint64_t kMaxGeneratedLines = 1'000'000'000;

[[noreturn]] void fail(const std::string& message) {
    throw CommandLineError(message);
}

/// Walks the argument list and splits "--name=value" forms.
class ArgumentCursor {
public:
    explicit ArgumentCursor(const std::vector<std::string_view>& args, std::size_t start = 0)
        : args_(args), index_(start) {}

    [[nodiscard]] bool done() const noexcept { return index_ >= args_.size(); }

    /// Advances to the next argument. For "--name=value", `name()` is "--name"
    /// and the inline value is remembered for `value()`.
    void advance() {
        current_ = args_[index_++];
        inlineValue_.reset();
        if (current_.starts_with("--")) {
            if (const auto eq = current_.find('='); eq != std::string_view::npos) {
                inlineValue_ = current_.substr(eq + 1);
                current_ = current_.substr(0, eq);
            }
        }
    }

    [[nodiscard]] std::string_view name() const noexcept { return current_; }

    [[nodiscard]] bool isOption() const noexcept { return current_.size() > 1 && current_.front() == '-'; }

    /// The option's value: inline ("--x=v") or the next argument ("--x v").
    [[nodiscard]] std::string_view value() {
        if (inlineValue_) {
            return *inlineValue_;
        }
        if (done()) {
            fail(std::format("option '{}' requires a value", current_));
        }
        return args_[index_++];
    }

    /// For flags: rejects "--flag=value".
    void requireNoValue() const {
        if (inlineValue_) {
            fail(std::format("option '{}' does not take a value", current_));
        }
    }

private:
    const std::vector<std::string_view>& args_;
    std::size_t index_;
    std::string_view current_;
    std::optional<std::string_view> inlineValue_;
};

// ---------------------------------------------------------------------------
// Value parsers
// ---------------------------------------------------------------------------

std::uint64_t parseNumber(std::string_view option, std::string_view text, std::uint64_t min, std::uint64_t max) {
    const auto value = util::parseUnsigned(text);
    if (!value || *value < min || *value > max) {
        fail(std::format("invalid value '{}' for {}: expected an integer between {} and {}", text, option, min, max));
    }
    return *value;
}

double parseFraction(std::string_view option, std::string_view text) {
    const std::string copy(text); // strtod needs a NUL-terminated string
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(copy.c_str(), &end);
    if (copy.empty() || end != copy.c_str() + copy.size() || errno != 0 || !(value >= 0.0 && value <= 1.0)) {
        fail(std::format("invalid value '{}' for {}: expected a number between 0 and 1", text, option));
    }
    return value;
}

unsigned parseThreads(std::string_view option, std::string_view text) {
    if (util::equalsIgnoreCase(text, "auto")) {
        return 0;
    }
    return static_cast<unsigned>(parseNumber(option, text, 0, kMaxThreads));
}

std::vector<unsigned> parseThreadList(std::string_view option, std::string_view text) {
    std::vector<unsigned> counts;
    for (const std::string_view part : util::split(text, ',')) {
        counts.push_back(static_cast<unsigned>(parseNumber(option, util::trim(part), 1, kMaxThreads)));
    }
    return counts;
}

/// "ERROR" or "WARNING,ERROR,CRITICAL"; repeated options accumulate.
void parseLevels(std::string_view text, std::vector<LogLevel>& levels) {
    for (const std::string_view part : util::split(text, ',')) {
        const std::string_view name = util::trim(part);
        const auto level = parseLogLevel(name);
        if (!level) {
            fail(std::format("unknown log level '{}' (expected TRACE, DEBUG, INFO, WARNING, ERROR or CRITICAL)",
                             name));
        }
        levels.push_back(*level);
    }
}

void parseKeywords(std::string_view text, std::vector<std::string>& keywords) {
    for (const std::string_view part : util::split(text, ',')) {
        const std::string_view keyword = util::trim(part);
        if (keyword.empty()) {
            fail(std::format("empty keyword in '{}'", text));
        }
        keywords.emplace_back(keyword);
    }
}

/// Accepts "YYYY-MM-DD HH:MM:SS" or a bare date. A bare date means the start
/// of the day for --from and the end of the day for --to, so that
/// `--from 2026-10-20 --to 2026-10-20` selects that whole day.
Timestamp parseTimeBound(std::string_view option, std::string_view text, bool endOfDay) {
    const std::string_view trimmed = util::trim(text);
    if (const auto full = util::parseTimestamp(trimmed)) {
        return *full;
    }
    if (const auto date = util::parseDate(trimmed)) {
        return endOfDay ? *date + std::chrono::hours{23} + std::chrono::minutes{59} + std::chrono::seconds{59}
                        : *date;
    }
    fail(std::format("invalid value '{}' for {}: expected \"YYYY-MM-DD HH:MM:SS\" or \"YYYY-MM-DD\"", text,
                     option));
}

ReportFormat formatFromExtension(const std::filesystem::path& path) {
    const std::string extension = util::toLower(path.extension().string());
    if (extension == ".json") {
        return ReportFormat::Json;
    }
    if (extension == ".csv") {
        return ReportFormat::Csv;
    }
    return ReportFormat::Text;
}

// ---------------------------------------------------------------------------
// Options shared by 'analyze' and 'benchmark'
// ---------------------------------------------------------------------------

/// Handles filter, analysis and chunk-size options. Returns false if the
/// current option is not one of them.
bool parseSharedOption(ArgumentCursor& cursor, FilterOptions& filter, AnalysisOptions& analysis,
                       std::size_t& chunkSizeBytes) {
    const std::string_view name = cursor.name();
    if (name == "-l" || name == "--level") {
        parseLevels(cursor.value(), filter.levels);
    } else if (name == "-k" || name == "--keyword") {
        parseKeywords(cursor.value(), filter.keywords);
    } else if (name == "--match") {
        const std::string_view mode = cursor.value();
        if (util::equalsIgnoreCase(mode, "any")) {
            filter.keywordMatch = KeywordMatch::Any;
        } else if (util::equalsIgnoreCase(mode, "all")) {
            filter.keywordMatch = KeywordMatch::All;
        } else {
            fail(std::format("invalid value '{}' for --match: expected 'any' or 'all'", mode));
        }
    } else if (name == "-c" || name == "--case-sensitive") {
        cursor.requireNoValue();
        filter.caseSensitive = true;
    } else if (name == "--from") {
        filter.from = parseTimeBound(name, cursor.value(), false);
    } else if (name == "--to") {
        filter.to = parseTimeBound(name, cursor.value(), true);
    } else if (name == "--top") {
        analysis.topCount = static_cast<std::size_t>(parseNumber(name, cursor.value(), 1, kMaxTop));
    } else if (name == "--period") {
        const std::string_view text = cursor.value();
        const auto bucket = parseTimeBucket(text);
        if (!bucket) {
            fail(std::format("invalid value '{}' for --period: expected minute, hour or day", text));
        }
        analysis.bucket = *bucket;
    } else if (name == "-d" || name == "--duplicates") {
        cursor.requireNoValue();
        analysis.detectDuplicates = true;
    } else if (name == "--chunk-size") {
        chunkSizeBytes = static_cast<std::size_t>(parseNumber(name, cursor.value(), 1, kMaxChunkKiB)) * 1024;
    } else {
        return false;
    }
    return true;
}

void validateTimeRange(const FilterOptions& filter) {
    if (filter.from && filter.to && *filter.from > *filter.to) {
        fail(std::format("invalid time range: --from ({}) is after --to ({})", util::formatTimestamp(*filter.from),
                         util::formatTimestamp(*filter.to)));
    }
}

/// Stores the single positional argument (input or output file).
void setPositional(std::optional<std::filesystem::path>& target, std::string_view value, std::string_view command) {
    if (target) {
        fail(std::format("unexpected argument '{}': '{}' takes exactly one file", value, command));
    }
    target = std::filesystem::path(std::string(value));
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

Command parseAnalyze(const std::vector<std::string_view>& args) {
    AnalyzeCommand cmd;
    std::optional<ReportFormat> explicitFormat;
    std::optional<std::filesystem::path> input;

    ArgumentCursor cursor(args, 1);
    while (!cursor.done()) {
        cursor.advance();
        const std::string_view name = cursor.name();

        if (!cursor.isOption()) {
            setPositional(input, name, "analyze");
        } else if (name == "-h" || name == "--help") {
            return HelpCommand{"analyze"};
        } else if (parseSharedOption(cursor, cmd.filter, cmd.analysis, cmd.processing.chunkSizeBytes)) {
            // handled
        } else if (name == "-t" || name == "--threads") {
            cmd.processing.threadCount = parseThreads(name, cursor.value());
        } else if (name == "-f" || name == "--format") {
            const std::string_view text = cursor.value();
            explicitFormat = parseReportFormat(text);
            if (!explicitFormat) {
                fail(std::format("invalid value '{}' for --format: expected txt, csv or json", text));
            }
        } else if (name == "-o" || name == "--output") {
            const std::string_view text = cursor.value();
            if (text.empty()) {
                fail("--output requires a non-empty path");
            }
            cmd.output = std::filesystem::path(std::string(text));
        } else if (name == "--samples") {
            const auto samples = static_cast<std::size_t>(parseNumber(name, cursor.value(), 0, kMaxSamples));
            cmd.processing.maxMatchedSamples = samples;
            cmd.processing.maxInvalidSamples = samples;
        } else {
            fail(std::format("unknown option '{}' for 'analyze'", name));
        }
    }

    if (!input) {
        fail("missing input file: usage is 'loganalyzer analyze <file> [options]'");
    }
    cmd.input = std::move(*input);
    validateTimeRange(cmd.filter);
    // An explicit --format wins; otherwise infer it from the output file name.
    cmd.format = explicitFormat.value_or(cmd.output ? formatFromExtension(*cmd.output) : ReportFormat::Text);
    return cmd;
}

Command parseBenchmark(const std::vector<std::string_view>& args) {
    BenchmarkCommand cmd;
    std::optional<std::filesystem::path> input;

    ArgumentCursor cursor(args, 1);
    while (!cursor.done()) {
        cursor.advance();
        const std::string_view name = cursor.name();

        if (!cursor.isOption()) {
            setPositional(input, name, "benchmark");
        } else if (name == "-h" || name == "--help") {
            return HelpCommand{"benchmark"};
        } else if (parseSharedOption(cursor, cmd.options.filter, cmd.options.analysis, cmd.options.chunkSizeBytes)) {
            // handled
        } else if (name == "-t" || name == "--threads") {
            cmd.options.threadCounts = parseThreadList(name, cursor.value());
        } else if (name == "-n" || name == "--iterations") {
            cmd.options.iterations = static_cast<unsigned>(parseNumber(name, cursor.value(), 1, kMaxIterations));
        } else if (name == "--no-warmup") {
            cursor.requireNoValue();
            cmd.options.warmup = false;
        } else {
            fail(std::format("unknown option '{}' for 'benchmark'", name));
        }
    }

    if (!input) {
        fail("missing input file: usage is 'loganalyzer benchmark <file> [options]'");
    }
    cmd.input = std::move(*input);
    validateTimeRange(cmd.options.filter);
    return cmd;
}

Command parseGenerate(const std::vector<std::string_view>& args) {
    GenerateCommand cmd;
    std::optional<std::filesystem::path> output;

    ArgumentCursor cursor(args, 1);
    while (!cursor.done()) {
        cursor.advance();
        const std::string_view name = cursor.name();

        if (!cursor.isOption()) {
            setPositional(output, name, "generate");
        } else if (name == "-h" || name == "--help") {
            return HelpCommand{"generate"};
        } else if (name == "--lines") {
            cmd.options.lines = parseNumber(name, cursor.value(), 1, kMaxGeneratedLines);
        } else if (name == "--seed") {
            cmd.options.seed = static_cast<std::uint32_t>(parseNumber(name, cursor.value(), 0, std::numeric_limits<std::uint32_t>::max()));
        } else if (name == "--malformed-rate") {
            cmd.options.malformedRate = parseFraction(name, cursor.value());
        } else {
            fail(std::format("unknown option '{}' for 'generate'", name));
        }
    }

    if (!output) {
        fail("missing output file: usage is 'loganalyzer generate <file> [options]'");
    }
    cmd.output = std::move(*output);
    return cmd;
}

// ---------------------------------------------------------------------------
// Help texts
// ---------------------------------------------------------------------------

constexpr std::string_view kGeneralHelp = R"(Usage:
  loganalyzer <command> [options]

Commands:
  analyze <file>      Parse, filter and analyse a log file; print or save a report
  benchmark <file>    Compare single- and multithreaded processing speed
  generate <file>     Write a synthetic log file (for testing and benchmarks)
  help [command]      Show help for a command

Global options:
  -h, --help          Show this help
  -v, --version       Show version information

Log format:
  [YYYY-MM-DD HH:MM:SS] [LEVEL] [optional source.cpp:42] message
  Levels: TRACE, DEBUG, INFO, WARNING, ERROR, CRITICAL

Run 'loganalyzer <command> --help' for the options of a command.
)";

constexpr std::string_view kFilterHelp = R"(
Filtering (all given filters must match):
  -l, --level <LIST>        Levels, comma-separated or repeated: --level WARNING,ERROR
  -k, --keyword <LIST>      Keywords searched in the message: --keyword db,timeout
      --match <any|all>     With several keywords: any one (default) or all of them
  -c, --case-sensitive      Case-sensitive keyword search (default: insensitive)
      --from <TIME>         Only entries at or after TIME
      --to <TIME>           Only entries at or before TIME
                            TIME is "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DD"

Analysis:
      --top <N>             Length of the top-N lists (default: 10)
      --period <P>          Bucket for "most active periods": minute, hour (default), day
  -d, --duplicates          Detect repeated messages (timestamps are ignored)
)";

constexpr std::string_view kAnalyzeHelp = R"(Usage:
  loganalyzer analyze <file> [options]

Processing:
  -t, --threads <N|auto>    Worker threads: 1 = single-threaded, auto = one per
                            hardware thread, capped by the number of chunks
                            (default: auto, max 256)
      --chunk-size <KiB>    Size of the blocks handed to workers (default: 1024)
)";

constexpr std::string_view kAnalyzeOutputHelp = R"(      --samples <N>         Matching / invalid lines included in the report (default: 20)

Output:
  -f, --format <F>          txt (default), csv or json
  -o, --output <FILE>       Write the report to FILE instead of the console;
                            without --format the format follows the extension

Examples:
  loganalyzer analyze app.log
  loganalyzer analyze app.log --threads 4 --level ERROR,CRITICAL
  loganalyzer analyze app.log --keyword database --from "2026-10-20 10:00:00"
  loganalyzer analyze app.log --duplicates --format json --output report.json
)";

constexpr std::string_view kBenchmarkHelp = R"(Usage:
  loganalyzer benchmark <file> [options]

Runs the full pipeline (read, parse, filter, analyse) with each thread count,
reports median time, throughput, speedup and efficiency, and verifies that
every configuration produces exactly the same analysis results.

Benchmark:
  -t, --threads <LIST>      Thread counts to compare (default: 1,2,4,8; 1 is always run)
  -n, --iterations <N>      Timed runs per thread count, median reported (default: 3)
      --no-warmup           Skip the untimed warm-up run
      --chunk-size <KiB>    Size of the blocks handed to workers (default: 1024)
)";

constexpr std::string_view kBenchmarkExamples = R"(
Examples:
  loganalyzer generate large.log --lines 2000000
  loganalyzer benchmark large.log
  loganalyzer benchmark large.log --threads 1,2,4,8,16 --iterations 5 --duplicates
)";

constexpr std::string_view kGenerateHelp = R"(Usage:
  loganalyzer generate <file> [options]

Writes a realistic synthetic log. The same seed always produces the same file.

Options:
      --lines <N>           Number of lines (default: 100000)
      --seed <N>            Random seed (default: 42)
      --malformed-rate <R>  Fraction of deliberately broken lines, 0..1 (default: 0.001)

Example:
  loganalyzer generate samples/large.log --lines 2000000 --seed 7
)";

} // namespace

std::string CommandLineParser::helpText(std::string_view topic) {
    std::string text = std::format("{} {} - {}\n\n", version::kName, version::kVersion, version::kDescription);
    if (topic.empty()) {
        text += kGeneralHelp;
    } else if (topic == "analyze") {
        text += kAnalyzeHelp;
        text += kFilterHelp;
        text += kAnalyzeOutputHelp;
    } else if (topic == "benchmark") {
        text += kBenchmarkHelp;
        text += kFilterHelp;
        text += kBenchmarkExamples;
    } else if (topic == "generate") {
        text += kGenerateHelp;
    } else {
        fail(std::format("no help available for unknown command '{}'", topic));
    }
    return text;
}

Command CommandLineParser::parse(const std::vector<std::string_view>& args) {
    if (args.empty()) {
        fail("no command given");
    }
    const std::string_view command = args.front();

    if (command == "-h" || command == "--help") {
        return HelpCommand{};
    }
    if (command == "help") {
        if (args.size() > 2) {
            fail("'help' takes at most one argument");
        }
        HelpCommand help{args.size() == 2 ? std::string(args[1]) : std::string()};
        (void)helpText(help.topic); // validate the topic now
        return help;
    }
    if (command == "-v" || command == "--version" || command == "version") {
        if (args.size() > 1) {
            fail(std::format("unexpected argument '{}' after '{}'", args[1], command));
        }
        return VersionCommand{};
    }
    if (command == "analyze") {
        return parseAnalyze(args);
    }
    if (command == "benchmark") {
        return parseBenchmark(args);
    }
    if (command == "generate") {
        return parseGenerate(args);
    }
    if (command.starts_with('-')) {
        fail(std::format("unknown option '{}'", command));
    }
    fail(std::format("unknown command '{}'", command));
}

} // namespace loganalyzer
