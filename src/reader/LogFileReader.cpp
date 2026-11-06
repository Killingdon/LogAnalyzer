#include "reader/LogFileReader.h"

#include "common/Errors.h"

#include <algorithm>
#include <fstream>

namespace loganalyzer {

namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

std::size_t validateChunkSize(std::size_t chunkSize) {
    if (chunkSize == 0) {
        throw ConfigurationError("chunk size must be greater than zero");
    }
    return chunkSize;
}

std::unique_ptr<std::istream> openFile(const std::filesystem::path& path) {
    namespace fs = std::filesystem;

    // Check with error_code overloads: these never throw, so we can turn each
    // failure into a precise, user-friendly message.
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (ec || !fs::exists(status)) {
        throw FileError(path, "file does not exist");
    }
    if (fs::is_directory(status)) {
        throw FileError(path, "is a directory, not a file");
    }

    // Binary mode: no newline translation by the C runtime. We handle "\r\n"
    // ourselves in the parser, and offsets/byte counts stay exact.
    auto stream = std::make_unique<std::ifstream>(path, std::ios::in | std::ios::binary);
    if (!stream->is_open()) {
        throw FileError(path, "cannot open file for reading (check that you have read permission)");
    }
    return stream;
}

} // namespace

LogFileReader::LogFileReader(const std::filesystem::path& path, std::size_t chunkSize)
    : stream_(openFile(path)), path_(path), chunkSize_(validateChunkSize(chunkSize)) {
    std::error_code ec;
    if (const auto size = std::filesystem::file_size(path, ec); !ec) {
        sizeHint_ = size;
    }
}

LogFileReader::LogFileReader(std::unique_ptr<std::istream> stream, std::size_t chunkSize)
    : stream_(std::move(stream)), chunkSize_(validateChunkSize(chunkSize)) {
    if (!stream_) {
        throw ConfigurationError("LogFileReader requires a non-null stream");
    }
}

std::string LogFileReader::takeBuffer() {
    if (freeBuffers_.empty()) {
        std::string fresh;
        fresh.reserve(chunkSize_ + 256); // room for a carried-over partial line
        return fresh;
    }
    std::string buffer = std::move(freeBuffers_.back());
    freeBuffers_.pop_back();
    return buffer;
}

void LogFileReader::recycle(std::string&& buffer) {
    // A bounded pool: enough for every chunk in flight, without hoarding memory.
    constexpr std::size_t kMaxPooledBuffers = 64;
    if (freeBuffers_.size() < kMaxPooledBuffers && buffer.capacity() >= chunkSize_) {
        buffer.clear(); // keeps the capacity
        freeBuffers_.push_back(std::move(buffer));
    }
}

std::optional<LogChunk> LogFileReader::nextChunk() {
    if (endOfInput_ && carry_.empty()) {
        return std::nullopt;
    }

    // Start from the incomplete line left over by the previous call.
    std::string buffer = takeBuffer();
    buffer.append(carry_);
    carry_.clear();

    // Read blocks until the buffer contains at least one complete line, or the
    // input ends. Normally this loop runs exactly once.
    while (!endOfInput_) {
        const std::size_t oldSize = buffer.size();
        buffer.resize(oldSize + chunkSize_);
        stream_->read(buffer.data() + oldSize, static_cast<std::streamsize>(chunkSize_));
        const auto received = static_cast<std::size_t>(stream_->gcount());
        buffer.resize(oldSize + received);
        bytesRead_ += received;

        if (stream_->bad()) {
            throw FileError(path_, "read error");
        }
        if (received < chunkSize_) {
            endOfInput_ = true; // short read: end of file reached
        }

        std::size_t searchFrom = oldSize;
        if (atStart_) {
            atStart_ = false;
            if (std::string_view(buffer).starts_with(kUtf8Bom)) {
                buffer.erase(0, kUtf8Bom.size());
                searchFrom = 0;
            }
        }
        // Only the newly read bytes can contain the first newline.
        if (buffer.find('\n', searchFrom) != std::string::npos) {
            break;
        }
    }

    if (buffer.empty()) {
        return std::nullopt;
    }

    // Cut after the last newline; the tail is the beginning of a line whose
    // end has not been read yet and is carried over to the next chunk.
    if (!endOfInput_) {
        const std::size_t cut = buffer.rfind('\n') + 1;
        carry_.assign(buffer, cut, std::string::npos);
        buffer.resize(cut);
    }

    LogChunk chunk;
    chunk.index = nextIndex_++;
    chunk.firstLineNumber = nextLineNumber_;
    chunk.lineCount = static_cast<std::size_t>(std::count(buffer.begin(), buffer.end(), '\n'));
    if (buffer.back() != '\n') {
        ++chunk.lineCount; // final line of the file without a trailing newline
    }
    nextLineNumber_ += chunk.lineCount;
    chunk.data = std::move(buffer); // hand the buffer over without copying
    return chunk;
}

} // namespace loganalyzer
