#include "reporting/ReportGenerator.h"

#include "common/Errors.h"
#include "util/StringUtils.h"

#include <fstream>
#include <sstream>

namespace loganalyzer {

std::string_view toString(ReportFormat format) noexcept {
    switch (format) {
    case ReportFormat::Text:
        return "txt";
    case ReportFormat::Csv:
        return "csv";
    case ReportFormat::Json:
        return "json";
    }
    return "unknown";
}

std::optional<ReportFormat> parseReportFormat(std::string_view text) noexcept {
    if (util::equalsIgnoreCase(text, "txt") || util::equalsIgnoreCase(text, "text")) {
        return ReportFormat::Text;
    }
    if (util::equalsIgnoreCase(text, "csv")) {
        return ReportFormat::Csv;
    }
    if (util::equalsIgnoreCase(text, "json")) {
        return ReportFormat::Json;
    }
    return std::nullopt;
}

ReportGenerator::ReportGenerator(ReportFormat format) : formatter_(createFormatter(format)) {}

ReportGenerator::ReportGenerator(std::unique_ptr<ReportFormatter> formatter) : formatter_(std::move(formatter)) {
    if (!formatter_) {
        throw ReportError("ReportGenerator requires a formatter");
    }
}

std::unique_ptr<ReportFormatter> ReportGenerator::createFormatter(ReportFormat format) {
    switch (format) {
    case ReportFormat::Text:
        return std::make_unique<TextReportFormatter>();
    case ReportFormat::Csv:
        return std::make_unique<CsvReportFormatter>();
    case ReportFormat::Json:
        return std::make_unique<JsonReportFormatter>();
    }
    throw ReportError("unsupported report format");
}

void ReportGenerator::write(const AnalysisReport& report, std::ostream& out) const {
    formatter_->write(report, out);
}

std::string ReportGenerator::toString(const AnalysisReport& report) const {
    std::ostringstream out;
    write(report, out);
    return std::move(out).str();
}

void ReportGenerator::validateOutputPath(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    if (path.empty()) {
        throw ReportError("output path is empty");
    }
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        throw ReportError("cannot write report to '" + path.string() + "': it is a directory");
    }
    const fs::path parent = path.parent_path();
    if (!parent.empty() && !fs::is_directory(parent, ec)) {
        throw ReportError("cannot write report to '" + path.string() + "': directory '" + parent.string() +
                          "' does not exist");
    }
}

void ReportGenerator::writeToFile(const AnalysisReport& report, const std::filesystem::path& path) const {
    validateOutputPath(path);

    // Binary mode: identical bytes ("\n" line endings) on every platform.
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        throw ReportError("cannot open '" + path.string() + "' for writing (check permissions)");
    }
    write(report, out);
    out.flush();
    if (!out) {
        throw ReportError("failed while writing '" + path.string() + "' (disk full?)");
    }
}

} // namespace loganalyzer
