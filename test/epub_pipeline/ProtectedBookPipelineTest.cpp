// A protected book through the whole pipeline, the real lib/BookProtection in place of the stub the
// other pipeline tests link: the index build (OPF, NCX or nav, book.bin), every section, the images,
// all read through ZipFile's decryption. A protected copy lays out exactly as the plain book does.
// And the books that must not open: no key, a damaged key, an ended loan, no clock.
#include <BookKey.h>
#include <BookSession.h>
#include <CacheAtRest.h>
#include <CacheCipherFile.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <ZipFile.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Epub.h"
#include "PipelineRunner.h"
#include "ProtectedEpub.h"

namespace fs = std::filesystem;

namespace {

const std::vector<uint8_t> kKey = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                   0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
constexpr int64_t kToday = 1795000000;  // 2026-11-18

class ProtectedBookPipelineTest : public testing::Test {
 protected:
  void SetUp() override {
    work_ = fs::temp_directory_path() / ("protected_pipeline_" + std::to_string(getpid()) + "_" +
                                         testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work_);
    fs::create_directories(work_);
    HalClock::testLoanNow() = kToday;
    Storage.setCacheKeySource(book_protection::cacheFolderKey);  // as main.cpp installs it
  }
  void TearDown() override {
    ZipFile::unprotect(book_);
    Storage.setCacheKeySource(nullptr);
    fs::remove_all(work_);
  }

  // Every file's bytes under `folder` as they are on the card, by path (the marker left out).
  static std::vector<std::pair<fs::path, std::string>> filesOnCard(const fs::path& folder) {
    std::vector<std::pair<fs::path, std::string>> files;
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
      if (!entry.is_regular_file() || entry.path().filename() == cache_cipher::kMarkerName) continue;
      std::ifstream in(entry.path(), std::ios::binary);
      std::ostringstream bytes;
      bytes << in.rdbuf();
      files.emplace_back(entry.path(), bytes.str());
    }
    return files;
  }
  static bool anyHolds(const std::vector<std::pair<fs::path, std::string>>& files, const std::string& text) {
    for (const auto& [path, bytes] : files) {
      if (bytes.find(text) != std::string::npos) return true;
    }
    return false;
  }

  // The protected copy of `source` at book_, its key stored for it with `expires`.
  void protect(const std::string& source, const int64_t expires = 0, const bool storeKey = true) {
    book_ = (work_ / "loan.epub").string();
    ASSERT_GT(protected_epub::protectEpub(source, book_, kKey), 0);
    if (storeKey) ASSERT_TRUE(bookkey::write(book_, kKey.data(), kKey.size(), expires));
  }

  std::string dump(const std::string& epub, const std::string& cacheName) {
    std::ostringstream out;
    EXPECT_TRUE(pipeline_harness::runAndDump(epub, (work_ / cacheName).string(), pipeline_harness::Profile{}, out))
        << epub;
    return out.str();
  }

  book_protection::OpenResult openResult() {
    Epub epub(book_, (work_ / "cache").string());
    EXPECT_FALSE(epub.load(true));
    return epub.protectionResult();
  }

  fs::path work_;
  std::string book_;
};

}  // namespace

TEST_F(ProtectedBookPipelineTest, LaysOutAsThePlainBook) {
  for (const std::string source : {std::string(FIXTURE_EPUB), std::string(CORPUS_DIR "/test_inline_images.epub"),
                                   std::string(CORPUS_DIR "/test_jpeg_images.epub")}) {
    SCOPED_TRACE(source);
    protect(source);
    const std::string plain = dump(source, "plain");
    const std::string protectedDump = dump(book_, "protected");
    EXPECT_GT(plain.size(), 1000u);
    EXPECT_EQ(protectedDump, plain);
    EXPECT_TRUE(ZipFile::isProtected(book_));

    // The protected copy's cache is enciphered on the card (D2): every file of it, so no text of the book.
    const fs::path folder = Epub::cachePathFor(book_, (work_ / "protected").string());
    EXPECT_TRUE(Storage.cacheFolderEnciphered(folder.string().c_str()));
    const auto files = filesOnCard(folder);
    EXPECT_GE(files.size(), 3u);
    for (const auto& [path, bytes] : files) {
      if (bytes.empty()) continue;
      EXPECT_EQ(bytes.compare(0, 4, "WCC1"), 0) << path << " is on the card in the clear";
    }
    if (source == FIXTURE_EPUB) {
      EXPECT_TRUE(anyHolds(filesOnCard(Epub::cachePathFor(source, (work_ / "plain").string())), "Ishmael"))
          << "the plain book's cache should hold its text: the check below would prove nothing";
      EXPECT_FALSE(anyHolds(files, "Ishmael")) << "the protected book's text is on the card in the clear";
    }
    ZipFile::unprotect(book_);
    // Through Storage, as the firmware removes a cache: it forgets what it knew of the folder.
    Storage.removeDir((work_ / "plain").string().c_str());
    Storage.removeDir((work_ / "protected").string().c_str());
  }
}

// A protected book opened before its cache was enciphered: the folder holds its text in the clear. The next open
// empties it, keeps where the reader was, and builds the rest again enciphered.
TEST_F(ProtectedBookPipelineTest, AnOlderCacheIsEncipheredAndKeepsTheReadingState) {
  protect(FIXTURE_EPUB);
  const std::string cacheRoot = (work_ / "cache").string();
  const fs::path folder = Epub::cachePathFor(book_, cacheRoot);
  const std::string progress = "\x01\x02\x03 where the reader was";
  {
    // What the firmware before D2 left: the book's derived text, a progress file and the manifest, in the clear.
    fs::create_directories(folder / "spines" / "0");
    std::ofstream(folder / "spines" / "0" / "html_1.bin", std::ios::binary) << "Call me Ishmael. Some years ago";
    std::ofstream(folder / "progress.bin", std::ios::binary) << progress;
  }
  ASSERT_FALSE(Storage.cacheFolderEnciphered(folder.string().c_str()));

  Epub epub(book_, cacheRoot);
  ASSERT_TRUE(epub.load(true));
  EXPECT_EQ(epub.protectionResult(), book_protection::OpenResult::Protected);
  EXPECT_TRUE(Storage.cacheFolderEnciphered(folder.string().c_str()));
  const auto files = filesOnCard(folder);
  EXPECT_FALSE(anyHolds(files, "Ishmael")) << "the older cache's text is still on the card in the clear";
  for (const auto& [path, bytes] : files) {
    if (!bytes.empty()) EXPECT_EQ(bytes.compare(0, 4, "WCC1"), 0) << path << " is on the card in the clear";
  }
  std::string kept;
  ASSERT_TRUE(Storage.readFileToString("TEST", (folder / "progress.bin").string(), 4096, kept));
  EXPECT_EQ(kept, progress) << "the reading state did not survive";
}

// Another device's card, or a marker whose key cannot be had: nothing in the folder opens, so nothing of it is read
// as if it were plain, nor written in the clear.
TEST_F(ProtectedBookPipelineTest, AFolderWithoutItsKeyOpensNothing) {
  protect(FIXTURE_EPUB);
  const std::string cacheRoot = (work_ / "cache").string();
  {
    Epub epub(book_, cacheRoot);
    ASSERT_TRUE(epub.load(true));
  }
  ZipFile::unprotect(book_);
  const fs::path folder = Epub::cachePathFor(book_, cacheRoot);
  Storage.setCacheKeySource(nullptr);
  HalFile file;
  EXPECT_FALSE(Storage.openFileForRead("TEST", (folder / "book.bin").string(), file));
  EXPECT_FALSE(Storage.openFileForWrite("TEST", (folder / "progress.bin").string(), file));
  Storage.setCacheKeySource(book_protection::cacheFolderKey);
  EXPECT_TRUE(Storage.openFileForRead("TEST", (folder / "book.bin").string(), file));
}

TEST_F(ProtectedBookPipelineTest, WithoutItsKeyItDoesNotOpen) {
  protect(FIXTURE_EPUB, 0, /*storeKey=*/false);
  EXPECT_EQ(openResult(), book_protection::OpenResult::NoKey);
  EXPECT_FALSE(ZipFile::isProtected(book_));

  // A key file this device did not wrap, or one damaged.
  std::ofstream(book_ + ".key", std::ios::binary) << "FIBK\x02\x10 garbage that is no wrapped key at all....";
  EXPECT_EQ(openResult(), book_protection::OpenResult::OtherDevice);
}

TEST_F(ProtectedBookPipelineTest, LoanDates) {
  protect(FIXTURE_EPUB, kToday - 3600);
  EXPECT_EQ(openResult(), book_protection::OpenResult::Expired);

  protect(FIXTURE_EPUB, kToday + 3600);
  HalClock::testLoanNow() = 0;  // a clock never set
  EXPECT_EQ(openResult(), book_protection::OpenResult::NeedsTime);

  HalClock::testLoanNow() = kToday;
  Epub epub(book_, (work_ / "cache").string());
  EXPECT_TRUE(epub.load(true));
  EXPECT_EQ(epub.protectionResult(), book_protection::OpenResult::Protected);
}
