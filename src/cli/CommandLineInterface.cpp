#include "cli/CommandLineInterface.h"

#include "analysis/LogAnalyzer.h"
#include "benchmarking/BenchmarkRunner.h"
#include "common/Errors.h"
#include "common/Version.h"
#include "generation/SampleLogGenerator.h"
#include "processing/LogProcessor.h"
#include "reporting/ReportGenerator.h"
#include "util/StringUtils.h"
#include "util/TimeUtils.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <new>

namespace loganalyzer {

int CommandLineInterface::run(int argc, const char* const* argv) noexcept {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return run(args);
}

int CommandLineInterface::run(const std::vector<std::string_view>& args) noexcept {
    // Exception boundary of the application: the most specific handlers come
    // first. Each kind of failure gets a clear message and a distinct exit code.
    try {
        const Command command = CommandLineParser::parse(args);
        return std::visit([this](const auto& cmd) { return execute(cmd); }, command);
    } catch (const CommandLineError& e) {
        err_ << "error: " << e.what() << "\nRun 'loganalyzer --help' for usage.\n";
        return kExitUsage;
    } catch (const LogAnalyzerError& e) {
        err_ << "error: " << e.what() << '\n';
        return kExitFailure;
    } catch (const std::bad_alloc&) {
        err_ << "error: out of memory\n";
        return kExitFailure;
    } catch (const std::exception& e) {
        err_ << "unexpected error: " << e.what() << '\n';
        return kExitFailure;
    } catch (...) {
        err_ << "unexpected error\n";
        return kExitFailure;
    }
}

int CommandLineInterface::execute(const HelpCommand& command) {
    out_ << CommandLineParser::helpText(command.topic);
    return kExitSuccess;
}

int CommandLineInterface::execute(const VersionCommand&) {
    out_ << version::kName << ' ' << version::kVersion << '\n'
         << "Built with " << version::kCompiler << ", C++ standard " << __cplusplus << '\n';
    return kExitSuccess;
}

int CommandLineInterface::execute(const AnalyzeCommand& command) {
    // Fail fast: reject an unusable output path before a potentially long run.
    if (command.output) {
        ReportGenerator::validateOutputPath(*command.output);
    }

    LogFilter filter = LogFilter::fromOptions(command.filter);
    std::vector<std::string> filterDescriptions = filter.describe();

    const LogProcessor processor(std::move(filter), command.processing, command.analysis);
    const ProcessingResult result = processor.process(command.input);

    if (result.summary.totalLines == 0) {
        err_ << "warning: '" << command.input.string() << "' is empty\n";
    } else if (result.summary.validEntries == 0) {
        err_ << "warning: no valid log entries found in '" << command.input.string()
             << "' (expected lines like \"[2026-10-20 14:30:22] [INFO] message\")\n";
    }

    const AnalysisReport report =
        LogAnalyzer(command.analysis).analyze(result, command.input.string(), std::move(filterDescriptions));
    const ReportGenerator generator(command.format);

    if (!command.output) {
        generator.write(report, out_);
        return kExitSuccess;
    }

    generator.writeToFile(report, *command.output);
    out_ << std::format("Analysed {} lines ({} valid, {} invalid, {} matching) in {} using {} thread(s).\n",
                        util::formatWithSeparators(result.summary.totalLines),
                        util::formatWithSeparators(result.summary.validEntries),
                        util::formatWithSeparators(result.summary.invalidEntries),
                        util::formatWithSeparators(result.summary.matchedEntries),
                        util::formatDuration(result.performance.duration), result.performance.threadCount);
    out_ << std::format("{} report written to {}\n", util::toUpper(toString(command.format)),
                        command.output->string());
    return kExitSuccess;
}

int CommandLineInterface::execute(const BenchmarkCommand& command) {
    const BenchmarkRunner runner(command.options);
    // Progress goes to the error stream so that stdout contains only the table.
    const BenchmarkReport report =
        runner.run(command.input, [this](const std::string& step) { err_ << "  running: " << step << '\n'; });
    err_ << '\n';
    BenchmarkRunner::print(report, out_);
    // Differing results mean a concurrency bug: make that visible to scripts.
    return report.allResultsMatch ? kExitSuccess : kExitFailure;
}

int CommandLineInterface::execute(const GenerateCommand& command) {
    const SampleLogGenerator generator(command.options);
    const auto start = std::chrono::steady_clock::now();
    generator.generateToFile(command.output);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    std::error_code ec;
    const auto bytes = std::filesystem::file_size(command.output, ec);
    out_ << std::format("Generated {} lines ({:.1f} MiB) in {} -> {}\n",
                        util::formatWithSeparators(command.options.lines),
                        ec ? 0.0 : static_cast<double>(bytes) / (1024.0 * 1024.0), util::formatDuration(elapsed),
                        command.output.string());
    return kExitSuccess;
}

} // namespace loganalyzer
