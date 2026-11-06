#include "analysis/StatisticsCollector.h"

#include "util/StringUtils.h"

#include <iterator>
#include <utility>

namespace loganalyzer {

std::string_view toString(TimeBucket bucket) noexcept {
    switch (bucket) {
    case TimeBucket::Minute:
        return "minute";
    case TimeBucket::Hour:
        return "hour";
    case TimeBucket::Day:
        return "day";
    }
    return "unknown";
}

std::optional<TimeBucket> parseTimeBucket(std::string_view text) noexcept {
    if (util::equalsIgnoreCase(text, "minute")) {
        return TimeBucket::Minute;
    }
    if (util::equalsIgnoreCase(text, "hour")) {
        return TimeBucket::Hour;
    }
    if (util::equalsIgnoreCase(text, "day")) {
        return TimeBucket::Day;
    }
    return std::nullopt;
}

Timestamp bucketStart(Timestamp timestamp, TimeBucket bucket) noexcept {
    using namespace std::chrono;
    switch (bucket) {
    case TimeBucket::Minute:
        return floor<minutes>(timestamp);
    case TimeBucket::Hour:
        return floor<hours>(timestamp);
    case TimeBucket::Day:
        return floor<days>(timestamp);
    }
    return timestamp;
}

// ---------------------------------------------------------------------------
// MessageStats
// ---------------------------------------------------------------------------

void MessageStats::record(const Occurrence& occurrence) noexcept {
    if (count == 0 || occurrence.lineNumber < first.lineNumber) {
        first = occurrence;
    }
    if (count == 0 || occurrence.lineNumber > last.lineNumber) {
        last = occurrence;
    }
    ++count;
}

void MessageStats::merge(const MessageStats& other) noexcept {
    if (other.count == 0) {
        return;
    }
    if (count == 0) {
        *this = other;
        return;
    }
    if (other.first.lineNumber < first.lineNumber) {
        first = other.first;
    }
    if (other.last.lineNumber > last.lineNumber) {
        last = other.last;
    }
    count += other.count;
}

// ---------------------------------------------------------------------------
// StatisticsCollector
// ---------------------------------------------------------------------------

namespace {

void recordMessage(MessageMap& map, std::string_view message, const Occurrence& occurrence) {
    // Heterogeneous lookup: no std::string is built when the message is known.
    auto it = map.find(message);
    if (it == map.end()) {
        it = map.emplace(std::string(message), MessageStats{}).first; // first sighting: one allocation
    }
    it->second.record(occurrence);
}

/// Moves every entry of `from` into `into`, combining counts for shared keys.
void mergeMessageMaps(MessageMap& into, MessageMap&& from) {
    // Iterate over the smaller map. Safe because MessageStats::merge is symmetric.
    if (into.size() < from.size()) {
        std::swap(into, from);
    }
    for (auto it = from.begin(); it != from.end();) {
        const auto next = std::next(it); // extract() invalidates only `it`
        if (const auto found = into.find(it->first); found != into.end()) {
            found->second.merge(it->second);
        } else {
            // C++17 node handles: re-link the existing node (key string and all)
            // into the other map. No string copy, no new allocation.
            into.insert(from.extract(it));
        }
        it = next;
    }
    from.clear();
}

} // namespace

StatisticsCollector::StatisticsCollector(const AnalysisOptions& options)
    : bucket_(options.bucket), detectDuplicates_(options.detectDuplicates) {}

void StatisticsCollector::add(const LogEntryView& entry) {
    ++entryCount_;
    ++levelCounts_[toIndex(entry.level)];

    if (!earliest_ || entry.timestamp < *earliest_) {
        earliest_ = entry.timestamp;
    }
    if (!latest_ || entry.timestamp > *latest_) {
        latest_ = entry.timestamp;
    }

    const Occurrence occurrence{entry.lineNumber, entry.timestamp};
    if (isErrorLevel(entry.level)) {
        recordMessage(errorMessages_, entry.message, occurrence);
    }
    if (detectDuplicates_) {
        recordMessage(allMessages_, entry.message, occurrence);
    }

    ++activity_[bucketStart(entry.timestamp, bucket_).time_since_epoch().count()];
}

void StatisticsCollector::merge(StatisticsCollector&& other) {
    if (&other == this) {
        return;
    }
    entryCount_ += other.entryCount_;
    for (std::size_t i = 0; i < kLogLevelCount; ++i) {
        levelCounts_[i] += other.levelCounts_[i];
    }
    if (other.earliest_ && (!earliest_ || *other.earliest_ < *earliest_)) {
        earliest_ = other.earliest_;
    }
    if (other.latest_ && (!latest_ || *other.latest_ > *latest_)) {
        latest_ = other.latest_;
    }
    mergeMessageMaps(errorMessages_, std::move(other.errorMessages_));
    mergeMessageMaps(allMessages_, std::move(other.allMessages_));
    for (const auto& [bucketKey, count] : other.activity_) {
        activity_[bucketKey] += count;
    }

    other = StatisticsCollector(AnalysisOptions{.bucket = other.bucket_, .detectDuplicates = other.detectDuplicates_});
}

} // namespace loganalyzer
