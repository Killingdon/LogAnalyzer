#include "models/LogEntry.h"

#include "util/StringUtils.h"

namespace loganalyzer {

std::string_view toString(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARNING";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Critical:
        return "CRITICAL";
    }
    return "UNKNOWN";
}

std::optional<LogLevel> parseLogLevel(std::string_view text) noexcept {
    struct Alias {
        std::string_view name;
        LogLevel level;
    };
    // Canonical names first: they are by far the most common input.
    static constexpr std::array<Alias, 10> kNames{{
        {"INFO", LogLevel::Info},
        {"DEBUG", LogLevel::Debug},
        {"WARNING", LogLevel::Warning},
        {"ERROR", LogLevel::Error},
        {"TRACE", LogLevel::Trace},
        {"CRITICAL", LogLevel::Critical},
        {"WARN", LogLevel::Warning},
        {"ERR", LogLevel::Error},
        {"FATAL", LogLevel::Critical},
        {"CRIT", LogLevel::Critical},
    }};
    for (const auto& alias : kNames) {
        if (util::equalsIgnoreCase(text, alias.name)) {
            return alias.level;
        }
    }
    return std::nullopt;
}

} // namespace loganalyzer
