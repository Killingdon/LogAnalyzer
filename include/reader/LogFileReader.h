#pragma once

#include <cstddef>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace loganalyzer {

/// A block of complete lines read from the input.
///
/// Chunks are the unit of parallel work: each one is self-contained (it never
/// splits a line) and knows the line number of its first line, so a worker can
/// process it without talking to anybody else.
struct LogChunk {
    std::size_t index = 0;           ///< 0-based sequence number, in file order
    std::size_t firstLineNumber = 1; ///< 1-based number of the first line in `data`
    std::size_t lineCount = 0;       ///< number of lines in `data`
    std::string data;                ///< raw bytes; every line ends in '\n' except possibly the file's last
};

/// Calls `fn(std::string_view line, std::size_t lineNumber)` for each line in
/// `data` (without the '\n'). The views point into `data`.
template <typename Fn>
void forEachLine(std::string_view data, std::size_t firstLineNumber, Fn&& fn) {
    std::size_t lineNumber = firstLineNumber;
    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::size_t newline = data.find('\n', pos);
        const std::size_t end = (newline == std::string_view::npos) ? data.size() : newline;
        fn(data.substr(pos, end - pos), lineNumber++);
        if (newline == std::string_view::npos) {
            break;
        }
        pos = newline + 1;
    }
}

/// Streams a log file in fixed-size chunks of whole lines.
///
/// Memory use is bounded by the chunk size (plus the longest line), no matter
/// how large the file is: the file is never loaded into memory as a whole.
/// Lines longer than the chunk size are handled by growing that one chunk.
///
/// Not thread-safe: one thread (the coordinator) reads, workers receive the
/// chunks by value.
class LogFileReader {
public:
    static constexpr std::size_t kDefaultChunkSize = std::size_t{1} << 20; // 1 MiB

    /// Opens `path`. Throws FileError if it does not exist, is a directory, or
    /// cannot be opened (e.g. insufficient permissions).
    explicit LogFileReader(const std::filesystem::path& path, std::size_t chunkSize = kDefaultChunkSize);

    /// Reads from an arbitrary stream (used by tests with std::istringstream).
    explicit LogFileReader(std::unique_ptr<std::istream> stream, std::size_t chunkSize = kDefaultChunkSize);

    LogFileReader(const LogFileReader&) = delete;
    LogFileReader& operator=(const LogFileReader&) = delete;
    LogFileReader(LogFileReader&&) noexcept = default;
    LogFileReader& operator=(LogFileReader&&) noexcept = default;
    ~LogFileReader() = default;

    /// Returns the next chunk, or std::nullopt once the input is exhausted.
    /// Throws FileError on a low-level read failure.
    [[nodiscard]] std::optional<LogChunk> nextChunk();

    /// Gives a chunk's buffer back once it has been processed, so the next
    /// chunk can reuse its memory.
    ///
    /// Without this, every chunk is a fresh 1 MiB allocation that the OS must
    /// supply as new zeroed pages (page faults). Measured on a 116 MiB file,
    /// recycling reduced the reader's time from ~89 ms to ~59 ms - and the
    /// reader is the serial part of the pipeline that limits multithreaded speedup.
    /// Must be called from the thread that calls nextChunk() (no locking).
    void recycle(std::string&& buffer);

    [[nodiscard]] std::uint64_t bytesRead() const noexcept { return bytesRead_; }

    /// Size of the file in bytes when known (files: yes; arbitrary streams: no).
    [[nodiscard]] std::optional<std::uint64_t> sizeHint() const noexcept { return sizeHint_; }
    [[nodiscard]] std::size_t chunkSize() const noexcept { return chunkSize_; }

private:
    [[nodiscard]] std::string takeBuffer();

    std::unique_ptr<std::istream> stream_;
    std::filesystem::path path_; // for error messages; empty for plain streams
    std::size_t chunkSize_;
    std::string carry_;          // partial last line of the previous read
    std::vector<std::string> freeBuffers_; // recycled chunk buffers
    std::size_t nextIndex_ = 0;
    std::size_t nextLineNumber_ = 1;
    std::uint64_t bytesRead_ = 0;
    std::optional<std::uint64_t> sizeHint_;
    bool endOfInput_ = false;
    bool atStart_ = true;        // used to strip a UTF-8 byte-order mark
};

} // namespace loganalyzer
