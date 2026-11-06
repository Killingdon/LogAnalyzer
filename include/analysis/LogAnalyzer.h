#pragma once

#include "analysis/StatisticsCollector.h"
#include "processing/ProcessingResult.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace loganalyzer {

struct LevelStatistic {
    LogLevel level = LogLevel::Info;
    std::uint64_t count = 0;
    double percentage = 0.0; ///< of analysed entries

    bool operator==(const LevelStatistic&) const = default;
};

struct MessageFrequency {
    std::string message;
    MessageStats stats;

    bool operator==(const MessageFrequency&) const = default;
};

struct ActivityPeriod {
    Timestamp start{};
    std::uint64_t count = 0;

    bool operator==(const ActivityPeriod&) const = default;
};

/// Everything a report shows: the finished, presentation-ready analysis.
///
/// "Analysed entries" are the valid entries that passed the filter; with no
/// filter that is every valid entry.
struct AnalysisReport {
    std::string inputName;
    ProcessingSummary summary;
    std::vector<std::string> filterDescriptions; ///< empty = no filter

    std::vector<LevelStatistic> levelDistribution; ///< all levels, TRACE..CRITICAL
    std::uint64_t errorCount = 0;                  ///< ERROR + CRITICAL
    double errorPercentage = 0.0;
    std::optional<Timestamp> firstTimestamp;
    std::optional<Timestamp> lastTimestamp;

    std::vector<MessageFrequency> topErrors; ///< most frequent ERROR/CRITICAL messages
    std::uint64_t distinctErrorMessages = 0;

    TimeBucket activityBucket = TimeBucket::Hour;
    std::vector<ActivityPeriod> mostActivePeriods;

    bool duplicatesEnabled = false;
    std::vector<MessageFrequency> duplicates; ///< messages seen more than once (top N)
    std::uint64_t duplicateMessageCount = 0;  ///< distinct messages seen more than once

    std::vector<LogEntry> matchedSamples;
    std::vector<InvalidLine> invalidSamples;

    PerformanceMetrics performance;
    double linesPerSecond = 0.0;
    double megabytesPerSecond = 0.0;
};

/// True if the two reports agree on every analytical result (everything except
/// timing). Used to prove that multithreaded runs match single-threaded ones.
[[nodiscard]] bool haveSameResults(const AnalysisReport& a, const AnalysisReport& b);

/// Turns raw collected statistics into a finished AnalysisReport:
/// percentages, sorted top-N lists, throughput.
class LogAnalyzer {
public:
    explicit LogAnalyzer(AnalysisOptions options = {});

    [[nodiscard]] AnalysisReport analyze(const ProcessingResult& result, std::string inputName,
                                         std::vector<std::string> filterDescriptions = {}) const;

    [[nodiscard]] const AnalysisOptions& options() const noexcept { return options_; }

private:
    AnalysisOptions options_;
};

} // namespace loganalyzer
