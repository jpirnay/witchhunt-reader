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
struct FontManifestCallbacks;

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

  // `takesPacks`: the hyphenation packs (format "hyphenation") and the UI language packs (format "language") are read
  // too. The Font Manager and the web font API install them; a reader that installs fonts alone leaves them out as
  // formats it does not know.
  FontManifestReader(const char* logTag, NameCheck familyNameOk, NameCheck fileNameOk, bool takesPacks = false)
      : logTag(logTag), familyNameOk(familyNameOk), fileNameOk(fileNameOk), takesPacks(takesPacks) {}

  FontManifestReader(const FontManifestReader&) = delete;
  FontManifestReader& operator=(const FontManifestReader&) = delete;

  // Reads `file` from its start. On Ok, `catalog` and `baseUrl` hold the manifest; otherwise both are
  // left empty. OutOfMemory: the parser or the catalog's block could not be allocated, or the list is too large for
  // one block.
  FontManifestStatus read(HalFile& file, FontCatalog& catalog, std::string& baseUrl);

  // What forEachFamily() hands over, in order.
  struct FamilyVisitor {
    void* ctx = nullptr;
    // The document passed the check read() makes: the families follow. Called once, before the first of them.
    void (*checked)(void* ctx) = nullptr;
    // One family the manifest keeps, alone in `family` (index 0), which holds it for the call only.
    void (*family)(void* ctx, FontCatalog& family) = nullptr;
  };

  // read() a family at a time, for an answer written as the file is read: the list is never held whole. The first
  // pass checks the document as read()'s does and notes the size each family takes (4 bytes a family); the second
  // gives each family the manifest keeps a block of exactly that size and hands it over. Statuses as read(); a failure
  // in the second pass comes after `checked` and some families.
  FontManifestStatus forEachFamily(HalFile& file, const FamilyVisitor& visitor, std::string& baseUrl);

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
  FontManifestStatus firstPass(HalFile& file);
  static FontManifestCallbacks parserCallbacks(FontManifestReader* reader);

  void familyBegin();
  void familyName(const char* value, size_t len);
  void familyDescription(const char* value, size_t len);
  void familyFormat(const char* value, size_t len);
  void familyInDownloadAll(bool value);
  void fileEntry(const FontManifestFile& f);
  void familyEnd();
  bool keepOrDrop();
  void visitingBegin();
  void visitingEnd(bool kept);

  const char* logTag;
  NameCheck familyNameOk;
  NameCheck fileNameOk;
  bool takesPacks;

  // Both live only inside read() or forEachFamily().
  Work* work = nullptr;
  FontCatalog::Builder* builder = nullptr;
  bool filling = false;

  // forEachFamily() alone (`listing`): the next family's place in Work's size table, the visitor (set for the second
  // pass), where the family started in the first pass's count, and whether a family went wrong.
  static constexpr uint32_t kDropped = UINT32_MAX;
  bool listing = false;
  size_t nextFamily = 0;
  const FamilyVisitor* visitor = nullptr;
  FontCatalog::Size familyStart;
  bool visitFailed = false;

  const char* failureReason = nullptr;
  int manifestVersion = 0;
};
