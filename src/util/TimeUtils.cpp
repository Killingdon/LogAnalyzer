#include "util/TimeUtils.h"

#include "util/StringUtils.h"

#include <format>

namespace loganalyzer::util {

namespace {

/// Converts a fixed-width run of ASCII digits to an int. Hand-rolled instead
/// of std::get_time/std::chrono::parse: those go through iostreams and locales,
/// which is far slower than this for millions of timestamps.
[[nodiscard]] constexpr bool parseFixedDigits(std::string_view text, int& out) noexcept {
    if (text.empty()) {
        return false;
    }
    int value = 0;
    for (const char c : text) {
        if (!isDigit(c)) {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

[[nodiscard]] std::optional<std::chrono::sys_days> parseYearMonthDay(std::string_view text) noexcept {
    // Layout: YYYY-MM-DD
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    if (!parseFixedDigits(text.substr(0, 4), year) || !parseFixedDigits(text.substr(5, 2), month) ||
        !parseFixedDigits(text.substr(8, 2), day)) {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{std::chrono::year{year},
                                          std::chrono::month{static_cast<unsigned>(month)},
                                          std::chrono::day{static_cast<unsigned>(day)}};
    // ok() validates the full calendar date, including month lengths and leap years.
    if (!ymd.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{ymd};
}

} // namespace

std::optional<Timestamp> parseTimestamp(std::string_view text) noexcept {
    // Layout: YYYY-MM-DD HH:MM:SS[.fraction]
    //         0123456789012345678
    constexpr std::size_t kBaseLength = 19;
    if (text.size() < kBaseLength) {
        return std::nullopt;
    }
    if (text[10] != ' ' && text[10] != 'T') {
        return std::nullopt;
    }
    if (text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    if (text.size() > kBaseLength) {
        // Optional fraction: '.' followed by 1-9 digits. Only seconds are kept.
        const std::string_view fraction = text.substr(kBaseLength);
        int ignored = 0;
        if (fraction.size() < 2 || fraction.size() > 10 || fraction[0] != '.' ||
            !parseFixedDigits(fraction.substr(1), ignored)) {
            return std::nullopt;
        }
    }

    const auto date = parseYearMonthDay(text.substr(0, 10));
    if (!date) {
        return std::nullopt;
    }

    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    if (!parseFixedDigits(text.substr(11, 2), hours) || !parseFixedDigits(text.substr(14, 2), minutes) ||
        !parseFixedDigits(text.substr(17, 2), seconds)) {
        return std::nullopt;
    }
    if (hours > 23 || minutes > 59 || seconds > 59) {
        return std::nullopt;
    }

    return Timestamp{*date} + std::chrono::hours{hours} + std::chrono::minutes{minutes} +
           std::chrono::seconds{seconds};
}

std::optional<Timestamp> parseDate(std::string_view text) noexcept {
    const auto date = parseYearMonthDay(text);
    if (!date) {
        return std::nullopt;
    }
    return Timestamp{*date};
}

std::string formatTimestamp(Timestamp timestamp) {
    const auto day = std::chrono::floor<std::chrono::days>(timestamp);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::hh_mm_ss time{timestamp - day};
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", static_cast<int>(ymd.year()),
                       static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                       time.hours().count(), time.minutes().count(), time.seconds().count());
}

std::string formatDuration(std::chrono::nanoseconds elapsed) {
    using namespace std::chrono;
    const auto ns = elapsed.count();
    if (ns < 1'000'000) {
        return std::format("{} us", duration_cast<microseconds>(elapsed).count());
    }
    if (ns < 1'000'000'000) {
        return std::format("{:.2f} ms", duration<double, std::milli>(elapsed).count());
    }
    return std::format("{:.3f} s", duration<double>(elapsed).count());
}

} // namespace loganalyzer::util
