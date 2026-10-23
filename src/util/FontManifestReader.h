#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "FontCatalog.h"

// Where the Font Manager and the web font API fetch the manifest. fonts-v3.json lists every hosted family, each with
// its format; fonts.json lists the .cpfont families alone, for firmware that reads versions 1 and 2 only
// (scripts/generate-font-manifest.py writes both).
#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL \
  "https://raw.githubusercontent.com/jpirnay/witchhunt-reader/master/assets/sd-fonts/fonts-v3.json"
#endif

struct FontManifestFile;

enum class FontManifestStatus : uint8_t { Ok, Invalid, UnsupportedVersion, OutOfMemory };

// Reads the downloaded font manifest into a FontCatalog: the part the Font Manager
// (FontDownloadActivity) and the web font API (CrossPointWebServer) share.
//
// The file is streamed through FontManifestParser a block at a time; a whole-document parse of the
// 24 KB manifest aborted the X3 for want of heap. Two passes: the first checks the document and counts
// what its families hold, so nothing is built for a manifest that is rejected and the catalog's one
// block is allocated at exactly its final size; the second fills the block. Each family is checked
// when it closes, as the whole-document parse checked it: its name, then its file names in order. The
// first that fails is logged (in the second pass) and the family left out; it takes no room. A family of a format this
// reader does not know (version 3 names each family's) is left out the same way, and so is one with a file that is
// not of its format.
class FontManifestReader {
 public:
  using NameCheck = bool (*)(const char* name);

  // A family name this long or longer is rejected without asking the check. FontInstaller allows 31.
  static constexpr size_t FAMILY_NAME_BUF_SIZE = 64;

  FontManifestReader(const char* logTag, NameCheck familyNameOk, NameCheck fileNameOk)
      : logTag(logTag), familyNameOk(familyNameOk), fileNameOk(fileNameOk) {}

  FontManifestReader(const FontManifestReader&) = delete;
  FontManifestReader& operator=(const FontManifestReader&) = delete;

  // Reads `file` from its start. On Ok, `catalog` and `baseUrl` hold the manifest; otherwise both are
  // left empty. OutOfMemory: the parser or the catalog's block could not be allocated, or the list is too large for
  // one block.
  FontManifestStatus read(HalFile& file, FontCatalog& catalog, std::string& baseUrl);

  // Why read() did not return Ok, for the log.
  const char* failure() const { return failureReason; }
  // The manifest's version, once read() has checked the document; 0 if it gives none.
  int version() const { return manifestVersion; }

 private:
  struct Work;

  bool fail(const char* why) {
    failureReason = why;
    return false;
  }
  bool feedFile(HalFile& file);

  void familyBegin();
  void familyName(const char* value, size_t len);
  void familyDescription(const char* value, size_t len);
  void familyFormat(const char* value, size_t len);
  void familyInDownloadAll(bool value);
  void fileEntry(const FontManifestFile& f);
  void familyEnd();

  const char* logTag;
  NameCheck familyNameOk;
  NameCheck fileNameOk;

  // Both live only inside read().
  Work* work = nullptr;
  FontCatalog::Builder* builder = nullptr;
  bool filling = false;

  const char* failureReason = nullptr;
  int manifestVersion = 0;
};
