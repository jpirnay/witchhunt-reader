# Library Plan 2b: Index Builder, New/Authors Model Modes, Hosting

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the book index on the device and serve it to the browser:
- `LibraryBuilder`, host-tested, runs walk, join, publish, resolve and republish a step at a time;
- `FileBrowserModel` gains `Mode::Added` and `Mode::Authors`;
- `FileBrowserActivity` hosts the builder in `loop()` under the spec §3.6 freshness rules.

This is the first point where the index library is linked into the firmware, so this plan ends with the flash measurement for the user's cost gate.

**Architecture:**
- **`lib/LibraryIndex/LibraryBuilder`** owns the walk, with metadata sidecars paired by a per-folder table. It drives `LibraryJoin`/`LibraryPublish`, patches resolved authors into `records.bin` in place, and appends their names with one shared writer (`library::appendNameEntry`). The resolver is injected, so the builder is host-testable.
- **On the device (`src/`):** a resolver over `BookDetailsLookup`, a freshness rule, model modes that read the index through `LibraryIndexReader`, and hosting that gives the lent framebuffer to the builder after titles and before covers.

**Spec:** `docs/superpowers/specs/2026-10-09-library-view-design.md` §3.5 (phases 1, 4), §3.6 (freshness), §4 (model, hosting). This plan continues `docs/superpowers/plans/2026-10-09-library-plan-2a-index-lib.md`.

## Global Constraints

- **No resident RAM growth.** The builder lives only while a build runs, as one heap object. It holds the walk stack (≤ 9 open directories, each with a sidecar table of 8 B per sidecar, ≤ 128 per folder) and a 500-byte name buffer.
- **Lent framebuffer:** join, publish and resolve run in it, and the builder resets it at each step. Before a step needs it, the host stops the cover loader, the arena's other user.
- **Before every publish,** the host closes the model's open reader (publish removes and renames the file); after it, the host reopens it.
- **Working directory:** `/.crosspoint/library`. The builder creates it, because the device's open does not create parents.
- **Walk rules:**
  - books only, by Browse Files' rule (`FileBrowserModel::isBookName`);
  - dot entries only when `showHiddenFiles` is on, and `/.crosspoint` never;
  - "System Volume Information" never;
  - depth ≤ 8;
  - paths longer than `library::MAX_STRING` are left out.
- **Freshness (spec §3.6):** build on entering New or Authors when any of these holds:
  - no valid index;
  - no build has finished this boot;
  - `contentGeneration` has changed since the build started;
  - `acceptRules` differs from `showHiddenFiles`.
- **Build host tests from Git Bash only.** Format with clang-format 21. Never `git add -A`.
- **Firmware builds:** `export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1` first, one `pio run` at a time, in the background.

## Review Focus

1. **A book whose file vanished between the walk and its resolve** must stay pending without failing the build. Test in Task 2.
2. **A folder with more metadata sidecars than `MAX_SIDECARS`** must mark the unmatched books `SIDECAR_UNKNOWN` rather than `SIDECAR_NONE`. Test in Task 2.
3. **The resolver failing for a book** (out of memory) must leave it pending and ask again on the next build. Test in Task 2.
4. **The cap reached mid-walk** must stop the walk, publish a partial index, and keep the books found. Test in Task 2.
5. **Leaving the screen mid-build** must abandon the build cleanly: framebuffer returned, no reader left closed for good. This is device-only, so it is covered by a device check in Task 4 and by the hosting code's ordering in Task 3.

---

### Task 0: Plan

- [ ] **Step 1: Commit this plan**

```bash
cd /c/_development/witchhunt-reader && git add docs/superpowers/plans/2026-10-09-library-plan-2b-builder-model.md && git commit -m "docs: library plan 2b (builder, model modes, hosting)"
```

---

### Task 1: One names-entry writer

**Files:**
- Modify: `lib/LibraryIndex/LibraryBlob.h`, `lib/LibraryIndex/LibraryBlob.cpp`, `lib/LibraryIndex/LibraryJoin.cpp`

**Interfaces:**
- Produces: `bool library::appendNameEntry(HalFile& names, uint32_t hash, bool fromFileAs, const std::string& name, const std::string& key)`. Join's `carryNames` uses it, and so does the builder in Task 2.

- [ ] **Step 1: Add the writer**

In `lib/LibraryIndex/LibraryBlob.h`, before the closing `}  // namespace library`, add:

```cpp
// One entry of a build's names file: the author's hash, whether its key came from a file-as, its
// display name and sort key. The one place that format is written.
bool appendNameEntry(HalFile& names, uint32_t hash, bool fromFileAs, const std::string& name, const std::string& key);
```

In `lib/LibraryIndex/LibraryBlob.cpp`, before the closing `}  // namespace library`, add:

```cpp
bool appendNameEntry(HalFile& names, const uint32_t hash, const bool fromFileAs, const std::string& name,
                     const std::string& key) {
  const uint8_t flags = fromFileAs ? 1 : 0;
  return writeExact(names, &hash, sizeof(hash)) && writeExact(names, &flags, sizeof(flags)) &&
         writeBlobString(names, name) && writeBlobString(names, key);
}
```

- [ ] **Step 2: Use it in join**

In `lib/LibraryIndex/LibraryJoin.cpp`, `carryNames`, replace the block from `// The index keeps no flag:` to the end of the `if (...) { return false; }` with:

```cpp
    // The index keeps no flag: a key that is not what the name alone gives came from a file-as.
    const bool fromFileAs = key != LibraryKeys::authorSortKey(name, "");
    if (!library::appendNameEntry(names, author.hash, fromFileAs, name, key)) return false;
```

- [ ] **Step 3: Run the library tests**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target LibraryIndexTest -j 8 2>&1 | grep -E "error:|warning:" | cut -c1-200 | head -3; ./library_index/LibraryIndexTest.exe 2>&1 | grep -E "^\[  (FAILED|PASSED) "
```
Expected: `[  PASSED  ] 42 tests.`, unchanged. This is a pure refactor.

- [ ] **Step 4: Commit**

```bash
cd /c/_development/witchhunt-reader
git add lib/LibraryIndex/LibraryBlob.h lib/LibraryIndex/LibraryBlob.cpp lib/LibraryIndex/LibraryJoin.cpp
git commit -m "refactor(library): one writer for names-file entries"
```

---

### Task 2: The builder

**Files:**
- Modify: `test/zip_entry_reader/HalStorage.h`. Directory entries get their size and a FAT date/time, and `getCreateDateTime` is added.
- Create: `lib/LibraryIndex/LibraryBuilder.h`, `lib/LibraryIndex/LibraryBuilder.cpp`, `test/library_index/LibraryBuilderTest.cpp`
- Modify: `test/library_index/CMakeLists.txt`

**Interfaces:**
- Consumes: `LibraryJoin::join`, `LibraryPublish::publish`, `LibraryKeys::*`, `library::appendNameEntry`/`writeBlobString`/`writeExact`/`readExact`/`readBlobString`.
- Produces:
  - `class LibraryBuilder` with `struct Author { name, fileAs }`
  - `using Resolver = bool (*)(void*, const std::string& path, uint32_t size, Author&, BuildArena*)`
  - `struct Config { root, workDir, indexPath, showHidden, resolveAll, maxBooks, republishEvery, isBook, resolve, resolveUser }`
  - `enum class Phase { Walk, Join, Publish, Resolve, Done, Failed }`
  - `Phase step(BuildArena*)`, `phase()`, `finished()`, `needsArena()`, `nextStepPublishes()`, `takePublished()`, `booksFound()`, `pending()`, `resolved()`

- [ ] **Step 1: Give the host shim's directory entries their size and dates**

In `test/zip_entry_reader/HalStorage.h`:

1. Add `#include <chrono>` and `#include <ctime>` with the other includes.
2. Replace `struct DirEntry { std::string name; bool isDir = false; };` with:

```cpp
  struct DirEntry {
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
    uint16_t fatDate = 0;
    uint16_t fatTime = 0;
  };
```

3. Add these members next to `std::string name_;`:

```cpp
  uint64_t entrySize_ = 0;  // a directory entry's size and FAT stamp, as SdFat's openNext gives them
  uint16_t entryDate_ = 0;
  uint16_t entryTime_ = 0;
```

4. In the move constructor and the move assignment, carry them over next to `name_`: `entrySize_(o.entrySize_), entryDate_(o.entryDate_), entryTime_(o.entryTime_),` in the constructor's initialiser list, and `entrySize_ = o.entrySize_; entryDate_ = o.entryDate_; entryTime_ = o.entryTime_;` in the assignment.

5. In `openDir`, replace the `entries_.push_back(...)` line with:

```cpp
      DirEntry entry{e.path().filename().string(), e.is_directory()};
      if (!entry.isDir) {
        entry.size = static_cast<uint64_t>(std::filesystem::file_size(e.path(), ec));
        fatStamp(e.path(), entry.fatDate, entry.fatTime);
      }
      entries_.push_back(std::move(entry));
```

6. Add this private static helper above `closeQuiet()`:

```cpp
  // A file's last write as FAT date and time, in UTC: the stamp SdFat reads from a directory entry.
  static void fatStamp(const std::filesystem::path& path, uint16_t& date, uint16_t& time) {
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    date = 0;
    time = 0;
    if (ec) return;
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::file_clock::to_sys(written));
    const std::time_t t = std::chrono::system_clock::to_time_t(seconds);
    const std::tm* tm = std::gmtime(&t);
    if (tm == nullptr || tm->tm_year < 80) return;
    date = static_cast<uint16_t>(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
    time = static_cast<uint16_t>((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
  }
```

7. In `openNextFile()`, after `f.isDir_ = e.isDir;`, add:

```cpp
    f.entrySize_ = e.size;
    f.entryDate_ = e.fatDate;
    f.entryTime_ = e.fatTime;
```

8. Make `fileSize()` answer for an entry: replace its first line `if (!fp_) return 0;` with `if (!fp_) return hasName_ ? static_cast<size_t>(entrySize_) : 0;`.
9. Replace `bool getModifyDateTime(uint16_t*, uint16_t*) { return false; }` with:

```cpp
  bool getModifyDateTime(uint16_t* pdate, uint16_t* ptime) {
    if (!hasName_ || pdate == nullptr || ptime == nullptr) return false;
    *pdate = entryDate_;
    *ptime = entryTime_;
    return true;
  }
  // std::filesystem keeps no creation time: the write time stands in for it.
  bool getCreateDateTime(uint16_t* pdate, uint16_t* ptime) { return getModifyDateTime(pdate, ptime); }
```

`std::error_code ec;` is already declared in `openDir` (the iterator uses it). Reuse it for `file_size`.

- [ ] **Step 2: Write the failing builder tests**

`test/library_index/LibraryBuilderTest.cpp`:

```cpp
// The builder walks a card, joins, publishes and resolves a step at a time (spec §3.5). These tests
// run it over a real directory tree through the stdio storage shim.

#include <BuildArena.h>
#include <LibraryBuilder.h>
#include <LibraryFormat.h>
#include <LibraryIndexReader.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool isBook(const char* name) {
  const std::string n(name);
  const auto ends = [&n](const char* ext) {
    const size_t len = std::strlen(ext);
    return n.size() > len && n.compare(n.size() - len, len, ext) == 0;
  };
  return ends(".epub") || ends(".txt") || ends(".md") || ends(".xtc");
}

// The authors the card's books carry, by filename; and which ones cannot be read right now.
struct FakeCatalog {
  std::map<std::string, LibraryBuilder::Author> authors;
  std::set<std::string> unreadable;
  int calls = 0;
};

bool fakeResolve(void* user, const std::string& path, uint32_t, LibraryBuilder::Author& out, BuildArena* scratch) {
  auto& catalog = *static_cast<FakeCatalog*>(user);
  ++catalog.calls;
  EXPECT_NE(scratch, nullptr);
  const std::string file = path.substr(path.rfind('/') + 1);
  if (catalog.unreadable.count(file) != 0) return false;
  const auto found = catalog.authors.find(file);
  out = found != catalog.authors.end() ? found->second : LibraryBuilder::Author{};
  return true;
}

class LibraryBuilderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    card_ = (fs::temp_directory_path() / ("library_builder_" + std::string(info->name()))).generic_string();
    fs::remove_all(card_);
    fs::create_directories(card_);
  }
  void TearDown() override { fs::remove_all(card_); }

  void file(const std::string& rel, const size_t bytes = 100) const {
    const fs::path path = fs::path(card_) / rel;
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << std::string(bytes, 'x');
  }

  std::string workDir() const { return card_ + "/.crosspoint/library"; }
  std::string indexPath() const { return workDir() + "/library.bin"; }

  LibraryBuilder::Config config() {
    LibraryBuilder::Config c;
    c.root = card_;
    c.workDir = workDir();
    c.indexPath = indexPath();
    c.isBook = &isBook;
    c.resolve = &fakeResolve;
    c.resolveUser = &catalog_;
    return c;
  }

  // Runs a build to its end; returns the phase it ended in and counts the steps and publishes.
  LibraryBuilder::Phase build(LibraryBuilder::Config c, int* walkSteps = nullptr, int* publishes = nullptr) {
    LibraryBuilder builder(std::move(c));
    BuildArena framebuffer(48000);
    for (int i = 0; i < 100000 && !builder.finished(); ++i) {
      if (walkSteps != nullptr && builder.phase() == LibraryBuilder::Phase::Walk) ++*walkSteps;
      builder.step(&framebuffer);
      if (builder.takePublished() && publishes != nullptr) ++*publishes;
    }
    return builder.phase();
  }

  std::vector<std::string> authorNames() const {
    LibraryIndexReader index;
    EXPECT_TRUE(index.open(indexPath()));
    std::vector<std::string> names;
    for (uint16_t i = 0; i < index.header().authorCount; ++i) {
      library::AuthorRecord author{};
      std::string name;
      EXPECT_TRUE(index.author(i, author));
      EXPECT_TRUE(index.authorName(author, name));
      if (author.hash == library::AUTHOR_UNKNOWN) name = "<unknown>";
      if (author.hash == library::AUTHOR_PENDING) name = "<pending>";
      names.push_back(name + " x" + std::to_string(author.count));
    }
    return names;
  }

  std::string card_;
  FakeCatalog catalog_;
};

TEST_F(LibraryBuilderTest, AFirstBuildIndexesTheBooksAndTheirAuthors) {
  file("Books/Mort.epub");
  file("Books/Eric.epub");
  file("Books/Notes.txt");
  file("Pictures/cover.jpg");
  catalog_.authors["Mort.epub"] = {"Terry Pratchett", ""};
  catalog_.authors["Eric.epub"] = {"Terry Pratchett", "Pratchett, Terry"};
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(indexPath()));
  EXPECT_EQ(index.header().bookCount, 3);
  EXPECT_EQ(authorNames(), (std::vector<std::string>{"Terry Pratchett x2", "<unknown> x1"}));
  EXPECT_EQ(catalog_.calls, 3);
}

TEST_F(LibraryBuilderTest, ASecondBuildResolvesNothingAgain) {
  file("Books/Mort.epub");
  catalog_.authors["Mort.epub"] = {"Terry Pratchett", ""};
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  catalog_.calls = 0;
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_EQ(catalog_.calls, 0);
  EXPECT_EQ(authorNames(), (std::vector<std::string>{"Terry Pratchett x1"}));
}

TEST_F(LibraryBuilderTest, AnEditedSidecarResolvesItsBookAgain) {
  file("Books/Mort.epub");
  file("Books/Mort.opf", 50);
  file("Books/Eric.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  catalog_.calls = 0;
  file("Books/Mort.opf", 60);  // edited: another size
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_EQ(catalog_.calls, 1);
}

TEST_F(LibraryBuilderTest, HiddenFoldersAreListedOnlyWhenShownAndTheCacheNever) {
  file("Books/Mort.epub");
  file(".hidden/Secret.epub");
  file(".crosspoint/Cached.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(indexPath()));
  EXPECT_EQ(index.header().bookCount, 1);
  EXPECT_EQ(index.header().acceptRules, 0);
  index.close();
  auto shown = config();
  shown.showHidden = true;
  ASSERT_EQ(build(shown), LibraryBuilder::Phase::Done);
  ASSERT_TRUE(index.open(indexPath()));
  EXPECT_EQ(index.header().bookCount, 2);
  EXPECT_EQ(index.header().acceptRules, 1);
}

// Review focus 3.
TEST_F(LibraryBuilderTest, ABookThatCannotBeReadNowStaysPendingAndIsAskedAgain) {
  file("Books/Mort.epub");
  catalog_.unreadable.insert("Mort.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_EQ(authorNames(), (std::vector<std::string>{"<pending> x1"}));
  catalog_.unreadable.clear();
  catalog_.authors["Mort.epub"] = {"Terry Pratchett", ""};
  catalog_.calls = 0;
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_EQ(catalog_.calls, 1);
  EXPECT_EQ(authorNames(), (std::vector<std::string>{"Terry Pratchett x1"}));
}

// Review focus 4.
TEST_F(LibraryBuilderTest, TheCapStopsTheWalkAndMarksTheIndexPartial) {
  file("Books/a.epub");
  file("Books/b.epub");
  file("Books/c.epub");
  auto capped = config();
  capped.maxBooks = 2;
  ASSERT_EQ(build(capped), LibraryBuilder::Phase::Done);
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(indexPath()));
  EXPECT_EQ(index.header().bookCount, 2);
  EXPECT_TRUE(index.partial());
}

TEST_F(LibraryBuilderTest, AuthorsArePublishedAsTheyAreResolved) {
  file("Books/a.epub");
  file("Books/b.epub");
  file("Books/c.epub");
  auto often = config();
  often.republishEvery = 1;
  int publishes = 0;
  ASSERT_EQ(build(often, nullptr, &publishes), LibraryBuilder::Phase::Done);
  EXPECT_GE(publishes, 4) << "the first publish, then one per resolved book";
}

TEST_F(LibraryBuilderTest, TheWalkYieldsBetweenSteps) {
  for (int i = 0; i < 100; ++i) file("Books/b" + std::to_string(i) + ".txt");
  int walkSteps = 0;
  ASSERT_EQ(build(config(), &walkSteps), LibraryBuilder::Phase::Done);
  EXPECT_GT(walkSteps, 3);
}

TEST_F(LibraryBuilderTest, TheWorkingFilesAreRemovedWhenDone) {
  file("Books/Mort.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_TRUE(fs::exists(indexPath()));
  for (const char* name : {"stage.bin", "paths.bin", "records.bin", "names.bin"}) {
    EXPECT_FALSE(fs::exists(workDir() + "/" + name)) << name;
  }
}

TEST_F(LibraryBuilderTest, NewListsTheBooksAddedSinceTheLastBuild) {
  file("Books/Old.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  file("Books/Fresh.epub");
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  LibraryIndexReader index;
  ASSERT_TRUE(index.open(indexPath()));
  uint16_t newest = 0;
  library::BookRecord record{};
  std::string path;
  ASSERT_TRUE(index.newBook(0, newest));
  ASSERT_TRUE(index.book(newest, record));
  ASSERT_TRUE(index.blobString(record.pathOff, path));
  EXPECT_EQ(path.substr(path.rfind('/') + 1), "Fresh.epub");
}

// Review focus 1.
TEST_F(LibraryBuilderTest, ABookGoneBeforeItsResolveStaysPending) {
  file("Books/Mort.epub");
  LibraryBuilder builder(config());
  BuildArena framebuffer(48000);
  while (!builder.finished() && builder.phase() != LibraryBuilder::Phase::Resolve) builder.step(&framebuffer);
  ASSERT_EQ(builder.phase(), LibraryBuilder::Phase::Resolve);
  fs::remove(fs::path(card_) / "Books/Mort.epub");
  catalog_.unreadable.insert("Mort.epub");  // as the real resolver finds a missing book
  while (!builder.finished()) builder.step(&framebuffer);
  EXPECT_EQ(builder.phase(), LibraryBuilder::Phase::Done);
}

// Review focus 2.
TEST_F(LibraryBuilderTest, AFolderWithTooManySidecarsMarksTheUnpairedBooksUnknown) {
  for (int i = 0; i < 130; ++i) file("Books/s" + std::to_string(i) + ".opf", 10);
  file("Books/zz.epub");  // its sidecar would sort after the 128 the table keeps
  file("Books/zz.opf", 10);
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  catalog_.calls = 0;
  ASSERT_EQ(build(config()), LibraryBuilder::Phase::Done);
  EXPECT_EQ(catalog_.calls, 1) << "an unpaired book goes through the exact details.bin check every build";
}

TEST_F(LibraryBuilderTest, PhasesThatNeedTheFramebufferWaitForIt) {
  file("Books/Mort.epub");
  LibraryBuilder builder(config());
  for (int i = 0; i < 1000 && builder.phase() == LibraryBuilder::Phase::Walk; ++i) builder.step(nullptr);
  ASSERT_EQ(builder.phase(), LibraryBuilder::Phase::Join);
  EXPECT_TRUE(builder.needsArena());
  EXPECT_EQ(builder.step(nullptr), LibraryBuilder::Phase::Join) << "no framebuffer, no progress";
}

}  // namespace
```

In `test/library_index/CMakeLists.txt`, add `LibraryBuilderTest.cpp` to the test sources and `${REPO_ROOT}/lib/LibraryIndex/LibraryBuilder.cpp` to the library sources.

- [ ] **Step 3: Run and watch it fail**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target LibraryIndexTest -j 8 2>&1 | grep -E "Cannot find source|error:" | cut -c1-160 | head -3
```
Expected: `LibraryBuilder.cpp`/`.h` are missing.

- [ ] **Step 4: The builder**

`lib/LibraryIndex/LibraryBuilder.h`:

```cpp
#pragma once

#include <BuildArena.h>
#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

#include "LibraryFormat.h"

// Builds the book index a step at a time (spec section 3.5): walks the card, joins what it found
// with the previous index, publishes, then resolves the books whose author is not known yet,
// publishing again every so often so the Authors tab fills in as it goes.
//
// A step is one bounded piece of work, so the screen hosting it stays responsive: a few dozen
// directory entries, the join, a publish, or one book's author. Every phase but the walk needs the
// lent framebuffer as `arena`, and resets it; without one it waits. The walk holds only the folders
// it is in and their metadata sidecars, 8 bytes each and at most MAX_SIDECARS per folder.
class LibraryBuilder {
 public:
  struct Author {
    std::string name;    // the primary author; "" for a book without one
    std::string fileAs;  // its opf:file-as; "" when the book gives none
  };
  // Looks up a book's author. False when that cannot be done just now (for want of memory, most
  // likely): the book stays pending and the next build asks again. `scratch` is the arena, reset.
  using Resolver = bool (*)(void* user, const std::string& path, uint32_t size, Author& out, BuildArena* scratch);

  struct Config {
    std::string root = "/";
    std::string workDir = library::DIR;
    std::string indexPath = library::INDEX_PATH;
    bool showHidden = false;  // what Browse Files lists; recorded in the index
    bool resolveAll = false;  // Refresh library
    uint16_t maxBooks = library::MAX_BOOKS;
    uint16_t republishEvery = 100;
    bool (*isBook)(const char* name) = nullptr;
    Resolver resolve = nullptr;
    void* resolveUser = nullptr;
  };

  enum class Phase : uint8_t { Walk, Join, Publish, Resolve, Done, Failed };

  explicit LibraryBuilder(Config config);

  Phase step(BuildArena* arena);
  Phase phase() const { return phase_; }
  bool finished() const { return phase_ == Phase::Done || phase_ == Phase::Failed; }
  bool needsArena() const { return phase_ == Phase::Join || phase_ == Phase::Publish || phase_ == Phase::Resolve; }
  // The next step replaces the index file: a reader holding it open must close it first.
  bool nextStepPublishes() const { return phase_ == Phase::Publish; }
  // True once after each publish, for the screen to reopen the index.
  bool takePublished();
  uint16_t booksFound() const { return found_; }
  uint16_t pending() const { return pending_; }
  uint16_t resolved() const { return resolved_; }

 private:
  static constexpr size_t MAX_DEPTH = 8;
  static constexpr size_t MAX_SIDECARS = 128;
  static constexpr int WALK_BUDGET = 32;

  struct Sidecar {
    uint32_t stemHash;
    uint32_t sig;
  };
  struct Level {
    HalFile dir;
    std::string path;
    bool booksPass = false;  // false: noting the folder's sidecars; true: staging books, going down
    bool overflow = false;   // the folder had more sidecars than MAX_SIDECARS
    std::vector<Sidecar> sidecars;
  };

  std::string work(const char* name) const;
  bool startWalk();
  void walkStep();
  bool listable(const char* name, bool atRoot) const;
  void noteSidecar(Level& level, HalFile& entry);
  uint32_t sidecarFor(const Level& level) const;
  bool stageBook(const Level& level, HalFile& entry);
  void join(BuildArena& arena);
  void publish(BuildArena& arena);
  void resolveStep(BuildArena& arena);
  bool openResolveFiles();
  void closeResolveFiles();
  void removeWorkingFiles() const;
  void fail(const char* what);

  Config config_;
  Phase phase_ = Phase::Walk;
  bool started_ = false;
  bool partial_ = false;
  bool published_ = false;
  bool resolveOpen_ = false;
  std::vector<Level> levels_;
  HalFile stage_;
  HalFile paths_;
  HalFile records_;
  HalFile names_;
  uint32_t pathsBytes_ = 0;
  uint32_t buildGen_ = 0;
  uint16_t found_ = 0;
  uint16_t pending_ = 0;
  uint16_t resolved_ = 0;
  uint16_t cursor_ = 0;
  uint16_t sincePublish_ = 0;
  char name_[500] = {};
};
```

`lib/LibraryIndex/LibraryBuilder.cpp`:

```cpp
#include "LibraryBuilder.h"

#include <Logging.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

#include "LibraryBlob.h"
#include "LibraryJoin.h"
#include "LibraryKeys.h"
#include "LibraryPublish.h"

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

// What a book and its metadata sidecar share: the filename before its extension.
uint32_t stemHash(const char* name) {
  const char* dot = std::strrchr(name, '.');
  const size_t len = (dot != nullptr && dot != name) ? static_cast<size_t>(dot - name) : std::strlen(name);
  return fnv1a(FNV_BASIS, name, len);
}

// The extensions SidecarFiles::kMetadataExtensions resolves.
bool isMetadataSidecar(const char* name) {
  const size_t len = std::strlen(name);
  return len > 4 && (std::strcmp(name + len - 4, ".opf") == 0 || std::strcmp(name + len - 4, ".OPF") == 0);
}

// The later of an entry's modified and created times, as FAT (date << 16) | time.
uint32_t packDate(HalFile& entry) {
  uint16_t date = 0;
  uint16_t time = 0;
  uint32_t latest = 0;
  if (entry.getModifyDateTime(&date, &time)) latest = (uint32_t{date} << 16) | time;
  if (entry.getCreateDateTime(&date, &time)) latest = std::max(latest, (uint32_t{date} << 16) | time);
  return latest;
}

std::string childPath(const std::string& parent, const char* name) {
  return (parent.empty() || parent.back() == '/') ? parent + name : parent + '/' + name;
}

}  // namespace

LibraryBuilder::LibraryBuilder(Config config) : config_(std::move(config)) { levels_.reserve(MAX_DEPTH + 1); }

std::string LibraryBuilder::work(const char* name) const { return config_.workDir + "/" + name; }

bool LibraryBuilder::takePublished() {
  const bool was = published_;
  published_ = false;
  return was;
}

void LibraryBuilder::fail(const char* what) {
  LOG_ERR("LIB", "library build failed: %s", what);
  levels_.clear();
  if (resolveOpen_) closeResolveFiles();
  phase_ = Phase::Failed;
}

LibraryBuilder::Phase LibraryBuilder::step(BuildArena* arena) {
  if (finished()) return phase_;
  if (needsArena() && (arena == nullptr || !arena->valid())) return phase_;
  switch (phase_) {
    case Phase::Walk:
      walkStep();
      break;
    case Phase::Join:
      join(*arena);
      break;
    case Phase::Publish:
      publish(*arena);
      break;
    case Phase::Resolve:
      resolveStep(*arena);
      break;
    default:
      break;
  }
  return phase_;
}

bool LibraryBuilder::startWalk() {
  started_ = true;
  if (!Storage.exists(config_.workDir.c_str())) Storage.mkdir(config_.workDir.c_str());
  if (!Storage.openFileForWrite("LIB", work("stage.bin"), stage_) ||
      !Storage.openFileForWrite("LIB", work("paths.bin"), paths_)) {
    return false;
  }
  HalFile root = Storage.open(config_.root.c_str());
  if (!root || !root.isDirectory()) return false;
  root.rewindDirectory();
  levels_.push_back(Level{std::move(root), config_.root});
  return true;
}

bool LibraryBuilder::listable(const char* name, const bool atRoot) const {
  if (name[0] == '.') {
    if (!config_.showHidden) return false;
    if (atRoot && std::strcmp(name, ".crosspoint") == 0) return false;  // the firmware's own cache
  }
  return std::strcmp(name, "System Volume Information") != 0;
}

void LibraryBuilder::noteSidecar(Level& level, HalFile& entry) {
  if (level.sidecars.size() >= MAX_SIDECARS) {
    level.overflow = true;
    return;
  }
  const auto size = static_cast<uint32_t>(entry.fileSize());
  const uint32_t date = packDate(entry);
  uint32_t sig = fnv1a(fnv1a(FNV_BASIS, &size, sizeof(size)), &date, sizeof(date));
  if (sig == library::SIDECAR_NONE || sig == library::SIDECAR_UNKNOWN) sig = 1;
  level.sidecars.push_back({stemHash(name_), sig});
}

uint32_t LibraryBuilder::sidecarFor(const Level& level) const {
  const uint32_t stem = stemHash(name_);
  for (const Sidecar& sidecar : level.sidecars) {
    if (sidecar.stemHash == stem) return sidecar.sig;
  }
  return level.overflow ? library::SIDECAR_UNKNOWN : library::SIDECAR_NONE;
}

bool LibraryBuilder::stageBook(const Level& level, HalFile& entry) {
  const std::string path = childPath(level.path, name_);
  if (path.size() > library::MAX_STRING) return true;  // a path the index cannot hold is left out
  const auto size = static_cast<uint32_t>(entry.fileSize());
  const library::StagedBook book{LibraryKeys::bookIdentity(name_, size), packDate(entry), sidecarFor(level),
                                 pathsBytes_};
  if (!library::writeBlobString(paths_, path) || !library::writeExact(stage_, &book, sizeof(book))) return false;
  pathsBytes_ += static_cast<uint32_t>(sizeof(uint16_t) + path.size());
  ++found_;
  return true;
}

void LibraryBuilder::walkStep() {
  if (!started_ && !startWalk()) return fail("cannot start the walk");
  for (int budget = WALK_BUDGET; budget > 0 && !levels_.empty(); --budget) {
    Level& level = levels_.back();
    HalFile entry = level.dir.openNextFile();
    if (!entry) {
      if (!level.booksPass) {
        level.booksPass = true;  // the folder's sidecars are known: now its books
        level.dir.rewindDirectory();
      } else {
        levels_.pop_back();
      }
      continue;
    }
    entry.getName(name_, sizeof(name_));
    const bool isDir = entry.isDirectory();
    if (!listable(name_, levels_.size() == 1)) continue;
    if (!level.booksPass) {
      if (!isDir && isMetadataSidecar(name_)) noteSidecar(level, entry);
      continue;
    }
    if (isDir) {
      if (levels_.size() > MAX_DEPTH) continue;
      std::string child = childPath(level.path, name_);
      HalFile sub = Storage.open(child.c_str());
      if (!sub || !sub.isDirectory()) continue;
      sub.rewindDirectory();
      levels_.push_back(Level{std::move(sub), std::move(child)});  // reserved: `level` stays valid
      continue;
    }
    if (config_.isBook == nullptr || !config_.isBook(name_)) continue;
    if (!stageBook(level, entry)) return fail("cannot stage a book");
    if (found_ >= config_.maxBooks) {
      partial_ = true;
      levels_.clear();
    }
  }
  if (!levels_.empty()) return;
  if (!stage_.close() || !paths_.close()) return fail("cannot finish the walk");
  phase_ = Phase::Join;
}

void LibraryBuilder::join(BuildArena& arena) {
  LibraryJoin::Input in;
  in.stagePath = work("stage.bin");
  in.stageCount = found_;
  in.previousPath = config_.indexPath;
  in.resolveAll = config_.resolveAll;
  in.recordsPath = work("records.bin");
  in.namesPath = work("names.bin");
  LibraryJoin::Result result;
  if (!LibraryJoin::join(in, arena, result)) return fail("join");
  pending_ = result.pending;
  buildGen_ = result.buildGen;
  phase_ = Phase::Publish;
}

void LibraryBuilder::publish(BuildArena& arena) {
  LibraryPublish::Input in;
  in.recordsPath = work("records.bin");
  in.pathsPath = work("paths.bin");
  in.namesPath = work("names.bin");
  in.bookCount = found_;
  in.buildGen = buildGen_;
  in.acceptRules = config_.showHidden ? 1 : 0;
  in.partial = partial_;
  if (!LibraryPublish::publish(in, arena, config_.indexPath)) return fail("publish");
  published_ = true;
  sincePublish_ = 0;
  if (pending_ > 0 && cursor_ < found_) {
    phase_ = Phase::Resolve;
    return;
  }
  removeWorkingFiles();
  phase_ = Phase::Done;
}

bool LibraryBuilder::openResolveFiles() {
  if (!Storage.openFileForUpdate("LIB", work("records.bin"), records_) ||
      !Storage.openFileForRead("LIB", work("paths.bin"), paths_) ||
      !Storage.openFileForUpdate("LIB", work("names.bin"), names_)) {
    return false;
  }
  resolveOpen_ = true;
  return names_.seek(names_.fileSize());  // names are appended
}

void LibraryBuilder::closeResolveFiles() {
  records_.close();
  paths_.close();
  names_.close();
  resolveOpen_ = false;
}

void LibraryBuilder::resolveStep(BuildArena& arena) {
  if (!resolveOpen_ && !openResolveFiles()) return fail("cannot open the working files");
  library::BookRecord record{};
  while (cursor_ < found_) {
    if (!records_.seek(uint32_t{cursor_} * sizeof(record)) || !library::readExact(records_, &record, sizeof(record))) {
      return fail("cannot read a record");
    }
    if (record.authorHash == library::AUTHOR_PENDING) break;
    ++cursor_;
  }
  if (cursor_ >= found_) {  // every pending book has had its turn
    closeResolveFiles();
    phase_ = Phase::Publish;
    return;
  }
  std::string path;
  if (!paths_.seek(record.pathOff) || !library::readBlobString(paths_, path)) return fail("cannot read a path");
  uint32_t size = 0;
  {
    HalFile book = Storage.open(path.c_str());
    if (book) size = static_cast<uint32_t>(book.fileSize());
  }
  const uint16_t at = cursor_++;
  Author author;
  arena.reset();
  if (config_.resolve == nullptr || !config_.resolve(config_.resolveUser, path, size, author, &arena)) return;
  const uint32_t hash = LibraryKeys::authorHash(author.name);
  if (!records_.seek(uint32_t{at} * sizeof(record) + offsetof(library::BookRecord, authorHash)) ||
      !library::writeExact(records_, &hash, sizeof(hash))) {
    return fail("cannot patch a record");
  }
  if (hash != library::AUTHOR_UNKNOWN &&
      !library::appendNameEntry(names_, hash, !author.fileAs.empty(), author.name,
                                LibraryKeys::authorSortKey(author.name, author.fileAs))) {
    return fail("cannot record a name");
  }
  --pending_;
  ++resolved_;
  if (++sincePublish_ >= config_.republishEvery) {
    closeResolveFiles();
    phase_ = Phase::Publish;
  }
}

void LibraryBuilder::removeWorkingFiles() const {
  for (const char* name : {"stage.bin", "paths.bin", "records.bin", "names.bin"}) Storage.remove(work(name).c_str());
}
```

- [ ] **Step 5: Run the tests**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . --target LibraryIndexTest -j 8 2>&1 | grep -E "error:|warning:" | cut -c1-200 | head -5; ./library_index/LibraryIndexTest.exe 2>&1 | grep -E "^\[  (FAILED|PASSED) "
```
Expected: `[  PASSED  ] 55 tests.` (42 plus 13).

- [ ] **Step 6: Run the rest of the suites that use the shim**

```bash
cd /c/_development/witchhunt-reader/build/test && cmake --build . -j 8 -- -k 0 2>&1 | grep -E "error:" | cut -c1-150; ctest -j 8 2>&1 | grep -E "tests passed|tests failed"
```
Expected: `100% tests passed out of 1842`. The shim change affects every suite that includes it.

- [ ] **Step 7: Commit**

```bash
cd /c/_development/witchhunt-reader
git add test/zip_entry_reader/HalStorage.h lib/LibraryIndex/LibraryBuilder.h lib/LibraryIndex/LibraryBuilder.cpp test/library_index/LibraryBuilderTest.cpp test/library_index/CMakeLists.txt
git commit -m "feat(library): the index builder

Walks the card a few dozen entries per step (sidecars paired per folder,
hidden entries by Browse Files' rule, the firmware cache never), joins,
publishes, then resolves pending authors one book per step through an
injected resolver, patching records in place and publishing again every
hundred books. The host shim's directory entries now carry size and date."
```

---

### Task 3: Model modes, freshness, hosting (device code)

**Files:**
- Create: `src/activities/home/LibraryFreshness.h`, `src/activities/home/LibraryFreshness.cpp`
- Modify: `src/activities/home/FileBrowserModel.h`, `src/activities/home/FileBrowserModel.cpp`
- Modify: `src/activities/home/FileBrowserActivity.h`, `src/activities/home/FileBrowserActivity.cpp`
- Modify: `lib/I18n/translations/english.yaml`

**Interfaces:**
- Consumes: `LibraryIndexReader`, `LibraryBuilder`, `BookDetailsLookup::cached/parse`, `BookDetails::primaryAuthor/authorSort`.
- Produces (for Plan 3):
  - `FileBrowserModel::Mode::Added`, `Mode::Authors`
  - `static bool FileBrowserModel::isBookName(const char*)`
  - `bool atAuthorList() const`, `bool openAuthor(size_t row)`, `size_t closeAuthor()`, `bool authorAt(size_t row, uint16_t& books, uint32_t& hash)`
  - `void releaseIndex()`, `const LibraryIndexReader& index() const`
  - `LibraryFreshness::stale(const LibraryIndexReader&)`, `LibraryFreshness::built(uint32_t generationAtStart)`
  - `FileBrowserActivity` hosts the builder while in Added or Authors mode.

There are no host tests for this task: `FileBrowserModel` and the activity depend on Arduino, the settings and the renderer. It is verified by the firmware build in Task 4 and by the device checks once Plan 3 makes the modes reachable.

- [ ] **Step 1: Strings**

In `lib/I18n/translations/english.yaml`, after the line `STR_NO_RECENT_BOOKS: "No recent books"`, add:

```yaml
STR_UNKNOWN_AUTHOR: "Unknown author"
STR_NOT_YET_INDEXED: "Not yet indexed"
```

Do not run `scripts/gen_i18n.py` by hand. The build regenerates the strings (see memory note i18n-manual-regen-trap).

- [ ] **Step 2: Freshness**

`src/activities/home/LibraryFreshness.h`:

```cpp
#pragma once

#include <cstdint>

class LibraryIndexReader;

// When the book index has to be built again before New or Authors show it (spec section 3.6).
namespace LibraryFreshness {

// True when there is no valid index, no build has finished since boot (a card edited elsewhere while
// the device was off or asleep is invisible otherwise), the card has changed since the last build
// started, or Browse Files' hidden-files setting differs from the one the index was built with.
bool stale(const LibraryIndexReader& index);

// A build that started at `generationAtStart` (HalStorage::contentGeneration) has finished.
void built(uint32_t generationAtStart);

}  // namespace LibraryFreshness
```

`src/activities/home/LibraryFreshness.cpp`:

```cpp
#include "LibraryFreshness.h"

#include <HalStorage.h>
#include <LibraryIndexReader.h>

#include "CrossPointSettings.h"

namespace {
bool builtThisBoot = false;
uint32_t generationOfLastBuild = 0;
}  // namespace

namespace LibraryFreshness {

bool stale(const LibraryIndexReader& index) {
  return !index.isOpen() || !builtThisBoot || generationOfLastBuild != Storage.contentGeneration() ||
         index.header().acceptRules != (SETTINGS.showHiddenFiles ? 1 : 0);
}

void built(const uint32_t generationAtStart) {
  builtThisBoot = true;
  generationOfLastBuild = generationAtStart;
}

}  // namespace LibraryFreshness
```

- [ ] **Step 3: Model modes**

In `src/activities/home/FileBrowserModel.h`:
- Add `#include <LibraryIndexReader.h>` with the other includes.
- Change the enum to `enum class Mode { Books, AllFiles, PickFirmware, PickFolder, Recents, Added, Authors };`.
- Extend the comment above it with:

```cpp
  // Added lists the books added to the card most recently (the book index's New); Authors lists the
  // book index's authors and, once one is opened, that author's books. Both read the index that
  // LibraryBuilder keeps.
```

- Replace `listsPaths()` with:

```cpp
  [[nodiscard]] bool listsPaths() const {
    return deepSearch || mode == Mode::Recents || mode == Mode::Added || (mode == Mode::Authors && openAuthorRow >= 0);
  }
```

- In the public section, after `listsPaths()`, add:

```cpp
  // A readable book's filename, by the rule Browse Files lists books with; for the library builder.
  static bool isBookName(const char* name);

  // Authors mode, showing the author list rather than one author's books.
  [[nodiscard]] bool atAuthorList() const { return mode == Mode::Authors && openAuthorRow < 0; }
  // Shows the books of the author at `row` of the author list, by series, series index and title.
  // False when there is no such row.
  bool openAuthor(size_t row);
  // Back from an author's books to the author list; returns the row of the author that was open.
  size_t closeAuthor();
  // A row of the author list: its books, and its author hash (library::AUTHOR_UNKNOWN and
  // AUTHOR_PENDING gather books with no author and books not indexed yet).
  bool authorAt(size_t row, uint16_t& books, uint32_t& hash);
  // The book index New and Authors read. Let go before the builder replaces it; load() reopens it.
  void releaseIndex();
  [[nodiscard]] const LibraryIndexReader& index() const { return bookIndex; }
```

- In the private section, add:

```cpp
  void loadAdded();
  void loadAuthors();
  void orderAuthorBooks();
  std::string authorRowName(size_t row);
  std::string authorBookName(size_t index);

  LibraryIndexReader bookIndex;
  int openAuthorRow = -1;
  uint32_t openAuthorHash = 0;          // to find the open author again after the index is rebuilt
  std::vector<uint16_t> authorBooks;  // the open author's records, in display order
```

In `src/activities/home/FileBrowserModel.cpp`:
- Add `#include <I18n.h>` and `#include "BookDetails.h"` with the other includes.
- In `load()`, after `fileDateTimes.clear();`, add:

```cpp
  if (mode == Mode::Added) {
    loadAdded();
    return;
  }
  if (mode == Mode::Authors) {
    loadAuthors();
    return;
  }
```

- In `entryCount()`, insert as the first line: `if (mode == Mode::Authors) return atAuthorList() ? bookIndex.header().authorCount : authorBooks.size();`
- In `entryName()`, insert as the first line: `if (mode == Mode::Authors) return atAuthorList() ? authorRowName(displayIndex) : authorBookName(displayIndex);`
- In `findEntry()`, insert as the first lines:

```cpp
  if (mode == Mode::Authors) {
    const size_t count = entryCount();
    for (size_t i = 0; i < count; i++)
      if (entryName(i) == name) return i;
    return count;
  }
```

- In `resultFolder()`, replace `if (!listsPaths() || displayIndex >= deepResults.size()) return "";` and the line after it with:

```cpp
  if (!listsPaths() || displayIndex >= entryCount()) return "";
  const std::string rel = entryName(displayIndex);
```

- In `resort()`, change the first line to: `if (fileIndex || listsPaths() || mode == Mode::Authors) return;  // ordered at build time / by the index`
- In `clear()`, after `clearDeepSearch();`, add:

```cpp
  bookIndex.close();
  openAuthorRow = -1;
  authorBooks.clear();
  authorBooks.shrink_to_fit();
```

- Append the new functions at the end of the file:

```cpp
bool FileBrowserModel::isBookName(const char* name) { return isReadableBook(std::string_view{name}); }

void FileBrowserModel::releaseIndex() { bookIndex.close(); }

// The index's newest books, as paths from the root like Recents' rows. A book gone since the index
// was built is left out.
void FileBrowserModel::loadAdded() {
  clearDeepSearch();
  deepRoot = "/";
  if (!bookIndex.isOpen() && !bookIndex.open(library::INDEX_PATH)) return;
  std::string path;
  library::BookRecord record{};
  for (uint16_t rank = 0; rank < bookIndex.header().newCount; ++rank) {
    uint16_t index = 0;
    if (!bookIndex.newBook(rank, index) || !bookIndex.book(index, record) || !bookIndex.blobString(record.pathOff, path)) {
      continue;
    }
    if (path.size() > 1 && path.front() == '/' && Storage.exists(path.c_str())) deepResults.push_back(path.substr(1));
  }
}

// The author list, or -- when an author was open -- that author's books again, found by hash: a
// rebuilt index may have moved its row.
void FileBrowserModel::loadAuthors() {
  clearDeepSearch();
  deepRoot = "/";
  if (!bookIndex.isOpen()) bookIndex.open(library::INDEX_PATH);
  if (openAuthorRow < 0) return;
  openAuthorRow = -1;
  authorBooks.clear();
  library::AuthorRecord author{};
  for (uint16_t row = 0; row < bookIndex.header().authorCount; ++row) {
    if (bookIndex.author(row, author) && author.hash == openAuthorHash) {
      openAuthor(row);
      return;
    }
  }
}

bool FileBrowserModel::openAuthor(const size_t row) {
  library::AuthorRecord author{};
  if (!atAuthorList() || row > UINT16_MAX || !bookIndex.author(static_cast<uint16_t>(row), author)) return false;
  authorBooks.clear();
  authorBooks.reserve(author.count);
  for (uint32_t slot = author.firstBook; slot < uint32_t{author.firstBook} + author.count; ++slot) {
    uint16_t record = 0;
    if (bookIndex.authorBook(slot, record)) authorBooks.push_back(record);
  }
  openAuthorRow = static_cast<int>(row);
  openAuthorHash = author.hash;
  deepRoot = "/";
  orderAuthorBooks();
  return true;
}

size_t FileBrowserModel::closeAuthor() {
  const int row = openAuthorRow;
  openAuthorRow = -1;
  authorBooks.clear();
  authorBooks.shrink_to_fit();
  return row < 0 ? 0 : static_cast<size_t>(row);
}

bool FileBrowserModel::authorAt(const size_t row, uint16_t& books, uint32_t& hash) {
  library::AuthorRecord author{};
  if (row > UINT16_MAX || !bookIndex.author(static_cast<uint16_t>(row), author)) return false;
  books = author.count;
  hash = author.hash;
  return true;
}

// An author row, marked as a folder: opening it lists the author's books.
std::string FileBrowserModel::authorRowName(const size_t row) {
  library::AuthorRecord author{};
  std::string name;
  if (row > UINT16_MAX || !bookIndex.author(static_cast<uint16_t>(row), author)) return "";
  if (author.hash == library::AUTHOR_PENDING) {
    name = tr(STR_NOT_YET_INDEXED);
  } else if (author.hash == library::AUTHOR_UNKNOWN || !bookIndex.authorName(author, name) || name.empty()) {
    name = tr(STR_UNKNOWN_AUTHOR);
  }
  return name + '/';
}

std::string FileBrowserModel::authorBookName(const size_t index) {
  library::BookRecord record{};
  std::string path;
  if (index >= authorBooks.size() || !bookIndex.book(authorBooks[index], record) ||
      !bookIndex.blobString(record.pathOff, path) || path.size() < 2) {
    return "";
  }
  return path.substr(1);
}

// An author's books by series, then series index, then title, as the book lists show them; books in
// no series after the series. Past MAX_ORDERED books the index's order stands: the details reads
// would take seconds.
void FileBrowserModel::orderAuthorBooks() {
  constexpr size_t MAX_ORDERED = 200;
  if (authorBooks.size() < 2 || authorBooks.size() > MAX_ORDERED) return;
  struct Key {
    std::string series;
    float index;
    std::string title;
    uint16_t record;
  };
  std::vector<Key> keys;
  keys.reserve(authorBooks.size());
  library::BookRecord record{};
  std::string path;
  for (const uint16_t r : authorBooks) {
    Key key{{}, 0.0f, {}, r};
    if (bookIndex.book(r, record) && bookIndex.blobString(record.pathOff, path)) {
      BookDetails details;
      if (BookDetailsLookup::cached(path, 0, details)) {
        key.series = std::move(details.series);
        key.index = strtof(details.seriesIndex.c_str(), nullptr);
        key.title = std::move(details.title);
      }
      if (key.title.empty()) key.title = path.substr(path.rfind('/') + 1);
    }
    keys.push_back(std::move(key));
  }
  std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
    if (a.series.empty() != b.series.empty()) return !a.series.empty();
    if (a.series != b.series) return FsHelpers::naturalCompare(a.series.c_str(), b.series.c_str()) < 0;
    if (a.index != b.index) return a.index < b.index;
    return FsHelpers::naturalCompare(a.title.c_str(), b.title.c_str()) < 0;
  });
  for (size_t i = 0; i < keys.size(); ++i) authorBooks[i] = keys[i].record;
}
```

`isReadableBook` lives in the file's anonymous namespace near the top. `isBookName` is defined after it in the same file, so it can call it.

- [ ] **Step 4: Hosting**

In `src/activities/home/FileBrowserActivity.h`:
- Add the forward declaration `class LibraryBuilder;` next to `class CoverThumbLoader;`.
- In the private section, after `void returnLentBuffer(bool callerHoldsRenderLock);`, add:

```cpp
  // The book index build that New and Authors run when the index is stale (LibraryFreshness). It
  // gets the lent framebuffer after the title parses and before the covers.
  std::unique_ptr<LibraryBuilder> libraryBuilder;
  uint32_t libraryBuildGeneration = 0;
  void startLibraryBuildIfStale();
  bool stepLibraryBuild();
```

In `src/activities/home/FileBrowserActivity.cpp`:
- Add `#include <LibraryBuilder.h>`, `#include "BookDetails.h"` and `#include "LibraryFreshness.h"` with the other includes.
- Add this anonymous-namespace helper above `FileBrowserActivity::FileBrowserActivity(`:

```cpp
namespace {
// The builder's resolver: the author the book lists already show, from details.bin when it is there
// and from the book (and its sidecar) when not. False -- try again next build -- when that parse
// could not run just now.
bool resolveAuthor(void*, const std::string& path, const uint32_t size, LibraryBuilder::Author& out,
                   BuildArena* scratch) {
  BookDetails details;
  if (!BookDetailsLookup::cached(path, size, details) && !BookDetailsLookup::parse(path, size, details, scratch)) {
    return false;
  }
  out.name = std::move(details.primaryAuthor);
  out.fileAs = std::move(details.authorSort);
  return true;
}
}  // namespace
```

- In `onEnter()`, after `model.load();`, add `startLibraryBuildIfStale();`.
- In `onExit()`, as the first line after `UiListActivity::onExit();`, add `libraryBuilder.reset();  // abandoned: the next visit walks again; details.bin keeps the resolves`.
- In `loop()`, replace the final `generateCovers();` with:

```cpp
  if (stepLibraryBuild()) return;
  generateCovers();
```

- Append:

```cpp
void FileBrowserActivity::startLibraryBuildIfStale() {
  if (model.getMode() != Mode::Added && model.getMode() != Mode::Authors) return;
  if (libraryBuilder || !LibraryFreshness::stale(model.index())) return;
  LibraryBuilder::Config config;
  config.showHidden = SETTINGS.showHiddenFiles;
  config.isBook = &FileBrowserModel::isBookName;
  config.resolve = &resolveAuthor;
  libraryBuilder = makeUniqueNoThrow<LibraryBuilder>(std::move(config));
  libraryBuildGeneration = Storage.contentGeneration();
}

// One step of the index build, when there is one and nothing more urgent: true while it runs, so the
// covers wait. The builder resets the lent framebuffer at each step, so the cover loader -- its
// other user -- is stopped first; and the model's index is let go before a publish replaces it.
bool FileBrowserActivity::stepLibraryBuild() {
  if (!libraryBuilder) return false;
  if (renderer.isComposingFrame() || mappedInput.hasPendingInput()) return true;
  if (libraryBuilder->needsArena()) {
    coverLoader->reset();
    if (!lendForBackgroundWork()) return true;
  }
  if (libraryBuilder->nextStepPublishes()) {
    RenderLock lock(*this);
    model.releaseIndex();
  }
  HalPowerManager::Lock fullSpeed;
  const LibraryBuilder::Phase phase = libraryBuilder->step(coverScratch.get());
  const bool reload = libraryBuilder->takePublished() || phase == LibraryBuilder::Phase::Failed;
  if (reload) {
    RenderLock lock(*this);
    model.load();
    resetNavigation(std::min(nav.selected, std::max(0, listCount() - 1)));
  }
  if (libraryBuilder->finished()) {
    if (phase == LibraryBuilder::Phase::Done) LibraryFreshness::built(libraryBuildGeneration);
    libraryBuilder.reset();
  }
  if (reload) requestUpdate();
  return true;
}
```

- [ ] **Step 5: Firmware build**

Run in the background:

```bash
export PLATFORMIO_CORE_DIR='C:\pio' PYTHONUTF8=1 PYTHONIOENCODING=utf-8; cd /c/_development/witchhunt-reader && "$HOME/.platformio/penv/Scripts/pio.exe" run -e default > /tmp/pio-2b.log 2>&1; tail -5 /tmp/pio-2b.log; grep -E "^(RAM|Flash):" /tmp/pio-2b.log; cp .pio/build/default/firmware.map /tmp/plan2b-default.map
```
Expected: SUCCESS. Fix any compile error at its source; do not change the plan's behaviour to make it compile.

- [ ] **Step 6: Commit**

```bash
cd /c/_development/witchhunt-reader
git add src/activities/home/LibraryFreshness.h src/activities/home/LibraryFreshness.cpp src/activities/home/FileBrowserModel.h src/activities/home/FileBrowserModel.cpp src/activities/home/FileBrowserActivity.h src/activities/home/FileBrowserActivity.cpp lib/I18n/translations/english.yaml
git commit -m "feat(library): New and Authors modes; the browser builds the index

FileBrowserModel reads the book index for two new modes: Added (New, the
newest books as path rows) and Authors (the author list, then an author's
books by series and title). FileBrowserActivity builds the index when it is
stale (no build this boot, card changed, hidden-files setting changed),
giving the builder the lent framebuffer after titles and before covers."
```

---

### Task 4: Verification and the cost gate

- [ ] **Step 1: Format and full host suite**

```bash
cd /c/_development/witchhunt-reader && for f in $(git diff --name-only 8f535140e...HEAD -- '*.cpp' '*.h'); do "/c/tmp/cf21/bin/clang-format-21.exe" -i --style=file "$f"; done; git status --short | grep -v "^??"; cd build/test && cmake --build . -j 8 -- -k 0 2>&1 | grep -E "error:" | cut -c1-150; ctest -j 8 2>&1 | grep -E "tests passed|tests failed"
```
Expected: 1842/1842. Commit any formatting.

- [ ] **Step 2: Measure for the cost gate**

From the `default` build's map, attribute flash to `/libraryindex/`, `librarybuilder`, `libraryfreshness`, `filebrowsermodel` and `filebrowseractivity` with the map attribution script. Also compare the total `Flash:` line with Plan 1's build (6,231,733 B) and with the baseline (6,227,427 B). Report:

| | Flash |
|---|---|
| ours, data side so far | Plan 1 + Plan 2 |
| upstream Library | 53,318 B (+ compose table ~5.1 KB) |

If ours is not substantially lower, **stop and ask the user** before Plan 3.

- [ ] **Step 3: X4 Pro and T5S3 builds**

Run `x4pro` and `lilygo_t5s3`, one at a time. Both must succeed.
