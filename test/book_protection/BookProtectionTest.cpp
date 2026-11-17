#include <BuildArena.h>
#include <esp_heap_caps.h>
#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ProtectionManifest.h"
#include "TestZip.h"
#include "ZipFile.h"

// A protected book's encryption.xml into protect.bin, and the book read back through ZipFile with
// real AES: an Adobe-style book (AES-128, PKCS#7, deflated, no sizes) and a Readium LCP-style one
// (AES-256, xmlenc padding, OriginalLength, a stored resource), written here from moby-dick.epub's
// DEFLATE streams.

#ifndef FIXTURE_EPUB
#error "FIXTURE_EPUB must be defined (path to test/fixtures/moby-dick.epub)"
#endif

using book_protection::ManifestStatus;
using test_zip::EntrySpec;

namespace {

constexpr const char* kBigChapter = "OEBPS/3308270851576161417_2701-h-0.htm.html";
constexpr const char* kSmallChapter = "OEBPS/3308270851576161417_2701-h-26.htm.html";
constexpr const char* kOtherChapter = "OEBPS/3308270851576161417_2701-h-27.htm.html";
constexpr const char* kStylesheet = "OEBPS/pgepub.css";

constexpr const char* kAes128 = "http://www.w3.org/2001/04/xmlenc#aes128-cbc";
constexpr const char* kAes256 = "http://www.w3.org/2001/04/xmlenc#aes256-cbc";
constexpr const char* kFontObfuscation = "http://www.idpf.org/2008/embedding";

// IV, then AES-CBC of `plain`: PKCS#7 padding, or xmlenc's (fill bytes random, the count last).
std::string encryptAes(const std::string& plain, const std::vector<uint8_t>& key, const bool xmlenc = false) {
  std::string padded = plain;
  const size_t pad = 16 - padded.size() % 16;
  for (size_t i = 0; i + 1 < pad; ++i) padded.push_back(static_cast<char>(xmlenc ? 0x5A ^ (i * 31) : pad));
  padded.push_back(static_cast<char>(pad));
  unsigned char iv[16];
  for (int i = 0; i < 16; ++i) iv[i] = static_cast<unsigned char>(0xC0 + i * 3);
  std::string out(reinterpret_cast<const char*>(iv), 16);
  out.resize(16 + padded.size());
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int produced = 0;
  EXPECT_EQ(EVP_EncryptInit_ex(c, key.size() == 16 ? EVP_aes_128_cbc() : EVP_aes_256_cbc(), nullptr, key.data(), iv),
            1);
  EVP_CIPHER_CTX_set_padding(c, 0);
  EXPECT_EQ(EVP_EncryptUpdate(c, reinterpret_cast<unsigned char*>(out.data()) + 16, &produced,
                              reinterpret_cast<const unsigned char*>(padded.data()), static_cast<int>(padded.size())),
            1);
  EVP_CIPHER_CTX_free(c);
  EXPECT_EQ(produced, static_cast<int>(padded.size()));
  return out;
}

struct ManifestItem {
  std::string uri;
  const char* algorithm;
  const char* method = nullptr;  // Compression Method, when given
  long originalLength = -1;
};

// encryption.xml as Adobe writes it (prefixed elements), or as LCP does (default namespace).
std::string encryptionXml(const std::vector<ManifestItem>& items, const bool lcpStyle = false) {
  std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>)";
  xml += R"(<encryption xmlns="urn:oasis:names:tc:opendocument:xmlns:container")";
  xml += lcpStyle ? ">" : R"( xmlns:enc="http://www.w3.org/2001/04/xmlenc#">)";
  const std::string p = lcpStyle ? "" : "enc:";
  for (const auto& item : items) {
    xml += "<" + p + "EncryptedData" + (lcpStyle ? R"( xmlns="http://www.w3.org/2001/04/xmlenc#">)" : ">");
    xml += "<" + p + "EncryptionMethod Algorithm=\"" + item.algorithm + "\"/>";
    xml +=
        R"(<ds:KeyInfo xmlns:ds="http://www.w3.org/2000/09/xmldsig#"><ds:RetrievalMethod URI="license.lcpl#/encryption/content_key"/></ds:KeyInfo>)";
    xml += "<" + p + "CipherData><" + p + "CipherReference URI=\"" + item.uri + "\"/></" + p + "CipherData>";
    if (item.method != nullptr) {
      xml += "<" + p + "EncryptionProperties><" + p + "EncryptionProperty>";
      xml += R"(<Compression xmlns="http://www.idpf.org/2016/encryption#compression" Method=")";
      xml += item.method;
      xml += "\" OriginalLength=\"" + std::to_string(item.originalLength) + "\"/>";
      xml += "</" + p + "EncryptionProperty></" + p + "EncryptionProperties>";
    }
    xml += "</" + p + "EncryptedData>";
  }
  return xml + "</encryption>";
}

std::string readAll(ZipFile& zip, const char* name) {
  test_zip::StringSink sink;
  EXPECT_TRUE(zip.readFileToStream(name, sink, 1024)) << name;
  return sink.data;
}

uint64_t fingerprintOf(const std::string& path) {
  ZipFile zip(path);
  uint64_t fp = 0;
  EXPECT_TRUE(zip.contentFingerprint(&fp));
  return fp;
}

class BookProtectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string fixture = FIXTURE_EPUB;
    big_ = test_zip::deflatedEntry(fixture, kBigChapter);
    small_ = test_zip::deflatedEntry(fixture, kSmallChapter);
    other_ = test_zip::deflatedEntry(fixture, kOtherChapter);
    css_ = test_zip::deflatedEntry(fixture, kStylesheet);
    dir_ = ::testing::TempDir() + "book_protection_" + std::to_string(getpid()) + "/";
    std::filesystem::create_directories(dir_);
    book_ = dir_ + "book.epub";
    manifest_ = dir_ + "protect.bin";
  }
  void TearDown() override {
    ZipFile::unprotect(book_);
    std::filesystem::remove_all(dir_);
  }

  // An Adobe-style book: chapters, a stylesheet and a "cover" (any bytes; .jpg is what counts)
  // encrypted, a font only obfuscated, and a listed entry the archive lacks.
  void writeAdobeBook() {
    writeBook({{"mimetype", "application/epub+zip"},
               {"META-INF/encryption.xml", encryptionXml({{"OEBPS/chapter%20one.xhtml", kAes128},
                                                          {"OEBPS/small.xhtml", kAes128},
                                                          {"OEBPS/style.css", kAes128},
                                                          {"OEBPS/cover.jpg", kAes128},
                                                          {"OEBPS/font.otf", kFontObfuscation},
                                                          {"OEBPS/missing.xhtml", kAes128}})},
               {"OEBPS/content.opf", "<package/>"},
               {"OEBPS/chapter one.xhtml", encryptAes(big_.stream, key_)},
               {"OEBPS/small.xhtml", encryptAes(small_.stream, key_)},
               {"OEBPS/style.css", encryptAes(css_.stream, key_)},
               {"OEBPS/cover.jpg", encryptAes(other_.stream, key_)},
               {"OEBPS/font.otf", "obfuscated font bytes"}});
  }

  void writeBook(const std::vector<EntrySpec>& entries) {
    test_zip::writeZip(book_, entries);
    fingerprint_ = fingerprintOf(book_);
  }

  ManifestStatus build(const std::vector<uint8_t>& key, BuildArena* scratch = nullptr) {
    return book_protection::buildManifest(book_, fingerprint_, key.data(), key.size(), manifest_, scratch);
  }

  // Opens the manifest and registers its protection.
  void attach(const std::vector<uint8_t>& key) {
    std::unique_ptr<ZipFile::Protection> protection;
    ASSERT_EQ(book_protection::openManifest(manifest_, fingerprint_, key.data(), key.size(), &protection),
              ManifestStatus::Protected);
    ASSERT_TRUE(ZipFile::protect(book_, std::move(protection)));
  }

  const std::vector<uint8_t> key_ = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                     0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
  test_zip::DeflatedEntry big_, small_, other_, css_;
  std::string dir_, book_, manifest_;
  uint64_t fingerprint_ = 0;
};

}  // namespace

TEST_F(BookProtectionTest, ProbeCountsTheAesEntries) {
  writeAdobeBook();
  const auto probe = book_protection::probeManifest(book_);
  EXPECT_EQ(probe.status, ManifestStatus::Protected);
  EXPECT_EQ(probe.entries, 5u);  // the obfuscated font is not one
  EXPECT_EQ(probe.cipherBits, 128);
}

TEST_F(BookProtectionTest, PlainBooks) {
  const std::string fixture = FIXTURE_EPUB;
  EXPECT_EQ(book_protection::probeManifest(fixture).status, ManifestStatus::Plain);

  // Font obfuscation alone: a plain book, whose manifest says so for the next open.
  writeBook({{"mimetype", "application/epub+zip"},
             {"META-INF/encryption.xml", encryptionXml({{"OEBPS/font.otf", kFontObfuscation}})},
             {"OEBPS/font.otf", "obfuscated"}});
  EXPECT_EQ(book_protection::probeManifest(book_).status, ManifestStatus::Plain);
  EXPECT_EQ(build(key_), ManifestStatus::Plain);
  std::unique_ptr<ZipFile::Protection> protection;
  EXPECT_EQ(book_protection::openManifest(manifest_, fingerprint_, key_.data(), key_.size(), &protection),
            ManifestStatus::Plain);
  EXPECT_FALSE(protection);
  EXPECT_FALSE(ZipFile::isProtected(book_));
}

TEST_F(BookProtectionTest, AdobeBookReadsBack) {
  writeAdobeBook();
  ASSERT_EQ(build(key_), ManifestStatus::Protected);
  EXPECT_FALSE(ZipFile::isProtected(book_));  // the build leaves nothing registered
  attach(key_);

  ZipFile zip(book_);
  EXPECT_EQ(readAll(zip, "OEBPS/chapter one.xhtml"), big_.plain);
  EXPECT_EQ(readAll(zip, "OEBPS/small.xhtml"), small_.plain);
  EXPECT_EQ(readAll(zip, "OEBPS/style.css"), css_.plain);
  EXPECT_EQ(readAll(zip, "OEBPS/cover.jpg"), other_.plain);
  EXPECT_EQ(readAll(zip, "OEBPS/font.otf"), "obfuscated font bytes");
  EXPECT_EQ(readAll(zip, "OEBPS/content.opf"), "<package/>");

  // Measured at the build, but for the image, whose size stays an estimate.
  ZipFile::FileStatSlim stat{};
  ASSERT_TRUE(zip.loadFileStatSlim("OEBPS/chapter one.xhtml", &stat));
  EXPECT_TRUE(ZipFile::isEncrypted(stat));
  EXPECT_TRUE(ZipFile::hasExactSize(stat));
  EXPECT_EQ(stat.uncompressedSize, big_.plain.size());
  ASSERT_TRUE(zip.loadFileStatSlim("OEBPS/style.css", &stat));
  EXPECT_EQ(stat.uncompressedSize, css_.plain.size());
  ASSERT_TRUE(zip.loadFileStatSlim("OEBPS/cover.jpg", &stat));
  EXPECT_TRUE(ZipFile::isEncrypted(stat));
  EXPECT_FALSE(ZipFile::hasExactSize(stat));
  ASSERT_TRUE(zip.loadFileStatSlim("OEBPS/font.otf", &stat));
  EXPECT_FALSE(ZipFile::isEncrypted(stat));

  size_t size = 0;
  uint8_t* small = zip.readFileToMemory("OEBPS/small.xhtml", &size, true);
  ASSERT_NE(small, nullptr);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(small), size), small_.plain);
  free(small);
}

TEST_F(BookProtectionTest, ManifestPinsTheContent) {
  writeAdobeBook();
  ASSERT_EQ(build(key_), ManifestStatus::Protected);
  std::unique_ptr<ZipFile::Protection> protection;
  EXPECT_EQ(book_protection::openManifest(manifest_, fingerprint_ + 1, key_.data(), key_.size(), &protection),
            ManifestStatus::Missing);
  EXPECT_EQ(book_protection::openManifest(dir_ + "none.bin", fingerprint_, key_.data(), key_.size(), &protection),
            ManifestStatus::Missing);
  const std::vector<uint8_t> longKey(32, 7);
  EXPECT_EQ(book_protection::openManifest(manifest_, fingerprint_, longKey.data(), longKey.size(), &protection),
            ManifestStatus::Failed);
  EXPECT_FALSE(protection);
}

// A wrong key does not inflate: the build fails, writes nothing and leaves nothing registered.
TEST_F(BookProtectionTest, WrongKeyFails) {
  writeAdobeBook();
  std::vector<uint8_t> wrong = key_;
  wrong[0] ^= 1;
  EXPECT_EQ(build(wrong), ManifestStatus::Failed);
  EXPECT_FALSE(std::filesystem::exists(manifest_));
  EXPECT_FALSE(std::filesystem::exists(manifest_ + ".tmp"));
  EXPECT_FALSE(ZipFile::isProtected(book_));
  const std::vector<uint8_t> longKey(32, 7);
  EXPECT_EQ(build(longKey), ManifestStatus::Failed);
}

TEST_F(BookProtectionTest, LcpBookReadsBack) {
  std::vector<uint8_t> key(32);
  for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(0x60 + i);
  const std::string notes(1000, 'n');
  writeBook({{"mimetype", "application/epub+zip"},
             {"META-INF/encryption.xml",
              encryptionXml({{"OEBPS/chapter.xhtml", kAes256, "8", static_cast<long>(big_.plain.size())},
                             {"OEBPS/notes.txt", kAes256, "0", static_cast<long>(notes.size())}},
                            true)},
             {"OEBPS/chapter.xhtml", encryptAes(big_.stream, key, true)},
             {"OEBPS/notes.txt", encryptAes(notes, key, true)}});
  EXPECT_EQ(book_protection::probeManifest(book_).cipherBits, 256);
  ASSERT_EQ(build(key), ManifestStatus::Protected);
  attach(key);
  ZipFile zip(book_);
  EXPECT_EQ(readAll(zip, "OEBPS/chapter.xhtml"), big_.plain);
  EXPECT_EQ(readAll(zip, "OEBPS/notes.txt"), notes);
  ZipFile::FileStatSlim stat{};
  ASSERT_TRUE(zip.loadFileStatSlim("OEBPS/notes.txt", &stat));
  EXPECT_EQ(stat.method, ZipFile::METHOD_ENCRYPTED | 0);
  EXPECT_EQ(stat.uncompressedSize, notes.size());
}

// More entries than the memory at hand holds: the manifest is built in runs, each sorted, and a
// lookup searches them all.
TEST_F(BookProtectionTest, RunsWhenTheTableDoesNotFit) {
  std::vector<ManifestItem> items;
  std::vector<EntrySpec> entries = {{"mimetype", "application/epub+zip"}, {"META-INF/encryption.xml", ""}};
  for (int i = 0; i < 40; ++i) {
    const std::string name = "OEBPS/part" + std::to_string(i) + ".xhtml";
    items.push_back({name, kAes128});
    entries.push_back({name, encryptAes(small_.stream, key_)});
  }
  entries[1].data = encryptionXml(items);
  writeBook(entries);
  {
    const HostLargestFreeBlock tight(20 * 24 + 64);  // 20 entries a run
    ASSERT_EQ(build(key_), ManifestStatus::Protected);
  }
  // The header's run count (protect.bin: magic, version, cipher, fingerprint, records, runs).
  FILE* f = fopen(manifest_.c_str(), "rb");
  ASSERT_NE(f, nullptr);
  uint8_t header[24];
  ASSERT_EQ(fread(header, 1, sizeof(header), f), sizeof(header));
  fclose(f);
  uint32_t records = 0, runs = 0;
  memcpy(&records, header + 16, 4);
  memcpy(&runs, header + 20, 4);
  EXPECT_EQ(records, 40u);
  EXPECT_EQ(runs, 2u);
  attach(key_);
  ZipFile zip(book_);
  for (int i = 0; i < 40; i += 7) {
    const std::string name = "OEBPS/part" + std::to_string(i) + ".xhtml";
    EXPECT_EQ(readAll(zip, name.c_str()), small_.plain) << name;
  }
  ZipFile::FileStatSlim stat{};
  ASSERT_TRUE(zip.loadFileStatSlim("OEBPS/part39.xhtml", &stat));
  EXPECT_EQ(stat.uncompressedSize, small_.plain.size());
}

// In a lent region the table and the measuring reads leave it as they found it.
TEST_F(BookProtectionTest, BuildInALentRegion) {
  writeAdobeBook();
  std::vector<uint8_t> region(48 * 1024);
  BuildArena arena(region.data(), region.size());
  const HostLargestFreeBlock noHeap(0);  // the table must come from the region
  ASSERT_EQ(build(key_, &arena), ManifestStatus::Protected);
  EXPECT_EQ(arena.used(), 0u);
}
