#include "processing/LogProcessor.h"

#include "common/Errors.h"
#include "threading/ThreadPool.h"
#include "util/StringUtils.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <future>
#include <iterator>
#include <string>

namespace loganalyzer {

namespace {

constexpr std::size_t kMaxExcerptLength = 160;

/// Appends elements from `source` to `target` until `target` holds `limit`.
template <typename T>
void appendLimited(std::vector<T>& target, std::vector<T>&& source, std::size_t limit) {
    if (target.size() >= limit) {
        return;
    }
    const std::size_t take = std::min(limit - target.size(), source.size());
    target.insert(target.end(), std::make_move_iterator(source.begin()),
                  std::make_move_iterator(source.begin() + static_cast<std::ptrdiff_t>(take)));
}

} // namespace

/// Everything a worker produces for one chunk. Owned by exactly one thread at
/// a time: built by a worker, then handed to the coordinator via std::future.
struct LogProcessor::ChunkResult {
    std::size_t chunkIndex = 0;
    ProcessingSummary summary;
    StatisticsCollector statistics;
    std::vector<InvalidLine> invalidSamples;
    std::vector<LogEntry> matchedSamples;
    std::string buffer; ///< the chunk's memory, returned for recycling
};

LogProcessor::LogProcessor(LogFilter filter, ProcessingOptions options, AnalysisOptions analysisOptions)
    : filter_(std::move(filter)), options_(options), analysisOptions_(analysisOptions) {}

unsigned LogProcessor::effectiveThreadCount() const noexcept {
    return options_.threadCount == 0 ? static_cast<unsigned>(ThreadPool::hardwareThreads()) : options_.threadCount;
}

ProcessingResult LogProcessor::process(const std::filesystem::path& path) const {
    LogFileReader reader(path, options_.chunkSizeBytes);
    return run(reader);
}

ProcessingResult LogProcessor::process(std::unique_ptr<std::istream> stream) const {
    LogFileReader reader(std::move(stream), options_.chunkSizeBytes);
    return run(reader);
}

ProcessingResult LogProcessor::run(LogFileReader& reader) const {
    const auto start = std::chrono::steady_clock::now();
    unsigned threads = effectiveThreadCount();
    // In automatic mode, never start more workers than there will be chunks:
    // a small file is processed on the calling thread with no pool overhead.
    // An explicit thread count is always honoured (benchmarks rely on that).
    if (options_.threadCount == 0) {
        if (const auto size = reader.sizeHint()) {
            const std::uint64_t chunks = std::max<std::uint64_t>(1, (*size + reader.chunkSize() - 1) / reader.chunkSize());
            threads = static_cast<unsigned>(std::min<std::uint64_t>(threads, chunks));
        }
    }

    ProcessingResult result{.statistics = StatisticsCollector(analysisOptions_)};
    try {
        if (threads <= 1) {
            runSingleThreaded(reader, result);
        } else {
            runMultiThreaded(reader, result, threads);
        }
    } catch (const LogAnalyzerError&) {
        throw; // already meaningful (e.g. FileError from the reader): keep it
    } catch (const std::exception& e) {
        // Anything else is unexpected (a bug, std::bad_alloc, a throwing custom
        // filter). Both modes report it the same way.
        throw ProcessingError(std::string("processing failed: ") + e.what());
    }

    result.performance.threadCount = threads;
    result.performance.bytesProcessed = reader.bytesRead();
    result.performance.duration = std::chrono::steady_clock::now() - start;
    return result;
}

void LogProcessor::runSingleThreaded(LogFileReader& reader, ProcessingResult& result) const {
    while (auto chunk = reader.nextChunk()) {
        mergeChunk(result, processChunk(*chunk));
        reader.recycle(std::move(chunk->data));
    }
}

void LogProcessor::runMultiThreaded(LogFileReader& reader, ProcessingResult& result, unsigned threads) const {
    // Declaration order matters: locals are destroyed in reverse order, so the
    // pool (declared last) is destroyed - and its threads joined - *before*
    // `cancelled` and `inFlight`, which running tasks may still reference.
    std::atomic<bool> cancelled{false};
    std::deque<std::future<ChunkResult>> inFlight;
    ThreadPool pool(threads);

    const std::size_t maxInFlight = static_cast<std::size_t>(threads) * 2;

    // Waits for the oldest chunk and merges it. Futures are consumed strictly
    // in submission (= file) order, which keeps "first N samples" and every
    // other order-sensitive result identical to the single-threaded run.
    const auto mergeOldest = [&] {
        std::future<ChunkResult> future = std::move(inFlight.front());
        inFlight.pop_front();
        ChunkResult part = future.get(); // rethrows a worker's exception here
        reader.recycle(std::move(part.buffer)); // coordinator thread only: no lock needed
        mergeChunk(result, std::move(part));
    };

    try {
        while (auto chunk = reader.nextChunk()) {
            if (inFlight.size() >= maxInFlight) {
                mergeOldest(); // back-pressure: wait before reading further
            }
            // The chunk is moved into the task: the worker owns its data and
            // shares nothing mutable with other threads.
            // `mutable` lets the task move the buffer back out when it is done.
            inFlight.push_back(pool.submit([this, &cancelled, owned = std::move(*chunk)]() mutable -> ChunkResult {
                if (cancelled.load(std::memory_order_relaxed)) {
                    return ChunkResult{.chunkIndex = owned.index}; // another part failed: skip the work
                }
                ChunkResult part = processChunk(owned);
                part.buffer = std::move(owned.data);
                return part;
            }));
        }
        while (!inFlight.empty()) {
            mergeOldest();
        }
    } catch (...) {
        // A worker or the reader failed. Tasks still queued see `cancelled` and
        // return immediately, so the pool destructor (which waits for the queue
        // to drain) finishes quickly. run() turns the exception into a ProcessingError.
        cancelled.store(true, std::memory_order_relaxed);
        throw;
    }
}

LogProcessor::ChunkResult LogProcessor::processChunk(const LogChunk& chunk) const {
    ChunkResult part{.chunkIndex = chunk.index, .statistics = StatisticsCollector(analysisOptions_)};

    forEachLine(chunk.data, chunk.firstLineNumber, [&](std::string_view line, std::size_t lineNumber) {
        ++part.summary.totalLines;
        const ParseResult parsed = parser_.parseLine(line, lineNumber);

        if (!parsed) {
            if (parsed.error() == ParseErrorCode::EmptyLine) {
                ++part.summary.emptyLines;
                return;
            }
            ++part.summary.invalidEntries;
            if (part.invalidSamples.size() < options_.maxInvalidSamples) {
                part.invalidSamples.push_back(
                    {lineNumber, parsed.error(), util::truncate(util::trimRight(line), kMaxExcerptLength)});
            }
            return;
        }

        ++part.summary.validEntries;
        const LogEntryView& entry = parsed.entry();
        if (!filter_.matches(entry)) {
            return;
        }
        ++part.summary.matchedEntries;
        part.statistics.add(entry);
        if (part.matchedSamples.size() < options_.maxMatchedSamples) {
            part.matchedSamples.push_back(entry.toEntry()); // copy only what we keep
        }
    });
    return part;
}

void LogProcessor::mergeChunk(ProcessingResult& total, ChunkResult&& part) const {
    // Results must arrive in file order; otherwise sample lists would differ
    // between single- and multithreaded runs.
    assert(part.chunkIndex == total.performance.chunkCount && "chunks merged out of order");
    ++total.performance.chunkCount;
    total.summary += part.summary;
    total.statistics.merge(std::move(part.statistics));
    appendLimited(total.invalidSamples, std::move(part.invalidSamples), options_.maxInvalidSamples);
    appendLimited(total.matchedSamples, std::move(part.matchedSamples), options_.maxMatchedSamples);
}

} // namespace loganalyzer
