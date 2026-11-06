#pragma once

#include "models/LogEntry.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace loganalyzer {

/// Why a line could not be parsed.
enum class ParseErrorCode : std::uint8_t {
    EmptyLine,        ///< blank or whitespace-only (counted separately, not as invalid)
    MissingTimestamp, ///< line does not start with "[...]"
    InvalidTimestamp, ///< "[...]" present but not a valid date/time
    MissingLevel,     ///< no "[LEVEL]" after the timestamp
    UnknownLevel,     ///< "[...]" present but not a known level
    MissingMessage,   ///< nothing after the level / source location
};

/// Human-readable explanation, e.g. "invalid timestamp".
[[nodiscard]] std::string_view describe(ParseErrorCode code) noexcept;

/// Outcome of parsing one line: either a LogEntryView or an error code.
///
/// A tiny wrapper around std::variant (C++23 would use std::expected). Parsing
/// failures are *expected* for real-world logs, so they are returned as values
/// rather than thrown: exceptions would be far too slow on a hot path that may
/// see thousands of malformed lines.
class ParseResult {
public:
    ParseResult(LogEntryView entry) noexcept : value_(entry) {}  // NOLINT: implicit by design
    ParseResult(ParseErrorCode error) noexcept : value_(error) {} // NOLINT: implicit by design

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<LogEntryView>(value_); }
    explicit operator bool() const noexcept { return ok(); }

    /// Precondition: ok(). Throws std::bad_variant_access otherwise.
    [[nodiscard]] const LogEntryView& entry() const { return std::get<LogEntryView>(value_); }

    /// Precondition: !ok(). Throws std::bad_variant_access otherwise.
    [[nodiscard]] ParseErrorCode error() const { return std::get<ParseErrorCode>(value_); }

private:
    std::variant<LogEntryView, ParseErrorCode> value_;
};

/// Converts raw text lines into structured log entries.
///
/// Accepted format (whitespace between fields is flexible):
///
///     [2026-10-20 14:30:22] [ERROR] Database connection failed
///     [2026-10-20 14:30:22] [ERROR] [DatabasePool.cpp:142] Database connection failed
///
/// The optional third bracket is treated as a source location only when it has
/// the shape "<file>:<line>"; anything else (e.g. "[cache]") stays part of the
/// message. Trailing whitespace, including the '\r' of Windows line endings,
/// is ignored.
///
/// LogParser is stateless, so a single instance can be shared by any number of
/// threads without synchronisation.
class LogParser {
public:
    /// Parses one line (without its trailing '\n'). Never allocates, never throws.
    /// The returned view refers to `line`.
    [[nodiscard]] ParseResult parseLine(std::string_view line, std::size_t lineNumber = 0) const noexcept;

    /// Convenience wrapper that returns an owning entry, or nullopt on failure.
    [[nodiscard]] std::optional<LogEntry> parse(std::string_view line, std::size_t lineNumber = 0) const;
};

} // namespace loganalyzer
