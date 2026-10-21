#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace opencdc::test {

/// Creates a unique temporary directory and removes it on destruction.
class TempDir {
public:
    TempDir() {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("opencdc_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// Writes `content` to `relative` (creating parent directories) and returns the full path.
    std::filesystem::path write(const std::string& relative, std::string_view content) const {
        auto file = path_ / relative;
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary);
        if (!out)
            throw std::runtime_error("cannot write " + file.string());
        out << content;
        return file;
    }

private:
    std::filesystem::path path_;
};

inline std::filesystem::path examplesDir() {
    return std::filesystem::path(OPENCDC_EXAMPLES_DIR);
}

} // namespace opencdc::test
