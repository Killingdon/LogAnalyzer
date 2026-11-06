#include "generation/SampleLogGenerator.h"

#include "common/Errors.h"

#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>

namespace loganalyzer {

namespace {

struct LevelProfile {
    std::string_view name;
    unsigned cumulativePerMille; ///< upper bound of this level's share, out of 1000
};

// Roughly the mix of a production service log.
constexpr std::array<LevelProfile, 6> kLevels{{
    {"TRACE", 30},     // 3.0%
    {"DEBUG", 180},    // 15.0%
    {"INFO", 730},     // 55.0%
    {"WARNING", 880},  // 15.0%
    {"ERROR", 975},    // 9.5%
    {"CRITICAL", 1000} // 2.5%
}};

// Messages contain a "{}" placeholder filled with a number from a small range,
// so that some messages repeat (realistic duplicates) while others vary.
constexpr std::array<std::string_view, 5> kTrace{
    "Entering request handler (thread {})", "Cache lookup for key product:{}", "Acquired lock {}",
    "Serialising response ({} bytes)", "Polling queue partition {}"};
constexpr std::array<std::string_view, 5> kDebug{
    "Cache miss for key product:{}", "SQL query took {} ms", "Retrying connection (attempt {})",
    "Loaded {} records from cache", "Session {} refreshed"};
constexpr std::array<std::string_view, 7> kInfo{
    "Health check passed", "User login successful", "Order created", "Request completed in {} ms",
    "Payment authorised", "Scheduled job {} finished", "Configuration reloaded"};
constexpr std::array<std::string_view, 5> kWarning{
    "High memory usage detected ({}%)", "Slow query detected ({} ms)", "Disk usage at {}%",
    "Rate limit exceeded for client {}", "Invalid login attempt"};
constexpr std::array<std::string_view, 6> kError{
    "Database connection failed", "Payment gateway timeout after 30 s", "Failed to create order: database is read-only",
    "Upstream inventory-service returned 503", "Request timed out after {} ms", "Failed to send e-mail notification"};
constexpr std::array<std::string_view, 3> kCritical{
    "Database unavailable after 3 attempts", "Worker pool exhausted", "Out of memory while processing batch {}"};

constexpr std::array<std::string_view, 6> kSourceFiles{
    "Server.cpp", "DatabasePool.cpp", "OrderService.cpp", "PaymentClient.cpp", "Cache.cpp", "Scheduler.cpp"};

constexpr std::array<std::string_view, 4> kMalformed{
    "    at com.example.OrderService.create(OrderService.java:{})",
    "[2026-13-45 99:99:99] [ERROR] corrupted timestamp",
    "Segmentation fault (core dumped) in worker {}",
    "[{}] truncated line without level"};

class Generator {
public:
    explicit Generator(std::uint32_t seed) : rng_(seed) {}

    /// Uniform-enough integer in [0, bound). The tiny modulo bias is irrelevant
    /// for synthetic data, and unlike std::uniform_int_distribution the result
    /// is identical with every standard library.
    std::uint32_t below(std::uint32_t bound) { return static_cast<std::uint32_t>(rng_() % bound); }

    template <std::size_t N>
    std::string_view pick(const std::array<std::string_view, N>& items) {
        return items[below(static_cast<std::uint32_t>(N))];
    }

private:
    std::mt19937 rng_;
};

/// Replaces the first "{}" in `pattern` with `number`.
void appendWithNumber(std::string& out, std::string_view pattern, std::uint32_t number) {
    const std::size_t hole = pattern.find("{}");
    if (hole == std::string_view::npos) {
        out += pattern;
        return;
    }
    out += pattern.substr(0, hole);
    out += std::to_string(number);
    out += pattern.substr(hole + 2);
}

} // namespace

SampleLogGenerator::SampleLogGenerator(GeneratorOptions options) : options_(options) {
    if (options_.lines == 0) {
        throw ConfigurationError("number of lines to generate must be at least 1");
    }
    if (!(options_.malformedRate >= 0.0 && options_.malformedRate <= 1.0)) { // also rejects NaN
        throw ConfigurationError("malformed rate must be between 0 and 1");
    }
}

void SampleLogGenerator::generate(std::ostream& out) const {
    Generator gen(options_.seed);
    const auto malformedThreshold = static_cast<std::uint32_t>(options_.malformedRate * 1'000'000.0);
    Timestamp now = options_.start;

    std::string buffer;
    constexpr std::size_t kFlushThreshold = std::size_t{1} << 20;
    buffer.reserve(kFlushThreshold + 512);

    for (std::uint64_t i = 0; i < options_.lines; ++i) {
        now += std::chrono::seconds{gen.below(3)}; // 0-2 s between entries

        // NOTE: every random draw is a separate statement. The evaluation order
        // of function arguments is unspecified in C++, so f(gen.pick(), gen.below())
        // could consume the random numbers in a different order on another
        // compiler and break "same seed => same file".
        if (gen.below(1'000'000) < malformedThreshold) {
            const std::string_view pattern = gen.pick(kMalformed);
            const std::uint32_t value = gen.below(500);
            appendWithNumber(buffer, pattern, value);
        } else {
            const unsigned roll = gen.below(1000);
            std::size_t levelIndex = 0;
            while (roll >= kLevels[levelIndex].cumulativePerMille) {
                ++levelIndex;
            }
            std::format_to(std::back_inserter(buffer), "[{}] [{}] ", util::formatTimestamp(now),
                           kLevels[levelIndex].name);
            if (gen.below(10) < 3) { // 30% of entries carry a source location
                const std::string_view file = gen.pick(kSourceFiles);
                const std::uint32_t line = 20 + gen.below(480);
                std::format_to(std::back_inserter(buffer), "[{}:{}] ", file, line);
            }
            const std::uint32_t number = 1 + gen.below(50);
            switch (levelIndex) {
            case 0:
                appendWithNumber(buffer, gen.pick(kTrace), number);
                break;
            case 1:
                appendWithNumber(buffer, gen.pick(kDebug), number);
                break;
            case 2:
                appendWithNumber(buffer, gen.pick(kInfo), number * 10);
                break;
            case 3:
                appendWithNumber(buffer, gen.pick(kWarning), 50 + number);
                break;
            case 4:
                appendWithNumber(buffer, gen.pick(kError), 1000 * (1 + number % 5));
                break;
            default:
                appendWithNumber(buffer, gen.pick(kCritical), number);
                break;
            }
        }
        buffer += '\n';

        if (buffer.size() >= kFlushThreshold) {
            out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            buffer.clear(); // keeps the capacity: no reallocation next time
        }
    }
    out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}

void SampleLogGenerator::generateToFile(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        throw FileError(path, "cannot create file (does the directory exist and is it writable?)");
    }
    generate(out);
    out.flush();
    if (!out) {
        throw FileError(path, "write failed (disk full?)");
    }
}

} // namespace loganalyzer
