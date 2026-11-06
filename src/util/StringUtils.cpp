#include "util/StringUtils.h"

#include <algorithm>
#include <charconv>

namespace loganalyzer::util {

std::string toLower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), toLowerAscii);
    return result;
}

std::string toUpper(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), toUpperAscii);
    return result;
}

std::string_view trimLeft(std::string_view text) noexcept {
    std::size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) {
        ++begin;
    }
    return text.substr(begin);
}

std::string_view trimRight(std::string_view text) noexcept {
    std::size_t end = text.size();
    while (end > 0 && isSpace(text[end - 1])) {
        --end;
    }
    return text.substr(0, end);
}

std::string_view trim(std::string_view text) noexcept {
    return trimRight(trimLeft(text));
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) { return toLowerAscii(x) == toLowerAscii(y); });
}

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.size() > haystack.size()) {
        return false;
    }
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [](char x, char y) { return toLowerAscii(x) == toLowerAscii(y); });
    return it != haystack.end() || needle.empty();
}

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = text.find(delimiter, start);
        if (pos == std::string_view::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::optional<std::uint64_t> parseUnsigned(std::string_view text) noexcept {
    std::uint64_t value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    // std::from_chars: locale-independent, no allocation, no exceptions, and it
    // reports overflow (errc::result_out_of_range) instead of wrapping around.
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (text.empty() || ec != std::errc{} || ptr != last) {
        return std::nullopt;
    }
    return value;
}

std::string truncate(std::string_view text, std::size_t maxLength) {
    constexpr std::string_view kEllipsis = "...";
    if (text.size() <= maxLength) {
        return std::string(text);
    }
    if (maxLength <= kEllipsis.size()) {
        return std::string(text.substr(0, maxLength));
    }
    std::string result(text.substr(0, maxLength - kEllipsis.size()));
    result += kEllipsis;
    return result;
}

std::string formatWithSeparators(std::uint64_t value) {
    std::string digits = std::to_string(value);
    std::string result;
    result.reserve(digits.size() + digits.size() / 3);
    for (std::size_t i = 0; i < digits.size(); ++i) {
        // A separator goes before every digit that has a multiple of 3 digits
        // remaining (itself included): 1,234,567.
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            result.push_back(',');
        }
        result.push_back(digits[i]);
    }
    return result;
}

} // namespace loganalyzer::util
