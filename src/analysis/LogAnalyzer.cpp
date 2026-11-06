#include "analysis/LogAnalyzer.h"

#include <algorithm>
#include <vector>

namespace loganalyzer {

namespace {

double percentage(std::uint64_t part, std::uint64_t whole) noexcept {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

/// Ordering for "most frequent" lists: higher count first; ties broken by the
/// earliest first occurrence so the output is fully deterministic.
bool moreFrequent(const MessageMap::value_type* a, const MessageMap::value_type* b) noexcept {
    if (a->second.count != b->second.count) {
        return a->second.count > b->second.count;
    }
    return a->second.first.lineNumber < b->second.first.lineNumber;
}

/// Returns the `limit` most frequent messages with count >= minCount.
///
/// Works on pointers and std::partial_sort: only the top N elements are fully
/// sorted (O(n log N) instead of O(n log n)), and no message string is copied
/// until it is known to be in the result.
std::vector<MessageFrequency> topMessages(const MessageMap& messages, std::size_t limit, std::uint64_t minCount,
                                          std::uint64_t* matchingCount = nullptr) {
    std::vector<const MessageMap::value_type*> candidates;
    candidates.reserve(messages.size());
    for (const auto& item : messages) {
        if (item.second.count >= minCount) {
            candidates.push_back(&item);
        }
    }
    if (matchingCount != nullptr) {
        *matchingCount = candidates.size();
    }

    const std::size_t n = std::min(limit, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(n), candidates.end(),
                      moreFrequent);

    std::vector<MessageFrequency> result;
    result.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        result.push_back({candidates[i]->first, candidates[i]->second});
    }
    return result;
}

std::vector<ActivityPeriod> topPeriods(const StatisticsCollector& stats, std::size_t limit) {
    std::vector<ActivityPeriod> periods;
    periods.reserve(stats.activity().size());
    for (const auto& [bucketKey, count] : stats.activity()) {
        periods.push_back({Timestamp{std::chrono::seconds{bucketKey}}, count});
    }
    const std::size_t n = std::min(limit, periods.size());
    std::partial_sort(periods.begin(), periods.begin() + static_cast<std::ptrdiff_t>(n), periods.end(),
                      [](const ActivityPeriod& a, const ActivityPeriod& b) {
                          return a.count != b.count ? a.count > b.count : a.start < b.start;
                      });
    periods.resize(n);
    return periods;
}

} // namespace

LogAnalyzer::LogAnalyzer(AnalysisOptions options) : options_(options) {}

AnalysisReport LogAnalyzer::analyze(const ProcessingResult& result, std::string inputName,
                                    std::vector<std::string> filterDescriptions) const {
    const StatisticsCollector& stats = result.statistics;

    AnalysisReport report;
    report.inputName = std::move(inputName);
    report.summary = result.summary;
    report.filterDescriptions = std::move(filterDescriptions);

    const std::uint64_t analysed = stats.entryCount();
    report.levelDistribution.reserve(kLogLevelCount);
    for (const LogLevel level : kAllLogLevels) {
        const std::uint64_t count = stats.count(level);
        report.levelDistribution.push_back({level, count, percentage(count, analysed)});
    }
    report.errorCount = stats.count(LogLevel::Error) + stats.count(LogLevel::Critical);
    report.errorPercentage = percentage(report.errorCount, analysed);
    report.firstTimestamp = stats.earliest();
    report.lastTimestamp = stats.latest();

    report.topErrors = topMessages(stats.errorMessages(), options_.topCount, 1);
    report.distinctErrorMessages = stats.errorMessages().size();

    report.activityBucket = stats.bucket();
    report.mostActivePeriods = topPeriods(stats, options_.topCount);

    report.duplicatesEnabled = stats.detectsDuplicates();
    if (report.duplicatesEnabled) {
        report.duplicates = topMessages(stats.allMessages(), options_.topCount, 2, &report.duplicateMessageCount);
    }

    report.matchedSamples = result.matchedSamples;
    report.invalidSamples = result.invalidSamples;

    report.performance = result.performance;
    const double seconds = result.performance.seconds();
    if (seconds > 0.0) {
        report.linesPerSecond = static_cast<double>(result.summary.totalLines) / seconds;
        report.megabytesPerSecond = static_cast<double>(result.performance.bytesProcessed) / (1024.0 * 1024.0) / seconds;
    }
    return report;
}

bool haveSameResults(const AnalysisReport& a, const AnalysisReport& b) {
    return a.summary == b.summary && a.filterDescriptions == b.filterDescriptions &&
           a.levelDistribution == b.levelDistribution && a.errorCount == b.errorCount &&
           a.firstTimestamp == b.firstTimestamp && a.lastTimestamp == b.lastTimestamp &&
           a.topErrors == b.topErrors && a.distinctErrorMessages == b.distinctErrorMessages &&
           a.activityBucket == b.activityBucket && a.mostActivePeriods == b.mostActivePeriods &&
           a.duplicatesEnabled == b.duplicatesEnabled && a.duplicates == b.duplicates &&
           a.duplicateMessageCount == b.duplicateMessageCount && a.matchedSamples == b.matchedSamples &&
           a.invalidSamples == b.invalidSamples;
}

} // namespace loganalyzer
