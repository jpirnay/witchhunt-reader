// A protected book through the whole pipeline, the real lib/BookProtection in place of the stub the
// other pipeline tests link: the index build (OPF, NCX or nav, book.bin), every section, the images,
// all read through ZipFile's decryption. A protected copy lays out exactly as the plain book does.
// And the books that must not open: no key, a damaged key, an ended loan, no clock.
#include <BookKey.h>
#include <BookSession.h>
#include <HalClock.h>
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
  }
  void TearDown() override {
    ZipFile::unprotect(book_);
    fs::remove_all(work_);
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
    ZipFile::unprotect(book_);
    fs::remove_all(work_ / "plain");
    fs::remove_all(work_ / "protected");
  }
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
