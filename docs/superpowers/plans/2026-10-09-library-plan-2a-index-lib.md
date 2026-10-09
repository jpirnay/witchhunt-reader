# Library Plan 2a: the Book Index Library (`lib/LibraryIndex`)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A host-tested library that defines the book index file, reads it, assembles it from a build's working files (publish), and carries a previous index's authors and `firstSeen` forward to a new walk (join).

**Architecture:**
- **Keys** are pure functions: fold, surname-first sort key, author hash, book identity.
- **The index file** is sectioned and read on demand by `LibraryIndexReader`.
- **`LibraryPublish::publish()`** sorts the whole author table in one pass, using a `BuildArena` (the lent framebuffer on the device). It writes the result to a temp file and renames it into place.
- **`LibraryJoin::join()`** sorts the staged walk by identity and merges it with the previous index in one sequential pass.

Nothing calls the library yet. The builder, the model modes and the firmware flash measurement come in Plan 2b.

**Tech Stack:** C++20; `HalStorage`/`HalFile` (device) or the stdio shim `test/zip_entry_reader/HalStorage.h` (host); `BuildArena` (`lib/Memory`); `utf8NextCodepoint` (`lib/Utf8`); GoogleTest.

**Spec:** `docs/superpowers/specs/2026-10-09-library-view-design.md`, §3.2 (file), §3.3 (identity, New order), §3.4 (author key), §3.5 phases 2-3 (join, publish), §3.7 (memory). The cap is 2,000 books (user decision 2026-10-09).

## Global Constraints

- **Cap:** `library::MAX_BOOKS = 2000`. Publish needs at most ~22 B per book of arena (44 KB at the cap). Join needs 16 B per book (32 KB).
- **No heap outside the arena,** except: `std::string` locals for one name or key at a time, and the tie-fix vector (at most `MAX_TIE_RUN = 32` short keys). No locals of 256 B or more.
- **Reserved author hashes:** `AUTHOR_UNKNOWN = 0` and `AUTHOR_PENDING = 0xFFFFFFFF`. `SIDECAR_UNKNOWN = 0xFFFFFFFF` means "always resolve again".
- **Blob strings:** a `uint16_t` length followed by the bytes, at most `library::MAX_STRING = 1024`.
- **Never call `close()` on a `HalFile` that no `open` call ever touched;** the shim aborts on that, mirroring the device. Rely on the destructor for early returns.
- **Build host tests from Git Bash only:** `cd build/test && cmake --build . -j 8 -- -k 0`. `epub_build_inventory` fails on Windows by design.
- **Format with clang-format 21,** applied to every C++ file the branch touches: `"/c/tmp/cf21/bin/clang-format-21.exe" -i --style=file <files>`.
- **Never `git add -A`.** Add paths explicitly.
- **Branch:** `feat/library-index`, stacked on `feat/library-metadata` (PR #425).

## Review Focus

1. **A names file whose last entry is cut short** (power lost while the builder appended) must still publish, with every complete entry used. Test in Task 2.
2. **An author hash with no name entry** (a resolved author whose name was never recorded) must publish as a nameless author filed with the unknowns, without failing. Test in Task 2.
3. **An empty card** (zero books) must publish a valid empty index that the reader opens, and a zero-book join must work. Tests in Tasks 2 and 3.
4. **A long non-ASCII path** (300 bytes of UTF-8) must round-trip through the blob. Test in Task 2.
5. **More equal key prefixes than `MAX_TIE_RUN`** must still publish every author, in a valid order. Test in Task 2.

---

### Task 0: Branch and documents

**Files:** the spec (already edited for the 2,000 cap and the Refresh rule) and this plan.

- [ ] **Step 1: Confirm the branch**

```bash
cd /c/_development/witchhunt-reader && git branch --show-current
```
Expected: `feat/library-index`.

- [ ] **Step 2: Commit the spec update and this plan**

```bash
git add docs/superpowers/specs/2026-10-09-library-view-design.md docs/superpowers/plans/2026-10-09-library-plan-2a-index-lib.md
git commit -m "docs: library plan 2a (index library); cap 2,000 books, Refresh keeps firstSeen"
```

---

### Task 1: Format constants and keys

**Files:**
- Create: `lib/LibraryIndex/LibraryFormat.h`, `lib/LibraryIndex/LibraryKeys.h`, `lib/LibraryIndex/LibraryKeys.cpp`
- Create: `test/library_index/CMakeLists.txt`, `test/library_index/LibraryKeysTest.cpp`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(library_index)` after `add_subdirectory(progress_comparison)`)

**Interfaces:**
- Produces:
  - `namespace library` with `MAGIC`, `VERSION`, `MAX_BOOKS`, `NEW_COUNT`, `AUTHOR_UNKNOWN`, `AUTHOR_PENDING`, `SIDECAR_NONE`, `SIDECAR_UNKNOWN`, `FLAG_PARTIAL`, `MAX_STRING`, `DIR`, `INDEX_PATH`
  - structs `Header` (44 B), `BookRecord` (24 B), `AuthorRecord` (12 B), `StagedBook` (16 B)
  - `LibraryKeys::fold(const std::string&) -> std::string`
  - `LibraryKeys::authorSortKey(const std::string& name, const std::string& fileAs) -> std::string`
  - `LibraryKeys::authorHash(const std::string&) -> uint32_t`
  - `LibraryKeys::bookIdentity(const char* fileName, uint32_t size) -> uint32_t`

- [ ] **Step 1: Write the failing tests**

`test/library_index/CMakeLists.txt`:

```cmake
add_executable(LibraryIndexTest
  LibraryKeysTest.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryKeys.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(LibraryIndexTest PRIVATE
  # stdio-backed HalStorage.h; must shadow the test/shims stub, which cannot open files.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Logging
)

target_link_libraries(LibraryIndexTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LibraryIndexTest)
```

`test/library_index/LibraryKeysTest.cpp`:

```cpp
// How the book index tells books and authors apart, and the order authors file in (spec §3.3, §3.4).

#include <LibraryFormat.h>
#include <LibraryKeys.h>
#include <gtest/gtest.h>

#include <string>

namespace {

TEST(LibraryKeys, FoldLowercasesAndReducesLatinLettersToTheirBase) {
  EXPECT_EQ(LibraryKeys::fold("\xC3\x89mile Zola"), "emile zola");                     // É
  EXPECT_EQ(LibraryKeys::fold("\xC3\x96" "d\xC3\xB6n von Horv\xC3\xA1th"), "odon von horvath");  // Ö ö á
  EXPECT_EQ(LibraryKeys::fold("\xC5\x81ukasz Orbitowski"), "lukasz orbitowski");       // Ł
  EXPECT_EQ(LibraryKeys::fold("Dvo\xC5\x99\xC3\xA1k"), "dvorak");                      // ř á
}

TEST(LibraryKeys, FoldSpellsOutLettersThatAreTwo) {
  EXPECT_EQ(LibraryKeys::fold("Stra\xC3\x9F" "e"), "strasse");          // ß
  EXPECT_EQ(LibraryKeys::fold("\xC3\x86r\xC3\xB8"), "aero");            // Æ ø
  EXPECT_EQ(LibraryKeys::fold("\xC5\x92uvre"), "oeuvre");               // Œ
}

// macOS writes names decomposed: "O" followed by a combining diaeresis must file with "Ö".
TEST(LibraryKeys, FoldDropsCombiningMarksSoDecomposedNamesMatch) {
  EXPECT_EQ(LibraryKeys::fold("O\xCC\x88zil"), LibraryKeys::fold("\xC3\x96zil"));
  EXPECT_EQ(LibraryKeys::fold("O\xCC\x88zil"), "ozil");
}

// A pretty-printed OPF can leave line breaks inside a name (review minor M2 of plan 1).
TEST(LibraryKeys, FoldCollapsesWhitespaceRuns) {
  EXPECT_EQ(LibraryKeys::fold("  Ursula K.\n   Le\tGuin \xC2\xA0"), "ursula k. le guin");
}

TEST(LibraryKeys, FoldKeepsOtherScriptsAsTheyAre) {
  EXPECT_EQ(LibraryKeys::fold("\xD0\x9B\xD0\xB5\xD0\xB2"), "\xD0\x9B\xD0\xB5\xD0\xB2");  // Лев
}

TEST(LibraryKeys, TheSortKeyIsTheFileAsWhenThereIsOne) {
  EXPECT_EQ(LibraryKeys::authorSortKey("Ursula K. Le Guin", "Le Guin, Ursula K."), "le guin ursula k");
}

TEST(LibraryKeys, ANameWithACommaSortsAsWritten) {
  EXPECT_EQ(LibraryKeys::authorSortKey("Le Guin, Ursula K.", ""), "le guin ursula k");
}

TEST(LibraryKeys, OtherwiseTheLastWordComesFirst) {
  EXPECT_EQ(LibraryKeys::authorSortKey("Terry Pratchett", ""), "pratchett terry");
  EXPECT_EQ(LibraryKeys::authorSortKey("Homer", ""), "homer");
  EXPECT_EQ(LibraryKeys::authorSortKey("", ""), "");
}

TEST(LibraryKeys, TheAuthorHashIgnoresCaseAccentsAndSpacing) {
  EXPECT_EQ(LibraryKeys::authorHash("\xC3\x89mile  Zola"), LibraryKeys::authorHash("emile zola"));
  EXPECT_NE(LibraryKeys::authorHash("Emile Zola"), LibraryKeys::authorHash("Emil Zola"));
  EXPECT_EQ(LibraryKeys::authorHash(""), library::AUTHOR_UNKNOWN);
  EXPECT_EQ(LibraryKeys::authorHash(" \n "), library::AUTHOR_UNKNOWN);
  EXPECT_NE(LibraryKeys::authorHash("Terry Pratchett"), library::AUTHOR_PENDING);
}

TEST(LibraryKeys, ABooksIdentityIgnoresCaseButNotSizeOrName) {
  EXPECT_EQ(LibraryKeys::bookIdentity("Mort.epub", 1000), LibraryKeys::bookIdentity("MORT.EPUB", 1000));
  EXPECT_NE(LibraryKeys::bookIdentity("Mort.epub", 1000), LibraryKeys::bookIdentity("Mort.epub", 1001));
  EXPECT_NE(LibraryKeys::bookIdentity("Mort.epub", 1000), LibraryKeys::bookIdentity("Mort2.epub", 1000));
}

}  // namespace
```

Add `add_subdirectory(library_index)` to `test/CMakeLists.txt`, right after the line `add_subdirectory(progress_comparison)`.

- [ ] **Step 2: Run and watch it fail**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target LibraryIndexTest -j 8 2>&1 | grep -E "error|Cannot find" | head -3
```
Expected: CMake cannot find `lib/LibraryIndex/LibraryKeys.cpp`, or `LibraryFormat.h` is missing.

- [ ] **Step 3: Write `LibraryFormat.h`**

```cpp
#pragma once

#include <cstdint>

// The book index the Library's New and Authors tabs read, and the working files a build assembles
// it from. Design: docs/superpowers/specs/2026-10-09-library-view-design.md, section 3.
namespace library {

constexpr char MAGIC[4] = {'W', 'L', 'I', 'B'};
constexpr uint8_t VERSION = 1;

// Books beyond this are left out and the index is marked partial: publish sorts the whole author
// table in one pass through the lent framebuffer, which this many books fit.
constexpr uint16_t MAX_BOOKS = 2000;
// How many books the New tab lists.
constexpr uint16_t NEW_COUNT = 10;
// Reserved author hashes: a book with no author at all, and one not resolved yet.
constexpr uint32_t AUTHOR_UNKNOWN = 0;
constexpr uint32_t AUTHOR_PENDING = 0xFFFFFFFFu;
// Sidecar signatures: no metadata sidecar, and one the walk could not pair with its book (the
// folder's sidecar table overflowed), which always goes back through details.bin.
constexpr uint32_t SIDECAR_NONE = 0;
constexpr uint32_t SIDECAR_UNKNOWN = 0xFFFFFFFFu;
// Header flags.
constexpr uint8_t FLAG_PARTIAL = 0x01;
// Longest blob string: a path, an author's name or sort key.
constexpr uint16_t MAX_STRING = 1024;

// Where the index and a build's working files live on the card.
constexpr const char* DIR = "/.crosspoint/library";
constexpr const char* INDEX_PATH = "/.crosspoint/library/library.bin";

#pragma pack(push, 1)
struct Header {
  char magic[4];
  uint8_t version;
  uint8_t flags;
  uint8_t acceptRules;  // the walk's showHiddenFiles: another setting lists other books
  uint8_t reserved;
  uint32_t buildGen;
  uint16_t bookCount;
  uint16_t authorCount;
  uint16_t newCount;
  uint16_t reserved2;
  uint32_t recordsOff;
  uint32_t newOff;
  uint32_t authorsOff;
  uint32_t authorBooksOff;
  uint32_t blobOff;
  uint32_t blobLen;
};

// One book, in identity order. pathOff is into the blob.
struct BookRecord {
  uint32_t identity;
  uint32_t authorHash;
  uint32_t date;  // FAT (date << 16) | time, the later of modified and created
  uint32_t pathOff;
  uint32_t sidecarSig;
  uint32_t firstSeen;  // buildGen of the build that first saw this identity
};

// One author, in sort-key order. nameOff (into the blob) holds the display name and then the sort
// key; the author's books are the author-books table's slots [firstBook, firstBook + count).
struct AuthorRecord {
  uint32_t hash;
  uint32_t nameOff;
  uint16_t firstBook;
  uint16_t count;
};

// One book as the walk found it, before any metadata. pathOff is into the paths working file.
struct StagedBook {
  uint32_t identity;
  uint32_t date;
  uint32_t sidecarSig;
  uint32_t pathOff;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 44, "header layout");
static_assert(sizeof(BookRecord) == 24, "book record layout");
static_assert(sizeof(AuthorRecord) == 12, "author record layout");
static_assert(sizeof(StagedBook) == 16, "staged book layout");

}  // namespace library
```

- [ ] **Step 4: Write `LibraryKeys.h` and `LibraryKeys.cpp`**

`lib/LibraryIndex/LibraryKeys.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>

// How the book index tells books and authors apart and orders authors. Pure functions.
namespace LibraryKeys {

// Text folded for comparison: ASCII lowercased, Latin-1 and Latin Extended-A letters reduced to their
// base letter ("Ö" -> "o", "ß" -> "ss", "Ł" -> "l"), combining marks dropped (so a decomposed name
// from macOS folds like its composed form), whitespace runs collapsed to one space and trimmed.
// Other characters pass through unchanged.
std::string fold(const std::string& text);

// The key an author files under, surname first: the book's file-as when it has one; a name that
// already holds a comma as written; otherwise the last word moved to the front ("Terry Pratchett" ->
// "pratchett terry"). Folded, with commas and full stops dropped, so "Le Guin, Ursula K." and a
// file-as of "Le Guin Ursula K" are one key.
std::string authorSortKey(const std::string& name, const std::string& fileAs);

// One author across books: FNV-1a of the folded name, so the same name in another case, accent form
// or spacing still groups. An empty name is library::AUTHOR_UNKNOWN; no name hashes to that or to
// library::AUTHOR_PENDING.
uint32_t authorHash(const std::string& name);

// One book across folders: FNV-1a of the filename, ASCII-lowercased, then its size. A book moved to
// another folder, or renamed only in case, stays the same book; so are two copies of it.
uint32_t bookIdentity(const char* fileName, uint32_t size);

}  // namespace LibraryKeys
```

`lib/LibraryIndex/LibraryKeys.cpp`:

```cpp
#include "LibraryKeys.h"

#include <Utf8.h>

#include <cctype>

#include "LibraryFormat.h"

namespace {

constexpr uint32_t FNV_BASIS = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t fnv1a(uint32_t hash, const void* data, const size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// The base letter of each code point U+00C0..U+017F. '*' marks one that folds to two letters
// (expansion() handles those first) or has no base letter at all (the multiplication and division
// signs), which is kept as it is.
constexpr char kLatinBase[] =
    "aaaaaa*ceeeeiiiidnooooo*ouuuuy**"                                     // U+00C0..U+00DF
    "aaaaaa*ceeeeiiiidnooooo*ouuuuy*y"                                     // U+00E0..U+00FF
    "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiii**jjkkkllllllllll"  // U+0100..U+0142
    "nnnnnnnnnoooooo**rrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";       // U+0143..U+017F
static_assert(sizeof(kLatinBase) - 1 == 0x180 - 0xC0, "one entry per code point U+00C0..U+017F");

const char* expansion(const uint32_t cp) {
  switch (cp) {
    case 0xC6:
    case 0xE6:
      return "ae";
    case 0xDE:
    case 0xFE:
      return "th";
    case 0xDF:
      return "ss";
    case 0x132:
    case 0x133:
      return "ij";
    case 0x152:
    case 0x153:
      return "oe";
    default:
      return nullptr;
  }
}

bool isSpace(const uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0; }

void appendUtf8(std::string& out, const uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

}  // namespace

namespace LibraryKeys {

std::string fold(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool space = false;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  for (uint32_t cp = utf8NextCodepoint(&p); cp != 0; cp = utf8NextCodepoint(&p)) {
    if (isSpace(cp)) {
      space = !out.empty();
      continue;
    }
    if (cp >= 0x300 && cp <= 0x36F) continue;  // combining marks
    if (space) {
      out += ' ';
      space = false;
    }
    if (cp < 0x80) {
      out += static_cast<char>(std::tolower(static_cast<int>(cp)));
    } else if (const char* letters = expansion(cp)) {
      out += letters;
    } else if (cp >= 0xC0 && cp < 0x180 && kLatinBase[cp - 0xC0] != '*') {
      out += kLatinBase[cp - 0xC0];
    } else {
      appendUtf8(out, cp);
    }
  }
  return out;
}

std::string authorSortKey(const std::string& name, const std::string& fileAs) {
  std::string key = fold(fileAs);
  if (key.empty()) {
    key = fold(name);
    const size_t lastSpace = key.rfind(' ');
    if (key.find(',') == std::string::npos && lastSpace != std::string::npos) {
      key = key.substr(lastSpace + 1) + ' ' + key.substr(0, lastSpace);
    }
  }
  std::string out;
  out.reserve(key.size());
  for (const char c : key) {
    if (c == ',' || c == '.') continue;
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

uint32_t authorHash(const std::string& name) {
  const std::string folded = fold(name);
  if (folded.empty()) return library::AUTHOR_UNKNOWN;
  uint32_t hash = fnv1a(FNV_BASIS, folded.data(), folded.size());
  if (hash == library::AUTHOR_UNKNOWN) hash = 1;
  if (hash == library::AUTHOR_PENDING) hash = library::AUTHOR_PENDING - 1;
  return hash;
}

uint32_t bookIdentity(const char* fileName, const uint32_t size) {
  uint32_t hash = FNV_BASIS;
  for (const char* c = fileName; *c != '\0'; ++c) {
    const auto lower = static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(*c)));
    hash = fnv1a(hash, &lower, 1);
  }
  const uint8_t sizeBytes[4] = {static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8),
                                static_cast<uint8_t>(size >> 16), static_cast<uint8_t>(size >> 24)};
  return fnv1a(hash, sizeBytes, sizeof(sizeBytes));
}

}  // namespace LibraryKeys
```

- [ ] **Step 5: Run the keys tests**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target LibraryIndexTest -j 8 2>&1 | grep -E "error|warning:" | head -3; ./library_index/LibraryIndexTest.exe 2>&1 | grep -E "^\[  (FAILED|PASSED) "
```
Expected: `[  PASSED  ] 10 tests.`

- [ ] **Step 6: Commit**

```bash
cd /c/_development/witchhunt-reader
git add lib/LibraryIndex/LibraryFormat.h lib/LibraryIndex/LibraryKeys.h lib/LibraryIndex/LibraryKeys.cpp test/library_index/CMakeLists.txt test/library_index/LibraryKeysTest.cpp test/CMakeLists.txt
git commit -m "feat(library): index format constants and author/book keys

Fold (Latin base letters, combining marks dropped, whitespace collapsed),
surname-first author sort keys, author hashes and book identities for the
book index (spec section 3)."
```

---

### Task 2: Reader and publish

**Files:**
- Create: `lib/LibraryIndex/LibraryBlob.h`, `lib/LibraryIndex/LibraryBlob.cpp`, `lib/LibraryIndex/LibraryIndexReader.h`, `lib/LibraryIndex/LibraryIndexReader.cpp`, `lib/LibraryIndex/LibraryPublish.h`, `lib/LibraryIndex/LibraryPublish.cpp`
- Create: `test/library_index/LibraryIndexFixture.h`, `test/library_index/LibraryPublishTest.cpp`
- Modify: `test/library_index/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1's `library::*` and `LibraryKeys::*`.
- Produces:
  - `library::readExact(HalFile&, void*, size_t)`, `library::writeExact(HalFile&, const void*, size_t)`, `library::readBlobString(HalFile&, std::string&)`, `library::writeBlobString(HalFile&, const std::string&)`
  - class `LibraryIndexReader`:
    - `open(const std::string&)`, `close()`, `isOpen()`, `header()`, `partial()`
    - `book(uint16_t, library::BookRecord&)`
    - `newBook(uint16_t rank, uint16_t& recordIndex)`
    - `author(uint16_t, library::AuthorRecord&)`
    - `authorBook(uint32_t slot, uint16_t& recordIndex)`
    - `blobString(uint32_t, std::string&)`
    - `authorName(const library::AuthorRecord&, std::string& name, std::string* key)`
  - `LibraryPublish::Input { recordsPath, pathsPath, namesPath, bookCount, buildGen, acceptRules, partial }` and `bool LibraryPublish::publish(const Input&, BuildArena&, const std::string& outPath)`
  - **Names working-file format:** `uint32_t hash`, `uint8_t flags` (bit 0 = the key came from the book's `file-as`), blob string name, blob string sort key, repeated. Per hash, an entry with a `file-as` key beats one without (spec §3.4: any of the author's books may supply it); otherwise the first wins.

- [ ] **Step 1: Write the test fixture**

`test/library_index/LibraryIndexFixture.h`:

```cpp
#pragma once

// Writes the working files a library build produces, so publish and join can be tested on their own;
// the walk that writes them for real comes with the builder.

#include <BuildArena.h>
#include <LibraryFormat.h>
#include <LibraryIndexReader.h>
#include <LibraryKeys.h>
#include <LibraryPublish.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

class LibraryIndexFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = (std::filesystem::temp_directory_path() /
            ("library_index_" + std::string(info->test_suite_name()) + "_" + info->name()))
               .generic_string();
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    std::ofstream(at("paths.bin"), std::ios::binary);  // a build always leaves a paths file, if empty
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string at(const char* name) const { return dir_ + "/" + name; }

  // Appends a blob string to a working file; returns where it starts.
  uint32_t appendString(const std::string& file, const std::string& s) const {
    std::error_code ec;
    const auto offset = static_cast<uint32_t>(std::filesystem::exists(file) ? std::filesystem::file_size(file) : 0);
    std::ofstream out(file, std::ios::binary | std::ios::app);
    const auto len = static_cast<uint16_t>(s.size());
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
    return offset;
  }

  template <typename T>
  void writeAll(const std::string& file, const std::vector<T>& items) const {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(items.data()), static_cast<std::streamsize>(items.size() * sizeof(T)));
  }

  // An author's names-file entry, as the builder records one when it resolves a book.
  void addName(const std::string& name, const std::string& fileAs = "") const {
    const uint32_t hash = LibraryKeys::authorHash(name);
    const uint8_t flags = fileAs.empty() ? 0 : 1;
    {
      std::ofstream out(at("names.bin"), std::ios::binary | std::ios::app);
      out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
      out.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
    }
    appendString(at("names.bin"), name);
    appendString(at("names.bin"), LibraryKeys::authorSortKey(name, fileAs));
  }

  // A book record whose path goes into the paths working file.
  library::BookRecord book(const std::string& path, const std::string& author, const uint32_t date = 0,
                           const uint32_t firstSeen = 1, const uint32_t size = 100) const {
    const std::string file = path.substr(path.rfind('/') + 1);
    return {LibraryKeys::bookIdentity(file.c_str(), size), LibraryKeys::authorHash(author), date,
            appendString(at("paths.bin"), path), library::SIDECAR_NONE, firstSeen};
  }

  // Publishes `records` in identity order, as a join leaves them.
  bool publish(std::vector<library::BookRecord> records, const uint32_t buildGen = 1, BuildArena* arena = nullptr,
               const bool partial = false) const {
    std::sort(records.begin(), records.end(),
              [](const library::BookRecord& a, const library::BookRecord& b) { return a.identity < b.identity; });
    writeAll(at("records.bin"), records);
    LibraryPublish::Input in;
    in.recordsPath = at("records.bin");
    in.pathsPath = at("paths.bin");
    in.namesPath = at("names.bin");
    in.bookCount = static_cast<uint16_t>(records.size());
    in.buildGen = buildGen;
    in.acceptRules = 1;
    in.partial = partial;
    BuildArena framebuffer(48000);  // the X3/X4 secondary framebuffer
    return LibraryPublish::publish(in, arena != nullptr ? *arena : framebuffer, at("library.bin"));
  }

  // The published authors in table order; the reserved ones by a marker.
  static std::vector<std::string> authorNames(LibraryIndexReader& index) {
    std::vector<std::string> names;
    for (uint16_t i = 0; i < index.header().authorCount; ++i) {
      library::AuthorRecord author{};
      std::string name;
      EXPECT_TRUE(index.author(i, author));
      EXPECT_TRUE(index.authorName(author, name));
      if (author.hash == library::AUTHOR_UNKNOWN) name = "<unknown>";
      if (author.hash == library::AUTHOR_PENDING) name = "<pending>";
      names.push_back(name);
    }
    return names;
  }

  static std::string pathOf(LibraryIndexReader& index, const uint16_t record) {
    library::BookRecord book{};
    std::string path;
    EXPECT_TRUE(index.book(record, book));
    EXPECT_TRUE(index.blobString(book.pathOff, path));
    return path;
  }

  // Paths of an author's books, in table order.
  static std::vector<std::string> booksOf(LibraryIndexReader& index, const uint16_t authorIndex) {
    library::AuthorRecord author{};
    EXPECT_TRUE(index.author(authorIndex, author));
    std::vector<std::string> paths;
    for (uint32_t slot = author.firstBook; slot < uint32_t{author.firstBook} + author.count; ++slot) {
      uint16_t record = 0;
      EXPECT_TRUE(index.authorBook(slot, record));
      paths.push_back(pathOf(index, record));
    }
    std::sort(paths.begin(), paths.end());
    return paths;
  }

  std::string dir_;
};
```

- [ ] **Step 2: Write the failing publish and reader tests**

`test/library_index/LibraryPublishTest.cpp`:

```cpp
// Publish assembles the book index from a build's working files; the reader serves it to the
// screens (spec §3.2, §3.5 phase 3).

#include <filesystem>
#include <fstream>

#include "LibraryIndexFixture.h"

namespace {

using LibraryPublishTest = LibraryIndexFixture;

TEST_F(LibraryPublishTest, APublishedIndexReadsBackWhatWasWritten) {
  addName("Terry Pratchett");
  const auto mort = book("/Books/Mort.epub", "Terry Pratchett", 7, 1);
  ASSERT_TRUE(publish({mort}, 3));

  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(index.header().bookCount, 1);
  EXPECT_EQ(index.header().buildGen, 3u);
  EXPECT_EQ(index.header().acceptRules, 1);
  EXPECT_FALSE(index.partial());
  library::BookRecord record{};
  ASSERT_TRUE(index.book(0, record));
  EXPECT_EQ(record.identity, mort.identity);
  EXPECT_EQ(record.authorHash, mort.authorHash);
  EXPECT_EQ(record.date, 7u);
  EXPECT_EQ(pathOf(index, 0), "/Books/Mort.epub");
}

TEST_F(LibraryPublishTest, AuthorsFileSurnameFirstWithUnknownAndPendingLast) {
  addName("Terry Pratchett");
  addName("Iain Banks");
  addName("Ursula K. Le Guin", "Le Guin, Ursula K.");
  auto pending = book("/c.epub", "Someone");
  pending.authorHash = library::AUTHOR_PENDING;
  ASSERT_TRUE(publish({book("/a.epub", "Terry Pratchett"), book("/b.epub", "Iain Banks"), pending,
                       book("/d.epub", "Ursula K. Le Guin"), book("/e.txt", "")}));

  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(authorNames(index),
            (std::vector<std::string>{"Iain Banks", "Ursula K. Le Guin", "Terry Pratchett", "<unknown>", "<pending>"}));
}

TEST_F(LibraryPublishTest, EachAuthorListsTheirOwnBooks) {
  addName("Terry Pratchett");
  addName("Iain Banks");
  ASSERT_TRUE(publish({book("/Disc/Mort.epub", "Terry Pratchett"), book("/Culture/Excession.epub", "Iain Banks"),
                       book("/Disc/Small Gods.epub", "Terry Pratchett")}));

  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  ASSERT_EQ(authorNames(index), (std::vector<std::string>{"Iain Banks", "Terry Pratchett"}));
  EXPECT_EQ(booksOf(index, 0), (std::vector<std::string>{"/Culture/Excession.epub"}));
  EXPECT_EQ(booksOf(index, 1), (std::vector<std::string>{"/Disc/Mort.epub", "/Disc/Small Gods.epub"}));
}

// New lists the books first seen most recently; within one build, the newest file date first.
TEST_F(LibraryPublishTest, NewIsByFirstSeenThenDate) {
  ASSERT_TRUE(publish({book("/old.epub", "", 900, 1), book("/later.epub", "", 100, 2), book("/latest.epub", "", 200, 2)}));

  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  ASSERT_EQ(index.header().newCount, 3);
  std::vector<std::string> order;
  for (uint16_t rank = 0; rank < 3; ++rank) {
    uint16_t record = 0;
    ASSERT_TRUE(index.newBook(rank, record));
    order.push_back(pathOf(index, record));
  }
  EXPECT_EQ(order, (std::vector<std::string>{"/latest.epub", "/later.epub", "/old.epub"}));
}

TEST_F(LibraryPublishTest, NewStopsAtTen) {
  std::vector<library::BookRecord> books;
  for (int i = 0; i < 15; ++i) books.push_back(book("/b" + std::to_string(i) + ".epub", "", static_cast<uint32_t>(i)));
  ASSERT_TRUE(publish(books));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(index.header().newCount, library::NEW_COUNT);
  uint16_t record = 0;
  ASSERT_TRUE(index.newBook(0, record));
  EXPECT_EQ(pathOf(index, record), "/b14.epub");
}

// The sort compares an 8-byte prefix first; authors sharing it are put in full-key order.
TEST_F(LibraryPublishTest, AuthorsSharingTheKeyPrefixAreInFullKeyOrder) {
  addName("Terry Pratchett");
  addName("Anne Pratchett");
  ASSERT_TRUE(publish({book("/t.epub", "Terry Pratchett"), book("/a.epub", "Anne Pratchett")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(authorNames(index), (std::vector<std::string>{"Anne Pratchett", "Terry Pratchett"}));
}

// Review focus 5: a tie run longer than MAX_TIE_RUN keeps hash order, but nothing is lost.
TEST_F(LibraryPublishTest, AVeryLongTieRunStillPublishesEveryAuthor) {
  std::vector<library::BookRecord> books;
  for (int i = 0; i < 40; ++i) {
    const std::string name = "Zed Aaaaaaaaxx" + std::to_string(i);
    addName(name);
    books.push_back(book("/z" + std::to_string(i) + ".epub", name));
  }
  ASSERT_TRUE(publish(books));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(index.header().authorCount, 40);
}

TEST_F(LibraryPublishTest, TheFirstNameEntryForAnAuthorWins) {
  addName("Terry Pratchett");
  addName("TERRY PRATCHETT");  // same hash, later entry
  ASSERT_TRUE(publish({book("/a.epub", "Terry Pratchett")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(authorNames(index), (std::vector<std::string>{"Terry Pratchett"}));
}

// Spec §3.4: if any of the author's books supplies a file-as, that is the key.
TEST_F(LibraryPublishTest, AFileAsEntryBeatsAnEarlierPlainOne) {
  addName("Ursula K. Le Guin");                        // a book without file-as: "guin ursula k le"
  addName("Ursula K. Le Guin", "Le Guin, Ursula K.");  // a later book with one
  ASSERT_TRUE(publish({book("/a.epub", "Ursula K. Le Guin"), book("/b.epub", "Ursula K. Le Guin")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  library::AuthorRecord author{};
  std::string name;
  std::string key;
  ASSERT_TRUE(index.author(0, author));
  ASSERT_TRUE(index.authorName(author, name, &key));
  EXPECT_EQ(key, "le guin ursula k");
}

// Review focus 1: power lost while the builder appended a name.
TEST_F(LibraryPublishTest, ANamesFileCutShortStillPublishes) {
  addName("Terry Pratchett");
  {
    std::ofstream out(at("names.bin"), std::ios::binary | std::ios::app);
    const uint32_t hash = LibraryKeys::authorHash("Iain Banks");
    const uint8_t flags = 0;
    const uint16_t len = 10;
    out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
    out.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    out.write("Iain", 4);
  }
  ASSERT_TRUE(publish({book("/a.epub", "Terry Pratchett"), book("/b.epub", "Iain Banks")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  const auto names = authorNames(index);
  EXPECT_NE(std::find(names.begin(), names.end(), "Terry Pratchett"), names.end());
}

// Review focus 2: a resolved author whose name never reached the names file.
TEST_F(LibraryPublishTest, AnAuthorWithNoNameEntryPublishesNameless) {
  ASSERT_TRUE(publish({book("/a.epub", "Ghost Writer")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  ASSERT_EQ(index.header().authorCount, 1);
  library::AuthorRecord author{};
  std::string name;
  ASSERT_TRUE(index.author(0, author));
  ASSERT_TRUE(index.authorName(author, name));
  EXPECT_EQ(name, "");
  EXPECT_EQ(booksOf(index, 0), (std::vector<std::string>{"/a.epub"}));
}

// Review focus 3.
TEST_F(LibraryPublishTest, AnEmptyCardPublishesAnEmptyIndex) {
  ASSERT_TRUE(publish({}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(index.header().bookCount, 0);
  EXPECT_EQ(index.header().authorCount, 0);
  EXPECT_EQ(index.header().newCount, 0);
}

// Review focus 4.
TEST_F(LibraryPublishTest, ALongNonAsciiPathRoundTrips) {
  std::string path = "/B\xC3\xBC" "cher/";
  while (path.size() < 300) path += "\xC3\xA4";
  path += ".epub";
  ASSERT_TRUE(publish({book(path, "")}));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(pathOf(index, 0), path);
}

TEST_F(LibraryPublishTest, ThePartialFlagIsRecorded) {
  ASSERT_TRUE(publish({book("/a.epub", "")}, 1, nullptr, true));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_TRUE(index.partial());
}

// A failed publish -- here the arena cannot hold the sort -- leaves the previous index as it was.
TEST_F(LibraryPublishTest, AFailedPublishLeavesThePreviousIndex) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/a.epub", "Terry Pratchett")}));
  const auto before = std::filesystem::file_size(at("library.bin"));
  BuildArena tiny(16);
  EXPECT_FALSE(publish({book("/a.epub", "Terry Pratchett"), book("/b.epub", "Terry Pratchett")}, 2, &tiny));
  EXPECT_EQ(std::filesystem::file_size(at("library.bin")), before);
  EXPECT_FALSE(std::filesystem::exists(at("library.bin.tmp")));
}

TEST_F(LibraryPublishTest, TheReaderRejectsATruncatedOrForeignFile) {
  ASSERT_TRUE(publish({book("/a.epub", "")}));
  const auto size = std::filesystem::file_size(at("library.bin"));
  std::filesystem::resize_file(at("library.bin"), size - 1);
  LibraryIndexReader index;
  EXPECT_FALSE(index.open(at("library.bin")));
  ASSERT_TRUE(publish({book("/a.epub", "")}));
  {
    std::fstream f(at("library.bin"), std::ios::in | std::ios::out | std::ios::binary);
    f.put('X');
  }
  EXPECT_FALSE(index.open(at("library.bin")));
  EXPECT_FALSE(index.open(at("missing.bin")));
}

}  // namespace
```

In `test/library_index/CMakeLists.txt`, add these lines to the `add_executable` list:

```cmake
  LibraryPublishTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryBlob.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryIndexReader.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryPublish.cpp
```

- [ ] **Step 3: Run and watch it fail**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target LibraryIndexTest -j 8 2>&1 | grep -E "error|Cannot find" | head -3
```
Expected: the new library sources are missing.

- [ ] **Step 4: Blob helpers**

`lib/LibraryIndex/LibraryBlob.h`:

```cpp
#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <string>

// Exact reads and writes on the index's files, and its strings: a uint16_t length, then the bytes.
namespace library {

bool readExact(HalFile& file, void* out, size_t len);
bool writeExact(HalFile& file, const void* data, size_t len);
// False when the string exceeds MAX_STRING or the file came up short.
bool readBlobString(HalFile& file, std::string& out);
bool writeBlobString(HalFile& file, const std::string& s);

}  // namespace library
```

`lib/LibraryIndex/LibraryBlob.cpp`:

```cpp
#include "LibraryBlob.h"

#include "LibraryFormat.h"

namespace library {

bool readExact(HalFile& file, void* out, const size_t len) {
  return len == 0 || file.read(out, len) == static_cast<int>(len);
}

bool writeExact(HalFile& file, const void* data, const size_t len) {
  return len == 0 || file.write(data, len) == len;
}

bool readBlobString(HalFile& file, std::string& out) {
  uint16_t len = 0;
  if (!readExact(file, &len, sizeof(len)) || len > MAX_STRING) return false;
  out.resize(len);
  return readExact(file, len > 0 ? &out[0] : nullptr, len);
}

bool writeBlobString(HalFile& file, const std::string& s) {
  if (s.size() > MAX_STRING) return false;
  const auto len = static_cast<uint16_t>(s.size());
  return writeExact(file, &len, sizeof(len)) && writeExact(file, s.data(), s.size());
}

}  // namespace library
```

- [ ] **Step 5: The reader**

`lib/LibraryIndex/LibraryIndexReader.h`:

```cpp
#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>

#include "LibraryFormat.h"

// Reads a published book index. Holds one open file and its header; every record, name and path is
// read when asked for, so it costs the same whatever the card holds.
class LibraryIndexReader {
 public:
  LibraryIndexReader() = default;
  ~LibraryIndexReader() { close(); }
  LibraryIndexReader(const LibraryIndexReader&) = delete;
  LibraryIndexReader& operator=(const LibraryIndexReader&) = delete;

  // False, with nothing open, when the file is missing, from another version, or its sections do not
  // fit it. The caller treats all of those as "no index".
  bool open(const std::string& path);
  void close();
  bool isOpen() const { return opened; }

  const library::Header& header() const { return hdr; }
  bool partial() const { return (hdr.flags & library::FLAG_PARTIAL) != 0; }

  bool book(uint16_t index, library::BookRecord& out);
  // The record index of the rank-th newest book, 0 being the newest.
  bool newBook(uint16_t rank, uint16_t& recordIndex);
  bool author(uint16_t index, library::AuthorRecord& out);
  // The record index at one slot of the author-books table.
  bool authorBook(uint32_t slot, uint16_t& recordIndex);
  bool blobString(uint32_t offset, std::string& out);
  // An author's display name, and its sort key when `key` is given.
  bool authorName(const library::AuthorRecord& author, std::string& name, std::string* key = nullptr);

 private:
  bool readAt(uint32_t offset, void* out, size_t len);

  HalFile file;
  library::Header hdr{};
  bool attempted = false;  // an open was tried on `file`, so close() is legal on it
  bool opened = false;
};
```

`lib/LibraryIndex/LibraryIndexReader.cpp`:

```cpp
#include "LibraryIndexReader.h"

#include <cstring>

#include "LibraryBlob.h"

namespace {

bool fits(const uint32_t offset, const uint64_t length, const uint32_t fileSize) {
  return uint64_t{offset} + length <= fileSize;
}

bool sectionsFit(const library::Header& h, const uint32_t size) {
  return h.bookCount <= library::MAX_BOOKS && h.newCount <= library::NEW_COUNT && h.newCount <= h.bookCount &&
         h.authorCount <= h.bookCount && h.recordsOff >= sizeof(library::Header) &&
         fits(h.recordsOff, uint64_t{h.bookCount} * sizeof(library::BookRecord), size) &&
         fits(h.newOff, uint64_t{h.newCount} * sizeof(uint16_t), size) &&
         fits(h.authorsOff, uint64_t{h.authorCount} * sizeof(library::AuthorRecord), size) &&
         fits(h.authorBooksOff, uint64_t{h.bookCount} * sizeof(uint16_t), size) && fits(h.blobOff, h.blobLen, size);
}

}  // namespace

bool LibraryIndexReader::open(const std::string& path) {
  close();
  attempted = true;
  if (!Storage.openFileForRead("LIB", path, file)) return false;
  library::Header h{};
  const auto size = static_cast<uint32_t>(file.fileSize());
  if (!readAt(0, &h, sizeof(h)) || std::memcmp(h.magic, library::MAGIC, sizeof(h.magic)) != 0 ||
      h.version != library::VERSION || !sectionsFit(h, size)) {
    close();
    return false;
  }
  hdr = h;
  opened = true;
  return true;
}

void LibraryIndexReader::close() {
  if (attempted) file.close();
  attempted = false;
  opened = false;
  hdr = {};
}

bool LibraryIndexReader::readAt(const uint32_t offset, void* out, const size_t len) {
  return file.seek(offset) && library::readExact(file, out, len);
}

bool LibraryIndexReader::book(const uint16_t index, library::BookRecord& out) {
  if (!opened || index >= hdr.bookCount) return false;
  return readAt(hdr.recordsOff + uint32_t{index} * sizeof(library::BookRecord), &out, sizeof(out));
}

bool LibraryIndexReader::newBook(const uint16_t rank, uint16_t& recordIndex) {
  if (!opened || rank >= hdr.newCount) return false;
  return readAt(hdr.newOff + uint32_t{rank} * sizeof(uint16_t), &recordIndex, sizeof(recordIndex)) &&
         recordIndex < hdr.bookCount;
}

bool LibraryIndexReader::author(const uint16_t index, library::AuthorRecord& out) {
  if (!opened || index >= hdr.authorCount) return false;
  return readAt(hdr.authorsOff + uint32_t{index} * sizeof(library::AuthorRecord), &out, sizeof(out));
}

bool LibraryIndexReader::authorBook(const uint32_t slot, uint16_t& recordIndex) {
  if (!opened || slot >= hdr.bookCount) return false;
  return readAt(hdr.authorBooksOff + slot * sizeof(uint16_t), &recordIndex, sizeof(recordIndex)) &&
         recordIndex < hdr.bookCount;
}

bool LibraryIndexReader::blobString(const uint32_t offset, std::string& out) {
  uint16_t len = 0;
  if (!opened || uint64_t{offset} + sizeof(len) > hdr.blobLen) return false;
  if (!readAt(hdr.blobOff + offset, &len, sizeof(len))) return false;
  if (len > library::MAX_STRING || uint64_t{offset} + sizeof(len) + len > hdr.blobLen) return false;
  out.resize(len);
  return library::readExact(file, len > 0 ? &out[0] : nullptr, len);
}

bool LibraryIndexReader::authorName(const library::AuthorRecord& author, std::string& name, std::string* key) {
  if (!blobString(author.nameOff, name)) return false;
  return key == nullptr || blobString(author.nameOff + sizeof(uint16_t) + static_cast<uint32_t>(name.size()), *key);
}
```

- [ ] **Step 6: Publish**

`lib/LibraryIndex/LibraryPublish.h`:

```cpp
#pragma once

#include <BuildArena.h>

#include <cstdint>
#include <string>

// Assembles the book index from a build's working files (spec section 3.5, phase 3).
namespace LibraryPublish {

struct Input {
  std::string recordsPath;  // library::BookRecord x bookCount, identity order; pathOff into pathsPath
  std::string pathsPath;    // the books' paths, blob strings
  std::string namesPath;    // author names: uint32_t hash, uint8_t flags, name, sort key (see loadNames)
  uint16_t bookCount = 0;
  uint32_t buildGen = 0;
  uint8_t acceptRules = 0;
  bool partial = false;
};

// Writes the index to outPath + ".tmp" and renames it into place. The author table is sorted in
// `arena` (the lent framebuffer on the device), at most ~22 bytes per book. False, with the previous
// index left as it was, when the arena is too small or a read or write fails.
bool publish(const Input& in, BuildArena& arena, const std::string& outPath);

}  // namespace LibraryPublish
```

`lib/LibraryIndex/LibraryPublish.cpp`:

```cpp
#include "LibraryPublish.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "LibraryBlob.h"
#include "LibraryFormat.h"

namespace {

// Keys are compared on this many bytes in the sort; equal prefixes are then put in full-key order.
constexpr size_t KEY_PREFIX = 8;
// Longest run of equal prefixes put in full-key order; a longer one keeps its hash order.
constexpr uint16_t MAX_TIE_RUN = 32;
// Filed after every real key, as no UTF-8 starts with 0xFF: books with no author, then books whose
// author is not known yet.
constexpr char UNKNOWN_KEY[] = "\xff\xfe";
constexpr char PENDING_KEY[] = "\xff\xff";
constexpr uint32_t NO_NAME = 0xFFFFFFFFu;
// Set in a names-file offset held in Tables::nameAt when that entry's key came from a file-as.
constexpr uint32_t FILE_AS_BIT = 0x80000000u;
constexpr uint8_t ENTRY_FILE_AS = 0x01;

struct KeyPrefix {
  char bytes[KEY_PREFIX];
};

void setPrefix(KeyPrefix& prefix, const std::string& key) {
  std::memset(prefix.bytes, 0, KEY_PREFIX);
  std::memcpy(prefix.bytes, key.data(), std::min(key.size(), KEY_PREFIX));
}

// The NEW_COUNT newest books: first seen in a later build first, then the newer file date.
struct Newest {
  struct Entry {
    uint16_t index;
    uint32_t firstSeen;
    uint32_t date;
    uint32_t identity;
  };
  Entry entries[library::NEW_COUNT];
  uint16_t size = 0;

  static bool newer(const Entry& a, const Entry& b) {
    if (a.firstSeen != b.firstSeen) return a.firstSeen > b.firstSeen;
    if (a.date != b.date) return a.date > b.date;
    return a.identity < b.identity;
  }

  void offer(const uint16_t index, const library::BookRecord& record) {
    const Entry entry{index, record.firstSeen, record.date, record.identity};
    if (size == library::NEW_COUNT && !newer(entry, entries[size - 1])) return;
    uint16_t at = size < library::NEW_COUNT ? size++ : static_cast<uint16_t>(size - 1);
    while (at > 0 && newer(entry, entries[at - 1])) {
      entries[at] = entries[at - 1];
      --at;
    }
    entries[at] = entry;
  }
};

// The arrays the author table is built in, all in the arena.
struct Tables {
  uint32_t* hashes = nullptr;  // distinct author hashes, ascending
  uint16_t* counts = nullptr;  // books per author, by hash index
  uint32_t* nameAt = nullptr;  // names-file offset (| FILE_AS_BIT) per hash index; its rank once written
  uint16_t* order = nullptr;   // hash index per rank; that rank's first slot once the table is written
  uint16_t* slots = nullptr;   // the author-books table
  uint16_t authors = 0;
};

// Reads every record once: its author into `hashes`, and the newest books into `newest`.
bool scanRecords(const std::string& path, const uint16_t books, uint32_t* hashes, Newest& newest) {
  HalFile records;
  if (!Storage.openFileForRead("LIB", path, records)) return false;
  library::BookRecord record{};
  for (uint16_t i = 0; i < books; ++i) {
    if (!library::readExact(records, &record, sizeof(record))) return false;
    hashes[i] = record.authorHash;
    newest.offer(i, record);
  }
  return true;
}

// Sorts `hashes` and folds it to its distinct values, counting each one's books into `counts`.
uint16_t distinctAuthors(uint32_t* hashes, uint16_t* counts, const uint16_t books) {
  std::sort(hashes, hashes + books);
  uint16_t authors = 0;
  for (uint16_t i = 0; i < books; ++i) {
    if (authors > 0 && hashes[authors - 1] == hashes[i]) {
      ++counts[authors - 1];
    } else {
      hashes[authors] = hashes[i];
      counts[authors] = 1;
      ++authors;
    }
  }
  return authors;
}

bool readNameEntry(HalFile& names, const uint32_t at, std::string& name, std::string& key) {
  uint32_t hash = 0;
  uint8_t flags = 0;
  return names.seek(at & ~FILE_AS_BIT) && library::readExact(names, &hash, sizeof(hash)) &&
         library::readExact(names, &flags, sizeof(flags)) && library::readBlobString(names, name) &&
         library::readBlobString(names, key);
}

// Every author's names-file entry, and its key prefix: one whose key came from a file-as beats one
// that did not, otherwise the first wins. An author with none -- no author at all, not resolved yet,
// or a name that never reached the file -- gets a key that files it last. A cut-short last entry
// ends the scan; the entries before it count.
void loadNames(const std::string& path, Tables& t, KeyPrefix* prefixes) {
  for (uint16_t i = 0; i < t.authors; ++i) {
    t.nameAt[i] = NO_NAME;
    setPrefix(prefixes[i], t.hashes[i] == library::AUTHOR_PENDING ? PENDING_KEY : UNKNOWN_KEY);
  }
  HalFile names;
  if (!Storage.openFileForRead("LIB", path, names)) return;
  std::string name;
  std::string key;
  while (true) {
    const auto at = static_cast<uint32_t>(names.position());
    uint32_t hash = 0;
    uint8_t flags = 0;
    if (!library::readExact(names, &hash, sizeof(hash)) || !library::readExact(names, &flags, sizeof(flags)) ||
        !library::readBlobString(names, name) || !library::readBlobString(names, key)) {
      break;
    }
    if (hash == library::AUTHOR_UNKNOWN || hash == library::AUTHOR_PENDING) continue;
    const uint32_t* found = std::lower_bound(t.hashes, t.hashes + t.authors, hash);
    if (found == t.hashes + t.authors || *found != hash) continue;
    const auto index = static_cast<uint16_t>(found - t.hashes);
    const bool fileAs = (flags & ENTRY_FILE_AS) != 0;
    const uint32_t held = t.nameAt[index];
    if (held != NO_NAME && ((held & FILE_AS_BIT) != 0 || !fileAs)) continue;
    t.nameAt[index] = at | (fileAs ? FILE_AS_BIT : 0);
    setPrefix(prefixes[index], key);
  }
}

void sortAuthors(Tables& t, const KeyPrefix* prefixes) {
  for (uint16_t i = 0; i < t.authors; ++i) t.order[i] = i;
  std::sort(t.order, t.order + t.authors, [prefixes](const uint16_t a, const uint16_t b) {
    const int c = std::memcmp(prefixes[a].bytes, prefixes[b].bytes, KEY_PREFIX);
    return c != 0 ? c < 0 : a < b;
  });
}

// Puts runs of authors whose keys fill and share the whole prefix into full-key order. A key shorter
// than the prefix has been compared whole already.
void orderTies(Tables& t, const KeyPrefix* prefixes, const std::string& namesPath) {
  HalFile names;
  bool opened = false;
  std::vector<std::pair<std::string, uint16_t>> run;
  std::string name;
  uint16_t start = 0;
  while (start < t.authors) {
    uint16_t end = start + 1;
    while (end < t.authors &&
           std::memcmp(prefixes[t.order[start]].bytes, prefixes[t.order[end]].bytes, KEY_PREFIX) == 0) {
      ++end;
    }
    const bool fullPrefix = std::memchr(prefixes[t.order[start]].bytes, 0, KEY_PREFIX) == nullptr;
    if (end - start > 1 && end - start <= MAX_TIE_RUN && fullPrefix) {
      if (!opened) opened = Storage.openFileForRead("LIB", namesPath, names);
      run.clear();
      for (uint16_t i = start; i < end; ++i) {
        std::string key;
        const uint32_t at = t.nameAt[t.order[i]];
        if (opened && at != NO_NAME) readNameEntry(names, at, name, key);
        run.emplace_back(std::move(key), t.order[i]);
      }
      std::sort(run.begin(), run.end());
      for (uint16_t i = start; i < end; ++i) t.order[i] = run[i - start].second;
    }
    start = end;
  }
}

// Writes the author table in key order, and each author's name and key into `section`. Leaves
// `nameAt` holding every author's rank and `order` every rank's first slot, with `counts` zeroed for
// placing the books.
bool writeAuthors(HalFile& out, HalFile& section, const std::string& namesPath, const uint32_t blobBase, Tables& t,
                  uint32_t& sectionBytes) {
  HalFile names;
  const bool haveNames = Storage.openFileForRead("LIB", namesPath, names);
  std::string name;
  std::string key;
  uint16_t firstBook = 0;
  sectionBytes = 0;
  for (uint16_t rank = 0; rank < t.authors; ++rank) {
    const uint16_t index = t.order[rank];
    if (t.nameAt[index] == NO_NAME || !haveNames || !readNameEntry(names, t.nameAt[index], name, key)) {
      name.clear();
      key = t.hashes[index] == library::AUTHOR_PENDING ? PENDING_KEY : UNKNOWN_KEY;
    }
    const library::AuthorRecord record{t.hashes[index], blobBase + sectionBytes, firstBook, t.counts[index]};
    if (!library::writeExact(out, &record, sizeof(record)) || !library::writeBlobString(section, name) ||
        !library::writeBlobString(section, key)) {
      return false;
    }
    sectionBytes += static_cast<uint32_t>(2 * sizeof(uint16_t) + name.size() + key.size());
    t.nameAt[index] = rank;
    t.order[rank] = firstBook;
    firstBook = static_cast<uint16_t>(firstBook + t.counts[index]);
    t.counts[index] = 0;
  }
  return true;
}

// Puts every book into its author's slots, in record order.
bool placeBooks(const std::string& recordsPath, const uint16_t books, Tables& t) {
  HalFile records;
  if (!Storage.openFileForRead("LIB", recordsPath, records)) return false;
  library::BookRecord record{};
  for (uint16_t i = 0; i < books; ++i) {
    if (!library::readExact(records, &record, sizeof(record))) return false;
    const uint32_t* found = std::lower_bound(t.hashes, t.hashes + t.authors, record.authorHash);
    if (found == t.hashes + t.authors || *found != record.authorHash) return false;  // changed under us
    const auto index = static_cast<uint16_t>(found - t.hashes);
    t.slots[t.order[t.nameAt[index]] + t.counts[index]] = i;
    ++t.counts[index];
  }
  return true;
}

bool writeIndex(const LibraryPublish::Input& in, const std::string& tmpPath, const std::string& sectionPath,
                Tables& t, const Newest& newest) {
  HalFile paths;
  if (!Storage.openFileForRead("LIB", in.pathsPath, paths)) return false;
  const auto pathsBytes = static_cast<uint32_t>(paths.fileSize());

  library::Header header{};
  std::memcpy(header.magic, library::MAGIC, sizeof(header.magic));
  header.version = library::VERSION;
  header.flags = in.partial ? library::FLAG_PARTIAL : 0;
  header.acceptRules = in.acceptRules;
  header.buildGen = in.buildGen;
  header.bookCount = in.bookCount;
  header.authorCount = t.authors;
  header.newCount = newest.size;
  header.recordsOff = sizeof(library::Header);
  header.newOff = header.recordsOff + in.bookCount * static_cast<uint32_t>(sizeof(library::BookRecord));
  header.authorsOff = header.newOff + newest.size * static_cast<uint32_t>(sizeof(uint16_t));
  header.authorBooksOff = header.authorsOff + t.authors * static_cast<uint32_t>(sizeof(library::AuthorRecord));
  header.blobOff = header.authorBooksOff + in.bookCount * static_cast<uint32_t>(sizeof(uint16_t));

  HalFile out;
  if (!Storage.openFileForWrite("LIB", tmpPath, out)) return false;
  bool ok = library::writeExact(out, &header, sizeof(header));  // blobLen is filled in at the end
  {
    HalFile records;
    ok = ok && Storage.openFileForRead("LIB", in.recordsPath, records) &&
         serialization::copyBytes(records, out, in.bookCount * static_cast<uint32_t>(sizeof(library::BookRecord)));
  }
  for (uint16_t i = 0; ok && i < newest.size; ++i) {
    ok = library::writeExact(out, &newest.entries[i].index, sizeof(uint16_t));
  }
  uint32_t sectionBytes = 0;
  {
    HalFile section;
    ok = ok && Storage.openFileForWrite("LIB", sectionPath, section) &&
         writeAuthors(out, section, in.namesPath, pathsBytes, t, sectionBytes);
  }
  ok = ok && placeBooks(in.recordsPath, in.bookCount, t) &&
       library::writeExact(out, t.slots, in.bookCount * sizeof(uint16_t)) &&
       serialization::copyBytes(paths, out, pathsBytes);
  {
    HalFile section;
    ok = ok && Storage.openFileForRead("LIB", sectionPath, section) && serialization::copyBytes(section, out, sectionBytes);
  }
  header.blobLen = pathsBytes + sectionBytes;
  ok = ok && out.seek(0) && library::writeExact(out, &header, sizeof(header));
  out.close();
  return ok;
}

}  // namespace

namespace LibraryPublish {

bool publish(const Input& in, BuildArena& arena, const std::string& outPath) {
  const uint16_t books = in.bookCount;
  if (books > library::MAX_BOOKS || !arena.valid()) return false;
  arena.reset();
  void* newestAt = arena.alloc(sizeof(Newest), alignof(Newest));
  Tables t;
  t.hashes = arena.allocArray<uint32_t>(books);
  t.counts = arena.allocArray<uint16_t>(books);
  if (newestAt == nullptr || t.hashes == nullptr || t.counts == nullptr) {
    LOG_ERR("LIB", "publish: arena too small for %u books", static_cast<unsigned>(books));
    return false;
  }
  Newest& newest = *new (newestAt) Newest();
  if (!scanRecords(in.recordsPath, books, t.hashes, newest)) return false;
  t.authors = distinctAuthors(t.hashes, t.counts, books);

  t.nameAt = arena.allocArray<uint32_t>(t.authors);
  auto* prefixes = arena.allocArray<KeyPrefix>(t.authors);
  t.order = arena.allocArray<uint16_t>(t.authors);
  t.slots = arena.allocArray<uint16_t>(books);
  if (t.nameAt == nullptr || prefixes == nullptr || t.order == nullptr || t.slots == nullptr) {
    LOG_ERR("LIB", "publish: arena too small for %u authors", static_cast<unsigned>(t.authors));
    return false;
  }
  loadNames(in.namesPath, t, prefixes);
  sortAuthors(t, prefixes);
  orderTies(t, prefixes, in.namesPath);

  const std::string tmpPath = outPath + ".tmp";
  const std::string sectionPath = outPath + ".names";
  const bool written = writeIndex(in, tmpPath, sectionPath, t, newest);
  Storage.remove(sectionPath.c_str());
  if (!written) {
    LOG_ERR("LIB", "publish: writing %s failed", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  Storage.remove(outPath.c_str());
  return Storage.rename(tmpPath.c_str(), outPath.c_str());
}

}  // namespace LibraryPublish
```

- [ ] **Step 7: Run the tests**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target LibraryIndexTest -j 8 2>&1 | grep -E "error|warning:" | head -5; ./library_index/LibraryIndexTest.exe 2>&1 | grep -E "^\[  (FAILED|PASSED) "
```
Expected: `[  PASSED  ] 26 tests.` (10 key tests and 16 publish tests).

- [ ] **Step 8: Commit**

```bash
cd /c/_development/witchhunt-reader
git add lib/LibraryIndex/LibraryBlob.h lib/LibraryIndex/LibraryBlob.cpp lib/LibraryIndex/LibraryIndexReader.h lib/LibraryIndex/LibraryIndexReader.cpp lib/LibraryIndex/LibraryPublish.h lib/LibraryIndex/LibraryPublish.cpp test/library_index/LibraryIndexFixture.h test/library_index/LibraryPublishTest.cpp test/library_index/CMakeLists.txt
git commit -m "feat(library): index reader and publish

publish() assembles the index from a build's working files: the newest
books, the author table sorted surname-first in one pass through a
BuildArena (about 22 bytes per book), and every author's books. Written to
a temp file and renamed into place; a failure keeps the previous index."
```

---

### Task 3: Join

**Files:**
- Create: `lib/LibraryIndex/LibraryJoin.h`, `lib/LibraryIndex/LibraryJoin.cpp`, `test/library_index/LibraryJoinTest.cpp`
- Modify: `test/library_index/CMakeLists.txt`

**Interfaces:**
- Consumes: `LibraryIndexReader`, `library::readExact`/`writeExact`/`writeBlobString`, and the names-file format (Task 2).
- Produces:
  - `LibraryJoin::Input { stagePath, stageCount, previousPath, resolveAll, recordsPath, namesPath }`
  - `LibraryJoin::Result { pending, buildGen }`
  - `bool LibraryJoin::join(const Input&, BuildArena&, Result&)`

- [ ] **Step 1: Write the failing tests**

`test/library_index/LibraryJoinTest.cpp`:

```cpp
// Join merges a new walk with the previous index: books it knows keep their author and when they
// were first seen; books it does not are new (spec §3.5, phase 2).

#include <LibraryJoin.h>

#include <fstream>

#include "LibraryIndexFixture.h"

namespace {

class LibraryJoinTest : public LibraryIndexFixture {
 protected:
  library::StagedBook staged(const std::string& path, const uint32_t size = 100, const uint32_t date = 0,
                             const uint32_t sidecar = library::SIDECAR_NONE) const {
    const std::string file = path.substr(path.rfind('/') + 1);
    return {LibraryKeys::bookIdentity(file.c_str(), size), date, sidecar, appendString(at("paths.bin"), path)};
  }

  LibraryJoin::Result join(const std::vector<library::StagedBook>& books, const bool resolveAll = false) {
    writeAll(at("stage.bin"), books);
    LibraryJoin::Input in;
    in.stagePath = at("stage.bin");
    in.stageCount = static_cast<uint16_t>(books.size());
    in.previousPath = at("library.bin");
    in.resolveAll = resolveAll;
    in.recordsPath = at("records.bin");
    in.namesPath = at("names.bin");
    BuildArena framebuffer(48000);
    LibraryJoin::Result result;
    EXPECT_TRUE(LibraryJoin::join(in, framebuffer, result));
    return result;
  }

  std::vector<library::BookRecord> joined() const {
    std::ifstream in(at("records.bin"), std::ios::binary);
    std::vector<library::BookRecord> records;
    library::BookRecord record{};
    while (in.read(reinterpret_cast<char*>(&record), sizeof(record))) records.push_back(record);
    return records;
  }

  // Publishes what the join left, as the builder does before resolving anything.
  bool publishJoined(const LibraryJoin::Result& result) const {
    LibraryPublish::Input in;
    in.recordsPath = at("records.bin");
    in.pathsPath = at("paths.bin");
    in.namesPath = at("names.bin");
    in.bookCount = static_cast<uint16_t>(joined().size());
    in.buildGen = result.buildGen;
    BuildArena framebuffer(48000);
    return LibraryPublish::publish(in, framebuffer, at("library.bin"));
  }
};

TEST_F(LibraryJoinTest, AFirstBuildLeavesEveryBookPendingAndNew) {
  const auto result = join({staged("/c.epub"), staged("/a.epub"), staged("/b.epub")});
  EXPECT_EQ(result.pending, 3);
  EXPECT_EQ(result.buildGen, 1u);
  const auto records = joined();
  ASSERT_EQ(records.size(), 3u);
  for (size_t i = 0; i < records.size(); ++i) {
    EXPECT_EQ(records[i].authorHash, library::AUTHOR_PENDING);
    EXPECT_EQ(records[i].firstSeen, 1u);
    if (i > 0) EXPECT_LT(records[i - 1].identity, records[i].identity) << "records must come out in identity order";
  }
}

TEST_F(LibraryJoinTest, AuthorsAndFirstSeenAreCarriedByIdentity) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Disc/Mort.epub", "Terry Pratchett", 0, 4)}, 4));
  const auto result = join({staged("/Disc/Mort.epub")});
  EXPECT_EQ(result.pending, 0);
  EXPECT_EQ(result.buildGen, 4u) << "nothing new: the generation stays";
  const auto records = joined();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].authorHash, LibraryKeys::authorHash("Terry Pratchett"));
  EXPECT_EQ(records[0].firstSeen, 4u);
}

TEST_F(LibraryJoinTest, AMovedBookKeepsItsAuthorAndFirstSeen) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Inbox/Mort.epub", "Terry Pratchett", 0, 2)}, 2));
  const auto result = join({staged("/Discworld/Mort.epub")});
  EXPECT_EQ(result.pending, 0);
  EXPECT_EQ(joined()[0].firstSeen, 2u);
}

TEST_F(LibraryJoinTest, ANewBookTakesTheNextGenerationAndIsNewest) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Mort.epub", "Terry Pratchett", 5000, 1)}, 1));
  const auto result = join({staged("/Mort.epub", 100, 5000), staged("/Eric.epub", 100, 10)});
  EXPECT_EQ(result.buildGen, 2u);
  EXPECT_EQ(result.pending, 1);
  ASSERT_TRUE(publishJoined(result));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  uint16_t newest = 0;
  ASSERT_TRUE(index.newBook(0, newest));
  EXPECT_EQ(pathOf(index, newest), "/Eric.epub") << "newly seen beats a newer file date";
}

TEST_F(LibraryJoinTest, AChangedSidecarResolvesAgainButKeepsFirstSeen) {
  addName("Terry Pratchett");
  auto mort = book("/Mort.epub", "Terry Pratchett", 0, 3);
  mort.sidecarSig = 5;
  ASSERT_TRUE(publish({mort}, 3));
  const auto result = join({staged("/Mort.epub", 100, 0, 6)});
  EXPECT_EQ(result.pending, 1);
  EXPECT_EQ(joined()[0].firstSeen, 3u);
}

TEST_F(LibraryJoinTest, AnUnpairedSidecarAlwaysResolvesAgain) {
  addName("Terry Pratchett");
  auto mort = book("/Mort.epub", "Terry Pratchett");
  mort.sidecarSig = library::SIDECAR_UNKNOWN;
  ASSERT_TRUE(publish({mort}));
  EXPECT_EQ(join({staged("/Mort.epub", 100, 0, library::SIDECAR_UNKNOWN)}).pending, 1);
}

// Refresh library: every author is resolved again, but New must not reshuffle.
TEST_F(LibraryJoinTest, ResolveAllCarriesNoAuthorButKeepsFirstSeen) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Mort.epub", "Terry Pratchett", 0, 2)}, 2));
  const auto result = join({staged("/Mort.epub")}, true);
  EXPECT_EQ(result.pending, 1);
  EXPECT_EQ(joined()[0].firstSeen, 2u);
}

TEST_F(LibraryJoinTest, ThePreviousAuthorsNamesAreCarried) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Mort.epub", "Terry Pratchett")}));
  std::filesystem::remove(at("names.bin"));  // the join must write it afresh from the index
  const auto result = join({staged("/Mort.epub")});
  ASSERT_TRUE(publishJoined(result));
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(at("library.bin")));
  EXPECT_EQ(authorNames(index), (std::vector<std::string>{"Terry Pratchett"}));
}

TEST_F(LibraryJoinTest, TwoCopiesOfABookBothKeepItsAuthor) {
  addName("Terry Pratchett");
  ASSERT_TRUE(publish({book("/Mort.epub", "Terry Pratchett")}));
  const auto result = join({staged("/A/Mort.epub"), staged("/B/Mort.epub")});
  EXPECT_EQ(result.pending, 0);
  EXPECT_EQ(joined().size(), 2u);
}

// Review focus 3.
TEST_F(LibraryJoinTest, AnEmptyCardJoinsToNothing) {
  const auto result = join({});
  EXPECT_EQ(result.pending, 0);
  EXPECT_TRUE(joined().empty());
  EXPECT_TRUE(publishJoined(result));
}

}  // namespace
```

In `test/library_index/CMakeLists.txt`, add to the `add_executable` list:

```cmake
  LibraryJoinTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryJoin.cpp
```

- [ ] **Step 2: Run and watch it fail**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target LibraryIndexTest -j 8 2>&1 | grep -E "error|Cannot find" | head -3
```
Expected: `LibraryJoin.cpp` / `LibraryJoin.h` are missing.

- [ ] **Step 3: Implement join**

`lib/LibraryIndex/LibraryJoin.h`:

```cpp
#pragma once

#include <BuildArena.h>

#include <cstdint>
#include <string>

// Merges a new walk with the previous index (spec section 3.5, phase 2).
namespace LibraryJoin {

struct Input {
  std::string stagePath;     // library::StagedBook x stageCount, in walk order
  uint16_t stageCount = 0;
  std::string previousPath;  // the published index to carry from; missing or invalid = a first build
  bool resolveAll = false;   // Refresh library: carry no author, but keep firstSeen
  std::string recordsPath;   // out: library::BookRecord x stageCount, identity order
  std::string namesPath;     // out: the previous index's authors, so their names are not looked up again
};

struct Result {
  uint16_t pending = 0;   // books whose author has to be resolved
  uint32_t buildGen = 0;  // this build's generation: one up when any book is new
};

// The staged books are sorted by identity in `arena` (16 bytes each) and merged with the previous
// index's records, which are already in identity order, in one sequential pass. A book the index
// knows keeps when it was first seen, and its author too unless its metadata sidecar changed; a new
// book is first seen in this build. False when the arena is too small or a file fails.
bool join(const Input& in, BuildArena& arena, Result& out);

}  // namespace LibraryJoin
```

`lib/LibraryIndex/LibraryJoin.cpp`:

```cpp
#include "LibraryJoin.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <string>

#include "LibraryBlob.h"
#include "LibraryFormat.h"
#include "LibraryIndexReader.h"
#include "LibraryKeys.h"

namespace {

bool readStage(const std::string& path, library::StagedBook* staged, const uint16_t count) {
  if (count == 0) return true;
  HalFile stage;
  return Storage.openFileForRead("LIB", path, stage) &&
         library::readExact(stage, staged, count * sizeof(library::StagedBook));
}

// The previous index's named authors, in the names-file format, so publish finds their names without
// resolving any of their books again.
bool carryNames(LibraryIndexReader& previous, const bool hasPrevious, const std::string& namesPath) {
  HalFile names;
  if (!Storage.openFileForWrite("LIB", namesPath, names)) return false;
  if (!hasPrevious) return true;
  library::AuthorRecord author{};
  std::string name;
  std::string key;
  for (uint16_t i = 0; i < previous.header().authorCount; ++i) {
    if (!previous.author(i, author) || !previous.authorName(author, name, &key)) return false;
    if (author.hash == library::AUTHOR_UNKNOWN || author.hash == library::AUTHOR_PENDING) continue;
    // The index keeps no flag: a key that is not what the name alone gives came from a file-as.
    const uint8_t flags = key != LibraryKeys::authorSortKey(name, "") ? 1 : 0;
    if (!library::writeExact(names, &author.hash, sizeof(author.hash)) ||
        !library::writeExact(names, &flags, sizeof(flags)) || !library::writeBlobString(names, name) ||
        !library::writeBlobString(names, key)) {
      return false;
    }
  }
  return true;
}

}  // namespace

namespace LibraryJoin {

bool join(const Input& in, BuildArena& arena, Result& out) {
  out = {};
  const uint16_t count = in.stageCount;
  if (count > library::MAX_BOOKS || !arena.valid()) return false;
  arena.reset();
  auto* staged = arena.allocArray<library::StagedBook>(count);
  if (staged == nullptr) {
    LOG_ERR("LIB", "join: arena too small for %u books", static_cast<unsigned>(count));
    return false;
  }
  if (!readStage(in.stagePath, staged, count)) return false;
  std::sort(staged, staged + count, [](const library::StagedBook& a, const library::StagedBook& b) {
    return a.identity != b.identity ? a.identity < b.identity : a.pathOff < b.pathOff;
  });

  LibraryIndexReader previous;
  const bool hasPrevious = previous.open(in.previousPath);
  const uint32_t previousGen = hasPrevious ? previous.header().buildGen : 0;
  if (!carryNames(previous, hasPrevious, in.namesPath)) return false;

  HalFile records;
  if (!Storage.openFileForWrite("LIB", in.recordsPath, records)) return false;
  const uint32_t newGen = previousGen + 1;
  const uint16_t previousCount = hasPrevious ? previous.header().bookCount : 0;
  uint16_t at = 0;
  library::BookRecord known{};
  bool haveKnown = previousCount > 0 && previous.book(0, known);
  bool anyNew = false;
  for (uint16_t i = 0; i < count; ++i) {
    const library::StagedBook& book = staged[i];
    while (haveKnown && known.identity < book.identity) {
      ++at;
      haveKnown = at < previousCount && previous.book(at, known);
    }
    library::BookRecord record{book.identity, library::AUTHOR_PENDING, book.date, book.pathOff, book.sidecarSig, newGen};
    if (haveKnown && known.identity == book.identity) {
      record.firstSeen = known.firstSeen;
      const bool sameSidecar = known.sidecarSig == book.sidecarSig && book.sidecarSig != library::SIDECAR_UNKNOWN;
      if (!in.resolveAll && sameSidecar) record.authorHash = known.authorHash;
    } else {
      anyNew = true;
    }
    if (record.authorHash == library::AUTHOR_PENDING) ++out.pending;
    if (!library::writeExact(records, &record, sizeof(record))) return false;
  }
  out.buildGen = (anyNew || !hasPrevious) ? newGen : previousGen;
  return true;
}

}  // namespace LibraryJoin
```

- [ ] **Step 4: Run the tests**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target LibraryIndexTest -j 8 2>&1 | grep -E "error|warning:" | head -5; ./library_index/LibraryIndexTest.exe 2>&1 | grep -E "^\[  (FAILED|PASSED) "
```
Expected: `[  PASSED  ] 36 tests.`

- [ ] **Step 5: Commit**

```bash
cd /c/_development/witchhunt-reader
git add lib/LibraryIndex/LibraryJoin.h lib/LibraryIndex/LibraryJoin.cpp test/library_index/LibraryJoinTest.cpp test/library_index/CMakeLists.txt
git commit -m "feat(library): join a new walk with the previous index

Sorts the staged books by identity and merges them with the previous
index in one pass: known books keep firstSeen, and their author unless
the sidecar changed or Refresh asked for everything again; new books take
the next generation. The previous authors' names are carried over."
```

---

### Task 4: Verification

**Files:** only those clang-format touches.

- [ ] **Step 1: Format**

```bash
cd /c/_development/witchhunt-reader && for f in $(git diff --name-only feat/library-metadata...HEAD -- '*.cpp' '*.h'); do "/c/tmp/cf21/bin/clang-format-21.exe" -i --style=file "$f"; done; git status --short | grep -v "^??"
```
Commit any changes as `style: clang-format 21`, adding the files explicitly.

- [ ] **Step 2: Full host suite**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . -j 8 -- -k 0 2>&1 | grep -E "error:" ; ctest -j 8 2>&1 | grep -E "tests passed|tests failed"
```
Expected: `100% tests passed out of 1823` (1787 plus 36). The only build error is `dlfcn.h` in `epub_build_inventory`.

- [ ] **Step 3: Hand back**

Report the counts. The firmware does not link this library until Plan 2b, so no `pio run` is needed here, and there is no flash number yet. The cost gate is measured at the end of Plan 2b.
