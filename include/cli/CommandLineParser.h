#pragma once

#include "analysis/StatisticsCollector.h"
#include "benchmarking/BenchmarkRunner.h"
#include "filtering/LogFilter.h"
#include "generation/SampleLogGenerator.h"
#include "processing/LogProcessor.h"
#include "reporting/ReportFormatter.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace loganalyzer {

/// `loganalyzer analyze <file> [options]`
struct AnalyzeCommand {
    std::filesystem::path input;
    FilterOptions filter;
    AnalysisOptions analysis;
    ProcessingOptions processing;
    ReportFormat format = ReportFormat::Text;
    std::optional<std::filesystem::path> output; ///< stdout when absent
};

/// `loganalyzer benchmark <file> [options]`
struct BenchmarkCommand {
    std::filesystem::path input;
    BenchmarkOptions options;
};

/// `loganalyzer generate <file> [options]`
struct GenerateCommand {
    std::filesystem::path output;
    GeneratorOptions options;
};

/// `loganalyzer --help` or `loganalyzer <command> --help`
struct HelpCommand {
    std::string topic; ///< empty = general help, otherwise a command name
};

/// `loganalyzer --version`
struct VersionCommand {};

/// The parsed command line. std::variant gives a closed set of alternatives,
/// and std::visit makes the compiler check that every one is handled.
using Command = std::variant<HelpCommand, VersionCommand, AnalyzeCommand, BenchmarkCommand, GenerateCommand>;

/// Converts command-line arguments into a validated Command.
///
/// Pure function: no I/O and no side effects, so it is easy to unit test.
/// Every invalid input results in a CommandLineError with a message that tells
/// the user what is wrong and what was expected.
class CommandLineParser {
public:
    /// `args` excludes the program name (argv[0]).
    [[nodiscard]] static Command parse(const std::vector<std::string_view>& args);

    /// Help text for a topic ("" = general). Throws CommandLineError for an unknown topic.
    [[nodiscard]] static std::string helpText(std::string_view topic = {});
};

} // namespace loganalyzer
