#include "reporting/ReportFormatter.h"

#include "util/TimeUtils.h"

#include <format>
#include <string>

namespace loganalyzer {

namespace {

/// Quotes a field per RFC 4180 when needed.
///
/// `untrusted` marks text that came from the log file. Such text is also
/// protected against CSV/formula injection: a cell starting with = + - @ would
/// be executed as a formula by spreadsheet software, so it is prefixed with a
/// single quote (the OWASP-recommended mitigation).
std::string csv(std::string_view value, bool untrusted = false) {
    std::string text;
    if (untrusted && !value.empty() &&
        (value.front() == '=' || value.front() == '+' || value.front() == '-' || value.front() == '@' ||
         value.front() == '\t' || value.front() == '\r')) {
        text = "'";
    }
    text += value;

    const bool needsQuotes = text.find_first_of(",\"\r\n") != std::string::npos ||
                             (!text.empty() && (text.front() == ' ' || text.back() == ' '));
    if (!needsQuotes) {
        return text;
    }
    std::string quoted = "\"";
    for (const char c : text) {
        if (c == '"') {
            quoted += '"'; // a quote inside a quoted field is doubled
        }
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

std::string optionalTimestamp(const std::optional<Timestamp>& timestamp) {
    return timestamp ? util::formatTimestamp(*timestamp) : "";
}

void messageRows(std::ostream& out, std::string_view section, const std::vector<MessageFrequency>& rows) {
    out << "section,rank,message,count,first_timestamp,first_line,last_timestamp,last_line\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        out << section << ',' << (i + 1) << ',' << csv(r.message, true) << ',' << r.stats.count << ','
            << util::formatTimestamp(r.stats.first.timestamp) << ',' << r.stats.first.lineNumber << ','
            << util::formatTimestamp(r.stats.last.timestamp) << ',' << r.stats.last.lineNumber << '\n';
    }
}

} // namespace

void CsvReportFormatter::write(const AnalysisReport& report, std::ostream& out) const {
    const ProcessingSummary& s = report.summary;
    const PerformanceMetrics& p = report.performance;

    const auto metric = [&out](std::string_view name, const std::string& value) {
        out << "summary," << name << ',' << value << '\n';
    };

    out << "section,metric,value\n";
    metric("input", csv(report.inputName, true));
    std::string filters;
    for (std::size_t i = 0; i < report.filterDescriptions.size(); ++i) {
        filters += (i == 0 ? "" : "; ") + report.filterDescriptions[i];
    }
    metric("filters", csv(filters));
    metric("total_lines", std::to_string(s.totalLines));
    metric("valid_entries", std::to_string(s.validEntries));
    metric("invalid_entries", std::to_string(s.invalidEntries));
    metric("empty_lines", std::to_string(s.emptyLines));
    metric("matched_entries", std::to_string(s.matchedEntries));
    metric("error_count", std::to_string(report.errorCount));
    metric("error_percentage", std::format("{:.2f}", report.errorPercentage));
    metric("first_timestamp", optionalTimestamp(report.firstTimestamp));
    metric("last_timestamp", optionalTimestamp(report.lastTimestamp));
    metric("distinct_error_messages", std::to_string(report.distinctErrorMessages));
    metric("threads", std::to_string(p.threadCount));
    metric("chunks", std::to_string(p.chunkCount));
    metric("bytes_processed", std::to_string(p.bytesProcessed));
    metric("duration_ms", std::format("{:.3f}", p.seconds() * 1000.0));
    metric("lines_per_second", std::format("{:.0f}", report.linesPerSecond));
    metric("megabytes_per_second", std::format("{:.2f}", report.megabytesPerSecond));

    out << "\nsection,level,count,percentage\n";
    for (const auto& level : report.levelDistribution) {
        out << "level_distribution," << toString(level.level) << ',' << level.count << ','
            << std::format("{:.2f}", level.percentage) << '\n';
    }

    out << '\n';
    messageRows(out, "top_errors", report.topErrors);

    out << "\nsection,granularity,period_start,count\n";
    for (const auto& period : report.mostActivePeriods) {
        out << "active_periods," << toString(report.activityBucket) << ',' << util::formatTimestamp(period.start)
            << ',' << period.count << '\n';
    }

    if (report.duplicatesEnabled) {
        out << '\n';
        messageRows(out, "duplicates", report.duplicates);
    }

    if (!report.filterDescriptions.empty()) {
        out << "\nsection,line,timestamp,level,source_file,source_line,message\n";
        for (const auto& e : report.matchedSamples) {
            out << "matched_entries," << e.lineNumber << ',' << util::formatTimestamp(e.timestamp) << ','
                << toString(e.level) << ',' << csv(e.sourceFile.value_or(""), true) << ','
                << (e.sourceLine ? std::to_string(*e.sourceLine) : "") << ',' << csv(e.message, true) << '\n';
        }
    }

    if (!report.invalidSamples.empty()) {
        out << "\nsection,line,error,content\n";
        for (const auto& invalid : report.invalidSamples) {
            out << "invalid_entries," << invalid.lineNumber << ',' << csv(describe(invalid.error)) << ','
                << csv(invalid.excerpt, true) << '\n';
        }
    }
}

} // namespace loganalyzer
