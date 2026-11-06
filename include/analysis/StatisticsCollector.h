#pragma once

#include "models/LogEntry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace loganalyzer {

/// Granularity for "most active time periods".
enum class TimeBucket : std::uint8_t { Minute, Hour, Day };

[[nodiscard]] std::string_view toString(TimeBucket bucket) noexcept;
[[nodiscard]] std::optional<TimeBucket> parseTimeBucket(std::string_view text) noexcept;

/// Start of the bucket that contains `timestamp` (e.g. 14:37:12 -> 14:00:00 for Hour).
[[nodiscard]] Timestamp bucketStart(Timestamp timestamp, TimeBucket bucket) noexcept;

/// What the analysis should compute.
struct AnalysisOptions {
    std::size_t topCount = 10;          ///< length of "top errors" / "active periods" / duplicates lists
    TimeBucket bucket = TimeBucket::Hour;
    bool detectDuplicates = false;      ///< track every distinct message (costs memory)
};

/// Where a message occurred. lineNumber gives a total order over the file.
struct Occurrence {
    std::size_t lineNumber = 0;
    Timestamp timestamp{};

    bool operator==(const Occurrence&) const = default;
};

/// How often a message occurred, and its first and last occurrence in file order.
struct MessageStats {
    std::uint64_t count = 0;
    Occurrence first;
    Occurrence last;

    void record(const Occurrence& occurrence) noexcept;

    /// Combines two partial results. Commutative and associative, which is what
    /// makes the parallel merge order-independent.
    void merge(const MessageStats& other) noexcept;

    bool operator==(const MessageStats&) const = default;
};

/// Hash that accepts std::string *and* std::string_view.
///
/// With `is_transparent`, unordered_map::find(string_view) works without first
/// building a std::string key - so looking up a message that is already known
/// costs no allocation. (C++20 heterogeneous lookup for unordered containers.)
struct TransparentStringHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept {
        return std::hash<std::string_view>{}(text);
    }
};

using MessageMap = std::unordered_map<std::string, MessageStats, TransparentStringHash, std::equal_to<>>;

/// Accumulates raw statistics for a set of log entries.
///
/// Each worker thread owns a private collector for the chunk it is processing,
/// so `add` needs no locks. Partial collectors are then combined with `merge`
/// (a map-reduce pattern). Because every merged quantity is a sum, a min or a
/// max, the final result does not depend on how the input was split - which is
/// why single- and multi-threaded runs produce identical statistics.
class StatisticsCollector {
public:
    // Not explicit: aggregates holding a collector (e.g. ProcessingResult) can
    // then omit it in designated initializers, which copy-list-initialise it.
    StatisticsCollector() : StatisticsCollector(AnalysisOptions{}) {}
    explicit StatisticsCollector(const AnalysisOptions& options);

    void add(const LogEntryView& entry);
    void add(const LogEntry& entry) { add(entry.view()); }

    /// Absorbs `other` (which is left empty). Takes an rvalue so that map nodes
    /// can be moved instead of copied.
    void merge(StatisticsCollector&& other);

    [[nodiscard]] std::uint64_t entryCount() const noexcept { return entryCount_; }
    [[nodiscard]] const std::array<std::uint64_t, kLogLevelCount>& levelCounts() const noexcept {
        return levelCounts_;
    }
    [[nodiscard]] std::uint64_t count(LogLevel level) const noexcept { return levelCounts_[toIndex(level)]; }
    [[nodiscard]] const std::optional<Timestamp>& earliest() const noexcept { return earliest_; }
    [[nodiscard]] const std::optional<Timestamp>& latest() const noexcept { return latest_; }

    /// ERROR/CRITICAL messages only.
    [[nodiscard]] const MessageMap& errorMessages() const noexcept { return errorMessages_; }
    /// Every distinct message; populated only when duplicate detection is enabled.
    [[nodiscard]] const MessageMap& allMessages() const noexcept { return allMessages_; }
    /// Entries per bucket; key = bucket start in seconds since the epoch.
    [[nodiscard]] const std::unordered_map<std::int64_t, std::uint64_t>& activity() const noexcept {
        return activity_;
    }

    [[nodiscard]] TimeBucket bucket() const noexcept { return bucket_; }
    [[nodiscard]] bool detectsDuplicates() const noexcept { return detectDuplicates_; }

private:
    TimeBucket bucket_;
    bool detectDuplicates_;
    std::uint64_t entryCount_ = 0;
    std::array<std::uint64_t, kLogLevelCount> levelCounts_{};
    std::optional<Timestamp> earliest_;
    std::optional<Timestamp> latest_;
    MessageMap errorMessages_;
    MessageMap allMessages_;
    std::unordered_map<std::int64_t, std::uint64_t> activity_;
};

} // namespace loganalyzer
