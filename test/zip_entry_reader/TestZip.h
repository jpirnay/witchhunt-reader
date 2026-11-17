#pragma once

// Archives for host tests of ZipFile and what reads through it: a STORED-only zip writer (an
// entry's method is only what the directory claims), and the DEFLATE streams of moby-dick.epub's
// entries with their plaintext, for entries a test enciphers itself.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ZipFile.h"

namespace test_zip {

inline void put16(std::string& out, const uint16_t v) {
  out.push_back(static_cast<char>(v));
  out.push_back(static_cast<char>(v >> 8));
}
inline void put32(std::string& out, const uint32_t v) {
  for (int i = 0; i < 4; i++) out.push_back(static_cast<char>(v >> (8 * i)));
}

struct EntrySpec {
  std::string name;
  std::string data;
  uint16_t method = 0;
  uint32_t uncompressedSize = 0;  // as the directory claims it; 0: data's size
};

// Writes the archive and returns each entry's local-header offset.
inline std::vector<uint32_t> writeZip(const std::string& path, const std::vector<EntrySpec>& entries) {
  std::string zip;
  std::vector<uint32_t> offsets;
  const auto header = [&](const EntrySpec& e) {
    put16(zip, e.method);
    put16(zip, 0);  // time
    put16(zip, 0);  // date
    put32(zip, 0);  // crc32, which no reader here checks
    put32(zip, static_cast<uint32_t>(e.data.size()));
    put32(zip, e.uncompressedSize != 0 ? e.uncompressedSize : static_cast<uint32_t>(e.data.size()));
    put16(zip, static_cast<uint16_t>(e.name.size()));
    put16(zip, 0);  // extra length
  };
  for (const auto& e : entries) {
    offsets.push_back(static_cast<uint32_t>(zip.size()));
    put32(zip, 0x04034b50);
    put16(zip, 20);  // version needed
    put16(zip, 0);   // flags
    header(e);
    zip += e.name;
    zip += e.data;
  }
  const auto cdOffset = static_cast<uint32_t>(zip.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    put32(zip, 0x02014b50);
    put16(zip, 20);  // version made by
    put16(zip, 20);  // version needed
    put16(zip, 0);   // flags
    header(entries[i]);
    put16(zip, 0);  // comment length
    put16(zip, 0);  // disk number
    put16(zip, 0);  // internal attributes
    put32(zip, 0);  // external attributes
    put32(zip, offsets[i]);
    zip += entries[i].name;
  }
  const auto cdSize = static_cast<uint32_t>(zip.size()) - cdOffset;
  put32(zip, 0x06054b50);
  put16(zip, 0);
  put16(zip, 0);
  put16(zip, static_cast<uint16_t>(entries.size()));
  put16(zip, static_cast<uint16_t>(entries.size()));
  put32(zip, cdSize);
  put32(zip, cdOffset);
  put16(zip, 0);
  FILE* f = fopen(path.c_str(), "wb");
  EXPECT_NE(f, nullptr) << path;
  if (f) {
    fwrite(zip.data(), 1, zip.size(), f);
    fclose(f);
  }
  return offsets;
}

class StringSink : public Print {
 public:
  std::string data;
  size_t write(uint8_t b) override {
    data.push_back(static_cast<char>(b));
    return 1;
  }
  size_t write(const uint8_t* buf, size_t n) override {
    data.append(reinterpret_cast<const char*>(buf), n);
    return n;
  }
};

// An entry of an archive: its DEFLATE stream as stored, and its plaintext.
struct DeflatedEntry {
  std::string stream;
  std::string plain;
};

inline DeflatedEntry deflatedEntry(const std::string& archive, const char* name) {
  DeflatedEntry out;
  ZipFile zip(archive);
  StringSink plain;
  EXPECT_TRUE(zip.readFileToStream(name, plain, 1024)) << name;
  out.plain = plain.data;
  ZipFile::FileStatSlim stat{};
  EXPECT_TRUE(zip.loadFileStatSlim(name, &stat)) << name;
  EXPECT_EQ(stat.method, 8) << name;
  FILE* f = fopen(archive.c_str(), "rb");
  if (f == nullptr) return out;
  uint8_t local[30];
  fseek(f, static_cast<long>(stat.localHeaderOffset), SEEK_SET);
  EXPECT_EQ(fread(local, 1, sizeof(local), f), sizeof(local));
  const long data =
      static_cast<long>(stat.localHeaderOffset) + 30 + (local[26] | local[27] << 8) + (local[28] | local[29] << 8);
  out.stream.resize(stat.compressedSize);
  fseek(f, data, SEEK_SET);
  EXPECT_EQ(fread(out.stream.data(), 1, out.stream.size(), f), out.stream.size());
  fclose(f);
  return out;
}

}  // namespace test_zip
