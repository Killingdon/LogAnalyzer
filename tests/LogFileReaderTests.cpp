#include "common/Errors.h"
#include "processing/LogProcessor.h"
#include "reader/LogFileReader.h"

#include "TestHelpers.h"

#include <gtest/gtest.h>

#include <sstream>
#include <vector>

using namespace loganalyzer;

namespace {

/// Reads everything and returns the individual lines, checking chunk invariants.
std::vector<std::string> readAllLines(LogFileReader& reader) {
    std::vector<std::string> lines;
    std::size_t expectedIndex = 0;
    std::size_t expectedLine = 1;
    while (auto chunk = reader.nextChunk()) {
        EXPECT_EQ(chunk->index, expectedIndex++);
        EXPECT_EQ(chunk->firstLineNumber, expectedLine);
        std::size_t linesInChunk = 0;
        forEachLine(chunk->data, chunk->firstLineNumber, [&](std::string_view line, std::size_t number) {
            EXPECT_EQ(number, expectedLine + linesInChunk);
            lines.emplace_back(line);
            ++linesInChunk;
        });
        EXPECT_EQ(linesInChunk, chunk->lineCount);
        expectedLine += chunk->lineCount;
    }
    return lines;
}

std::vector<std::string> readAllLines(std::string content, std::size_t chunkSize) {
    LogFileReader reader(std::make_unique<std::istringstream>(std::move(content)), chunkSize);
    return readAllLines(reader);
}

} // namespace

TEST(LogFileReaderTest, MissingFileThrowsFileError) {
    EXPECT_THROW(LogFileReader("definitely/not/here.log"), FileError);
}

TEST(LogFileReaderTest, DirectoryThrowsFileError) {
    const test::TempDirectory dir;
    try {
        LogFileReader reader(dir.path());
        FAIL() << "expected FileError";
    } catch (const FileError& e) {
        EXPECT_NE(std::string(e.what()).find("directory"), std::string::npos);
    }
}

TEST(LogFileReaderTest, ZeroChunkSizeIsRejected) {
    EXPECT_THROW(LogFileReader(std::make_unique<std::istringstream>("x"), 0), ConfigurationError);
}

TEST(LogFileReaderTest, EmptyFileProducesNoChunks) {
    const test::TempDirectory dir;
    LogFileReader reader(dir.writeFile("empty.log", ""));
    EXPECT_FALSE(reader.nextChunk().has_value());
    EXPECT_FALSE(reader.nextChunk().has_value()); // stays exhausted
    EXPECT_EQ(reader.bytesRead(), 0u);
}

TEST(LogFileReaderTest, ReadsAllLinesInOrderAcrossManyChunkSizes) {
    std::string content;
    std::vector<std::string> expected;
    for (int i = 1; i <= 200; ++i) {
        expected.push_back("line number " + std::to_string(i) + std::string(static_cast<std::size_t>(i % 17), '#'));
        content += expected.back() + "\n";
    }
    // Tiny chunk sizes force lines to straddle chunk boundaries in every way.
    for (const std::size_t chunkSize : {1u, 2u, 7u, 64u, 1000u, 1u << 20}) {
        EXPECT_EQ(readAllLines(content, chunkSize), expected) << "chunk size " << chunkSize;
    }
}

TEST(LogFileReaderTest, LastLineWithoutNewlineIsKept) {
    const auto lines = readAllLines("first\nsecond\nlast-no-newline", 4);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines.back(), "last-no-newline");
}

TEST(LogFileReaderTest, BlankLinesAreCounted) {
    const auto lines = readAllLines("a\n\n\nb\n", 3);
    EXPECT_EQ(lines, (std::vector<std::string>{"a", "", "", "b"}));
}

TEST(LogFileReaderTest, StripsUtf8ByteOrderMark) {
    const auto lines = readAllLines("\xEF\xBB\xBF" "first\nsecond\n", 1024);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines.front(), "first");
}

TEST(LogFileReaderTest, LineLongerThanChunkSizeIsReadWhole) {
    const std::string longLine(10'000, 'x');
    const auto lines = readAllLines("short\n" + longLine + "\nend\n", 16);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[1], longLine);
}

TEST(LogFileReaderTest, CountsBytesRead) {
    const test::TempDirectory dir;
    const std::string content = "[2026-10-20 14:30:22] [INFO] a\r\n[2026-10-20 14:30:23] [INFO] b\r\n";
    LogFileReader reader(dir.writeFile("crlf.log", content), 8);
    const auto lines = readAllLines(reader);
    EXPECT_EQ(lines.size(), 2u);
    EXPECT_EQ(reader.bytesRead(), content.size());
}

// ---------------------------------------------------------------------------
// Basic processing on top of the reader
// ---------------------------------------------------------------------------

TEST(LogProcessorBasicTest, CountsValidInvalidAndEmptyLines) {
    const test::TempDirectory dir;
    const auto file = dir.writeFile("mixed.log",
                                    "[2026-10-20 14:30:22] [INFO] Application started\r\n"
                                    "garbage line\r\n"
                                    "\r\n"
                                    "[2026-10-20 14:30:30] [ERROR] Database connection failed\r\n"
                                    "[2026-10-20 99:30:30] [ERROR] bad time");
    const ProcessingResult result = LogProcessor{}.process(file);
    EXPECT_EQ(result.summary.totalLines, 5u);
    EXPECT_EQ(result.summary.validEntries, 2u);
    EXPECT_EQ(result.summary.invalidEntries, 2u);
    EXPECT_EQ(result.summary.emptyLines, 1u);
    ASSERT_EQ(result.invalidSamples.size(), 2u);
    EXPECT_EQ(result.invalidSamples[0].lineNumber, 2u);
    EXPECT_EQ(result.invalidSamples[0].error, ParseErrorCode::MissingTimestamp);
    EXPECT_EQ(result.invalidSamples[0].excerpt, "garbage line"); // '\r' stripped
    EXPECT_EQ(result.invalidSamples[1].lineNumber, 5u);
    EXPECT_EQ(result.invalidSamples[1].error, ParseErrorCode::InvalidTimestamp);
}

TEST(LogProcessorBasicTest, MalformedSampleNeverThrows) {
    ProcessingResult result;
    ASSERT_NO_THROW(result = LogProcessor{}.process(test::samplePath("malformed.log")));
    EXPECT_EQ(result.summary.totalLines, result.summary.validEntries + result.summary.invalidEntries +
                                             result.summary.emptyLines);
    EXPECT_GT(result.summary.invalidEntries, 0u);
}
