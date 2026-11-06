#include "parser/LogParser.h"

#include "util/StringUtils.h"

#include <limits>

namespace loganalyzer {

std::string_view describe(ParseErrorCode code) noexcept {
    switch (code) {
    case ParseErrorCode::EmptyLine:
        return "empty line";
    case ParseErrorCode::MissingTimestamp:
        return "missing timestamp: expected '[YYYY-MM-DD HH:MM:SS]' at start of line";
    case ParseErrorCode::InvalidTimestamp:
        return "invalid timestamp";
    case ParseErrorCode::MissingLevel:
        return "missing log level: expected '[LEVEL]' after timestamp";
    case ParseErrorCode::UnknownLevel:
        return "unknown log level";
    case ParseErrorCode::MissingMessage:
        return "missing message";
    }
    return "unknown error";
}

namespace {

/// If `text` starts with "[...]", returns the content between the brackets and
/// advances `text` past the closing bracket.
[[nodiscard]] std::optional<std::string_view> takeBracketed(std::string_view& text) noexcept {
    if (text.empty() || text.front() != '[') {
        return std::nullopt;
    }
    const std::size_t close = text.find(']', 1);
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view content = text.substr(1, close - 1);
    text.remove_prefix(close + 1);
    return content;
}

struct SourceLocationView {
    std::string_view file;
    std::uint32_t line = 0;
};

/// Recognises "<file>:<line>", e.g. "DatabasePool.cpp:142" or "C:\src\db.cpp:7".
/// rfind is used so that drive letters and namespaces with ':' still work.
[[nodiscard]] std::optional<SourceLocationView> parseSourceLocation(std::string_view text) noexcept {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }
    const std::string_view file = util::trim(text.substr(0, colon));
    const auto line = util::parseUnsigned(text.substr(colon + 1));
    if (file.empty() || !line || *line > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    // A file name never contains spaces in practice; requiring that keeps
    // ordinary bracketed text such as "[retry 2: 5]" in the message.
    if (file.find(' ') != std::string_view::npos) {
        return std::nullopt;
    }
    return SourceLocationView{file, static_cast<std::uint32_t>(*line)};
}

} // namespace

ParseResult LogParser::parseLine(std::string_view line, std::size_t lineNumber) const noexcept {
    // All work below only narrows string_views over `line`: no copies.
    std::string_view rest = util::trimRight(line);
    if (util::trimLeft(rest).empty()) {
        return ParseErrorCode::EmptyLine;
    }

    // 1. Timestamp
    const auto timestampText = takeBracketed(rest);
    if (!timestampText) {
        return ParseErrorCode::MissingTimestamp;
    }
    const auto timestamp = util::parseTimestamp(util::trim(*timestampText));
    if (!timestamp) {
        return ParseErrorCode::InvalidTimestamp;
    }

    // 2. Level
    rest = util::trimLeft(rest);
    const auto levelText = takeBracketed(rest);
    if (!levelText) {
        return ParseErrorCode::MissingLevel;
    }
    const auto level = parseLogLevel(util::trim(*levelText));
    if (!level) {
        return ParseErrorCode::UnknownLevel;
    }

    LogEntryView entry;
    entry.timestamp = *timestamp;
    entry.level = *level;
    entry.lineNumber = lineNumber;

    // 3. Optional source location. Work on a copy so that a non-matching
    //    bracket (e.g. "[cache] miss") is left in the message untouched.
    rest = util::trimLeft(rest);
    std::string_view probe = rest;
    if (const auto bracketed = takeBracketed(probe)) {
        if (const auto location = parseSourceLocation(*bracketed)) {
            entry.sourceFile = location->file;
            entry.sourceLine = location->line;
            rest = util::trimLeft(probe);
        }
    }

    // 4. Message
    if (rest.empty()) {
        return ParseErrorCode::MissingMessage;
    }
    entry.message = rest;
    return entry;
}

std::optional<LogEntry> LogParser::parse(std::string_view line, std::size_t lineNumber) const {
    const ParseResult result = parseLine(line, lineNumber);
    if (!result) {
        return std::nullopt;
    }
    return result.entry().toEntry();
}

} // namespace loganalyzer
