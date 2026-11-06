#pragma once

#include "analysis/StatisticsCollector.h"
#include "filtering/LogFilter.h"
#include "parser/LogParser.h"
#include "processing/ProcessingResult.h"
#include "reader/LogFileReader.h"

#include <cstddef>
#include <filesystem>
#include <istream>
#include <memory>

namespace loganalyzer {

struct ProcessingOptions {
    /// 1 = single-threaded (everything on the calling thread);
    /// N > 1 = N worker threads plus the calling thread, which reads and merges;
    /// 0 = automatic: one worker per hardware thread, but never more workers
    ///     than the file has chunks (small files run single-threaded).
    unsigned threadCount = 0;
    std::size_t chunkSizeBytes = LogFileReader::kDefaultChunkSize;
    std::size_t maxInvalidSamples = 20; ///< invalid lines kept for the report
    std::size_t maxMatchedSamples = 20; ///< matching entries kept for the report
};

/// Coordinates reading, parsing, filtering and statistics collection, in
/// single-threaded or multithreaded mode.
///
/// Both modes run exactly the same per-chunk code (processChunk) and merge the
/// partial results in file order, so they produce identical results; only the
/// timing differs.
///
/// Multithreaded pipeline:
///
///   calling thread                       worker threads (ThreadPool)
///   --------------                       ---------------------------
///   read chunk k  --- submit(chunk) -->  parse + filter + count chunk k
///   ...                                  -> ChunkResult via std::future
///   merge results in order <------------
///
/// At most 2 * threadCount chunks are in flight. That bounds memory use and
/// provides back-pressure: the reader cannot race ahead of the workers.
class LogProcessor {
public:
    explicit LogProcessor(LogFilter filter = {}, ProcessingOptions options = {},
                          AnalysisOptions analysisOptions = {});

    /// Throws FileError for unreadable input and ProcessingError if a worker fails.
    [[nodiscard]] ProcessingResult process(const std::filesystem::path& path) const;
    [[nodiscard]] ProcessingResult process(std::unique_ptr<std::istream> stream) const;

    /// Thread count with 0 resolved to the hardware value. (For files, automatic
    /// mode may use fewer; the actual count is in PerformanceMetrics::threadCount.)
    [[nodiscard]] unsigned effectiveThreadCount() const noexcept;

    [[nodiscard]] const LogFilter& filter() const noexcept { return filter_; }
    [[nodiscard]] const ProcessingOptions& options() const noexcept { return options_; }

private:
    struct ChunkResult;

    [[nodiscard]] ProcessingResult run(LogFileReader& reader) const;
    void runSingleThreaded(LogFileReader& reader, ProcessingResult& result) const;
    void runMultiThreaded(LogFileReader& reader, ProcessingResult& result, unsigned threads) const;

    /// Pure function of (chunk, configuration): touches no shared mutable state,
    /// so any number of chunks can be processed concurrently.
    [[nodiscard]] ChunkResult processChunk(const LogChunk& chunk) const;
    void mergeChunk(ProcessingResult& total, ChunkResult&& part) const;

    LogFilter filter_;
    ProcessingOptions options_;
    AnalysisOptions analysisOptions_;
    LogParser parser_;
};

} // namespace loganalyzer
