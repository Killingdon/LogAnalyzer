#pragma once

#include "util/TimeUtils.h"

#include <cstdint>
#include <filesystem>
#include <ostream>

namespace loganalyzer {

struct GeneratorOptions {
    std::uint64_t lines = 100'000;
    std::uint32_t seed = 42;
    double malformedRate = 0.001; ///< fraction of lines that are deliberately broken (0..1)
    Timestamp start = Timestamp{std::chrono::sys_days{std::chrono::year{2026} / 10 / 20}};
};

/// Writes realistic synthetic logs, e.g. for benchmarks.
///
/// Output is fully determined by the options: the same seed always produces
/// the same file. To guarantee that across compilers, only std::mt19937 is
/// used (its output sequence is specified by the C++ standard) and never
/// std::uniform_int_distribution, whose algorithm differs between standard
/// library implementations.
class SampleLogGenerator {
public:
    /// Throws ConfigurationError for invalid options.
    explicit SampleLogGenerator(GeneratorOptions options);

    void generate(std::ostream& out) const;

    /// Throws FileError if the file cannot be written.
    void generateToFile(const std::filesystem::path& path) const;

private:
    GeneratorOptions options_;
};

} // namespace loganalyzer
