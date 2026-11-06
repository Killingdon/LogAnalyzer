#pragma once

// Private header: a minimal streaming JSON writer used by JsonReportFormatter.
// Kept out of include/ because it is an implementation detail.

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace loganalyzer::detail {

/// Escapes text for a JSON string literal (without the surrounding quotes).
///
/// Log files are untrusted input, so this must produce valid JSON for *any*
/// byte sequence: control characters become \uXXXX, and byte sequences that
/// are not valid UTF-8 are replaced by U+FFFD (the Unicode replacement character).
std::string escapeJson(std::string_view text);

/// Streams JSON with indentation and automatic comma placement.
///
///   JsonWriter w(out);
///   w.beginObject();
///   w.key("count").value(42);
///   w.endObject();
class JsonWriter {
public:
    explicit JsonWriter(std::ostream& out, int indentWidth = 2) : out_(out), indentWidth_(indentWidth) {}

    JsonWriter& beginObject() { return open('{'); }
    JsonWriter& endObject() { return close('}'); }
    JsonWriter& beginArray() { return open('['); }
    JsonWriter& endArray() { return close(']'); }

    JsonWriter& key(std::string_view name);

    JsonWriter& value(std::string_view text);
    JsonWriter& value(const char* text) { return value(std::string_view(text)); }
    JsonWriter& value(std::uint64_t number);
    JsonWriter& value(double number, int decimals = 2);
    JsonWriter& value(bool flag);
    JsonWriter& null();

private:
    JsonWriter& open(char bracket);
    JsonWriter& close(char bracket);
    void beforeValue();
    void newline();

    std::ostream& out_;
    int indentWidth_;
    std::vector<bool> hasElements_; // one entry per open container
    bool afterKey_ = false;
};

} // namespace loganalyzer::detail
