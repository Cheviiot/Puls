#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace puls::testing {

// A uniquely named directory in the system temporary directory, removed
// with its contents at the end of the scope.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() /
                ("puls-test-" + std::to_string(random()) + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace puls::testing
