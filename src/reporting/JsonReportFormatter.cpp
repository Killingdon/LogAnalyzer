#include "reporting/ReportFormatter.h"

#include "JsonWriter.h"
#include "common/Version.h"
#include "util/TimeUtils.h"

namespace loganalyzer {

namespace {

using detail::JsonWriter;

void writeOptionalTimestamp(JsonWriter& w, const std::optional<Timestamp>& timestamp) {
    if (timestamp) {
        w.value(util::formatTimestamp(*timestamp));
    } else {
        w.null();
    }
}

void writeOccurrence(JsonWriter& w, const Occurrence& o) {
    w.beginObject();
    w.key("line").value(static_cast<std::uint64_t>(o.lineNumber));
    w.key("timestamp").value(util::formatTimestamp(o.timestamp));
    w.endObject();
}

void writeMessages(JsonWriter& w, const std::vector<MessageFrequency>& rows) {
    w.beginArray();
    for (const auto& row : rows) {
        w.beginObject();
        w.key("message").value(row.message);
        w.key("count").value(row.stats.count);
        w.key("first_occurrence");
        writeOccurrence(w, row.stats.first);
        w.key("last_occurrence");
        writeOccurrence(w, row.stats.last);
        w.endObject();
    }
    w.endArray();
}

} // namespace

void JsonReportFormatter::write(const AnalysisReport& report, std::ostream& out) const {
    const ProcessingSummary& s = report.summary;
    const PerformanceMetrics& p = report.performance;
    JsonWriter w(out);

    w.beginObject();

    w.key("tool").beginObject();
    w.key("name").value(version::kName);
    w.key("version").value(version::kVersion);
    w.endObject();

    w.key("input").value(report.inputName);
    w.key("filters").beginArray();
    for (const auto& description : report.filterDescriptions) {
        w.value(description);
    }
    w.endArray();

    w.key("summary").beginObject();
    w.key("total_lines").value(s.totalLines);
    w.key("valid_entries").value(s.validEntries);
    w.key("invalid_entries").value(s.invalidEntries);
    w.key("empty_lines").value(s.emptyLines);
    w.key("matched_entries").value(s.matchedEntries);
    w.key("error_count").value(report.errorCount);
    w.key("error_percentage").value(report.errorPercentage);
    w.key("first_timestamp");
    writeOptionalTimestamp(w, report.firstTimestamp);
    w.key("last_timestamp");
    writeOptionalTimestamp(w, report.lastTimestamp);
    w.endObject();

    w.key("level_distribution").beginArray();
    for (const auto& level : report.levelDistribution) {
        w.beginObject();
        w.key("level").value(toString(level.level));
        w.key("count").value(level.count);
        w.key("percentage").value(level.percentage);
        w.endObject();
    }
    w.endArray();

    w.key("distinct_error_messages").value(report.distinctErrorMessages);
    w.key("top_errors");
    writeMessages(w, report.topErrors);

    w.key("most_active_periods").beginObject();
    w.key("granularity").value(toString(report.activityBucket));
    w.key("periods").beginArray();
    for (const auto& period : report.mostActivePeriods) {
        w.beginObject();
        w.key("start").value(util::formatTimestamp(period.start));
        w.key("count").value(period.count);
        w.endObject();
    }
    w.endArray();
    w.endObject();

    w.key("duplicates");
    if (report.duplicatesEnabled) {
        w.beginObject();
        w.key("repeated_message_count").value(report.duplicateMessageCount);
        w.key("top");
        writeMessages(w, report.duplicates);
        w.endObject();
    } else {
        w.null();
    }

    w.key("matched_entries_sample").beginArray();
    for (const auto& e : report.matchedSamples) {
        w.beginObject();
        w.key("line").value(static_cast<std::uint64_t>(e.lineNumber));
        w.key("timestamp").value(util::formatTimestamp(e.timestamp));
        w.key("level").value(toString(e.level));
        w.key("source_file");
        e.sourceFile ? w.value(*e.sourceFile) : w.null();
        w.key("source_line");
        e.sourceLine ? w.value(static_cast<std::uint64_t>(*e.sourceLine)) : w.null();
        w.key("message").value(e.message);
        w.endObject();
    }
    w.endArray();

    w.key("invalid_entries_sample").beginArray();
    for (const auto& invalid : report.invalidSamples) {
        w.beginObject();
        w.key("line").value(static_cast<std::uint64_t>(invalid.lineNumber));
        w.key("error").value(describe(invalid.error));
        w.key("content").value(invalid.excerpt);
        w.endObject();
    }
    w.endArray();

    w.key("performance").beginObject();
    w.key("threads").value(static_cast<std::uint64_t>(p.threadCount));
    w.key("chunks").value(static_cast<std::uint64_t>(p.chunkCount));
    w.key("bytes_processed").value(p.bytesProcessed);
    w.key("duration_ms").value(p.seconds() * 1000.0, 3);
    w.key("lines_per_second").value(report.linesPerSecond, 0);
    w.key("megabytes_per_second").value(report.megabytesPerSecond);
    w.endObject();

    w.endObject();
}

} // namespace loganalyzer
