#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

namespace loganalyzer {

/// Root of the application's exception hierarchy. Catching LogAnalyzerError
/// catches every "expected" failure (bad input, bad arguments, I/O problems),
/// while programming errors still surface as std::logic_error and friends.
class LogAnalyzerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// An input or output file could not be found, opened, read or written.
class FileError : public LogAnalyzerError {
public:
    FileError(std::filesystem::path path, const std::string& reason)
        : LogAnalyzerError("'" + path.string() + "': " + reason), path_(std::move(path)) {}

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

/// Options that are individually valid but inconsistent (e.g. --from after --to).
class ConfigurationError : public LogAnalyzerError {
public:
    using LogAnalyzerError::LogAnalyzerError;
};

/// A worker failed while processing a chunk of the input.
class ProcessingError : public LogAnalyzerError {
public:
    using LogAnalyzerError::LogAnalyzerError;
};

/// The thread pool could not start, or a task was submitted after shutdown.
class ThreadPoolError : public LogAnalyzerError {
public:
    using LogAnalyzerError::LogAnalyzerError;
};

/// A report could not be produced or written.
class ReportError : public LogAnalyzerError {
public:
    using LogAnalyzerError::LogAnalyzerError;
};

/// Invalid command-line usage. Reported with exit code 2.
class CommandLineError : public LogAnalyzerError {
public:
    using LogAnalyzerError::LogAnalyzerError;
};

} // namespace loganalyzer
