#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "CrossPointPosition.h"

// Our own last successful push of a book, kept beside its progress.bin (kosync_push.bin).
//
// The push named the page the reader was on: its spine, page and content offset. A server record
// that is exactly that push (same document id, same XPath) is therefore a position this device
// already knows to the byte, and the sync screen takes it from here instead of inflating and
// parsing the chapter. The reverse mapping is the sync's memory peak (up to 32 KB of inflate
// window plus the parser), and a compare's record is most often our own last push.
//
// Plain data: a bounded XPath slot, no std::string. An XPath that does not fit is not cached.
struct LastPush {
  static constexpr size_t kMaxXPath = 192;   // with the NUL; KOReader XPaths run under 160 bytes
  static constexpr size_t kHashLength = 32;  // a KOReader document id: 32 hex characters
  static constexpr uint8_t kVersion = 1;
  // version, document id, XPath length, XPath, spine, page, offset, offset flag
  static constexpr size_t kMaxEncoded = 1 + kHashLength + 1 + (kMaxXPath - 1) + 4 + 4 + 4 + 1;

  char documentHash[kHashLength + 1] = {};
  char xpath[kMaxXPath] = {};
  int32_t spineIndex = 0;
  int32_t page = 0;
  uint32_t visibleOffset = 0;
  bool hasVisibleOffset = false;

  // The record for a push of `xpath` under `documentHash`; nullopt when it cannot be cached (an
  // empty or over-long XPath, a document id that is not 32 characters).
  static std::optional<LastPush> make(const char* hash, const char* path, const int spine, const int pageNumber,
                                      const uint32_t offset, const bool hasOffset) {
    if (!hash || !path || std::strlen(hash) != kHashLength) return std::nullopt;
    const size_t pathLength = std::strlen(path);
    if (pathLength == 0 || pathLength >= kMaxXPath) return std::nullopt;
    LastPush push;
    std::memcpy(push.documentHash, hash, kHashLength + 1);
    std::memcpy(push.xpath, path, pathLength + 1);
    push.spineIndex = spine;
    push.page = pageNumber;
    push.visibleOffset = offset;
    push.hasVisibleOffset = hasOffset;
    return push;
  }

  // The position the push named, as the reverse mapping of its XPath would have resolved it.
  CrossPointPosition position() const {
    CrossPointPosition pos{};
    pos.spineIndex = spineIndex;
    pos.pageNumber = page;
    pos.totalPages = 0;
    pos.visibleTextOffset = visibleOffset;
    pos.hasVisibleTextOffset = hasVisibleOffset;
    pos.hasResolvedSpineIndex = true;
    return pos;
  }

  // Returns the number of bytes written; little-endian scalars, the XPath without its NUL.
  size_t encode(uint8_t (&out)[kMaxEncoded]) const {
    const size_t pathLength = std::strlen(xpath);
    size_t at = 0;
    out[at++] = kVersion;
    std::memcpy(out + at, documentHash, kHashLength);
    at += kHashLength;
    out[at++] = static_cast<uint8_t>(pathLength);
    std::memcpy(out + at, xpath, pathLength);
    at += pathLength;
    at = put32(out, at, static_cast<uint32_t>(spineIndex));
    at = put32(out, at, static_cast<uint32_t>(page));
    at = put32(out, at, visibleOffset);
    out[at++] = hasVisibleOffset ? 1 : 0;
    return at;
  }

  // nullopt for another version, a truncated or overlong file, or a malformed record: the caller
  // then has no cached push and maps the record like any other.
  static std::optional<LastPush> decode(const uint8_t* data, const size_t size) {
    if (!data || size < 1 + kHashLength + 1 || data[0] != kVersion) return std::nullopt;
    size_t at = 1;
    LastPush push;
    std::memcpy(push.documentHash, data + at, kHashLength);
    push.documentHash[kHashLength] = '\0';
    at += kHashLength;
    const size_t pathLength = data[at++];
    if (pathLength == 0 || pathLength >= kMaxXPath || size != at + pathLength + 4 + 4 + 4 + 1) return std::nullopt;
    std::memcpy(push.xpath, data + at, pathLength);
    push.xpath[pathLength] = '\0';
    at += pathLength;
    push.spineIndex = static_cast<int32_t>(get32(data, at));
    push.page = static_cast<int32_t>(get32(data, at + 4));
    push.visibleOffset = get32(data, at + 8);
    const uint8_t flag = data[at + 12];
    if (flag > 1 || std::strlen(push.documentHash) != kHashLength || std::strlen(push.xpath) != pathLength) {
      return std::nullopt;
    }
    push.hasVisibleOffset = flag == 1;
    return push;
  }

 private:
  static size_t put32(uint8_t* out, size_t at, const uint32_t v) {
    for (int shift = 0; shift < 32; shift += 8) out[at++] = static_cast<uint8_t>((v >> shift) & 0xFF);
    return at;
  }
  static uint32_t get32(const uint8_t* in, const size_t at) {
    return static_cast<uint32_t>(in[at]) | (static_cast<uint32_t>(in[at + 1]) << 8) |
           (static_cast<uint32_t>(in[at + 2]) << 16) | (static_cast<uint32_t>(in[at + 3]) << 24);
  }
};

// True when the server's record is exactly our last push: the same document id and the same XPath,
// byte for byte. The record's device id is not compared: a server may rewrite it, and the XPath
// already names the position to the character.
inline bool remoteIsOurLastPush(const LastPush& last, const char* documentHash, const char* remoteXPath) {
  if (!documentHash || !remoteXPath || last.xpath[0] == '\0' || last.documentHash[0] == '\0') return false;
  return std::strcmp(last.documentHash, documentHash) == 0 && std::strcmp(last.xpath, remoteXPath) == 0;
}
