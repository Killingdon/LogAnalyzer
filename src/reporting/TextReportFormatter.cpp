#include "reporting/ReportFormatter.h"

#include "util/StringUtils.h"
#include "util/TimeUtils.h"

#include <algorithm>
#include <format>
#include <string>

namespace loganalyzer {

namespace {

constexpr std::size_t kWidth = 78;
constexpr std::size_t kBarWidth = 30;
constexpr std::size_t kMaxMessageWidth = 60;

void heading(std::ostream& out, std::string_view title) {
    out << '\n' << title << '\n' << std::string(kWidth, '-') << '\n';
}

void field(std::ostream& out, std::string_view label, std::string_view value) {
    out << std::format(" {:<18}: {}\n", label, value);
}

std::string number(std::uint64_t value) {
    return util::formatWithSeparators(value);
}

std::string optionalTimestamp(const std::optional<Timestamp>& timestamp) {
    return timestamp ? util::formatTimestamp(*timestamp) : "-";
}

/// Proportional ASCII bar, e.g. "##########". Non-zero values get at least one '#'.
std::string bar(std::uint64_t value, std::uint64_t max) {
    if (value == 0 || max == 0) {
        return {};
    }
    const auto length = std::max<std::size_t>(
        1, static_cast<std::size_t>(static_cast<double>(value) / static_cast<double>(max) * kBarWidth));
    return std::string(length, '#');
}

std::string occurrence(const Occurrence& o) {
    return std::format("{} (line {})", util::formatTimestamp(o.timestamp), o.lineNumber);
}

void writeMessageTable(std::ostream& out, const std::vector<MessageFrequency>& rows) {
    out << std::format(" {:>3}  {:>7}  {:<30}  {:<30}\n", "#", "Count", "First occurrence", "Last occurrence");
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        out << std::format(" {:>3}  {:>7}  {:<30}  {:<30}\n", i + 1, number(row.stats.count),
                           occurrence(row.stats.first), occurrence(row.stats.last));
        out << "      " << util::truncate(row.message, kMaxMessageWidth + 10) << '\n';
    }
}

} // namespace

void TextReportFormatter::write(const AnalysisReport& report, std::ostream& out) const {
    const ProcessingSummary& s = report.summary;

    out << std::string(kWidth, '=') << '\n'
        << " LogAnalyzer Report\n"
        << std::string(kWidth, '=') << '\n';
    field(out, "Input", report.inputName);
    if (report.filterDescriptions.empty()) {
        field(out, "Filters", "none");
    } else {
        for (std::size_t i = 0; i < report.filterDescriptions.size(); ++i) {
            field(out, i == 0 ? "Filters" : "", report.filterDescriptions[i]);
        }
    }

    // ---- Summary -----------------------------------------------------------
    heading(out, "PROCESSING SUMMARY");
    field(out, "Total lines", number(s.totalLines));
    field(out, "Valid entries", number(s.validEntries));
    field(out, "Invalid entries", number(s.invalidEntries));
    field(out, "Empty lines", number(s.emptyLines));
    if (!report.filterDescriptions.empty()) {
        field(out, "Matching filter", number(s.matchedEntries));
    }
    field(out, "Errors", std::format("{} ({:.2f}% of analysed entries)", number(report.errorCount),
                                     report.errorPercentage));
    field(out, "First timestamp", optionalTimestamp(report.firstTimestamp));
    field(out, "Last timestamp", optionalTimestamp(report.lastTimestamp));

    // ---- Level distribution -------------------------------------------------
    heading(out, "LOG LEVEL DISTRIBUTION");
    std::uint64_t maxCount = 0;
    for (const auto& level : report.levelDistribution) {
        maxCount = std::max(maxCount, level.count);
    }
    for (const auto& level : report.levelDistribution) {
        out << std::format(" {:<9} {:>12} {:>7.2f}%  {}\n", toString(level.level), number(level.count),
                           level.percentage, bar(level.count, maxCount));
    }

    // ---- Top errors ----------------------------------------------------------
    heading(out, std::format("TOP ERRORS ({} distinct ERROR/CRITICAL messages)", report.distinctErrorMessages));
    if (report.topErrors.empty()) {
        out << " No errors found.\n";
    } else {
        writeMessageTable(out, report.topErrors);
    }

    // ---- Active periods ---------------------------------------------------------
    heading(out, std::format("MOST ACTIVE PERIODS (per {})", toString(report.activityBucket)));
    if (report.mostActivePeriods.empty()) {
        out << " No entries.\n";
    } else {
        const std::uint64_t maxPeriod = report.mostActivePeriods.front().count;
        for (const auto& period : report.mostActivePeriods) {
            out << std::format(" {}  {:>12} {:<7}  {}\n", util::formatTimestamp(period.start),
                               number(period.count), period.count == 1 ? "entry" : "entries",
                               bar(period.count, maxPeriod));
        }
    }

    // ---- Duplicates ---------------------------------------------------------------
    if (report.duplicatesEnabled) {
        heading(out, std::format("DUPLICATE MESSAGES ({} distinct messages occur more than once)",
                                 number(report.duplicateMessageCount)));
        if (report.duplicates.empty()) {
            out << " No duplicate messages.\n";
        } else {
            writeMessageTable(out, report.duplicates);
        }
    }

    // ---- Filter results -------------------------------------------------------------
    if (!report.filterDescriptions.empty()) {
        heading(out, std::format("MATCHING ENTRIES (showing {} of {})", report.matchedSamples.size(),
                                 number(s.matchedEntries)));
        if (report.matchedSamples.empty()) {
            out << " No entries match the filter.\n";
        }
        for (const auto& entry : report.matchedSamples) {
            std::string location;
            if (entry.sourceFile) {
                location = std::format("[{}:{}] ", *entry.sourceFile, entry.sourceLine.value_or(0));
            }
            out << std::format(" {:>7}  {}  {:<8}  {}{}\n", entry.lineNumber, util::formatTimestamp(entry.timestamp),
                               toString(entry.level), location, util::truncate(entry.message, kMaxMessageWidth));
        }
    }

    // ---- Invalid lines ------------------------------------------------------------------
    if (s.invalidEntries > 0) {
        heading(out, std::format("INVALID ENTRIES (showing {} of {})", report.invalidSamples.size(),
                                 number(s.invalidEntries)));
        for (const auto& invalid : report.invalidSamples) {
            out << std::format(" line {:<6} {}\n", invalid.lineNumber, describe(invalid.error));
            out << "             > " << util::truncate(invalid.excerpt, kMaxMessageWidth) << '\n';
        }
    }

    // ---- Performance --------------------------------------------------------------------------
    const PerformanceMetrics& p = report.performance;
    heading(out, "PERFORMANCE");
    field(out, "Threads", p.threadCount == 1 ? "1 (single-threaded)" : std::to_string(p.threadCount));
    field(out, "Chunks", number(p.chunkCount));
    field(out, "Bytes processed", number(p.bytesProcessed));
    field(out, "Processing time", util::formatDuration(p.duration));
    field(out, "Throughput", std::format("{} lines/s ({:.1f} MiB/s)",
                                         number(static_cast<std::uint64_t>(report.linesPerSecond)),
                                         report.megabytesPerSecond));
    out << std::string(kWidth, '=') << '\n';
}

} // namespace loganalyzer
