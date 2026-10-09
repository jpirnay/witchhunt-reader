# Library Plan 1: Primary Author, file-as and Sidecars for Every Format

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every book's details record carries the author the Library will group by (`primaryAuthor`) and how that author files (`authorSort`, from `opf:file-as`). The `.opf` sidecar labels TXT, Markdown and XTC books as well as EPUBs.

**Architecture:**
- `ContentOpfParser` records each `dc:creator` with its role and filing name (EPUB 2 `opf:` attributes and EPUB 3 `<meta refines>`), and picks the primary author when the `<metadata>` block ends.
- The sidecar overlay moves out of `Epub` into a free function, `MetadataSidecar::read()`, that works for any book.
- `BookDetailsLookup` uses it for every format, reads XTC headers, and always parses an EPUB's OPF, never `book.bin`, which keeps no primary author.
- `details.bin` goes to version 2.

**Tech Stack:** C++20, ESP32-C3 Arduino/PlatformIO firmware; host tests with GoogleTest via CMake (MSYS2 UCRT64 toolchain).

**Spec:** `docs/superpowers/specs/2026-10-09-library-view-design.md`. This plan is delivery step 1 of §7 and implements §3.4.

## Global Constraints

- RAM is the hard ceiling. No new resident allocations. Locals under 256 B. Heap growth that can fail uses `makeUniqueNoThrow` (the device builds with `-fno-exceptions`).
- **`book.bin` stays unchanged:** do not add fields to its serialization and do not bump `BookMetadataCache`'s version.
- `details.bin` `VERSION` goes from 1 to 2.
- The display `author` keeps its meaning: every `dc:creator`, joined with `", "`.
- A **primary author** is the first `dc:creator` whose role is `aut` (case-insensitive) or which has no role at all.
- Sidecars: a non-empty sidecar field wins and an empty one never blanks the book's own value. The cap is 16384 B (`Epub::MAX_METADATA_SIDECAR_BYTES`).
- **Build host tests from Git Bash only, never PowerShell:** `cd build/test && cmake --build . -j 8 -- -k 0`. `epub_build_inventory` fails on Windows by design; `-k 0` keeps going past it.
- **Format with clang-format 21, never 22:** `PATH="/c/tmp/cf21/bin:$PATH" ./bin/clang-format-fix -g`.
- **Never `git add -A`:** the `freeink-sdk` submodule shows as modified and must not be staged. Add paths explicitly.
- Firmware: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e <env>`, one `pio run` at a time, and in the background (a cold build takes more than 600 s).
- Comments match the codebase: they say why, in full sentences, and cite symbols, not line numbers.

## Review Focus

1. **A creator written "Surname, First" with no `file-as`** (`<dc:creator>Le Guin, Ursula K.</dc:creator>`) must stay one name: `primaryAuthor == "Le Guin, Ursula K."`. Test in Task 1.
2. **More than four creators.** The display author must still list all of them; only the primary-author search is bounded. Test in Task 1.
3. **A whitespace-only creator** (`<dc:creator> </dc:creator>`) must not leave a stray `", "` in the display author or become the primary author. Test in Task 1.
4. **A malformed, empty or oversized sidecar beside a TXT book** must leave the book titled by its filename, recorded once, not re-parsed on every visit. Test in Task 2 (the sidecar reader returns no fields) plus the existing `EmptyRecordOfAFailedParseIsStillAnAnswer` (an empty record is still an answer).
5. **A `<meta refines>` pointing at an id that is not a creator** (a collection's `group-position`, a title's `file-as`, a dangling `#nobody`) must not touch any creator and must still set the series index. Test in Task 1.

---

### Task 0: Branch and baseline

**Files:** none changed.

- [ ] **Step 1: Branch from master**

```bash
cd /c/_development/witchhunt-reader
git checkout master && git pull --ff-only
git checkout -b feat/library-metadata
```

- [ ] **Step 2: Commit the spec and this plan on the branch**

```bash
git add docs/superpowers/specs/2026-10-09-library-view-design.md docs/superpowers/plans/2026-10-09-library-plan-1-metadata.md
git commit -m "docs: library view spec and plan 1 (metadata)"
```

- [ ] **Step 3: Record the baseline sizes**

Run in the background: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default`.
Expected: SUCCESS. Copy the `RAM:` and `Flash:` lines into the PR notes as the baseline.

- [ ] **Step 4: Record the baseline host suite**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . -j 8 -- -k 0; ctest -j 8 2>&1 | tail -3
```

Expected: `100% tests passed` (only `epub_build_inventory` NOT_BUILT). Note the test count.

---

### Task 1: `ContentOpfParser` records creators, picks the primary author and its file-as

**Files:**
- Modify: `lib/Epub/Epub/parsers/ContentOpfParser.h`
- Modify: `lib/Epub/Epub/parsers/ContentOpfParser.cpp` (the anonymous namespace ending `}  // namespace` near `trim()`; `startElement`, `characterData`, `endElement`)
- Test: `test/epub_opf/ContentOpfParserTest.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: public `std::string ContentOpfParser::primaryAuthor` and `std::string ContentOpfParser::authorSort`, filled when `</metadata>` closes. `author` keeps its meaning (every creator joined with `", "`) but is now built from whole, trimmed creator texts.

- [ ] **Step 1: Write the failing tests**

Append to `test/epub_opf/ContentOpfParserTest.cpp`, before the final closing of the file. Also add one assertion to the existing `ExtractsMetadataManifestAndGuideFields` test, after `EXPECT_EQ(parser.author, "Author One, Author Two");`:

```cpp
  EXPECT_EQ(parser.primaryAuthor, "Author One");
```

New tests, appended at the end of the file:

```cpp
namespace {

// What the creator handling produced for one <metadata> block.
struct CreatorResult {
  bool parsed = false;
  std::string author;
  std::string primaryAuthor;
  std::string authorSort;
  std::string series;
  std::string seriesIndex;
};

// Parses an OPF whose <metadata> holds `metadata`, fed `chunk` bytes per write() (0 = all at once),
// so a test can make the parser receive one creator's text in many pieces.
CreatorResult parseCreators(const std::string& metadata, const size_t chunk = 0) {
  CreatorResult result;
  const std::string cacheDir = makeTempDir();
  if (cacheDir.empty()) return result;
  TempDirGuard dirGuard(cacheDir);
  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata>" +
      metadata +
      "</metadata>"
      "<manifest><item id='ncx' href='toc.ncx' media-type='application/x-dtbncx+xml'/></manifest>"
      "<spine/>"
      "</package>";
  ContentOpfParser parser(cacheDir, base, xml.size(), nullptr);
  if (!parser.setup()) return result;
  const auto* data = reinterpret_cast<const uint8_t*>(xml.data());
  const size_t step = chunk == 0 ? xml.size() : chunk;
  for (size_t at = 0; at < xml.size(); at += step) {
    const size_t n = std::min(step, xml.size() - at);
    if (parser.write(data + at, n) != n) return result;
  }
  result.parsed = true;
  result.author = parser.author;
  result.primaryAuthor = parser.primaryAuthor;
  result.authorSort = parser.authorSort;
  result.series = parser.series;
  result.seriesIndex = parser.seriesIndex;
  return result;
}

}  // namespace

// The display line lists every creator; the Library groups by the first one credited as author.
TEST(ContentOpfParserCreators, JoinsEveryCreatorAndPicksTheFirstAuthorAsPrimary) {
  const auto r = parseCreators(
      "<dc:creator opf:role='trl'>Anthea Bell</dc:creator>"
      "<dc:creator opf:role='aut' opf:file-as='Pratchett, Terry'>Terry Pratchett</dc:creator>"
      "<dc:creator opf:role='aut'>Neil Gaiman</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Anthea Bell, Terry Pratchett, Neil Gaiman");
  EXPECT_EQ(r.primaryAuthor, "Terry Pratchett");
  EXPECT_EQ(r.authorSort, "Pratchett, Terry");
}

TEST(ContentOpfParserCreators, ACreatorWithNoRoleIsAnAuthor) {
  const auto r = parseCreators("<dc:creator>\n  Ursula K. Le Guin \n</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Ursula K. Le Guin");
  EXPECT_EQ(r.primaryAuthor, "Ursula K. Le Guin");
  EXPECT_EQ(r.authorSort, "");
}

TEST(ContentOpfParserCreators, TheRoleCodeIsMatchedWhateverItsCase) {
  const auto r = parseCreators("<dc:creator opf:role='edt'>Ed Itor</dc:creator><dc:creator opf:role='AUT'>Au Thor</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.primaryAuthor, "Au Thor");
}

TEST(ContentOpfParserCreators, NoCreatorCreditedAsAuthorLeavesNoPrimaryAuthor) {
  const auto r = parseCreators("<dc:creator opf:role='edt'>Ed Itor</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Ed Itor");
  EXPECT_EQ(r.primaryAuthor, "");
  EXPECT_EQ(r.authorSort, "");
}

// EPUB 3 states a creator's role and filing name in <meta refines="#id">, after the creator.
TEST(ContentOpfParserCreators, Epub3RefinementsGiveRoleAndFileAs) {
  const auto r = parseCreators(
      "<dc:creator id='ill'>Pauline Baynes</dc:creator>"
      "<dc:creator id='c2'>Ursula K. Le Guin</dc:creator>"
      "<meta refines='#ill' property='role' scheme='marc:relators'>ill</meta>"
      "<meta refines='#c2' property='role' scheme='marc:relators'>aut</meta>"
      "<meta refines='#c2' property='file-as'> Le Guin, Ursula K. </meta>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Pauline Baynes, Ursula K. Le Guin");
  EXPECT_EQ(r.primaryAuthor, "Ursula K. Le Guin");
  EXPECT_EQ(r.authorSort, "Le Guin, Ursula K.");
}

// Review focus 5: a refinement of something that is not a creator is not ours to take.
TEST(ContentOpfParserCreators, RefinementsOfOtherThingsLeaveCreatorsAlone) {
  const auto r = parseCreators(
      "<dc:title id='t'>Tehanu</dc:title>"
      "<dc:creator id='c1'>Ursula K. Le Guin</dc:creator>"
      "<meta refines='#t' property='file-as'>Tehanu, The Last Book</meta>"
      "<meta refines='#nobody' property='role'>edt</meta>"
      "<meta property='belongs-to-collection' id='coll'>Earthsea</meta>"
      "<meta refines='#coll' property='group-position'>4</meta>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.primaryAuthor, "Ursula K. Le Guin");
  EXPECT_EQ(r.authorSort, "");
  EXPECT_EQ(r.series, "Earthsea");
  EXPECT_EQ(r.seriesIndex, "4");
}

// One creator's text may reach the parser in several pieces (a write boundary, an entity). The
// separator belongs between creators, never inside one.
TEST(ContentOpfParserCreators, ACreatorFedOneByteAtATimeStaysOneName) {
  const auto r = parseCreators("<dc:creator>Laurel &amp; Hardy</dc:creator><dc:creator>Second</dc:creator>", 1);
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Laurel & Hardy, Second");
  EXPECT_EQ(r.primaryAuthor, "Laurel & Hardy");
}

// Review focus 1: a creator written surname-first is still one creator.
TEST(ContentOpfParserCreators, ASurnameFirstCreatorStaysOneName) {
  const auto r = parseCreators("<dc:creator>Le Guin, Ursula K.</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Le Guin, Ursula K.");
  EXPECT_EQ(r.primaryAuthor, "Le Guin, Ursula K.");
}

// Review focus 2: only the first MAX_CREATORS (4) are candidates for primary author, but the
// display line keeps them all.
TEST(ContentOpfParserCreators, ManyCreatorsAllReachTheDisplayLine) {
  const auto r = parseCreators(
      "<dc:creator opf:role='ill'>I1</dc:creator><dc:creator opf:role='ill'>I2</dc:creator>"
      "<dc:creator opf:role='ill'>I3</dc:creator><dc:creator opf:role='ill'>I4</dc:creator>"
      "<dc:creator opf:role='aut'>A5</dc:creator><dc:creator>A6</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "I1, I2, I3, I4, A5, A6");
  EXPECT_EQ(r.primaryAuthor, "");
}

// Review focus 3: an empty creator adds nothing, not even a separator.
TEST(ContentOpfParserCreators, AWhitespaceOnlyCreatorIsSkipped) {
  const auto r = parseCreators("<dc:creator> </dc:creator><dc:creator>Real Name</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Real Name");
  EXPECT_EQ(r.primaryAuthor, "Real Name");
}
```

- [ ] **Step 2: Run them and watch them fail**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target ContentOpfParserTest -j 8
```

Expected: a compile error, `'class ContentOpfParser' has no member named 'primaryAuthor'`.

- [ ] **Step 3: Declare the creator state in the header**

In `lib/Epub/Epub/parsers/ContentOpfParser.h`:

1. Add `IN_CREATOR_REFINE,` to `enum ParserState`, right after `IN_BOOK_SERIES_INDEX,`.
2. In the private section, right after `static constexpr uint16_t LARGE_SPINE_THRESHOLD = 400;`, add:

```cpp
  // One dc:creator as the <metadata> block describes it. Kept only until the block ends, to pick
  // the primary author then: EPUB 3 states a creator's role and filing name in <meta refines="#id">
  // elements that come after it.
  struct Creator {
    std::string id;
    std::string name;
    std::string role;
    std::string fileAs;
  };
  // The first few creators are enough: books credit their authors first. Allocated from the heap,
  // and only once a dc:creator appears, because the parser itself lives on the caller's stack.
  static constexpr uint8_t MAX_CREATORS = 4;
  // Longest creator name, id, role or filing name kept.
  static constexpr size_t MAX_CREATOR_FIELD = 256;
  std::unique_ptr<Creator[]> creators_;
  uint8_t creatorCount_ = 0;
  Creator* currentCreator_ = nullptr;    // the dc:creator being read; null past MAX_CREATORS or on OOM
  std::string creatorText_;              // its text, gathered across however many pieces it arrives in
  std::string* refineTarget_ = nullptr;  // the creator field a <meta refines> element's text fills

  Creator* beginCreator(const char** atts);
  std::string* creatorField(const char* id, const char* property);
  void pickPrimaryAuthor();
```

3. In the public section, replace `  std::string author;` with:

```cpp
  // Every dc:creator's text, joined with ", ": what the book shows as its author.
  std::string author;
  // The first dc:creator credited as the book's author (role "aut", or no role at all), and how it
  // files (opf:file-as, or EPUB 3 <meta refines property="file-as">). For grouping and ordering by
  // author; `author` stays the display line. Empty when no creator qualifies.
  std::string primaryAuthor;
  std::string authorSort;
```

- [ ] **Step 4: Implement the helpers**

In `lib/Epub/Epub/parsers/ContentOpfParser.cpp`:

- Add `#include <cctype>` with the other standard includes.
- Inside the anonymous namespace, just before the `}  // namespace` that follows `trim()`, add:

```cpp
// Appends no further than `cap` bytes in total: a creator field is a name, not a document.
void appendCapped(std::string& out, const char* s, const size_t len, const size_t cap) {
  if (out.size() >= cap) return;
  out.append(s, std::min(len, cap - out.size()));
}

// No role means the creator is the author; MARC relator codes are lowercase by definition, but
// generators are not always careful.
bool isAuthorRole(const std::string& role) {
  const std::string r = trim(role);
  return r.empty() || (r.size() == 3 && std::tolower(static_cast<unsigned char>(r[0])) == 'a' &&
                       std::tolower(static_cast<unsigned char>(r[1])) == 'u' &&
                       std::tolower(static_cast<unsigned char>(r[2])) == 't');
}
```

After the anonymous namespace (anywhere before `startElement`), add the member functions:

```cpp
ContentOpfParser::Creator* ContentOpfParser::beginCreator(const char** atts) {
  if (creatorCount_ >= MAX_CREATORS) return nullptr;
  if (!creators_) {
    creators_ = makeUniqueNoThrow<Creator[]>(MAX_CREATORS);
    // Out of memory: the display author still works, there is just no primary author.
    if (!creators_) return nullptr;
  }
  Creator& creator = creators_[creatorCount_++];
  for (int i = 0; atts[i]; i += 2) {
    const char* value = atts[i + 1];
    if (strcmp(atts[i], "id") == 0) {
      appendCapped(creator.id, value, strlen(value), MAX_CREATOR_FIELD);
    } else if (strcmp(atts[i], "opf:role") == 0) {
      appendCapped(creator.role, value, strlen(value), MAX_CREATOR_FIELD);
    } else if (strcmp(atts[i], "opf:file-as") == 0) {
      appendCapped(creator.fileAs, value, strlen(value), MAX_CREATOR_FIELD);
    }
  }
  return &creator;
}

// The field a <meta refines="#id" property="..."> sets, or null when `id` is not a creator's or the
// property is not one this parser keeps.
std::string* ContentOpfParser::creatorField(const char* id, const char* property) {
  const bool role = strcmp(property, "role") == 0;
  if (!role && strcmp(property, "file-as") != 0) return nullptr;
  for (uint8_t i = 0; i < creatorCount_; ++i) {
    Creator& creator = creators_[i];
    if (!creator.id.empty() && creator.id == id) return role ? &creator.role : &creator.fileAs;
  }
  return nullptr;
}

void ContentOpfParser::pickPrimaryAuthor() {
  for (uint8_t i = 0; i < creatorCount_; ++i) {
    const Creator& creator = creators_[i];
    if (creator.name.empty() || !isAuthorRole(creator.role)) continue;
    primaryAuthor = creator.name;
    authorSort = trim(creator.fileAs);
    return;
  }
}
```

- [ ] **Step 5: Route the element callbacks through them**

In `startElement`, replace the `dc:creator` branch with:

```cpp
  if (self->state == IN_METADATA && strcmp(name, "dc:creator") == 0) {
    self->state = IN_BOOK_AUTHOR;
    self->creatorText_.clear();
    self->currentCreator_ = self->beginCreator(atts);
    return;
  }
```

In the `isMetaTag(name)` branch:
- declare `const char* metaRefines = nullptr;` next to `metaProperty`;
- add `} else if (strcmp(atts[i], "refines") == 0) { metaRefines = atts[i + 1];` to the attribute loop;
- insert the following directly after the loop, before `if (metaName && metaContent) {`:

```cpp
    // EPUB 3 creator refinements: <meta refines="#c1" property="role">aut</meta> and
    // property="file-as". Only for an id that names a creator; a refinement of anything else (a
    // collection's group-position, a title's file-as) falls through to the handling below.
    if (metaProperty && metaRefines && metaRefines[0] == '#') {
      if (std::string* field = self->creatorField(metaRefines + 1, metaProperty)) {
        field->clear();
        if (metaContent) {
          appendCapped(*field, metaContent, strlen(metaContent), MAX_CREATOR_FIELD);
        } else {
          self->refineTarget_ = field;
          self->state = IN_CREATOR_REFINE;
        }
        return;
      }
    }
```

In `characterData`, replace the `IN_BOOK_AUTHOR` branch with:

```cpp
  if (self->state == IN_BOOK_AUTHOR) {
    appendCapped(self->creatorText_, s, static_cast<size_t>(len), MAX_CREATOR_FIELD);
    return;
  }

  if (self->state == IN_CREATOR_REFINE) {
    if (self->refineTarget_) appendCapped(*self->refineTarget_, s, static_cast<size_t>(len), MAX_CREATOR_FIELD);
    return;
  }
```

In `endElement`, replace the `IN_BOOK_AUTHOR` branch with the following, and add the `IN_CREATOR_REFINE` branch after it:

```cpp
  if (self->state == IN_BOOK_AUTHOR && strcmp(name, "dc:creator") == 0) {
    // Joined here, once per creator, so a name that arrived in pieces is still one name.
    std::string text = trim(self->creatorText_);
    self->creatorText_.clear();
    if (!text.empty()) {
      if (!self->author.empty()) self->author.append(", ");
      self->author.append(text);
    }
    if (self->currentCreator_) self->currentCreator_->name = std::move(text);
    self->currentCreator_ = nullptr;
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_CREATOR_REFINE && isMetaTag(name)) {
    if (self->refineTarget_) *self->refineTarget_ = trim(*self->refineTarget_);
    self->refineTarget_ = nullptr;
    self->state = IN_METADATA;
    return;
  }
```

In `endElement`, replace the `IN_METADATA && isMetadataTag(name)` branch with:

```cpp
  if (self->state == IN_METADATA && isMetadataTag(name)) {
    self->pickPrimaryAuthor();
    self->state = IN_PACKAGE;
    return;
  }
```

- [ ] **Step 6: Run the parser tests**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target ContentOpfParserTest -j 8 && ./epub_opf/ContentOpfParserTest.exe
```

Expected: every test passes, including the 10 `ContentOpfParserCreators.*` tests and the old `ContentOpfParser.*` tests.

- [ ] **Step 7: Commit**

```bash
cd /c/_development/witchhunt-reader
git add lib/Epub/Epub/parsers/ContentOpfParser.h lib/Epub/Epub/parsers/ContentOpfParser.cpp test/epub_opf/ContentOpfParserTest.cpp
git commit -m "feat(opf): record the primary author and its file-as

The display author stays every dc:creator joined with \", \", now built per
element so a name delivered in pieces is no longer split by a separator.
Alongside it the parser keeps the first creator credited as author
(opf:role or EPUB 3 refines role \"aut\", or no role) and its filing name
(opf:file-as or refines file-as), for the Library's author grouping."
```

---

### Task 2: `MetadataSidecar` for any book; `Epub` carries the primary author

**Files:**
- Create: `lib/Epub/Epub/MetadataSidecar.h`, `lib/Epub/Epub/MetadataSidecar.cpp`
- Modify: `lib/Epub/Epub/BookMetadataCache.h` (`struct BookMetadata`)
- Modify: `lib/Epub/Epub.h` (`loadForMetadata` declaration, getters)
- Modify: `lib/Epub/Epub.cpp` (`parseContentOpf`, `applyMetadataSidecar`, `loadForMetadata`, getters)
- Modify: `test/epub_opf/CMakeLists.txt`, `test/epub_pipeline/CMakeLists.txt`
- Create: `test/epub_opf/MetadataSidecarReadTest.cpp`
- Modify: `test/epub_pipeline/MetadataSidecarTest.cpp`

**Interfaces:**
- Consumes: `ContentOpfParser::primaryAuthor`, `ContentOpfParser::authorSort` (Task 1).
- Produces:
  - `struct MetadataSidecarFields { title, author, primaryAuthor, authorSort, language, series, seriesIndex, description; }` (all `std::string`)
  - `bool MetadataSidecar::read(const std::string& bookPath, MetadataSidecarFields& out)`
  - `void MetadataSidecar::overlayPrimaryAuthor(const MetadataSidecarFields& sidecar, std::string& primaryAuthor, std::string& authorSort)`
  - `const std::string& Epub::getPrimaryAuthor() const`, `const std::string& Epub::getAuthorSort() const`
  - `bool Epub::loadForMetadata(BuildArena* scratch = nullptr, bool useBookBin = true)`

- [ ] **Step 1: Write the failing sidecar reader tests**

Create `test/epub_opf/MetadataSidecarReadTest.cpp`:

```cpp
// MetadataSidecar: the .opf beside a book, read for any format (the Library labels TXT, Markdown and
// XTC books with it too), and the rule that keeps an author and its filing name together.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "../../lib/Epub/Epub/MetadataSidecar.h"

namespace fs = std::filesystem;

namespace {

class MetadataSidecarReadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / ("sidecar_read_" + std::string(info->name()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    book_ = (dir_ / "Some Book.txt").generic_string();
    std::ofstream(book_) << "text";
  }
  void TearDown() override { fs::remove_all(dir_); }

  void writeSidecar(const std::string& xml) const {
    std::ofstream(dir_ / "Some Book.opf", std::ios::binary) << xml;
  }

  fs::path dir_;
  std::string book_;
};

const char* kCalibreSidecar =
    "<?xml version='1.0' encoding='utf-8'?>\n"
    "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\">\n"
    "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n"
    "    <dc:title>A Wizard of Earthsea</dc:title>\n"
    "    <dc:creator opf:file-as=\"Le Guin, Ursula K.\" opf:role=\"aut\">Ursula K. Le Guin</dc:creator>\n"
    "    <meta name=\"calibre:series\" content=\"Earthsea\"/>\n"
    "    <meta name=\"calibre:series_index\" content=\"1\"/>\n"
    "  </metadata>\n"
    "</package>\n";

TEST_F(MetadataSidecarReadTest, ReadsTheSidecarOfABookThatIsNotAnEpub) {
  writeSidecar(kCalibreSidecar);
  MetadataSidecarFields out;
  ASSERT_TRUE(MetadataSidecar::read(book_, out));
  EXPECT_EQ(out.title, "A Wizard of Earthsea");
  EXPECT_EQ(out.author, "Ursula K. Le Guin");
  EXPECT_EQ(out.primaryAuthor, "Ursula K. Le Guin");
  EXPECT_EQ(out.authorSort, "Le Guin, Ursula K.");
  EXPECT_EQ(out.series, "Earthsea");
  EXPECT_EQ(out.seriesIndex, "1");
}

TEST_F(MetadataSidecarReadTest, NoSidecarIsNoAnswer) {
  MetadataSidecarFields out;
  EXPECT_FALSE(MetadataSidecar::read(book_, out));
}

// Review focus 4: these leave the book titled by its filename.
TEST_F(MetadataSidecarReadTest, AnEmptySidecarIsNoAnswer) {
  writeSidecar("");
  MetadataSidecarFields out;
  EXPECT_FALSE(MetadataSidecar::read(book_, out));
}

// Whether the streaming parser reports an unclosed document as an error or simply never completes
// the field, what must hold is that nothing from it reaches the book (the same shape as
// MetadataSidecarFixture.MalformedSidecarIsIgnored in the pipeline suite).
TEST_F(MetadataSidecarReadTest, AMalformedSidecarGivesNothing) {
  writeSidecar("<package><metadata><dc:title>Unclosed");
  MetadataSidecarFields out;
  MetadataSidecar::read(book_, out);
  EXPECT_TRUE(out.title.empty());
  EXPECT_TRUE(out.author.empty());
  EXPECT_TRUE(out.primaryAuthor.empty());
}

TEST_F(MetadataSidecarReadTest, AnOversizedSidecarIsNoAnswer) {
  writeSidecar(std::string(kCalibreSidecar) + "<!--" + std::string(17000, 'x') + "-->");
  MetadataSidecarFields out;
  EXPECT_FALSE(MetadataSidecar::read(book_, out));
}

// The filing name belongs to the name it files.
TEST(MetadataSidecarOverlay, ASidecarWithoutAnAuthorChangesNothing) {
  std::string primary = "Terry Pratchett";
  std::string sort = "Pratchett, Terry";
  MetadataSidecar::overlayPrimaryAuthor(MetadataSidecarFields{}, primary, sort);
  EXPECT_EQ(primary, "Terry Pratchett");
  EXPECT_EQ(sort, "Pratchett, Terry");
}

TEST(MetadataSidecarOverlay, ADifferentAuthorWithoutFileAsDropsTheBooksFileAs) {
  std::string primary = "Terry Pratchett";
  std::string sort = "Pratchett, Terry";
  MetadataSidecarFields sidecar;
  sidecar.primaryAuthor = "Neil Gaiman";
  MetadataSidecar::overlayPrimaryAuthor(sidecar, primary, sort);
  EXPECT_EQ(primary, "Neil Gaiman");
  EXPECT_EQ(sort, "");
}

TEST(MetadataSidecarOverlay, TheSameAuthorWithoutFileAsKeepsTheBooksFileAs) {
  std::string primary = "Terry Pratchett";
  std::string sort = "Pratchett, Terry";
  MetadataSidecarFields sidecar;
  sidecar.primaryAuthor = "Terry Pratchett";
  MetadataSidecar::overlayPrimaryAuthor(sidecar, primary, sort);
  EXPECT_EQ(primary, "Terry Pratchett");
  EXPECT_EQ(sort, "Pratchett, Terry");
}

TEST(MetadataSidecarOverlay, ASidecarFileAsWins) {
  std::string primary = "Terry Pratchett";
  std::string sort = "Pratchett, Terry";
  MetadataSidecarFields sidecar;
  sidecar.primaryAuthor = "Terry Pratchett";
  sidecar.authorSort = "Pratchett, Sir Terry";
  MetadataSidecar::overlayPrimaryAuthor(sidecar, primary, sort);
  EXPECT_EQ(sort, "Pratchett, Sir Terry");
}

}  // namespace
```

In `test/epub_opf/CMakeLists.txt`, add these lines to the `add_executable(ContentOpfParserTest ...)` source list:

```cmake
  MetadataSidecarReadTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/MetadataSidecar.cpp
  ${REPO_ROOT}/lib/FsHelpers/SidecarFiles.cpp
```

- [ ] **Step 2: Write the failing `Epub` tests**

Append to `test/epub_pipeline/MetadataSidecarTest.cpp`, inside its anonymous namespace after the last `TEST_F`. The fixture's `sidecarXml()` always writes `opf:file-as="Sorted, Name"`.

```cpp
// The Library groups books by their primary author and files them by its opf:file-as. Both come
// from the sidecar when it names an author.
TEST_F(MetadataSidecarFixture, SidecarSuppliesThePrimaryAuthorAndItsFileAs) {
  writeSidecar(sidecarXml("Sidecar Title", "Sidecar Author"));
  Epub epub(bookPath.string(), cacheDir);
  ASSERT_TRUE(epub.load(true));
  EXPECT_EQ(epub.getPrimaryAuthor(), "Sidecar Author");
  EXPECT_EQ(epub.getAuthorSort(), "Sorted, Name");
}

// book.bin keeps no primary author, so the details lookup loads past it. A book that has been
// opened (book.bin written) must still come back with its author.
TEST_F(MetadataSidecarFixture, AMetadataLoadPastBookBinFindsThePrimaryAuthor) {
  {
    Epub opened(bookPath.string(), cacheDir);
    ASSERT_TRUE(opened.load(true));  // writes book.bin, as opening the book does
  }
  Epub epub(bookPath.string(), cacheDir);
  ASSERT_TRUE(epub.loadForMetadata(nullptr, /*useBookBin=*/false));
  EXPECT_EQ(epub.getAuthor(), "Test Suite");
  EXPECT_EQ(epub.getPrimaryAuthor(), "Test Suite");
}
```

In `test/epub_pipeline/CMakeLists.txt`, add `${REPO_ROOT}/lib/Epub/Epub/MetadataSidecar.cpp` to `PIPELINE_SOURCES`, directly after `${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp`.

- [ ] **Step 3: Run them and watch them fail**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target ContentOpfParserTest -j 8
```

Expected: `MetadataSidecar.h: No such file or directory`.

- [ ] **Step 4: Create the sidecar reader**

`lib/Epub/Epub/MetadataSidecar.h`:

```cpp
#pragma once

#include <string>

// What a Calibre-style metadata sidecar ("Book.opf" beside "Book.epub") says about its book: the
// fields the firmware lets it override (docs/sidecar-files.md). Empty means "not supplied", and an
// empty field never blanks the book's own value.
struct MetadataSidecarFields {
  std::string title;
  std::string author;         // every dc:creator, joined for display
  std::string primaryAuthor;  // the first creator credited as author
  std::string authorSort;     // its opf:file-as
  std::string language;
  std::string series;
  std::string seriesIndex;
  std::string description;
};

namespace MetadataSidecar {

// Reads the metadata sidecar beside `bookPath`, whatever the book's format: the same OPF parser the
// book's own metadata goes through, over a plain file, with no ZIP and no manifest. False when there
// is none, or it is empty or larger than Epub::MAX_METADATA_SIDECAR_BYTES, or it cannot be read. A
// malformed one yields no usable fields. Either way the book keeps its own metadata.
bool read(const std::string& bookPath, MetadataSidecarFields& out);

// Lays the sidecar's primary author over a book's. The filing name travels with the name it files:
// a sidecar naming a different author brings its own opf:file-as (or none), and the book's is kept
// only when the sidecar names the same author and gives no file-as of its own.
void overlayPrimaryAuthor(const MetadataSidecarFields& sidecar, std::string& primaryAuthor, std::string& authorSort);

}  // namespace MetadataSidecar
```

`lib/Epub/Epub/MetadataSidecar.cpp`:

```cpp
#include "MetadataSidecar.h"

#include <HalStorage.h>
#include <Logging.h>
#include <SidecarFiles.h>

#include "parsers/ContentOpfParser.h"

namespace MetadataSidecar {

bool read(const std::string& bookPath, MetadataSidecarFields& out) {
  const std::string path = SidecarFiles::metadataPath(bookPath);
  if (path.empty()) return false;

  size_t size = 0;
  {
    HalFile probe;
    if (!Storage.openFileForRead("MSC", path, probe)) return false;
    size = probe.fileSize();
  }
  if (size == 0 || size > Epub::MAX_METADATA_SIDECAR_BYTES) {
    LOG_DBG("MSC", "Ignoring metadata sidecar, %u bytes: %s", static_cast<unsigned>(size), path.c_str());
    return false;
  }

  // Null cache: a sidecar carries no real manifest or spine, so no item index must be built from it.
  // The parser holds both paths by reference, so they must outlive it.
  const std::string noCachePath;
  const std::string noContentBase;
  ContentOpfParser parser(noCachePath, noContentBase, size, nullptr);
  if (!parser.setup()) return false;
  if (!Storage.readFileToStream(path.c_str(), parser, 1024)) {
    LOG_DBG("MSC", "Could not read metadata sidecar: %s", path.c_str());
    return false;
  }

  out.title = std::move(parser.title);
  out.author = std::move(parser.author);
  out.primaryAuthor = std::move(parser.primaryAuthor);
  out.authorSort = std::move(parser.authorSort);
  out.language = std::move(parser.language);
  out.series = std::move(parser.series);
  out.seriesIndex = std::move(parser.seriesIndex);
  out.description = std::move(parser.description);
  return true;
}

void overlayPrimaryAuthor(const MetadataSidecarFields& sidecar, std::string& primaryAuthor, std::string& authorSort) {
  if (sidecar.primaryAuthor.empty()) return;
  if (!sidecar.authorSort.empty() || sidecar.primaryAuthor != primaryAuthor) authorSort = sidecar.authorSort;
  primaryAuthor = sidecar.primaryAuthor;
}

}  // namespace MetadataSidecar
```

- [ ] **Step 5: Run the sidecar reader tests**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target ContentOpfParserTest -j 8 && ./epub_opf/ContentOpfParserTest.exe --gtest_filter='MetadataSidecar*'
```

Expected: 9 tests pass.

- [ ] **Step 6: Carry the fields through `Epub`**

In `lib/Epub/Epub/BookMetadataCache.h`, in `struct BookMetadata`, after `std::string description;`, add:

```cpp
    // Not stored in book.bin: its version would have to go up and every book be cached again. Set by
    // a fresh OPF parse and by the metadata sidecar; BookDetailsLookup::parse() loads past book.bin
    // to get them.
    std::string primaryAuthor;
    std::string authorSort;
```

In `lib/Epub/Epub.cpp`, in `parseContentOpf`, after `bookMetadata.description = opfParser.description;`, add:

```cpp
  bookMetadata.primaryAuthor = opfParser.primaryAuthor;
  bookMetadata.authorSort = opfParser.authorSort;
```

Replace the body of `Epub::applyMetadataSidecar()` with the following, and add `#include "Epub/MetadataSidecar.h"` to `Epub.cpp`'s includes:

```cpp
void Epub::applyMetadataSidecar() const {
  if (!bookMetadataCache) return;
  MetadataSidecarFields sidecar;
  if (!MetadataSidecar::read(filepath, sidecar)) return;

  auto& md = bookMetadataCache->coreMetadata;
  if (!sidecar.title.empty()) md.title = sidecar.title;
  if (!sidecar.author.empty()) md.author = sidecar.author;
  MetadataSidecar::overlayPrimaryAuthor(sidecar, md.primaryAuthor, md.authorSort);
  if (!sidecar.language.empty()) md.language = sidecar.language;
  if (!sidecar.series.empty()) md.series = sidecar.series;
  if (!sidecar.seriesIndex.empty()) md.seriesIndex = sidecar.seriesIndex;
  if (!sidecar.description.empty()) md.description = sidecar.description;
  LOG_DBG("EBP", "Applied metadata sidecar for %s", filepath.c_str());
}
```

In `Epub::loadForMetadata`, change the signature to `bool Epub::loadForMetadata(BuildArena* scratch, const bool useBookBin)` and the fast-path condition to:

```cpp
  // Fast path: an existing book.bin already carries title/author/series, no OPF parse needed --
  // unless the caller wants what book.bin does not keep (the primary author and its file-as).
  if (useBookBin && bookMetadataCache->load()) {
```

In `lib/Epub/Epub.h`, change the declaration to the following, keeping the existing comment above it and adding one line about the flag:

```cpp
  // useBookBin = false skips book.bin and always parses the OPF: book.bin keeps no primary author.
  bool loadForMetadata(BuildArena* scratch = nullptr, bool useBookBin = true);
```

Then add, after `const std::string& getAuthor() const;`:

```cpp
  const std::string& getPrimaryAuthor() const;
  const std::string& getAuthorSort() const;
```

In `lib/Epub/Epub.cpp`, after `Epub::getAuthor()`, add:

```cpp
const std::string& Epub::getPrimaryAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }
  return bookMetadataCache->coreMetadata.primaryAuthor;
}

const std::string& Epub::getAuthorSort() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }
  return bookMetadataCache->coreMetadata.authorSort;
}
```

`Epub::metadataSidecarPath()` stays; other code may call it. Check with `grep -rn metadataSidecarPath src lib` and leave it in place.

- [ ] **Step 7: Run the `Epub` and parser suites**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . -j 8 -- -k 0 2>&1 | grep -E "error|FAILED" ; ctest -R "MetadataSidecar|ContentOpfParser" --output-on-failure
```

Expected:
- no errors other than `epub_build_inventory`;
- all `MetadataSidecarFixture.*` pass, including the two new tests and the existing `MalformedSidecarIsIgnored` / `OversizedSidecarIsIgnored`;
- all `ContentOpfParser*` and `MetadataSidecar*` pass.

- [ ] **Step 8: Commit**

```bash
cd /c/_development/witchhunt-reader
git add lib/Epub/Epub/MetadataSidecar.h lib/Epub/Epub/MetadataSidecar.cpp lib/Epub/Epub/BookMetadataCache.h lib/Epub/Epub.h lib/Epub/Epub.cpp test/epub_opf/CMakeLists.txt test/epub_opf/MetadataSidecarReadTest.cpp test/epub_pipeline/CMakeLists.txt test/epub_pipeline/MetadataSidecarTest.cpp
git commit -m "feat(epub): sidecar reader for any book; primary author through Epub

The .opf sidecar overlay moves out of Epub into MetadataSidecar::read(), a
ContentOpfParser pass over a plain file, so books that are not EPUBs can use
it. Epub carries the parser's primary author and file-as (not in book.bin,
whose version stays), and loadForMetadata() can skip book.bin for callers
that need them."
```

---

### Task 3: `details.bin` version 2 and every format through `BookDetailsLookup`

**Files:**
- Modify: `src/activities/home/BookDetails.h`
- Modify: `src/activities/home/BookDetailsCache.cpp`
- Modify: `src/activities/home/BookDetails.cpp`
- Test: `test/book_details_cache/BookDetailsCacheTest.cpp`
- Docs: `docs/sidecar-files.md`

**Interfaces:**
- Consumes:
  - `MetadataSidecar::read`, `MetadataSidecar::overlayPrimaryAuthor`
  - `Epub::loadForMetadata(scratch, false)`, `Epub::getPrimaryAuthor()`, `Epub::getAuthorSort()` (Task 2)
  - `Xtc::load()`, `Xtc::getTitle()`, `Xtc::getAuthor()`, `Xtc::setupCacheDir()`, `Txt::setupCacheDir()`
  - `ReaderActivity::bookCacheDir(const std::string&)`
- Produces: `BookDetails::primaryAuthor`, `BookDetails::authorSort`. `BookDetailsLookup::cached()` / `parse()` now answer for EPUB, XTC, TXT and Markdown alike. Plans 2 and 3 build the Library index on these.

- [ ] **Step 1: Write the failing cache test**

In `test/book_details_cache/BookDetailsCacheTest.cpp`, in `sample()`, add after `d.author = "Thomas Mann";`:

```cpp
    d.primaryAuthor = "Thomas Mann";
    d.authorSort = "Mann, Thomas";
```

In `RoundTrips`, add after `EXPECT_EQ(out.author, "Thomas Mann");`:

```cpp
  EXPECT_EQ(out.primaryAuthor, "Thomas Mann");
  EXPECT_EQ(out.authorSort, "Mann, Thomas");
```

Append before the closing `}  // namespace`:

```cpp
// A version 1 record has no primary author. Answering from it would group the book under "Unknown
// author" for good, so it is parsed again instead.
TEST_F(BookDetailsCacheTest, AVersionOneRecordIsNotAnswered) {
  ASSERT_TRUE(BookDetailsCache::write(path_, 1, 0, sample()));
  {
    std::fstream f(path_, std::ios::in | std::ios::out | std::ios::binary);
    f.put(static_cast<char>(1));
  }
  BookDetails out;
  EXPECT_FALSE(BookDetailsCache::read(path_, 1, 0, out));
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target BookDetailsCacheTest -j 8
```

Expected: a compile error, `'struct BookDetails' has no member named 'primaryAuthor'`.

- [ ] **Step 3: Version 2 of the record**

`src/activities/home/BookDetails.h`: replace the comment above `struct BookDetails`, and the struct itself, with:

```cpp
// What the book lists show for a book: its title, author and series as the book's own metadata (or
// its .opf sidecar) gives them rather than as the filename spells them, and the author the Library
// groups it by.
//
// Getting them costs a metadata-only OPF parse (~300 ms on the device) for an EPUB and a header read
// for an XTC; a TXT or Markdown book has only its sidecar, if any. The result is kept beside the
// book's other cache files as details.bin, so after the first time every book on the card is a
// small file read away.
struct BookDetails {
  std::string title;
  std::string author;         // every author the book credits, for display
  std::string primaryAuthor;  // the first creator credited as author: what the Library groups by
  std::string authorSort;     // its opf:file-as ("Le Guin, Ursula K."); "" when the book gives none
  std::string series;
  std::string seriesIndex;
};
```

Replace the comments on `BookDetailsLookup::cached` and `parse` with:

```cpp
// The answer without parsing anything: from details.bin, or, for a TXT or Markdown book with no
// sidecar (titled by its filename), from nothing at all. False for a book that needs parse() first.
bool cached(const std::string& bookPath, uint32_t bookSize, BookDetails& out);

// The slow path for a book cached() could not answer: an EPUB's OPF (always the OPF, never book.bin,
// which keeps no primary author), an XTC's header, and for every format the .opf sidecar over it.
// The result is recorded in details.bin. Always fills `out`. For an EPUB whose parse failed it is
// left empty and not recorded, so the next visit tries again: the failure may have been a lack of
// memory. `scratch` holds the OPF inflate ring instead of the heap when given. True when the lookup
// succeeded.
bool parse(const std::string& bookPath, uint32_t bookSize, BookDetails& out, BuildArena* scratch = nullptr);
```

`src/activities/home/BookDetailsCache.cpp`:
- set `constexpr uint8_t VERSION = 2;`
- in `write`, after `serialization::writeString(file, details.seriesIndex);`, add:

```cpp
  serialization::writeString(file, details.primaryAuthor);
  serialization::writeString(file, details.authorSort);
```

- in `read`, extend the `complete` chain:

```cpp
  const bool complete =
      current && serialization::readString(file, details.title) && serialization::readString(file, details.author) &&
      serialization::readString(file, details.series) && serialization::readString(file, details.seriesIndex) &&
      serialization::readString(file, details.primaryAuthor) && serialization::readString(file, details.authorSort);
```

- [ ] **Step 4: Run the cache tests**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target BookDetailsCacheTest -j 8 && ./book_details_cache/BookDetailsCacheTest.exe
```

Expected: 8 tests pass.

- [ ] **Step 5: Every format through the lookup**

Replace `src/activities/home/BookDetails.cpp` with:

```cpp
#include "BookDetails.h"

#include <Epub.h>
#include <Epub/MetadataSidecar.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <SidecarFiles.h>
#include <Txt.h>
#include <Xtc.h>

#include "activities/reader/ReaderActivity.h"

namespace {

// details.bin sits with the book's other cache files, whatever its format.
std::string detailsPath(const std::string& bookPath) { return ReaderActivity::bookCacheDir(bookPath) + "/details.bin"; }

// The sidecar over what the book said of itself, field by field: an empty sidecar field leaves the
// book's own.
void applySidecar(const std::string& bookPath, BookDetails& out) {
  MetadataSidecarFields sidecar;
  if (!MetadataSidecar::read(bookPath, sidecar)) return;
  if (!sidecar.title.empty()) out.title = sidecar.title;
  if (!sidecar.author.empty()) out.author = sidecar.author;
  MetadataSidecar::overlayPrimaryAuthor(sidecar, out.primaryAuthor, out.authorSort);
  if (!sidecar.series.empty()) out.series = sidecar.series;
  if (!sidecar.seriesIndex.empty()) out.seriesIndex = sidecar.seriesIndex;
}

}  // namespace

namespace BookDetailsLookup {

// details.bin only, never book.bin. Epub::loadForMetadata() reads book.bin when it is current but
// quietly falls back to the ~300 ms OPF parse when it is from an older cache version -- which after
// a firmware update is every book on the card -- and this runs while a page is being drawn.
bool cached(const std::string& bookPath, const uint32_t bookSize, BookDetails& out) {
  // A TXT or Markdown book says nothing about itself: without a sidecar there is nothing to look up,
  // and the view titles it by its filename. An existence probe, not the sidecar's hash.
  const bool describesItself = FsHelpers::hasEpubExtension(bookPath) || FsHelpers::hasXtcExtension(bookPath);
  if (!describesItself && SidecarFiles::metadataPath(bookPath).empty()) {
    out = {};
    return true;
  }
  const std::string path = detailsPath(bookPath);
  // Most misses are books never seen before: answer those without hashing the sidecar.
  if (!Storage.exists(path.c_str())) return false;
  return BookDetailsCache::read(path, bookSize, SidecarFiles::metadataStamp(bookPath), out);
}

bool parse(const std::string& bookPath, const uint32_t bookSize, BookDetails& out, BuildArena* scratch) {
  out = {};
  if (FsHelpers::hasEpubExtension(bookPath)) {
    Epub epub(bookPath, "/.crosspoint");
    // Past book.bin, which keeps no primary author: one OPF parse per book, then details.bin has it.
    // The .opf sidecar is applied by the load.
    if (!epub.loadForMetadata(scratch, /*useBookBin=*/false)) return false;
    out.title = epub.getTitle();
    out.author = epub.getAuthor();
    out.primaryAuthor = epub.getPrimaryAuthor();
    out.authorSort = epub.getAuthorSort();
    out.series = epub.getSeries();
    out.seriesIndex = epub.getSeriesIndex();
    epub.setupCacheDir();
  } else if (FsHelpers::hasXtcExtension(bookPath)) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (!xtc.load()) return false;
    out.title = xtc.getTitle();
    out.author = xtc.getAuthor();
    out.primaryAuthor = out.author;  // an XTC header names one author
    applySidecar(bookPath, out);
    xtc.setupCacheDir();
  } else {
    // TXT and Markdown: the sidecar is all there is. Recorded even when it gave nothing (a malformed
    // sidecar), so the book is not read again on every visit; editing the sidecar changes its stamp.
    applySidecar(bookPath, out);
    Txt(bookPath, "/.crosspoint").setupCacheDir();
  }
  BookDetailsCache::write(detailsPath(bookPath), bookSize, SidecarFiles::metadataStamp(bookPath), out);
  return true;
}

}  // namespace BookDetailsLookup
```

Notes for the implementer:
- `ReaderActivity::bookCacheDir` is a public static ([ReaderActivity.h](src/activities/reader/ReaderActivity.h), `bookCacheDir`). Including its header from this `.cpp` creates no cycle, because only headers include each other cyclically (see the note in `FileBrowserActivity.h` about `CoverThumbLoader`).
- A Markdown book goes to the `Txt` cache path, as `bookCacheDir` already does.
- `BookRowResolver` needs no change. It marks a row `needsParse` whenever `cached()` returns false, and calls `parse()` for it from the loop task, whatever the format.

- [ ] **Step 6: Document the sidecar's wider reach**

In `docs/sidecar-files.md`, change the `| author | <dc:creator> |` table row and add one row under it:

```markdown
| author | `<dc:creator>` (every creator, joined for display) |
| primary author and its filing name | the first `<dc:creator>` with `opf:role="aut"` or no role, and its `opf:file-as` (EPUB 3: `<meta refines="#id" property="role">` / `property="file-as"`) |
```

Then add this section directly before `### Rules`:

```markdown
### Books other than EPUB

TXT, Markdown and XTC books take their title, author and series from a metadata sidecar too, in
the book lists (Browse Files' Details and Covers views). Without a sidecar, an XTC book is labelled
from its own header, and a TXT or Markdown book, which carries no metadata, by its filename. The
reader itself still shows the book's own title.
```

- [ ] **Step 7: Build the firmware**

Run in the background: `"$HOME/.platformio/penv/Scripts/pio.exe" run -e default`.
Expected: SUCCESS. Note the `Flash:` line and compare it with Task 0's baseline. Expect a few hundred bytes to about 2 KB more; past 4 KB, stop and investigate.

- [ ] **Step 8: Commit**

```bash
cd /c/_development/witchhunt-reader
git add src/activities/home/BookDetails.h src/activities/home/BookDetailsCache.cpp src/activities/home/BookDetails.cpp test/book_details_cache/BookDetailsCacheTest.cpp docs/sidecar-files.md
git commit -m "feat(details): primary author in details.bin; sidecars for every format

details.bin v2 records the primary author and its file-as, read from the
OPF past book.bin (which keeps neither). TXT, Markdown and XTC books get
their title, author and series from the .opf sidecar, and an XTC from its
header, so the Details and Covers views stop showing filenames for them."
```

---

### Task 4: Whole-branch verification

**Files:** only those clang-format touches.

- [ ] **Step 1: Format**

```bash
cd /c/_development/witchhunt-reader && PATH="/c/tmp/cf21/bin:$PATH" ./bin/clang-format-fix -g && git status --short
```

Expected: either no changes, or only files this branch touched. Commit any formatting as `style: clang-format 21`, adding the files explicitly.

- [ ] **Step 2: Full host suite**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . -j 8 -- -k 0 2>&1 | grep -E "error:" ; ctest -j 8 2>&1 | tail -3
```

Expected: `100% tests passed`. The count is Task 0's plus 22 new tests: 10 creator tests, 9 sidecar-reader tests, 2 `Epub` tests and 1 cache test.

- [ ] **Step 3: The other two board builds, one at a time, in the background**

```bash
"$HOME/.platformio/penv/Scripts/pio.exe" run -e x4pro
"$HOME/.platformio/penv/Scripts/pio.exe" run -e lilygo_t5s3
```

Expected: both SUCCESS. Record the `Flash:` lines.

- [ ] **Step 4: Device checks (the user, on an X3 or X4)**

Prepare a folder holding:
- an EPUB never opened;
- an EPUB already opened;
- a TXT with `Name.opf` beside it, carrying a title, `opf:role="aut" opf:file-as=...` and a series;
- a TXT without a sidecar;
- an XTC.

Open Browse Files → Options → View → Details on that folder. Check:
- the never-opened and opened EPUBs show title and author;
- the TXT with a sidecar shows the sidecar's title and "Author · Series #n";
- the TXT without a sidecar shows its filename;
- the XTC shows its header title and author;
- leaving and re-entering the folder shows the same rows without a second parse (no `BRR details parse` lines in the serial log for those books);
- after editing the TXT's sidecar title over the web UI, the new title shows on the next visit.

- [ ] **Step 5: Hand back**

Report the host test count, the three `Flash:` deltas, and the device-check results. Do not push or open a PR unless asked.
