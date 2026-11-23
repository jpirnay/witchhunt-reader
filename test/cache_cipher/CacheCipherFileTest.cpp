// lib/CacheCipher/CacheCipherFile.h: the keystream block an open file keeps (the same bytes as CacheCipher, whatever
// the pieces), which paths lie in a book cache folder, and what the folder table forgets.
#include <CacheCipherFile.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

namespace {

constexpr uint8_t kKey[cache_cipher::kKeyBytes] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                                                   17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
constexpr uint8_t kNonce[CacheCipher::kNonceBytes] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2};

// A file read or written in pieces of every size, forwards, backwards and at random: each piece deciphers to what
// one CacheCipher call over it gives.
TEST(CipherStream, AnyPiecesGiveCacheCiphersBytes) {
  const CacheCipher reference(kKey, kNonce);
  cache_cipher::CipherStream stream(kKey, kNonce);
  std::mt19937 random(1234);
  for (int round = 0; round < 4000; ++round) {
    const uint64_t offset = random() % 5000;
    const size_t len = 1 + (round % 7 == 0 ? random() % 700 : random() % 70);
    std::vector<uint8_t> expected(len), actual(len);
    for (size_t i = 0; i < len; ++i) expected[i] = actual[i] = static_cast<uint8_t>(random());
    ASSERT_TRUE(reference.apply(expected.data(), len, offset));
    ASSERT_TRUE(stream.apply(actual.data(), len, offset));
    ASSERT_EQ(actual, expected) << "offset " << offset << " length " << len;
  }
}

TEST(CipherStream, ASequentialWriteInFieldSizedPiecesIsTheWholeOnce) {
  std::vector<uint8_t> whole(3000);
  for (size_t i = 0; i < whole.size(); ++i) whole[i] = static_cast<uint8_t>(i * 13);
  std::vector<uint8_t> pieces = whole;
  ASSERT_TRUE(CacheCipher(kKey, kNonce).apply(whole.data(), whole.size(), 0));
  cache_cipher::CipherStream stream(kKey, kNonce);
  const size_t sizes[] = {1, 2, 4, 2, 8, 3, 130, 1, 64, 5};
  for (size_t at = 0, i = 0; at < pieces.size(); ++i) {
    const size_t len = std::min(sizes[i % std::size(sizes)], pieces.size() - at);
    ASSERT_TRUE(stream.apply(pieces.data() + at, len, at));
    at += len;
  }
  EXPECT_EQ(pieces, whole);
}

TEST(CipherStream, AnOffsetPastTheCounterIsRefusedUntouched) {
  cache_cipher::CipherStream stream(kKey, kNonce);
  uint8_t bytes[4] = {1, 2, 3, 4};
  EXPECT_FALSE(stream.apply(bytes, sizeof(bytes), CacheCipher::kMaxOffset));
  EXPECT_EQ(bytes[0], 1);
  EXPECT_EQ(bytes[3], 4);
}

// The card's bytes of a file, for cache_cipher's templates.
struct MemoryFile {
  std::vector<uint8_t> bytes;
  uint64_t pos = 0;
  uint64_t size() const { return bytes.size(); }
  uint64_t position() const { return pos; }
  bool seek(uint64_t to) {
    pos = to;
    return true;
  }
  int read(void* buf, size_t count) {
    const size_t n = pos >= bytes.size() ? 0 : std::min(count, static_cast<size_t>(bytes.size() - pos));
    memcpy(buf, bytes.data() + pos, n);
    pos += n;
    return static_cast<int>(n);
  }
  size_t write(const void* buf, size_t count) {
    if (bytes.size() < pos + count) bytes.resize(pos + count);
    memcpy(bytes.data() + pos, buf, count);
    pos += count;
    return count;
  }
};

void fillZero(uint8_t* out, size_t len) { memset(out, 0, len); }

// A large write enciphered where it is puts the same bytes on the card as one through the chunk, and gives the
// caller's buffer back as it was.
TEST(CipherFileWrite, InPlaceWritesWhatTheChunkWritesAndRestoresTheBuffer) {
  std::vector<uint8_t> text(5000);
  for (size_t i = 0; i < text.size(); ++i) text[i] = static_cast<uint8_t>(i * 31 + 7);
  const std::vector<uint8_t> original = text;
  MemoryFile chunked, inPlace;
  std::unique_ptr<cache_cipher::CipherStream> a, b;
  ASSERT_EQ(cache_cipher::attach(chunked, true, kKey, fillZero, a), cache_cipher::Attach::Enciphered);
  ASSERT_EQ(cache_cipher::attach(inPlace, true, kKey, fillZero, b), cache_cipher::Attach::Enciphered);
  ASSERT_EQ(cache_cipher::write(chunked, *a, text.data(), 3, false), 3u);  // off the block grid first
  ASSERT_EQ(cache_cipher::write(inPlace, *b, text.data(), 3, true), 3u);
  ASSERT_EQ(cache_cipher::write(chunked, *a, text.data() + 3, text.size() - 3, false), text.size() - 3);
  ASSERT_EQ(cache_cipher::write(inPlace, *b, text.data() + 3, text.size() - 3, true), text.size() - 3);
  EXPECT_EQ(text, original) << "the in-place write did not give the buffer back";
  EXPECT_EQ(inPlace.bytes, chunked.bytes);
  ASSERT_EQ(inPlace.bytes.size(), cache_cipher::kHeaderBytes + text.size());

  // And it reads back.
  ASSERT_TRUE(inPlace.seek(0));
  std::unique_ptr<cache_cipher::CipherStream> reader;
  ASSERT_EQ(cache_cipher::attach(inPlace, false, kKey, fillZero, reader), cache_cipher::Attach::Enciphered);
  std::vector<uint8_t> back(text.size());
  ASSERT_EQ(cache_cipher::read(inPlace, *reader, back.data(), back.size()), static_cast<int>(back.size()));
  EXPECT_EQ(back, original);
}

TEST(FolderLength, OnlyAFileInsideABookCacheFolder) {
  const char* folder = "/.crosspoint/epub_123";
  EXPECT_EQ(cache_cipher::folderLength("/.crosspoint/epub_123/book.bin"), strlen(folder));
  EXPECT_EQ(cache_cipher::folderLength("/.crosspoint/epub_123/spines/0/html_1.bin"), strlen(folder));
  EXPECT_EQ(cache_cipher::folderLength("/tmp/run/cache/epub_9/x"), strlen("/tmp/run/cache/epub_9"));
  EXPECT_EQ(cache_cipher::folderLength("/.crosspoint/epub_123"), 0u);   // the folder itself
  EXPECT_EQ(cache_cipher::folderLength("/.crosspoint/epub_123/"), 0u);  // nothing inside
  EXPECT_EQ(cache_cipher::folderLength("/.crosspoint/epub_/x"), 0u);    // no digits
  EXPECT_EQ(cache_cipher::folderLength("/.crosspoint/epub_12a/x"), 0u);
  EXPECT_EQ(cache_cipher::folderLength("/.crosspoint/txt_123/index.bin"), 0u);
  EXPECT_EQ(cache_cipher::folderLength("/books/epub_123.epub"), 0u);
  EXPECT_EQ(cache_cipher::folderLength("/.crosspoint/settings.json"), 0u);
  EXPECT_TRUE(cache_cipher::isMarker("/.crosspoint/epub_123/cipher.bin", strlen(folder)));
  EXPECT_FALSE(cache_cipher::isMarker("/.crosspoint/epub_123/cipher.bin.part", strlen(folder)));
}

TEST(FolderTable, ForgetsWhatAPathTouches) {
  cache_cipher::FolderTable table;
  cache_cipher::FolderTable::Info info;
  info.state = cache_cipher::FolderState::Enciphered;
  const char* a = "/.crosspoint/epub_1";
  const char* b = "/.crosspoint/epub_12";
  table.put(a, strlen(a), info);
  table.put(b, strlen(b), info);
  table.forget("/.crosspoint/epub_1/progress.bin");  // inside a: a goes, not epub_12
  EXPECT_EQ(table.find(a, strlen(a)), nullptr);
  EXPECT_NE(table.find(b, strlen(b)), nullptr);
  table.forget("/.crosspoint");  // a parent: everything under it goes
  EXPECT_EQ(table.find(b, strlen(b)), nullptr);
}

TEST(FolderTable, KeepsTheLatestFour) {
  cache_cipher::FolderTable table;
  cache_cipher::FolderTable::Info info;
  const char* folders[] = {"/c/epub_1", "/c/epub_2", "/c/epub_3", "/c/epub_4", "/c/epub_5"};
  for (const char* folder : folders) table.put(folder, strlen(folder), info);
  EXPECT_EQ(table.find(folders[0], strlen(folders[0])), nullptr);
  for (size_t i = 1; i < std::size(folders); ++i) EXPECT_NE(table.find(folders[i], strlen(folders[i])), nullptr);
}

}  // namespace
