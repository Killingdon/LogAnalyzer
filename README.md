# LogAnalyzer

A multithreaded log analysis tool written in modern C++20. It streams large log files in chunks, parses them with no per-line allocation, filters and analyses entries in parallel on a thread pool, and produces text, CSV or JSON reports. Single-threaded and multithreaded runs are guaranteed to produce **identical** results, and the built-in benchmark checks this on every run.

```
loganalyzer analyze application.log --threads 4 --level ERROR,CRITICAL --format json --output report.json
```

---

## Contents

1. [Features](#features)
2. [Technology stack](#technology-stack)
3. [Architecture](#architecture)
4. [Project structure](#project-structure)
5. [Installation](#installation)
6. [Building](#building)
7. [Usage](#usage)
8. [Example output](#example-output)
9. [Testing](#testing)
10. [Benchmarking](#benchmarking)
11. [Design decisions and C++ concepts](#design-decisions-and-c-concepts)
12. [Limitations and future improvements](#limitations-and-future-improvements)

---

## Features

**Parsing**
- Format: `[2026-10-20 14:30:22] [LEVEL] [optional File.cpp:42] message`
- Levels `TRACE`, `DEBUG`, `INFO`, `WARNING`, `ERROR`, `CRITICAL` (case-insensitive; `WARN`, `ERR`, `FATAL`, `CRIT` accepted as aliases)
- Full calendar validation (`2026-02-30` and `25:61:00` are rejected), `T` separator and fractional seconds accepted
- Malformed lines never crash the program. They are counted and reported with their line number and a precise reason
- Handles CRLF line endings, a UTF-8 byte-order mark, a missing final newline, and lines longer than the chunk size

**Processing**
- Streams the file in fixed-size chunks (default 1 MiB), so memory use does not grow with file size
- Single-threaded mode or a thread pool with N workers (`--threads N|auto`)
- Bounded number of chunks in flight (back-pressure) and recycled chunk buffers
- Worker exceptions are caught, reported as a clear error, and all threads are shut down cleanly

**Filtering** (combinable, all must match)
- By level: `--level WARNING,ERROR,CRITICAL`
- By time range: `--from "2026-10-20 10:00:00" --to 2026-10-20` (a bare date for `--to` means end of day)
- By keyword(s): `--keyword database,timeout` with `--match any|all` and optional `--case-sensitive`

**Analysis**
- Total, valid, invalid, empty and matching line counts
- Level distribution with percentages, and error rate (ERROR + CRITICAL)
- Most frequent error messages, with first and last occurrence
- Most active periods per minute, hour or day
- First and last timestamp
- Processing time and throughput (lines/s, MiB/s)
- Optional duplicate detection (`--duplicates`): repeated messages regardless of timestamp, with count and first/last occurrence

**Reports**: TXT (console), CSV (spreadsheet-ready) and JSON (machine-readable), written to the console or to a file. JSON output is always valid, even when the log contains invalid UTF-8 or control characters. CSV output is protected against spreadsheet formula injection.

**Benchmarking**: compares 1/2/4/8 threads (configurable), reports median time, throughput, speedup and efficiency, and verifies that every configuration produced the same analysis.

**Test data**: `loganalyzer generate` writes realistic synthetic logs of any size, byte-identical for a given seed on every compiler.

---

## Technology stack

| | |
|---|---|
| Language | C++20 (`std::format`, `<chrono>` calendar, designated initializers, defaulted `operator==`, heterogeneous lookup) |
| Build | CMake ≥ 3.25 |
| Concurrency | `std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic`, `std::packaged_task` / `std::future` |
| Tests | GoogleTest 1.15.2 (downloaded automatically by CMake and verified with SHA-256, or taken from the system if installed) |
| Dependencies | None at runtime. Only the standard library. |

**Verified with:** MSVC 19.51 (Visual Studio 2026) and Clang 21.1 on Windows 11. Both build with zero warnings at `/W4` and `-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast`. The suite also passes under MSVC AddressSanitizer.
GCC was not available on the development machine. The code uses only standard C++20, so GCC 13+ (the first version with `<format>`) is expected to work but has not been tested.

---

## Architecture

```
                    ┌──────────────────────┐
  argv ───────────▶ │ CommandLineParser    │  pure function: args -> Command (std::variant)
                    └──────────┬───────────┘
                               ▼
                    ┌──────────────────────┐
                    │ CommandLineInterface │  runs the command, maps errors to exit codes
                    └──────────┬───────────┘
            ┌──────────────────┼─────────────────────┬──────────────────┐
            ▼                  ▼                     ▼                  ▼
     ┌─────────────┐   ┌───────────────┐    ┌─────────────────┐  ┌──────────────────┐
     │ LogProcessor│   │ LogAnalyzer   │    │ ReportGenerator │  │ BenchmarkRunner  │
     └──────┬──────┘   └───────────────┘    └────────┬────────┘  └──────────────────┘
            │      raw stats ──▶ AnalysisReport ──▶  │ ReportFormatter (Strategy)
            │                                        ├─ TextReportFormatter
   ┌────────┼──────────┬─────────────┐               ├─ CsvReportFormatter
   ▼        ▼          ▼             ▼               └─ JsonReportFormatter
LogFile  LogParser  LogFilter   StatisticsCollector
Reader              (criteria)  (per-chunk, mergeable)
   │
   └── ThreadPool ── BlockingQueue
```

### The multithreaded pipeline

```
 calling thread (coordinator)                     worker threads (ThreadPool)
 ────────────────────────────                     ───────────────────────────
 reader.nextChunk() ──── submit(chunk k) ──────▶  parse every line (string_view, no allocation)
        │                                         filter (read-only LogFilter, no locks)
        │  (at most 2 × threads chunks in flight) count into a *private* StatisticsCollector
        ▼                                                    │
 future[k].get()  ◀──── ChunkResult (stats + buffer) ────────┘
 merge into total in file order, recycle buffer
```

Key properties:

- **No shared mutable state on the hot path.** Each worker owns its chunk and its own `StatisticsCollector`. The parser and filter are immutable and shared read-only, so no mutex is ever taken per line.
- **Map-reduce merge.** Every statistic is a sum, a minimum or a maximum, and merging is commutative and associative. Splitting the input differently therefore cannot change the result. This is why multithreaded output equals single-threaded output.
- **Deterministic order.** Futures are consumed in submission order, so order-sensitive results (the "first 20 matching entries", tie-breaking in top-N lists) match the single-threaded run exactly.
- **Back-pressure.** The coordinator never has more than `2 × threads` chunks in flight. Memory stays bounded and the reader cannot run ahead of the workers.
- **Safe failure.** A worker exception travels through its `std::future`. The coordinator sets an atomic `cancelled` flag so queued tasks return immediately, the pool joins every thread, and the CLI reports a `ProcessingError`.

### Classes

| Class | Responsibility |
|---|---|
| `LogEntry` / `LogEntryView` | Owning entry (for samples and the API) and zero-copy view (for the hot path) |
| `LogParser` | Stateless line parser that returns `ParseResult` (entry or error code). Never throws, never allocates |
| `LogFileReader` | Streams a file as chunks of whole lines and recycles their buffers |
| `FilterCriterion`, `LevelCriterion`, `TimeRangeCriterion`, `KeywordCriterion` | One filter condition each; extensible by subclassing (Open/Closed) |
| `LogFilter` | AND-combination of criteria, cheapest checked first |
| `BlockingQueue<T>` | Thread-safe (optionally bounded) producer/consumer queue |
| `ThreadPool` | Fixed set of workers; `submit()` returns a `std::future`; RAII shutdown |
| `StatisticsCollector` | Per-chunk raw counters and maps with a `merge()` operation |
| `LogProcessor` | Coordinates reading, parsing, filtering and collection in either mode |
| `LogAnalyzer` | Turns raw statistics into a finished `AnalysisReport` (percentages, sorted top-N) |
| `ReportFormatter` + 3 implementations, `ReportGenerator` | Strategy pattern for output formats; file writing and path validation |
| `BenchmarkRunner` | Times each thread count, computes speedup and efficiency, verifies results |
| `SampleLogGenerator` | Deterministic synthetic logs |
| `CommandLineParser` / `CommandLineInterface` | Argument parsing (pure, testable) and execution (error → exit code) |

Errors use a small exception hierarchy rooted in `LogAnalyzerError`: `FileError`, `ConfigurationError`, `ProcessingError`, `ThreadPoolError`, `ReportError`, `CommandLineError`. Malformed *log lines* are not exceptions. They are expected data, returned as values.

---

## Project structure

```
.
├── CMakeLists.txt            # library + executable + install rule, warning flags
├── cmake/Version.h.in        # version header generated by CMake
├── include/                  # public headers, one folder per module
│   ├── analysis/             # StatisticsCollector.h, LogAnalyzer.h
│   ├── benchmarking/         # BenchmarkRunner.h
│   ├── cli/                  # CommandLineParser.h, CommandLineInterface.h
│   ├── common/               # Errors.h
│   ├── filtering/            # LogFilter.h
│   ├── generation/           # SampleLogGenerator.h
│   ├── models/               # LogEntry.h
│   ├── parser/               # LogParser.h
│   ├── processing/           # LogProcessor.h, ProcessingResult.h
│   ├── reader/               # LogFileReader.h
│   ├── reporting/            # ReportFormatter.h, ReportGenerator.h
│   ├── threading/            # BlockingQueue.h, ThreadPool.h
│   └── util/                 # StringUtils.h, TimeUtils.h
├── src/                      # implementations (same layout) + main.cpp
│   └── reporting/JsonWriter.h  # private helper header
├── tests/                    # GoogleTest suites (172 tests)
├── samples/                  # application.log, server.log, malformed.log
└── reports/                  # default place for generated reports (git-ignored)
```

All logic lives in the static library `loganalyzer_core`. The executable is a five-line `main.cpp`, and the tests link the same library, so they exercise exactly the code that ships.

---

## Installation

Requirements: a C++20 compiler with `<format>` support (see [Technology stack](#technology-stack)), CMake 3.25+, and Git. An internet connection is needed on the first configure to download GoogleTest, unless it is installed or tests are disabled (`-DLOGANALYZER_BUILD_TESTS=OFF`).

```bash
git clone <your-repository-url> LogAnalyzer
cd LogAnalyzer
```

To install the executable after building:

```bash
cmake --install build --prefix ~/.local     # puts loganalyzer in ~/.local/bin
```

---

## Building

**Any platform (Ninja or Makefiles, Release by default):**

```bash
cmake -S . -B build
cmake --build build --parallel
```

**Visual Studio (multi-config):**

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --parallel
```

You can also open the folder directly in Visual Studio or VS Code; both detect `CMakeLists.txt`.

| Option | Default | Effect |
|---|---|---|
| `LOGANALYZER_BUILD_TESTS` | `ON` | Build the GoogleTest suite |
| `LOGANALYZER_WARNINGS_AS_ERRORS` | `OFF` | `/WX` or `-Werror` |

The executable is `build/loganalyzer` (or `build/Release/loganalyzer.exe` with Visual Studio).

---

## Usage

```bash
loganalyzer --help                 # commands
loganalyzer analyze --help         # all options of a command
loganalyzer --version
```

### analyze

```bash
# Full analysis, printed to the console
loganalyzer analyze samples/application.log

# Four worker threads (default: auto = one per hardware thread, capped by the number of chunks)
loganalyzer analyze samples/application.log --threads 4

# Filters (combinable)
loganalyzer analyze samples/application.log --level ERROR
loganalyzer analyze samples/application.log --level WARNING,ERROR,CRITICAL
loganalyzer analyze samples/application.log --keyword database
loganalyzer analyze samples/application.log --keyword database,read-only --match all
loganalyzer analyze samples/application.log --from "2026-10-20 10:00:00" --to "2026-10-20 18:00:00"

# Duplicates and finer activity buckets
loganalyzer analyze samples/application.log --duplicates --period minute --top 5

# Reports to files (format follows the extension unless --format is given)
loganalyzer analyze samples/application.log --format json --output reports/report.json
loganalyzer analyze samples/application.log --output reports/report.csv
```

| Option | Description |
|---|---|
| `-t, --threads <N\|auto>` | 1 = single-threaded; N = N workers (max 256) |
| `-l, --level <LIST>` | Levels to keep, comma-separated or repeated |
| `-k, --keyword <LIST>` | Keywords searched in the message |
| `--match any\|all` | Combine several keywords with OR (default) or AND |
| `-c, --case-sensitive` | Case-sensitive keyword search |
| `--from`, `--to <TIME>` | Inclusive bounds, `"YYYY-MM-DD HH:MM:SS"` or `YYYY-MM-DD` |
| `--top <N>` | Length of the top-N lists (default 10) |
| `--period minute\|hour\|day` | Bucket for "most active periods" (default hour) |
| `-d, --duplicates` | Detect repeated messages |
| `--samples <N>` | Matching / invalid lines included in the report (default 20) |
| `--chunk-size <KiB>` | Chunk size (default 1024) |
| `-f, --format txt\|csv\|json` | Report format |
| `-o, --output <FILE>` | Write the report to a file |

Exit codes: `0` success, `1` runtime error (missing file, unwritable output, …), `2` invalid command line.

### benchmark and generate

```bash
loganalyzer generate samples/large.log --lines 2000000 --seed 7
loganalyzer benchmark samples/large.log                         # 1, 2, 4, 8 threads
loganalyzer benchmark samples/large.log --threads 1,2,4,8,16 --iterations 5 --duplicates
```

---

## Example output

All output below was produced by the program in this repository (MSVC Release build).

**`loganalyzer analyze samples/application.log --top 3`**

```
==============================================================================
 LogAnalyzer Report
==============================================================================
 Input             : samples/application.log
 Filters           : none

PROCESSING SUMMARY
------------------------------------------------------------------------------
 Total lines       : 80
 Valid entries     : 80
 Invalid entries   : 0
 Empty lines       : 0
 Errors            : 12 (15.00% of analysed entries)
 First timestamp   : 2026-10-20 08:00:01
 Last timestamp    : 2026-10-20 18:00:00

LOG LEVEL DISTRIBUTION
------------------------------------------------------------------------------
 TRACE                1    1.25%  #
 DEBUG                9   11.25%  #####
 INFO                47   58.75%  ##############################
 WARNING             11   13.75%  #######
 ERROR               11   13.75%  #######
 CRITICAL             1    1.25%  #

TOP ERRORS (5 distinct ERROR/CRITICAL messages)
------------------------------------------------------------------------------
   #    Count  First occurrence                Last occurrence
   1        4  2026-10-20 10:21:40 (line 32)   2026-10-20 14:30:30 (line 70)
      Database connection failed
   2        3  2026-10-20 09:12:30 (line 21)   2026-10-20 12:58:03 (line 61)
      Payment gateway timeout after 30 s
   3        3  2026-10-20 10:22:15 (line 39)   2026-10-20 10:23:30 (line 42)
      Failed to create order: database is read-only

MOST ACTIVE PERIODS (per hour)
------------------------------------------------------------------------------
 2026-10-20 10:00:00            25 entries  ##############################
 2026-10-20 08:00:00            16 entries  ###################
 2026-10-20 09:00:00            10 entries  ############

PERFORMANCE
------------------------------------------------------------------------------
 Threads           : 1 (single-threaded)
 Chunks            : 1
 Bytes processed   : 4,832
 Processing time   : 277 us
 Throughput        : 288,288 lines/s (16.6 MiB/s)
==============================================================================
```

(A 5 KB file fits in one chunk, so automatic mode correctly uses a single thread.)

**Filter results:** `loganalyzer analyze samples/server.log --level ERROR,CRITICAL --keyword upstream,timed`

```
 Filters           : level in {ERROR, CRITICAL}
                   : message contains any of "upstream", "timed" (case-insensitive)
...
MATCHING ENTRIES (showing 6 of 6)
------------------------------------------------------------------------------
      14  2026-10-21 02:31:17  ERROR     [UpstreamClient.cpp:150] Upstream inventory-service returned 503
      15  2026-10-21 02:31:18  ERROR     [UpstreamClient.cpp:150] Upstream inventory-service returned 503
      16  2026-10-21 02:31:20  ERROR     [UpstreamClient.cpp:150] Upstream inventory-service returned 503
      27  2026-10-21 06:02:47  ERROR     [RequestHandler.cpp:310] Request timed out after 5000 ms: POST /api/v1/checkout
      28  2026-10-21 06:02:48  ERROR     [RequestHandler.cpp:310] Request timed out after 5000 ms: POST /api/v1/checkout
      33  2026-10-21 07:30:18  ERROR     [UpstreamClient.cpp:150] Upstream inventory-service returned 503
```

**Malformed input:** `loganalyzer analyze samples/malformed.log --samples 4`

```
 Total lines       : 23
 Valid entries     : 10
 Invalid entries   : 11
 Empty lines       : 2
...
INVALID ENTRIES (showing 4 of 11)
------------------------------------------------------------------------------
 line 3      missing timestamp: expected '[YYYY-MM-DD HH:MM:SS]' at start of line
             > This line has no timestamp or level at all
 line 7      missing timestamp: expected '[YYYY-MM-DD HH:MM:SS]' at start of line
             > 2026-10-22 09:00:04 [ERROR] Missing brackets around the t...
 line 8      invalid timestamp
             > [2026-10-22 25:61:00] [ERROR] Impossible time of day
 line 9      invalid timestamp
             > [2026-02-30 09:00:05] [ERROR] February 30th does not exist
```

**JSON** (`--level ERROR --top 1 --format json`; excerpt, small objects compacted onto one line for brevity)

```json
{
  "tool": { "name": "loganalyzer", "version": "1.0.0" },
  "input": "samples/application.log",
  "filters": [ "level in {ERROR}" ],
  "summary": {
    "total_lines": 80,
    "valid_entries": 80,
    "invalid_entries": 0,
    "empty_lines": 0,
    "matched_entries": 11,
    "error_count": 11,
    "error_percentage": 100.00,
    "first_timestamp": "2026-10-20 09:12:30",
    "last_timestamp": "2026-10-20 14:30:30"
  },
  "top_errors": [
    {
      "message": "Database connection failed",
      "count": 4,
      "first_occurrence": { "line": 32, "timestamp": "2026-10-20 10:21:40" },
      "last_occurrence": { "line": 70, "timestamp": "2026-10-20 14:30:30" }
    }
  ],
  ...
}
```

**Errors** (to stderr, with distinct exit codes):

```
$ loganalyzer analyze missing.log
error: 'missing.log': file does not exist                                  (exit 1)

$ loganalyzer analyze app.log --from 2026-10-21 --to 2026-10-20
error: invalid time range: --from (2026-10-21 00:00:00) is after --to (2026-10-20 23:59:59)
Run 'loganalyzer --help' for usage.                                         (exit 2)

$ loganalyzer analyze app.log -o no/such/dir/r.json
error: cannot write report to 'no/such/dir/r.json': directory 'no/such/dir' does not exist
```

---

## Testing

```bash
ctest --test-dir build --output-on-failure            # single-config generators
ctest --test-dir build -C Release --output-on-failure # Visual Studio
# or run the binary directly, e.g. only the concurrency tests:
build/tests/loganalyzer_tests --gtest_filter='*Concurrency*:*ThreadPool*'
```

The suite has 172 tests:

| Suite | Covers |
|---|---|
| `LogParserTests` | valid lines, every level, source locations, CRLF, 14 kinds of malformed lines, timestamp and calendar validation |
| `LogFileReaderTests` | missing file, directory, empty file, chunk boundaries for chunk sizes 1 B to 1 MiB, BOM, no final newline, over-long lines |
| `LogFilterTests` | each criterion, combinations, inclusive bounds, case sensitivity, any/all, invalid configuration, custom criteria |
| `LogAnalyzerTests` | counts, percentages, top errors, active periods, duplicates, empty input, **merge = sequential for every split point** |
| `ThreadPoolTests` | results via futures, exception propagation, move-only tasks, shutdown semantics, concurrent submitters, blocking queue |
| `LogProcessorTests` | **multithreaded = single-threaded** for 1/2/3/4/8/16 threads × 3 chunk sizes, with filters, worker failure handling |
| `ReportGeneratorTests` | all sections, JSON validity and escaping (invalid UTF-8, control chars), CSV quoting and formula injection, file errors |
| `CommandLineTests` | every option, 18 invalid-argument cases, exit codes, end-to-end runs in-process |
| `BenchmarkAndGeneratorTests` | baseline handling, result verification, generator determinism and malformed rate |

Also verified during development: all tests pass under MSVC AddressSanitizer, and the 47 threading tests passed 30 consecutive repetitions under ASan (`--gtest_repeat=30`).

---

## Benchmarking

```bash
loganalyzer generate samples/large.log --lines 2000000 --seed 7
loganalyzer benchmark samples/large.log --iterations 5
```

Each configuration runs the full pipeline (read, parse, filter, collect statistics) on the same file after an untimed warm-up run. The **median** of the timed runs is reported, and every run's analysis is compared with the single-threaded baseline.

### Measured results

Machine: Intel Core i7-14700HX laptop (20 cores / 28 threads: 8 performance + 12 efficiency cores), 32 GB RAM, Windows 11, file in the OS cache. MSVC 19.51 Release build. Input: 2,000,000 generated lines, 116.4 MiB.

**Default analysis** (`benchmark large.log --iterations 5`):

```
  Threads        Median          Best         Lines/s   Speedup  Efficiency  Results
  ----------------------------------------------------------------------------
        1     300.32 ms     296.27 ms       6,659,618     1.00x      100.0%  baseline
        2     139.09 ms     137.84 ms      14,379,633     2.16x      108.0%  identical
        4      81.30 ms      79.32 ms      24,599,701     3.69x       92.3%  identical
        8     121.45 ms      87.75 ms      16,468,102     2.47x       30.9%  identical

All configurations produced identical analysis results.
```

**With duplicate detection and per-minute buckets** (`--duplicates --period minute --threads 1,2,4,8,16`):

```
  Threads        Median          Best         Lines/s   Speedup  Efficiency  Results
  ----------------------------------------------------------------------------
        1     429.85 ms     411.67 ms       4,652,776     1.00x      100.0%  baseline
        2     243.67 ms     196.49 ms       8,207,751     1.76x       88.2%  identical
        4     137.82 ms     136.90 ms      14,511,997     3.12x       78.0%  identical
        8     151.25 ms     147.32 ms      13,223,000     2.84x       35.5%  identical
       16     153.04 ms     148.50 ms      13,068,564     2.81x       17.6%  identical
```

### Interpreting the numbers

- **Why is 2 threads more than 2× faster?** "Threads" counts *worker* threads. In multithreaded mode the calling thread also works: it reads the file and merges results while the workers parse. In single-threaded mode one thread does everything in sequence.
- **Why does scaling stop at about 4 threads?** Amdahl's law. Reading the file and merging results happen on one coordinator thread. Measured on its own, the reader takes about **60 ms** for this file, so no number of workers can bring the total much below that, and 4 workers already get close. Adding more workers only adds scheduling and merge overhead.
- **Why is the 8-thread median noisy?** On this hybrid laptop CPU, the run-to-run variation at 8+ threads is large. In separate runs the 8-thread median was 88 ms and 121 ms, while the best times stayed at about 88 ms. Threads scheduled on efficiency cores and power management are the likely causes. Measure on your own machine before drawing conclusions.
- **Why does duplicate detection scale worse?** It keeps a map of every distinct message per chunk. Those maps are merged on the coordinator thread, which adds serial work.
- **An optimisation found by measuring:** an earlier version allocated a new 1 MiB buffer for every chunk. A standalone timing of the reader showed it was the bottleneck (~89 ms). Recycling buffers brought it down to ~59 ms, and the 4-thread result improved from ~103 ms to ~84 ms.

Your numbers will differ by CPU, storage, compiler and whether the file is cached.

---

## Design decisions and C++ concepts

Concepts worth being able to explain in an interview:

| Concept | Where | Why |
|---|---|---|
| `std::string_view` / zero-copy parsing | `LogParser`, `LogEntryView` | Parsing millions of lines without allocating; a `LogEntry` copy is made only for the few entries kept as samples |
| Error values instead of exceptions | `ParseResult` (variant of entry / error code) | Malformed lines are expected data; exceptions would be slow and misleading |
| Exception hierarchy | `common/Errors.h` | Callers can catch exactly what they can handle; the CLI maps each kind to an exit code |
| RAII | `ThreadPool` destructor, `std::lock_guard`, test `TempDirectory` | Threads always joined, locks always released, temp files always removed, even on exceptions |
| Mutex + condition variables | `BlockingQueue` | Classic producer/consumer; predicate `wait` handles spurious wake-ups |
| `std::packaged_task` / `std::future` | `ThreadPool::submit` | Return values *and exceptions* travel from worker to caller |
| `std::atomic` with relaxed ordering | pool counters, `cancelled` flag | Lock-free counters and a stop signal that need no ordering with other data |
| Map-reduce with a mergeable accumulator | `StatisticsCollector::merge` | Lock-free parallelism with results independent of the split |
| Move semantics | chunks moved into tasks, node `extract()` when merging maps, buffer recycling | Ownership transfer without copying megabytes of data |
| Heterogeneous lookup (C++20) | `TransparentStringHash` | `unordered_map<string>::find(string_view)` without building a temporary string |
| `std::partial_sort` | top-N lists | O(n log k) instead of sorting everything |
| Strategy pattern + factory | `ReportFormatter`, `ReportGenerator::createFormatter` | A new output format is one new class |
| Open/Closed principle | `FilterCriterion` | New filter types without changing `LogFilter` or the processor |
| `std::variant` + `std::visit` | `Command` | The compiler checks that every command is handled |
| Dependency injection | `CommandLineInterface(out, err)` | The CLI is tested in-process with `std::ostringstream` |
| Unspecified evaluation order | `SampleLogGenerator` | `f(rng(), rng())` may differ between compilers; draws are sequenced explicitly |

---

## Limitations and future improvements

- **Multi-line entries** (stack traces) are reported as invalid lines. A future option could attach continuation lines to the previous entry.
- **Message normalisation**: messages that differ only in IDs ("order 123 failed", "order 456 failed") count as different messages. Replacing numbers with a placeholder would group them.
- **Parallel reading**: the single reader is the scaling limit. Memory-mapped files or splitting the file by byte ranges (each worker finding its own line boundaries) would remove it.
- **Tree-shaped merging** of per-chunk statistics would reduce serial work when duplicate detection is enabled.
- **Configurable log formats** (custom patterns or regex), time zones, and compressed (`.gz`) input.
- **Streaming/follow mode** (`tail -f`-style live analysis).
- **Optional SQLite export** for querying results; the core intentionally has no external dependencies.
- **CI** (GitHub Actions) building with GCC, Clang and MSVC, plus ThreadSanitizer on Linux.
