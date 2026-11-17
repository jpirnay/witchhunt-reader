#include <InflateReader.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "BuildArena.h"
#include "TestZip.h"
#include "ZipFile.h"

// Entries of an OCF-encrypted archive through every ZipFile read path, once a Protection is
// registered for it (ZipFile::protect). The archive is written here: real DEFLATE streams taken
// from moby-dick.epub, enciphered with a toy CBC (block XOR key, chained) so the chaining, the
// IV, the chunking and the padding are what is tested; the AES itself is lib/BookProtection's.

#ifndef FIXTURE_EPUB
#error "FIXTURE_EPUB must be defined (path to test/fixtures/moby-dick.epub)"
#endif

namespace {

using test_zip::DeflatedEntry;
using test_zip::EntrySpec;
using test_zip::writeZip;

constexpr const char* kBigChapter = "OEBPS/3308270851576161417_2701-h-0.htm.html";     // 54 KB plain
constexpr const char* kSmallChapter = "OEBPS/3308270851576161417_2701-h-26.htm.html";  // 2.4 KB plain

// ---- the toy cipher: E(x) = x ^ key, CBC-chained ----

struct ToyState {
  uint8_t key[32];
  size_t keyLen;
  uint8_t prev[16];
};

bool toyBegin(void* state, const uint8_t* key, const size_t keyLen, const uint8_t iv[16]) {
  auto* s = static_cast<ToyState*>(state);
  memcpy(s->key, key, keyLen);
  s->keyLen = keyLen;
  memcpy(s->prev, iv, 16);
  return true;
}

bool toyDecrypt(void* state, uint8_t* data, const size_t len) {
  auto* s = static_cast<ToyState*>(state);
  if (len % 16 != 0) return false;
  for (size_t at = 0; at < len; at += 16) {
    uint8_t cipher[16];
    memcpy(cipher, data + at, 16);
    for (size_t i = 0; i < 16; ++i) data[at + i] = cipher[i] ^ s->key[i % s->keyLen] ^ s->prev[i];
    memcpy(s->prev, cipher, 16);
  }
  return true;
}

int g_toyEnds = 0;
void toyEnd(void* state) {
  memset(state, 0, sizeof(ToyState));
  ++g_toyEnds;
}

const ZipFile::CipherOps kToyCipher = {sizeof(ToyState), toyBegin, toyDecrypt, toyEnd};

// IV, then the CBC encryption of `plain` padded to whole blocks: PKCS#7, or with random fill and
// only the count byte meaningful (xmlenc) when `xmlenc`.
std::string encrypt(const std::string& plain, const std::vector<uint8_t>& key, const bool xmlenc = false) {
  std::string padded = plain;
  const size_t pad = 16 - padded.size() % 16;
  for (size_t i = 0; i + 1 < pad; ++i) padded.push_back(static_cast<char>(xmlenc ? 0xA0 + i : pad));
  padded.push_back(static_cast<char>(pad));
  uint8_t prev[16];
  for (int i = 0; i < 16; ++i) prev[i] = static_cast<uint8_t>(0x30 + i * 7);
  std::string out(reinterpret_cast<const char*>(prev), 16);
  for (size_t at = 0; at < padded.size(); at += 16) {
    uint8_t block[16];
    for (size_t i = 0; i < 16; ++i) {
      block[i] = static_cast<uint8_t>(padded[at + i]) ^ prev[i] ^ key[i % key.size()];
    }
    out.append(reinterpret_cast<const char*>(block), 16);
    memcpy(prev, block, 16);
  }
  return out;
}

// ---- a test protection: entries by local-header offset ----

class TestProtection : public ZipFile::Protection {
 public:
  TestProtection(std::map<uint32_t, ZipFile::EncryptedEntry> entries, std::vector<uint8_t> key, int* finds)
      : entries_(std::move(entries)), key_(std::move(key)), finds_(finds) {}
  bool find(const uint32_t localHeaderOffset, ZipFile::EncryptedEntry* out) override {
    if (finds_) ++*finds_;
    const auto it = entries_.find(localHeaderOffset);
    if (it == entries_.end()) return false;
    *out = it->second;
    return true;
  }
  size_t key(uint8_t out[32]) const override {
    memcpy(out, key_.data(), key_.size());
    return key_.size();
  }
  const ZipFile::CipherOps& cipher() const override { return kToyCipher; }

 private:
  std::map<uint32_t, ZipFile::EncryptedEntry> entries_;
  std::vector<uint8_t> key_;
  int* finds_;
};

std::string drainReader(ZipFile::EntryReader& reader, const size_t chunk) {
  std::string out;
  std::vector<uint8_t> buf(chunk);
  bool done = false;
  while (!done) {
    size_t produced = 0;
    if (!reader.step(buf.data(), chunk, &produced, &done)) {
      ADD_FAILURE() << "step failed after " << out.size() << " bytes";
      break;
    }
    out.append(reinterpret_cast<const char*>(buf.data()), produced);
  }
  return out;
}

class ZipEncryptedEntryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    big_ = test_zip::deflatedEntry(FIXTURE_EPUB, kBigChapter);
    small_ = test_zip::deflatedEntry(FIXTURE_EPUB, kSmallChapter);
    ASSERT_GT(big_.plain.size(), 32768u);
    notes_.clear();
    for (int i = 0; i < 1000; ++i) notes_.push_back(static_cast<char>('a' + i % 26));
    path_ = ::testing::TempDir() + "zip_encrypted_" + std::to_string(getpid()) + ".zip";
    // Each entry name says how it is stored. "padblock": 16 bytes of plaintext, so its last block
    // is padding only.
    const std::vector<EntrySpec> entries = {
        {"mimetype", "application/epub+zip"},
        {"big.xhtml", encrypt(big_.stream, key_)},
        {"small.xhtml", encrypt(small_.stream, key_)},
        {"notes.bin", encrypt(notes_, key_)},
        {"padblock.bin", encrypt(std::string(16, 'p'), key_)},
        {"plain.css", "body { margin: 0 }"},
        {"zipped.xhtml", encrypt(small_.stream, key_), 8, 5000},
    };
    offsets_ = writeZip(path_, entries);
  }
  void TearDown() override {
    ZipFile::unprotect(path_);
    std::remove(path_.c_str());
  }

  // Registers the archive's protection; `bigSizeKnown`: whether it knows big.xhtml's plaintext size.
  void protect(const bool bigSizeKnown = true) {
    std::map<uint32_t, ZipFile::EncryptedEntry> entries;
    entries[offsets_[1]] = {static_cast<uint32_t>(big_.plain.size()), bigSizeKnown, true};
    entries[offsets_[2]] = {static_cast<uint32_t>(small_.plain.size()), true, true};
    entries[offsets_[3]] = {static_cast<uint32_t>(notes_.size()), true, false};
    entries[offsets_[4]] = {16, true, false};
    entries[offsets_[6]] = {0, false, true};
    ASSERT_TRUE(ZipFile::protect(path_, std::make_unique<TestProtection>(entries, key_, &finds_)));
  }

  std::string streamed(ZipFile& zip, const char* name) {
    test_zip::StringSink sink;
    EXPECT_TRUE(zip.readFileToStream(name, sink, 1024)) << name;
    return sink.data;
  }

  std::string toMemory(ZipFile& zip, const char* name) {
    size_t size = 0;
    uint8_t* data = zip.readFileToMemory(name, &size, true);
    EXPECT_NE(data, nullptr) << name;
    if (!data) return {};
    EXPECT_EQ(data[size], '\0');
    std::string out(reinterpret_cast<const char*>(data), size);
    free(data);
    return out;
  }

  const std::vector<uint8_t> key_ = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                     0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xf0, 0x0f};
  DeflatedEntry big_;
  DeflatedEntry small_;
  std::string notes_;
  std::string path_;
  std::vector<uint32_t> offsets_;
  int finds_ = 0;
};

}  // namespace

// Without a protection the archive reads as it always did: the encrypted entry's raw bytes.
TEST_F(ZipEncryptedEntryTest, UnregisteredArchiveReadsRaw) {
  ZipFile zip(path_);
  ZipFile::FileStatSlim stat{};
  ASSERT_TRUE(zip.loadFileStatSlim("notes.bin", &stat));
  EXPECT_FALSE(ZipFile::isEncrypted(stat));
  EXPECT_EQ(stat.method, 0);
  EXPECT_FALSE(ZipFile::isProtected(path_));
  uint32_t offset = 0, size = 0;
  EXPECT_TRUE(zip.getStoredEntryRange("notes.bin", &offset, &size));
}

TEST_F(ZipEncryptedEntryTest, StatsSayEncrypted) {
  protect(false);
  ZipFile zip(path_);
  ZipFile::FileStatSlim stat{};
  ASSERT_TRUE(zip.loadFileStatSlim("small.xhtml", &stat));
  EXPECT_TRUE(ZipFile::isEncrypted(stat));
  EXPECT_TRUE(ZipFile::hasExactSize(stat));
  EXPECT_EQ(stat.method & 0xFF, 8);
  EXPECT_EQ(stat.uncompressedSize, small_.plain.size());

  ASSERT_TRUE(zip.loadFileStatSlim("notes.bin", &stat));
  EXPECT_EQ(stat.method, ZipFile::METHOD_ENCRYPTED | 0);
  EXPECT_EQ(stat.uncompressedSize, notes_.size());

  // An estimated size: the ciphertext's, and never below a full window.
  ASSERT_TRUE(zip.loadFileStatSlim("big.xhtml", &stat));
  EXPECT_FALSE(ZipFile::hasExactSize(stat));
  EXPECT_GE(stat.uncompressedSize, 32768u);
  size_t size = 0;
  ASSERT_TRUE(zip.getInflatedFileSize("big.xhtml", &size));
  EXPECT_EQ(size, stat.uncompressedSize);

  ASSERT_TRUE(zip.loadFileStatSlim("plain.css", &stat));
  EXPECT_FALSE(ZipFile::isEncrypted(stat));
}

TEST_F(ZipEncryptedEntryTest, WholeEntryReads) {
  for (const bool bigSizeKnown : {true, false}) {
    SCOPED_TRACE(bigSizeKnown ? "big size known" : "big size estimated");
    protect(bigSizeKnown);
    ZipFile zip(path_);
    EXPECT_EQ(streamed(zip, "big.xhtml"), big_.plain);
    EXPECT_EQ(toMemory(zip, "big.xhtml"), big_.plain);
    EXPECT_EQ(streamed(zip, "small.xhtml"), small_.plain);
    EXPECT_EQ(toMemory(zip, "small.xhtml"), small_.plain);
    EXPECT_EQ(streamed(zip, "notes.bin"), notes_);
    EXPECT_EQ(toMemory(zip, "notes.bin"), notes_);
    EXPECT_EQ(streamed(zip, "padblock.bin"), std::string(16, 'p'));
    EXPECT_EQ(toMemory(zip, "plain.css"), "body { margin: 0 }");
  }
}

TEST_F(ZipEncryptedEntryTest, EntryReaderSteps) {
  protect(false);
  ZipFile zip(path_);
  ZipFile::EntryReader reader(zip, 512);
  for (const size_t chunk : {size_t{97}, size_t{4096}}) {
    ASSERT_TRUE(reader.open("big.xhtml"));
    EXPECT_EQ(drainReader(reader, chunk), big_.plain);
    ASSERT_TRUE(reader.open("notes.bin"));
    EXPECT_EQ(reader.inflatedSize(), notes_.size());
    EXPECT_EQ(drainReader(reader, chunk), notes_);
    ASSERT_TRUE(reader.open("padblock.bin"));
    EXPECT_EQ(drainReader(reader, chunk), std::string(16, 'p'));
  }
  reader.close();
}

TEST_F(ZipEncryptedEntryTest, EntryReaderInArenaAndCapped) {
  protect(true);
  ZipFile zip(path_);
  std::vector<uint8_t> region(64 * 1024);
  BuildArena arena(region.data(), region.size());
  ZipFile::FileStatSlim stat{};
  ASSERT_TRUE(zip.loadFileStatSlim("big.xhtml", &stat));
  {
    ZipFile::EntryReader reader(zip, 1024, &arena);
    ASSERT_TRUE(reader.open(stat));
    EXPECT_EQ(drainReader(reader, 333), big_.plain);
  }
  EXPECT_EQ(arena.used(), 0u);
  {
    ZipFile::EntryReader reader(zip, 1024, &arena);
    ASSERT_TRUE(reader.open(stat, 5000));
    EXPECT_EQ(drainReader(reader, 1000), big_.plain.substr(0, 5000));
  }
  ASSERT_TRUE(zip.loadFileStatSlim("notes.bin", &stat));
  {
    ZipFile::EntryReader reader(zip, 64, &arena);
    ASSERT_TRUE(reader.open(stat, 100));
    EXPECT_EQ(drainReader(reader, 30), notes_.substr(0, 100));
  }
  EXPECT_EQ(arena.used(), 0u);
}

TEST_F(ZipEncryptedEntryTest, HeaderProbes) {
  protect(false);
  ZipFile zip(path_);
  std::vector<uint8_t> buf(5000);
  ASSERT_EQ(zip.readBytesFromEntry("big.xhtml", buf.data(), 4000), 4000u);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), 4000), big_.plain.substr(0, 4000));
  // STORED plaintext: whole blocks in place, then a partial one.
  ASSERT_EQ(zip.readBytesFromEntry("notes.bin", buf.data(), 37), 37u);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), 37), notes_.substr(0, 37));
  ASSERT_EQ(zip.readBytesFromEntry("notes.bin", buf.data(), 5000), notes_.size());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), notes_.size()), notes_);

  std::vector<uint8_t> region(48 * 1024);
  BuildArena arena(region.data(), region.size());
  ZipFile::FileStatSlim stat{};
  ASSERT_TRUE(zip.loadFileStatSlim("small.xhtml", &stat));
  ASSERT_EQ(zip.readBytesFromStat(stat, buf.data(), 1500, &arena), 1500u);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(buf.data()), 1500), small_.plain.substr(0, 1500));
  EXPECT_EQ(arena.used(), 0u);
}

// The raw bytes of an encrypted entry are never handed out; a plain entry's still are.
TEST_F(ZipEncryptedEntryTest, InPlaceRangeRefused) {
  protect();
  ZipFile zip(path_);
  uint32_t offset = 0, size = 0;
  EXPECT_FALSE(zip.getStoredEntryRange("notes.bin", &offset, &size));
  EXPECT_TRUE(zip.getStoredEntryRange("plain.css", &offset, &size));
  EXPECT_EQ(size, 18u);
}

// An encrypted entry the archive deflated is not an IV and cipher blocks: every read refuses it.
TEST_F(ZipEncryptedEntryTest, UnreadableLayoutRefused) {
  protect();
  ZipFile zip(path_);
  ZipFile::FileStatSlim stat{};
  ASSERT_TRUE(zip.loadFileStatSlim("zipped.xhtml", &stat));
  EXPECT_TRUE(ZipFile::isEncrypted(stat));
  test_zip::StringSink sink;
  EXPECT_FALSE(zip.readFileToStream("zipped.xhtml", sink, 1024));
  EXPECT_EQ(zip.readFileToMemory("zipped.xhtml"), nullptr);
  ZipFile::EntryReader reader(zip);
  EXPECT_FALSE(reader.open(stat));
  uint8_t buf[64];
  EXPECT_EQ(zip.readBytesFromStat(stat, buf, sizeof(buf)), 0u);
}

// A stat that says encrypted (from a cache, say) cannot be read once no key is registered.
TEST_F(ZipEncryptedEntryTest, EncryptedStatWithoutKeyFails) {
  protect();
  ZipFile zip(path_);
  ZipFile::FileStatSlim stat{};
  ASSERT_TRUE(zip.loadFileStatSlim("small.xhtml", &stat));
  ZipFile::unprotect(path_);
  ZipFile::EntryReader reader(zip);
  EXPECT_FALSE(reader.open(stat));
  uint8_t buf[64];
  EXPECT_EQ(zip.readBytesFromStat(stat, buf, sizeof(buf)), 0u);
}

TEST_F(ZipEncryptedEntryTest, BatchStatsSayEncrypted) {
  protect();
  ZipFile zip(path_);
  ZipFile::StatTarget targets[3] = {ZipFile::StatTarget::named("small.xhtml"), ZipFile::StatTarget::named("plain.css"),
                                    ZipFile::StatTarget::named("missing.xhtml")};
  std::sort(targets, targets + 3, ZipFile::StatTarget::before);
  std::vector<uint8_t> window(4096);
  EXPECT_EQ(zip.fillFileStats(targets, 3, window.data(), window.size()), 2);
  const auto* small = ZipFile::StatTarget::find(targets, targets + 3, "small.xhtml");
  const auto* css = ZipFile::StatTarget::find(targets, targets + 3, "plain.css");
  const auto* missing = ZipFile::StatTarget::find(targets, targets + 3, "missing.xhtml");
  ASSERT_TRUE(small && css && missing);
  EXPECT_TRUE(ZipFile::isEncrypted(small->stat()));
  EXPECT_EQ(small->uncompressedSize, small_.plain.size());
  EXPECT_FALSE(ZipFile::isEncrypted(css->stat()));
  EXPECT_FALSE(missing->found());
  ZipFile::EntryReader reader(zip);
  ASSERT_TRUE(reader.open(small->stat()));
  EXPECT_EQ(drainReader(reader, 512), small_.plain);
}

// A 32-byte key (AES-256, Readium LCP) pads as xmlenc does: random fill, only the count meaningful.
TEST_F(ZipEncryptedEntryTest, XmlencPaddingWithLongKey) {
  std::vector<uint8_t> longKey(32);
  for (size_t i = 0; i < longKey.size(); ++i) longKey[i] = static_cast<uint8_t>(i * 13 + 1);
  const std::vector<uint32_t> offsets = writeZip(path_, {{"mimetype", "application/epub+zip"},
                                                         {"notes.bin", encrypt(notes_, longKey, true)},
                                                         {"small.xhtml", encrypt(small_.stream, longKey, true)}});
  std::map<uint32_t, ZipFile::EncryptedEntry> entries;
  entries[offsets[1]] = {0, false, false};
  entries[offsets[2]] = {0, false, true};
  ASSERT_TRUE(ZipFile::protect(path_, std::make_unique<TestProtection>(entries, longKey, nullptr)));
  ZipFile zip(path_);
  EXPECT_EQ(streamed(zip, "notes.bin"), notes_);
  EXPECT_EQ(toMemory(zip, "notes.bin"), notes_);
  EXPECT_EQ(streamed(zip, "small.xhtml"), small_.plain);
}

// While a protection is registered, every other archive reads untouched and asks it nothing.
TEST_F(ZipEncryptedEntryTest, OtherArchivesUntouched) {
  protect();
  const int before = finds_;
  const std::string fixture = FIXTURE_EPUB;  // ZipFile keeps a reference to its path
  ZipFile other(fixture);
  test_zip::StringSink sink;
  ASSERT_TRUE(other.readFileToStream(kSmallChapter, sink, 1024));
  EXPECT_EQ(sink.data, small_.plain);
  EXPECT_EQ(finds_, before);
}

TEST_F(ZipEncryptedEntryTest, RegistryKeepsTheNewest) {
  g_toyEnds = 0;
  const std::string a = path_ + ".a", b = path_ + ".b";
  ASSERT_TRUE(ZipFile::protect(
      a, std::make_unique<TestProtection>(std::map<uint32_t, ZipFile::EncryptedEntry>{}, key_, nullptr)));
  ASSERT_TRUE(ZipFile::protect(
      b, std::make_unique<TestProtection>(std::map<uint32_t, ZipFile::EncryptedEntry>{}, key_, nullptr)));
  EXPECT_TRUE(ZipFile::isProtected(a));
  protect();  // a third: `a`, registered first, goes
  EXPECT_FALSE(ZipFile::isProtected(a));
  EXPECT_TRUE(ZipFile::isProtected(b));
  EXPECT_TRUE(ZipFile::isProtected(path_));
  ZipFile::unprotect(b);
  EXPECT_FALSE(ZipFile::isProtected(b));
  EXPECT_TRUE(ZipFile::isProtected(path_));
  EXPECT_FALSE(ZipFile::protect(a, nullptr));

  // Still reads after the slots moved.
  ZipFile zip(path_);
  EXPECT_EQ(streamed(zip, "notes.bin"), notes_);
  EXPECT_GT(g_toyEnds, 0);  // the cipher state is wiped when a read ends
}
