#pragma once

#include "analysis/LogAnalyzer.h"

#include <optional>
#include <ostream>
#include <string_view>

namespace loganalyzer {

enum class ReportFormat { Text, Csv, Json };

[[nodiscard]] std::string_view toString(ReportFormat format) noexcept;

/// Accepts "txt", "text", "csv", "json" (case-insensitive).
[[nodiscard]] std::optional<ReportFormat> parseReportFormat(std::string_view text) noexcept;

/// Strategy interface: one implementation per output format. ReportGenerator
/// depends only on this interface, so adding e.g. an HTML or Markdown report
/// means adding one class, without touching existing code.
class ReportFormatter {
public:
    virtual ~ReportFormatter() = default;

    virtual void write(const AnalysisReport& report, std::ostream& out) const = 0;
    [[nodiscard]] virtual ReportFormat format() const noexcept = 0;

protected:
    ReportFormatter() = default;
    ReportFormatter(const ReportFormatter&) = default;
    ReportFormatter& operator=(const ReportFormatter&) = default;
};

/// Human-readable report for the console or a .txt file.
class TextReportFormatter final : public ReportFormatter {
public:
    void write(const AnalysisReport& report, std::ostream& out) const override;
    [[nodiscard]] ReportFormat format() const noexcept override { return ReportFormat::Text; }
};

/// Spreadsheet-friendly report: one table per section, separated by blank
/// lines; the first column of every row names its section.
class CsvReportFormatter final : public ReportFormatter {
public:
    void write(const AnalysisReport& report, std::ostream& out) const override;
    [[nodiscard]] ReportFormat format() const noexcept override { return ReportFormat::Csv; }
};

/// Machine-readable report (a single JSON object, UTF-8).
class JsonReportFormatter final : public ReportFormatter {
public:
    void write(const AnalysisReport& report, std::ostream& out) const override;
    [[nodiscard]] ReportFormat format() const noexcept override { return ReportFormat::Json; }
};

} // namespace loganalyzer
