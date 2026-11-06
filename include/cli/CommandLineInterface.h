#pragma once

#include "cli/CommandLineParser.h"

#include <ostream>
#include <string_view>
#include <vector>

namespace loganalyzer {

/// Process exit codes.
enum ExitCode : int {
    kExitSuccess = 0,
    kExitFailure = 1, ///< runtime problem: missing file, unwritable output, ...
    kExitUsage = 2,   ///< invalid command line
};

/// Application entry point: parses the command line, runs the command and maps
/// every error to a message plus an exit code. Nothing escapes run().
///
/// Output goes to injected streams rather than std::cout directly, so tests
/// can capture it with std::ostringstream (dependency injection).
class CommandLineInterface {
public:
    CommandLineInterface(std::ostream& out, std::ostream& err) : out_(out), err_(err) {}

    [[nodiscard]] int run(int argc, const char* const* argv) noexcept;
    [[nodiscard]] int run(const std::vector<std::string_view>& args) noexcept;

private:
    int execute(const HelpCommand& command);
    int execute(const VersionCommand& command);
    int execute(const AnalyzeCommand& command);
    int execute(const BenchmarkCommand& command);
    int execute(const GenerateCommand& command);

    std::ostream& out_;
    std::ostream& err_;
};

} // namespace loganalyzer
