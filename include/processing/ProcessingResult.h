#pragma once

#include "analysis/StatisticsCollector.h"
#include "models/LogEntry.h"
#include "parser/LogParser.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace loganalyzer {

/// Line counters for one chunk or a whole file. Invariant:
/// totalLines == validEntries + invalidEntries + emptyLines.
struct ProcessingSummary {
    std::uint64_t totalLines = 0;
    std::uint64_t validEntries = 0;
    std::uint64_t invalidEntries = 0;
    std::uint64_t emptyLines = 0;
    std::uint64_t matchedEntries = 0; ///< valid entries that passed the filter

    ProcessingSummary& operator+=(const ProcessingSummary& other) noexcept {
        totalLines += other.totalLines;
        validEntries += other.validEntries;
        invalidEntries += other.invalidEntries;
        emptyLines += other.emptyLines;
        matchedEntries += other.matchedEntries;
        return *this;
    }

    bool operator==(const ProcessingSummary&) const = default;
};

/// A line that failed to parse, kept for the "invalid entries" report section.
struct InvalidLine {
    std::size_t lineNumber = 0;
    ParseErrorCode error = ParseErrorCode::MissingTimestamp;
    std::string excerpt; ///< the offending text, truncated to a sensible length

    bool operator==(const InvalidLine&) const = default;
};

/// How long processing took and with what resources.
struct PerformanceMetrics {
    std::chrono::nanoseconds duration{0};
    unsigned threadCount = 1;
    std::size_t chunkCount = 0;
    std::uint64_t bytesProcessed = 0;

    [[nodiscard]] double seconds() const noexcept {
        return std::chrono::duration<double>(duration).count();
    }
};

/// Raw output of LogProcessor: counters, accumulated statistics and samples.
/// LogAnalyzer turns it into a presentation-ready AnalysisReport.
struct ProcessingResult {
    ProcessingSummary summary;
    StatisticsCollector statistics;          ///< over the entries that passed the filter
    std::vector<InvalidLine> invalidSamples; ///< first N invalid lines, in file order
    std::vector<LogEntry> matchedSamples;    ///< first N entries that passed the filter, in file order
    PerformanceMetrics performance;
};

} // namespace loganalyzer
