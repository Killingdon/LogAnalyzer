#pragma once

#include "reporting/ReportFormatter.h"

#include <filesystem>
#include <memory>
#include <ostream>
#include <string>

namespace loganalyzer {

/// Produces a report in the requested format and writes it to a stream, a
/// string or a file.
class ReportGenerator {
public:
    explicit ReportGenerator(ReportFormat format);
    explicit ReportGenerator(std::unique_ptr<ReportFormatter> formatter);

    /// Factory: maps a format to its formatter (the only place that knows all of them).
    [[nodiscard]] static std::unique_ptr<ReportFormatter> createFormatter(ReportFormat format);

    void write(const AnalysisReport& report, std::ostream& out) const;
    [[nodiscard]] std::string toString(const AnalysisReport& report) const;

    /// Writes (or overwrites) `path`. Throws ReportError on any failure.
    void writeToFile(const AnalysisReport& report, const std::filesystem::path& path) const;

    /// Checks that `path` could be written: not a directory, parent directory
    /// exists. The CLI calls this *before* a long analysis so that a typo in
    /// --output fails immediately. Throws ReportError.
    static void validateOutputPath(const std::filesystem::path& path);

    [[nodiscard]] ReportFormat format() const noexcept { return formatter_->format(); }

private:
    std::unique_ptr<ReportFormatter> formatter_;
};

} // namespace loganalyzer
