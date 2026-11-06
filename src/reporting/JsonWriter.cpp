#include "JsonWriter.h"

#include <cmath>
#include <format>

namespace loganalyzer::detail {

namespace {

constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD"; // U+FFFD in UTF-8

/// Length of the valid UTF-8 sequence starting at text[pos], or 0 if invalid.
/// Rejects overlong encodings, surrogates and code points above U+10FFFF.
std::size_t utf8SequenceLength(std::string_view text, std::size_t pos) noexcept {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[pos + i]); };
    const auto isContinuation = [&](std::size_t i) { return (byte(i) & 0xC0) == 0x80; };
    const std::size_t remaining = text.size() - pos;
    const unsigned char lead = byte(0);

    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        return remaining >= 2 && isContinuation(1) ? 2 : 0;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        if (remaining < 3 || !isContinuation(1) || !isContinuation(2)) {
            return 0;
        }
        if ((lead == 0xE0 && byte(1) < 0xA0) || (lead == 0xED && byte(1) > 0x9F)) {
            return 0; // overlong form or UTF-16 surrogate
        }
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        if (remaining < 4 || !isContinuation(1) || !isContinuation(2) || !isContinuation(3)) {
            return 0;
        }
        if ((lead == 0xF0 && byte(1) < 0x90) || (lead == 0xF4 && byte(1) > 0x8F)) {
            return 0; // overlong form or beyond U+10FFFF
        }
        return 4;
    }
    return 0; // stray continuation byte or invalid lead byte
}

} // namespace

std::string escapeJson(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8);
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char c = text[pos];
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80) {
            const std::size_t length = utf8SequenceLength(text, pos);
            if (length == 0) {
                result += kReplacementCharacter;
                ++pos;
            } else {
                result.append(text.substr(pos, length));
                pos += length;
            }
            continue;
        }
        switch (c) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        default:
            if (u < 0x20 || u == 0x7F) {
                result += std::format("\\u{:04x}", static_cast<unsigned>(u));
            } else {
                result += c;
            }
        }
        ++pos;
    }
    return result;
}

void JsonWriter::newline() {
    out_ << '\n' << std::string(hasElements_.size() * static_cast<std::size_t>(indentWidth_), ' ');
}

void JsonWriter::beforeValue() {
    if (afterKey_) {
        afterKey_ = false; // value follows "key": on the same line
        return;
    }
    if (!hasElements_.empty()) {
        if (hasElements_.back()) {
            out_ << ',';
        }
        hasElements_.back() = true;
        newline();
    }
}

JsonWriter& JsonWriter::open(char bracket) {
    beforeValue();
    out_ << bracket;
    hasElements_.push_back(false);
    return *this;
}

JsonWriter& JsonWriter::close(char bracket) {
    const bool hadElements = hasElements_.back();
    hasElements_.pop_back();
    if (hadElements) {
        newline();
    }
    out_ << bracket;
    if (hasElements_.empty()) {
        out_ << '\n';
    }
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    beforeValue();
    out_ << '"' << escapeJson(name) << "\": ";
    afterKey_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view text) {
    beforeValue();
    out_ << '"' << escapeJson(text) << '"';
    return *this;
}

JsonWriter& JsonWriter::value(std::uint64_t number) {
    beforeValue();
    out_ << number;
    return *this;
}

JsonWriter& JsonWriter::value(double number, int decimals) {
    beforeValue();
    // JSON has no NaN or Infinity.
    out_ << (std::isfinite(number) ? std::format("{:.{}f}", number, decimals) : "null");
    return *this;
}

JsonWriter& JsonWriter::value(bool flag) {
    beforeValue();
    out_ << (flag ? "true" : "false");
    return *this;
}

JsonWriter& JsonWriter::null() {
    beforeValue();
    out_ << "null";
    return *this;
}

} // namespace loganalyzer::detail
