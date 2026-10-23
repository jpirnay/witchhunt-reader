#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"

// One entry of a family's files[], handed over whole when its object closes: the manifest does not
// promise its fields in any order.
struct FontManifestFile {
  const char* name;  // NUL-terminated; empty when absent or not a string
  size_t nameLen;
  // The name did not fit: cut to FILE_NAME_BUF_SIZE - 1 bytes, or empty when it was longer than
  // StreamingJsonParser's token buffer. Never a usable name.
  bool nameOverflow;
  uint32_t size;  // 0 unless a non-negative integer that fits uint32_t
  uint32_t crc32;
  bool hasCrc32;  // false when absent (a v1 manifest) or not a non-negative integer that fits uint32_t
};

// Any of these may be null.
struct FontManifestCallbacks {
  void* ctx;
  void (*onFamilyBegin)(void* ctx);
  void (*onFamilyName)(void* ctx, const char* value, size_t len);
  void (*onFamilyDescription)(void* ctx, const char* value, size_t len);
  void (*onFile)(void* ctx, const FontManifestFile& file);
  void (*onFamilyEnd)(void* ctx);
  void (*onFamilyFormat)(void* ctx, const char* value, size_t len);  // "format", version 3
  void (*onFamilyInDownloadAll)(void* ctx, bool value);              // "inDownloadAll", version 3; booleans only
};

// The font manifest (assets/sd-fonts/fonts.json, fonts-v3.json) as a stream of events, over StreamingJsonParser:
//
//   { "version": 3, "baseUrl": "https://...",
//     "families": [ { "name": "...", "description": "...", "styles": [...],
//                     "format": "cpfont", "inDownloadAll": true,
//                     "files": [ { "name": "Fam/Fam_10.cpfont", "size": 269830, "crc32": 3998528417 } ] } ] }
//
// "format" (a string) and "inDownloadAll" (a boolean) came with version 3; earlier manifests leave them out.
//
// Fed in pieces of any size, it holds one token and one file entry, never the document. No key order
// is assumed. styles[] and every unknown key are skipped whole; a known key holding the wrong type
// is ignored. An element of families[] or files[] that is not an object arrives as an empty entry,
// which the caller's checks reject, as a whole-document parse saw it (a null object).
//
// StreamingJsonParser drops a string longer than its 512-byte token buffer, without a word. Here the
// file name and base URL report it as overflow; a family name or description so long simply never
// arrives, so the family's name stays empty and fails the caller's check.
class FontManifestParser {
 public:
  // The shipped base URL is 82 bytes.
  static constexpr size_t BASE_URL_BUF_SIZE = 192;
  // Both callers reject a file name over 60 bytes, and build it into 128-byte paths.
  static constexpr size_t FILE_NAME_BUF_SIZE = 128;

  explicit FontManifestParser(const FontManifestCallbacks& callbacks);

  FontManifestParser(const FontManifestParser&) = delete;
  FontManifestParser& operator=(const FontManifestParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  // Not well-formed JSON, as far as it went: what StreamingJsonParser rejects, a closing bracket
  // before the root opened, or one that does not match a container the manifest has open. Nothing
  // after the root closes counts.
  bool hasError() const;
  // The root has closed. A document cut short never gets here; what follows the root is ignored.
  bool complete() const { return where == Where::DONE; }
  // 0 until an integer version is read.
  int version() const { return manifestVersion; }
  const char* baseUrl() const { return baseUrlBuf; }
  // The base URL did not fit, and baseUrl() is cut short or empty. Never a usable URL.
  bool baseUrlOverflow() const { return baseUrlOverflowed; }

 private:
  // The containers the parser understands, innermost last. Anything else is skipped whole.
  enum class Where : uint8_t { BEFORE_ROOT, IN_ROOT, IN_FAMILIES, IN_FAMILY, IN_FILES, IN_FILE, DONE };

  // The key whose value comes next.
  enum class Key : uint8_t {
    NONE,
    OTHER,
    VERSION,
    BASE_URL,
    FAMILIES,
    NAME,
    DESCRIPTION,
    FORMAT,
    IN_DOWNLOAD_ALL,
    FILES,
    SIZE,
    CRC32
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void onKey(const char* key, size_t len);
  void onString(const char* value, size_t len);
  void onNumber(const char* value, size_t len);
  void onBool(bool value);
  void onOtherValue();
  void onOpen(bool isArray);
  void onClose(bool isArray);

  bool ignoring() const { return where == Where::DONE || skipDepth > 0; }
  Key takeKey();
  void valueDropped(Key key);
  void beginFile();
  void emitFile();
  void emitEmptyFamily();
  void emitEmptyFile();

  StreamingJsonParser json;
  FontManifestCallbacks cb;

  Where where;
  Key pendingKey;
  // Depth inside a container being skipped; 0 when none is.
  uint8_t skipDepth;
  bool structureError;

  int manifestVersion;
  char baseUrlBuf[BASE_URL_BUF_SIZE];
  bool baseUrlOverflowed;

  char fileName[FILE_NAME_BUF_SIZE];
  size_t fileNameLen;
  bool fileNameOverflow;
  uint32_t fileSize;
  uint32_t fileCrc32;
  bool fileHasCrc32;
};
