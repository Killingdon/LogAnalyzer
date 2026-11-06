#pragma once

#include "models/LogEntry.h"

#include <bitset>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace loganalyzer {

/// One condition a log entry must satisfy.
///
/// This is the extension point of the filtering system (Open/Closed principle):
/// a new kind of filter - by source file, by regex, ... - is a new subclass;
/// LogFilter and the processor do not change.
///
/// Implementations must be immutable after construction so that `matches` can
/// be called from many worker threads at once without locking.
class FilterCriterion {
public:
    virtual ~FilterCriterion() = default;

    [[nodiscard]] virtual bool matches(const LogEntryView& entry) const = 0;

    /// Short human-readable description, e.g. "level in {ERROR, CRITICAL}".
    [[nodiscard]] virtual std::string describe() const = 0;

protected:
    FilterCriterion() = default;
    FilterCriterion(const FilterCriterion&) = default;
    FilterCriterion& operator=(const FilterCriterion&) = default;
};

/// Accepts entries whose level is in a given set.
class LevelCriterion final : public FilterCriterion {
public:
    explicit LevelCriterion(const std::vector<LogLevel>& levels);

    [[nodiscard]] bool matches(const LogEntryView& entry) const override;
    [[nodiscard]] std::string describe() const override;

private:
    std::bitset<kLogLevelCount> allowed_; // O(1) lookup, no allocation
};

/// Accepts entries with from <= timestamp <= to. Either bound may be open.
class TimeRangeCriterion final : public FilterCriterion {
public:
    /// Throws ConfigurationError if both bounds are set and from > to.
    TimeRangeCriterion(std::optional<Timestamp> from, std::optional<Timestamp> to);

    [[nodiscard]] bool matches(const LogEntryView& entry) const override;
    [[nodiscard]] std::string describe() const override;

private:
    std::optional<Timestamp> from_;
    std::optional<Timestamp> to_;
};

enum class KeywordMatch {
    Any, ///< at least one keyword must occur in the message (OR)
    All, ///< every keyword must occur in the message (AND)
};

/// Accepts entries whose message contains the keyword(s).
class KeywordCriterion final : public FilterCriterion {
public:
    /// Throws ConfigurationError if `keywords` is empty or contains an empty string.
    KeywordCriterion(std::vector<std::string> keywords, KeywordMatch mode, bool caseSensitive);

    [[nodiscard]] bool matches(const LogEntryView& entry) const override;
    [[nodiscard]] std::string describe() const override;

private:
    [[nodiscard]] bool contains(std::string_view message, const std::string& keyword) const noexcept;

    std::vector<std::string> keywords_;
    KeywordMatch mode_;
    bool caseSensitive_;
};

/// Plain-data description of a filter, as produced by the command line.
struct FilterOptions {
    std::vector<LogLevel> levels;            ///< empty = all levels
    std::optional<Timestamp> from;           ///< inclusive
    std::optional<Timestamp> to;             ///< inclusive
    std::vector<std::string> keywords;       ///< empty = no keyword filter
    KeywordMatch keywordMatch = KeywordMatch::Any;
    bool caseSensitive = false;
};

/// A conjunction (logical AND) of criteria. An empty filter accepts everything.
///
/// Thread safety: after construction LogFilter is only read, so one instance is
/// safely shared by all worker threads. It is move-only because it owns its
/// criteria through std::unique_ptr.
class LogFilter {
public:
    LogFilter() = default;

    /// Builds a filter from options. Cheap criteria are checked first (level,
    /// then time range, then the comparatively expensive keyword search), so
    /// most rejected entries are rejected after a single bit test.
    /// Throws ConfigurationError for inconsistent options.
    [[nodiscard]] static LogFilter fromOptions(const FilterOptions& options);

    LogFilter& add(std::unique_ptr<FilterCriterion> criterion);

    [[nodiscard]] bool matches(const LogEntryView& entry) const;
    [[nodiscard]] bool matches(const LogEntry& entry) const { return matches(entry.view()); }

    [[nodiscard]] bool empty() const noexcept { return criteria_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return criteria_.size(); }

    /// One description per criterion, in evaluation order.
    [[nodiscard]] std::vector<std::string> describe() const;

private:
    std::vector<std::unique_ptr<FilterCriterion>> criteria_;
};

} // namespace loganalyzer
