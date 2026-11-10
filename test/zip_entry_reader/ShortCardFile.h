#pragma once

// A copy of a file at a short temporary path, for a test that loads a font of the repository through SdCardFont.
//
// SdCardFont keeps the path it loads (filePath_, 128 bytes: ample for the card's "/.crosspoint/fonts/<family>/<file>")
// and reopens the file by it. Under a host checkout the same file's path depends on where the checkout lies, and a
// deep one passes the limit: the load then fails ("Path too long"). The copy's path does not depend on the checkout.
// It is removed with the object, which must outlive every font loaded from it.
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

class ShortCardFile {
 public:
  ShortCardFile() = default;
  // A copy of `source`, named after the process and a counter: ctest -j runs each test in its own process.
  static ShortCardFile copyOf(const std::string& source) {
    static std::atomic<unsigned> counter{0};
    ShortCardFile file;
    const std::filesystem::path from(source);
    file.path_ = (std::filesystem::temp_directory_path() /
                  ("card_" + std::to_string(getpid()) + "_" + std::to_string(counter++) + from.extension().string()))
                     .string();
    std::error_code error;
    std::filesystem::copy_file(from, file.path_, std::filesystem::copy_options::overwrite_existing, error);
    EXPECT_FALSE(error) << "copying " << source << ": " << error.message();
    return file;
  }
  ShortCardFile(ShortCardFile&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }
  ShortCardFile& operator=(ShortCardFile&& other) noexcept {
    if (this != &other) {
      remove();
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }
  ShortCardFile(const ShortCardFile&) = delete;
  ShortCardFile& operator=(const ShortCardFile&) = delete;
  ~ShortCardFile() { remove(); }

  const std::string& path() const { return path_; }
  const char* c_str() const { return path_.c_str(); }

 private:
  void remove() {
    if (path_.empty()) return;
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    path_.clear();
  }

  std::string path_;
};
