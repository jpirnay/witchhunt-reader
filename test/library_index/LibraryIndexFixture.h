#pragma once

// Writes the working files a library build produces, so publish and join can be tested on their own;
// the walk that writes them for real comes with the builder.

#include <BuildArena.h>
#include <LibraryFormat.h>
#include <LibraryIndexReader.h>
#include <LibraryKeys.h>
#include <LibraryPublish.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

class LibraryIndexFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = (std::filesystem::temp_directory_path() /
            ("library_index_" + std::string(info->test_suite_name()) + "_" + info->name()))
               .generic_string();
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    std::ofstream(at("paths.bin"), std::ios::binary);  // a build always leaves a paths file, if empty
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string at(const char* name) const { return dir_ + "/" + name; }

  // Appends a blob string to a working file; returns where it starts.
  uint32_t appendString(const std::string& file, const std::string& s) const {
    std::error_code ec;
    const auto offset = static_cast<uint32_t>(std::filesystem::exists(file) ? std::filesystem::file_size(file) : 0);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    const auto len = static_cast<uint16_t>(s.size());
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
    return offset;
  }

  template <typename T>
  void writeAll(const std::string& file, const std::vector<T>& items) const {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(items.data()), static_cast<std::streamsize>(items.size() * sizeof(T)));
  }

  // An author's names-file entry, as the builder records one when it resolves a book.
  void addName(const std::string& name, const std::string& fileAs = "") const {
    const uint32_t hash = LibraryKeys::authorHash(name);
    const uint8_t flags = fileAs.empty() ? 0 : 1;
    {
      std::ofstream out(at("names.bin"), std::ios::binary | std::ios::app);
      out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
      out.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
    }
    appendString(at("names.bin"), name);
    appendString(at("names.bin"), LibraryKeys::authorFilingName(name, fileAs));
  }

  // A book record whose path goes into the paths working file.
  library::BookRecord book(const std::string& path, const std::string& author, const uint32_t date = 0,
                           const uint32_t firstSeen = 1, const uint32_t size = 100) const {
    const std::string file = path.substr(path.rfind('/') + 1);
    return {LibraryKeys::bookIdentity(file.c_str(), size), LibraryKeys::authorHash(author), date,
            appendString(at("paths.bin"), path),           library::SIDECAR_NONE,           firstSeen};
  }

  // Publishes `records` in identity order, as a join leaves them.
  bool publish(std::vector<library::BookRecord> records, const uint32_t buildGen = 1, BuildArena* arena = nullptr,
               const bool partial = false) const {
    std::sort(records.begin(), records.end(),
              [](const library::BookRecord& a, const library::BookRecord& b) { return a.identity < b.identity; });
    writeAll(at("records.bin"), records);
    LibraryPublish::Input in;
    in.recordsPath = at("records.bin");
    in.pathsPath = at("paths.bin");
    in.namesPath = at("names.bin");
    in.bookCount = static_cast<uint16_t>(records.size());
    in.buildGen = buildGen;
    in.acceptRules = 1;
    in.partial = partial;
    BuildArena framebuffer(48000);  // the X3/X4 secondary framebuffer
    return LibraryPublish::publish(in, arena != nullptr ? *arena : framebuffer, at("library.bin"));
  }

  // The published authors in table order; the reserved ones by a marker.
  static std::vector<std::string> authorNames(LibraryIndexReader& index) {
    std::vector<std::string> names;
    for (uint16_t i = 0; i < index.header().authorCount; ++i) {
      library::AuthorRecord author{};
      std::string name;
      EXPECT_TRUE(index.author(i, author));
      EXPECT_TRUE(index.authorName(author, name));
      if (author.hash == library::AUTHOR_UNKNOWN) name = "<unknown>";
      if (author.hash == library::AUTHOR_PENDING) name = "<pending>";
      names.push_back(name);
    }
    return names;
  }

  static std::string pathOf(LibraryIndexReader& index, const uint16_t record) {
    library::BookRecord book{};
    std::string path;
    EXPECT_TRUE(index.book(record, book));
    EXPECT_TRUE(index.blobString(book.pathOff, path));
    return path;
  }

  // Paths of an author's books, in table order.
  static std::vector<std::string> booksOf(LibraryIndexReader& index, const uint16_t authorIndex) {
    library::AuthorRecord author{};
    EXPECT_TRUE(index.author(authorIndex, author));
    std::vector<std::string> paths;
    for (uint32_t slot = author.firstBook; slot < uint32_t{author.firstBook} + author.count; ++slot) {
      uint16_t record = 0;
      EXPECT_TRUE(index.authorBook(slot, record));
      paths.push_back(pathOf(index, record));
    }
    std::sort(paths.begin(), paths.end());
    return paths;
  }

  std::string dir_;
};
