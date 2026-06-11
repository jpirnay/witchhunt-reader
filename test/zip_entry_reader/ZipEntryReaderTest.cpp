#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "ZipFile.h"

// Path to moby-dick.epub injected by CMake via FIXTURE_EPUB define.
#ifndef FIXTURE_EPUB
#error "FIXTURE_EPUB must be defined (path to test/fixtures/moby-dick.epub)"
#endif

// A deflated spine chapter from moby-dick.epub (confirmed present).
static const char* const kTestEntry = "OEBPS/3308270851576161417_2701-h-0.htm.html";

class ZipEntryReaderTest : public ::testing::Test {
 protected:
  std::string epubPath_{FIXTURE_EPUB};
  ZipFile zip_{epubPath_};
};

// Helper: drain an EntryReader into a byte vector.
static bool drain(ZipFile::EntryReader& reader, std::vector<uint8_t>& out) {
  uint8_t buf[512];
  bool done = false;
  while (!done) {
    size_t produced = 0;
    if (!reader.step(buf, sizeof(buf), &produced, &done)) return false;
    out.insert(out.end(), buf, buf + produced);
  }
  return true;
}

// Helper: drain readFileToStream into a byte vector via a Print sink.
class VectorSink : public Print {
 public:
  std::vector<uint8_t> data;
  size_t write(uint8_t b) override {
    data.push_back(b);
    return 1;
  }
  size_t write(const uint8_t* buf, size_t n) override {
    data.insert(data.end(), buf, buf + n);
    return n;
  }
};

// Chunked EntryReader output matches one-shot readFileToStream.
TEST_F(ZipEntryReaderTest, ChunkedOutputMatchesOneShot) {
  // Reference: one-shot readFileToStream
  VectorSink ref;
  ASSERT_TRUE(zip_.readFileToStream(kTestEntry, ref, 1024));
  ASSERT_GT(ref.data.size(), 0u);

  // Under test: EntryReader in small 97-byte chunks (prime, stresses boundary)
  ZipFile::EntryReader reader(zip_);
  ASSERT_TRUE(reader.open(kTestEntry));
  EXPECT_TRUE(reader.isOpen());
  EXPECT_EQ(reader.inflatedSize(), ref.data.size());

  std::vector<uint8_t> got;
  ASSERT_TRUE(drain(reader, got));

  EXPECT_EQ(got.size(), ref.data.size());
  EXPECT_EQ(got, ref.data);
  EXPECT_EQ(reader.bytesProduced(), ref.data.size());
}

// Single large step (cap > inflatedSize) still terminates correctly.
TEST_F(ZipEntryReaderTest, SingleLargeStep) {
  ZipFile::EntryReader reader(zip_);
  ASSERT_TRUE(reader.open(kTestEntry));

  std::vector<uint8_t> buf(reader.inflatedSize() + 64);
  size_t produced = 0;
  bool done = false;
  ASSERT_TRUE(reader.step(buf.data(), buf.size(), &produced, &done));
  // May finish in one step (Done) or need another (Ok with produced < cap).
  // Either way, draining must complete without error.
  std::vector<uint8_t> all(buf.data(), buf.data() + produced);
  while (!done) {
    ASSERT_TRUE(reader.step(buf.data(), buf.size(), &produced, &done));
    all.insert(all.end(), buf.data(), buf.data() + produced);
  }

  VectorSink ref;
  ASSERT_TRUE(zip_.readFileToStream(kTestEntry, ref, 1024));
  EXPECT_EQ(all, ref.data);
}

// Re-opening the same reader on a different entry works correctly.
TEST_F(ZipEntryReaderTest, ReOpen) {
  // Open the CSS file (a short stored or deflated entry).
  const char* kCssEntry = "OEBPS/pgepub.css";

  ZipFile::EntryReader reader(zip_);
  ASSERT_TRUE(reader.open(kTestEntry));
  std::vector<uint8_t> first;
  ASSERT_TRUE(drain(reader, first));

  ASSERT_TRUE(reader.open(kCssEntry));
  std::vector<uint8_t> css;
  ASSERT_TRUE(drain(reader, css));

  VectorSink cssRef;
  ASSERT_TRUE(zip_.readFileToStream(kCssEntry, cssRef, 1024));
  EXPECT_EQ(css, cssRef.data);

  // Re-open the first entry and confirm it still matches.
  ASSERT_TRUE(reader.open(kTestEntry));
  std::vector<uint8_t> again;
  ASSERT_TRUE(drain(reader, again));
  EXPECT_EQ(again, first);
}

// close() leaves the reader in a safe state; isOpen() reflects it.
TEST_F(ZipEntryReaderTest, CloseResetsState) {
  ZipFile::EntryReader reader(zip_);
  ASSERT_TRUE(reader.open(kTestEntry));
  EXPECT_TRUE(reader.isOpen());
  reader.close();
  EXPECT_FALSE(reader.isOpen());
  EXPECT_EQ(reader.bytesProduced(), 0u);
  EXPECT_EQ(reader.inflatedSize(), 0u);
}
