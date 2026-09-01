#pragma once

// Minimal STORED-only zip writer for tests that need a purpose-shaped EPUB.
//
// Generated rather than checked in: several tests care about a book's SHAPE (spine sizes, note
// counts, a reference to an entry that does not exist) and a fixture per case would be a pile of
// binaries no one can diff. ZipFile reads method 0, so no deflater is needed here; the tests that
// specifically need the inflate path use the corpus books instead.

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace test_zip {

// --- minimal STORED-only zip writer -------------------------------------------------------

class StoredZipWriter {
 public:
  void add(const std::string& name, const std::string& data) { entries_.push_back({name, data, 0}); }

  void write(const std::string& path) {
    std::string out;
    for (Entry& e : entries_) {
      e.localOffset = static_cast<uint32_t>(out.size());
      appendLocalHeader(out, e);
      out += e.data;
    }
    const uint32_t centralStart = static_cast<uint32_t>(out.size());
    for (const Entry& e : entries_) {
      appendCentralHeader(out, e);
    }
    const uint32_t centralSize = static_cast<uint32_t>(out.size()) - centralStart;
    appendEocd(out, centralStart, centralSize, static_cast<uint16_t>(entries_.size()));
    std::ofstream f(path, std::ios::binary);
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
  }

 private:
  struct Entry {
    std::string name;
    std::string data;
    uint32_t localOffset;
  };

  static void put16(std::string& out, const uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
  }
  static void put32(std::string& out, const uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  // CRC-32 is checked by nothing on the read path here, but a zero would be a lie in a file we
  // hand to a real reader, so compute it properly.
  static uint32_t crc32(const std::string& data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char c : data) {
      crc ^= c;
      for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return ~crc;
  }
  static void appendLocalHeader(std::string& out, const Entry& e) {
    put32(out, 0x04034B50);
    put16(out, 20);  // version needed
    put16(out, 0);   // flags
    put16(out, 0);   // method: stored
    put16(out, 0);   // time
    put16(out, 0);   // date
    put32(out, crc32(e.data));
    put32(out, static_cast<uint32_t>(e.data.size()));
    put32(out, static_cast<uint32_t>(e.data.size()));
    put16(out, static_cast<uint16_t>(e.name.size()));
    put16(out, 0);
    out += e.name;
  }
  static void appendCentralHeader(std::string& out, const Entry& e) {
    put32(out, 0x02014B50);
    put16(out, 20);  // version made by
    put16(out, 20);  // version needed
    put16(out, 0);
    put16(out, 0);  // method: stored
    put16(out, 0);
    put16(out, 0);
    put32(out, crc32(e.data));
    put32(out, static_cast<uint32_t>(e.data.size()));
    put32(out, static_cast<uint32_t>(e.data.size()));
    put16(out, static_cast<uint16_t>(e.name.size()));
    put16(out, 0);  // extra
    put16(out, 0);  // comment
    put16(out, 0);  // disk
    put16(out, 0);  // internal attrs
    put32(out, 0);  // external attrs
    put32(out, e.localOffset);
    out += e.name;
  }
  static void appendEocd(std::string& out, const uint32_t centralStart, const uint32_t centralSize,
                         const uint16_t entryCount) {
    put32(out, 0x06054B50);
    put16(out, 0);  // disk number
    put16(out, 0);  // disk with central dir
    put16(out, entryCount);
    put16(out, entryCount);
    put32(out, centralSize);
    put32(out, centralStart);
    put16(out, 0);  // comment length
  }

  std::vector<Entry> entries_;
};

}  // namespace test_zip
