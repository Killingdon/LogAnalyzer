#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace loganalyzer::test {

/// Creates a unique temporary directory and removes it (recursively) when the
/// object goes out of scope - RAII keeps tests from leaking files even when an
/// assertion fails half-way through.
class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("loganalyzer_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ec; // never throw from a destructor
        std::filesystem::remove_all(path_, ec);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// Writes `content` verbatim (binary mode) to a file in this directory.
    std::filesystem::path writeFile(const std::string& name, std::string_view content) const {
        const auto file = path_ / name;
        std::ofstream out(file, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        return file;
    }

private:
    std::filesystem::path path_;
};

inline std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline std::filesystem::path samplePath(std::string_view name) {
    return std::filesystem::path(LOGANALYZER_SAMPLES_DIR) / name;
}

} // namespace loganalyzer::test
