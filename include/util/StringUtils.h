#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Small, allocation-conscious string helpers.
///
/// Case conversion is ASCII-only on purpose: log levels, keywords and file
/// formats are ASCII, and ASCII folding is locale-independent, branch-cheap and
/// thread-safe (std::tolower depends on the global C locale).
namespace loganalyzer::util {

[[nodiscard]] constexpr char toLowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr char toUpperAscii(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

[[nodiscard]] constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

[[nodiscard]] std::string toLower(std::string_view text);
[[nodiscard]] std::string toUpper(std::string_view text);

/// Trimming returns a narrower view of the same characters: no allocation.
[[nodiscard]] std::string_view trimLeft(std::string_view text) noexcept;
[[nodiscard]] std::string_view trimRight(std::string_view text) noexcept;
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;

[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept;

/// True if `needle` occurs in `haystack`, comparing ASCII letters case-insensitively.
/// An empty needle is found in every haystack (same rule as std::string_view::find).
[[nodiscard]] bool containsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept;

/// Splits on `delimiter`. Empty fields are kept ("a,,b" -> {"a", "", "b"}) so
/// that callers can detect and report them. The views refer to `text`.
[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char delimiter);

/// Parses a non-negative decimal integer. The whole string must be consumed.
[[nodiscard]] std::optional<std::uint64_t> parseUnsigned(std::string_view text) noexcept;

/// Shortens `text` to at most `maxLength` characters, appending "..." if cut.
[[nodiscard]] std::string truncate(std::string_view text, std::size_t maxLength);

/// Formats an integer with thousands separators: 1234567 -> "1,234,567".
[[nodiscard]] std::string formatWithSeparators(std::uint64_t value);

} // namespace loganalyzer::util
