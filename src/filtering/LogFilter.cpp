#include "filtering/LogFilter.h"

#include "common/Errors.h"
#include "util/StringUtils.h"

#include <algorithm>

namespace loganalyzer {

namespace {

std::string joinQuoted(const std::vector<std::string>& values, std::string_view separator) {
    std::string result;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            result += separator;
        }
        result += '"';
        result += values[i];
        result += '"';
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// LevelCriterion
// ---------------------------------------------------------------------------

LevelCriterion::LevelCriterion(const std::vector<LogLevel>& levels) {
    if (levels.empty()) {
        throw ConfigurationError("level filter requires at least one level");
    }
    for (const LogLevel level : levels) {
        allowed_.set(toIndex(level));
    }
}

bool LevelCriterion::matches(const LogEntryView& entry) const {
    return allowed_.test(toIndex(entry.level));
}

std::string LevelCriterion::describe() const {
    std::string result = "level in {";
    bool first = true;
    for (const LogLevel level : kAllLogLevels) {
        if (allowed_.test(toIndex(level))) {
            if (!first) {
                result += ", ";
            }
            result += toString(level);
            first = false;
        }
    }
    result += '}';
    return result;
}

// ---------------------------------------------------------------------------
// TimeRangeCriterion
// ---------------------------------------------------------------------------

TimeRangeCriterion::TimeRangeCriterion(std::optional<Timestamp> from, std::optional<Timestamp> to)
    : from_(from), to_(to) {
    if (!from_ && !to_) {
        throw ConfigurationError("time range filter requires at least one bound");
    }
    if (from_ && to_ && *from_ > *to_) {
        throw ConfigurationError("invalid time range: start (" + util::formatTimestamp(*from_) +
                                 ") is after end (" + util::formatTimestamp(*to_) + ")");
    }
}

bool TimeRangeCriterion::matches(const LogEntryView& entry) const {
    return (!from_ || entry.timestamp >= *from_) && (!to_ || entry.timestamp <= *to_);
}

std::string TimeRangeCriterion::describe() const {
    if (from_ && to_) {
        return "time between " + util::formatTimestamp(*from_) + " and " + util::formatTimestamp(*to_);
    }
    if (from_) {
        return "time from " + util::formatTimestamp(*from_);
    }
    return "time until " + util::formatTimestamp(*to_);
}

// ---------------------------------------------------------------------------
// KeywordCriterion
// ---------------------------------------------------------------------------

KeywordCriterion::KeywordCriterion(std::vector<std::string> keywords, KeywordMatch mode, bool caseSensitive)
    : keywords_(std::move(keywords)), mode_(mode), caseSensitive_(caseSensitive) {
    if (keywords_.empty()) {
        throw ConfigurationError("keyword filter requires at least one keyword");
    }
    if (std::any_of(keywords_.begin(), keywords_.end(), [](const std::string& k) { return k.empty(); })) {
        throw ConfigurationError("keywords must not be empty");
    }
}

bool KeywordCriterion::contains(std::string_view message, const std::string& keyword) const noexcept {
    return caseSensitive_ ? message.find(keyword) != std::string_view::npos
                          : util::containsIgnoreCase(message, keyword);
}

bool KeywordCriterion::matches(const LogEntryView& entry) const {
    const auto inMessage = [&](const std::string& keyword) { return contains(entry.message, keyword); };
    return mode_ == KeywordMatch::All ? std::all_of(keywords_.begin(), keywords_.end(), inMessage)
                                      : std::any_of(keywords_.begin(), keywords_.end(), inMessage);
}

std::string KeywordCriterion::describe() const {
    std::string result = "message contains ";
    if (keywords_.size() > 1) {
        result += mode_ == KeywordMatch::All ? "all of " : "any of ";
    }
    result += joinQuoted(keywords_, ", ");
    result += caseSensitive_ ? " (case-sensitive)" : " (case-insensitive)";
    return result;
}

// ---------------------------------------------------------------------------
// LogFilter
// ---------------------------------------------------------------------------

LogFilter LogFilter::fromOptions(const FilterOptions& options) {
    LogFilter filter;
    if (!options.levels.empty()) {
        filter.add(std::make_unique<LevelCriterion>(options.levels));
    }
    if (options.from || options.to) {
        filter.add(std::make_unique<TimeRangeCriterion>(options.from, options.to));
    }
    if (!options.keywords.empty()) {
        filter.add(std::make_unique<KeywordCriterion>(options.keywords, options.keywordMatch, options.caseSensitive));
    }
    return filter;
}

LogFilter& LogFilter::add(std::unique_ptr<FilterCriterion> criterion) {
    if (!criterion) {
        throw ConfigurationError("cannot add a null filter criterion");
    }
    criteria_.push_back(std::move(criterion));
    return *this;
}

bool LogFilter::matches(const LogEntryView& entry) const {
    // all_of short-circuits: evaluation stops at the first failing criterion.
    return std::all_of(criteria_.begin(), criteria_.end(),
                       [&](const std::unique_ptr<FilterCriterion>& c) { return c->matches(entry); });
}

std::vector<std::string> LogFilter::describe() const {
    std::vector<std::string> descriptions;
    descriptions.reserve(criteria_.size());
    for (const auto& criterion : criteria_) {
        descriptions.push_back(criterion->describe());
    }
    return descriptions;
}

} // namespace loganalyzer
