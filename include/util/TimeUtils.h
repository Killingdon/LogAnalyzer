#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace loganalyzer {

/// Point in time with one-second resolution.
///
/// Log timestamps carry no time-zone information, so they are treated as plain
/// wall-clock values: no conversion is ever applied. std::chrono::sys_seconds
/// gives us correct calendar arithmetic, ordering and hashing for free.
using Timestamp = std::chrono::sys_seconds;

} // namespace loganalyzer

namespace loganalyzer::util {

/// Parses "YYYY-MM-DD HH:MM:SS". Also accepts a 'T' separator
/// ("2026-10-20T14:30:22") and an optional fractional part (".123"), which is
/// truncated to whole seconds. Calendar validity is checked (no 2026-02-30).
/// Returns std::nullopt for anything else; never throws.
[[nodiscard]] std::optional<Timestamp> parseTimestamp(std::string_view text) noexcept;

/// Parses "YYYY-MM-DD" and returns midnight of that day.
[[nodiscard]] std::optional<Timestamp> parseDate(std::string_view text) noexcept;

/// Formats as "YYYY-MM-DD HH:MM:SS".
[[nodiscard]] std::string formatTimestamp(Timestamp timestamp);

/// Formats a duration for humans: "850 us", "12.35 ms", "3.402 s".
[[nodiscard]] std::string formatDuration(std::chrono::nanoseconds elapsed);

} // namespace loganalyzer::util
