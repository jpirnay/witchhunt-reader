// HalFile handles from lib/hal/HalStorage.cpp, built as the firmware builds it, over SdFat on an
// in-memory card. Whatever an open returns, its handle may be closed: an open that fails holds the
// Impl all failed opens share, costing no heap, and an open whose own Impl cannot be had fails like
// any other instead of aborting the device, an open for writing before it touches the card. A
// handle never opened keeps its assert. A remove deletes a file the card marks read-only.
// As in HalStorage.cpp: FsFile is SdFat's here, which the test mounts and writes through.
#define HAL_STORAGE_IMPL
#include <HalClock.h>
#include <HalSpiBus.h>
#include <HalStorage.h>
#include <SDCardManager.h>
#include <SdFat.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>

// --- What HalStorage.cpp links against ---------------------------------------------------------

HalSpiBus::Lock::Lock() {}
HalSpiBus::Lock::~Lock() {}

namespace HalClock {
bool isSynced() { return false; }
time_t now() { return 0; }
}  // namespace HalClock

// The heap: every allocation counted, and the nothrow ones refused on demand. Only those: a
// throwing new that fails aborts the device (-fno-exceptions), so the HAL must not rely on one.
namespace {
bool refuseNothrowNew = false;
size_t heapNews = 0;
size_t heapDeletes = 0;
}  // namespace

void* operator new(const std::size_t size) {
  ++heapNews;
  if (void* block = std::malloc(size != 0 ? size : 1)) return block;
  throw std::bad_alloc();
}
void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  if (refuseNothrowNew) return nullptr;
  ++heapNews;
  return std::malloc(size != 0 ? size : 1);
}
void operator delete(void* block) noexcept {
  if (block) ++heapDeletes;
  std::free(block);
}
void operator delete(void* block, std::size_t) noexcept {
  if (block) ++heapDeletes;
  std::free(block);
}

namespace {

// Keeps only the sectors that are not all zero.
class RamCard final : public FsBlockDeviceInterface {
 public:
  explicit RamCard(const Sector_t sectors) : sectors_(sectors) {}

  bool isBusy() override { return false; }
  bool readSector(const Sector_t sector, uint8_t* dst) override {
    if (sector >= sectors_) return false;
    const auto it = data_.find(sector);
    if (it == data_.end()) {
      memset(dst, 0, 512);
    } else {
      memcpy(dst, it->second.data(), 512);
    }
    return true;
  }
  bool readSectors(const Sector_t sector, uint8_t* dst, const size_t count) override {
    for (size_t i = 0; i < count; ++i) {
      if (!readSector(sector + i, dst + 512 * i)) return false;
    }
    return true;
  }
  Sector_t sectorCount() override { return sectors_; }
  bool syncDevice() override { return true; }
  bool writeSector(const Sector_t sector, const uint8_t* src) override {
    if (sector >= sectors_) return false;
    memcpy(data_[sector].data(), src, 512);
    return true;
  }
  bool writeSectors(const Sector_t sector, const uint8_t* src, const size_t count) override {
    for (size_t i = 0; i < count; ++i) {
      if (!writeSector(sector + i, src + 512 * i)) return false;
    }
    return true;
  }

 private:
  Sector_t sectors_;
  std::unordered_map<Sector_t, std::array<uint8_t, 512>> data_;
};

class HalStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    uint8_t buffer[512];
    FatFormatter formatter;
    ASSERT_TRUE(formatter.format(&card_, buffer, nullptr));
    ASSERT_TRUE(vol_.begin(&card_));
    SDCardManager::getInstance().vol = &vol_;
    ASSERT_TRUE(Storage.begin());
    refuseNothrowNew = false;
    heapNews = 0;
    heapDeletes = 0;
  }
  void TearDown() override {
    refuseNothrowNew = false;
    EXPECT_EQ(hostSemaphoreDepth(), 0) << "a StorageLock was not given back";
    SDCardManager::getInstance().vol = nullptr;
  }

  // A file of `text` at `path`, written straight through SdFat.
  void put(const char* path, const std::string& text) {
    FsFile file = vol_.open(path, O_RDWR | O_CREAT | O_TRUNC);
    ASSERT_TRUE(file);
    ASSERT_EQ(file.write(text.data(), text.size()), text.size());
    ASSERT_TRUE(file.close());
  }

  // The file at `path` as SdFat reads it; "<none>" when there is none.
  std::string contents(const char* path) {
    FsFile file = vol_.open(path, O_RDONLY);
    if (!file) return "<none>";
    std::string text(static_cast<size_t>(file.fileSize()), '\0');
    EXPECT_EQ(file.read(text.data(), text.size()), static_cast<int>(text.size()));
    file.close();
    return text;
  }

  // The user-settable attributes of `path`, as a PC sets them: through a handle open for reading,
  // since SdFat sets no read-only flag on one open for writing.
  void setAttributes(const char* path, const uint8_t bits) {
    FsFile file = vol_.open(path, O_RDONLY);
    ASSERT_TRUE(file);
    ASSERT_TRUE(file.attrib(bits));
    ASSERT_TRUE(file.close());
  }
  int attributes(const char* path) {
    FsFile file = vol_.open(path, O_RDONLY);
    return file ? file.attrib() : -1;
  }

  RamCard card_{64 * 2048};  // 64 MB
  FsVolume vol_;
};

TEST_F(HalStorageTest, AFileThatOpensHoldsAnImplOfItsOwn) {
  put("/book.txt", "page");
  HalFile file;
  heapNews = 0;

  ASSERT_TRUE(Storage.openFileForRead("TEST", "/book.txt", file));
  EXPECT_EQ(heapNews, 1u);
  char text[8] = {};
  EXPECT_EQ(file.read(text, sizeof(text)), 4);
  EXPECT_STREQ(text, "page");
  EXPECT_TRUE(file.close());
}

TEST_F(HalStorageTest, AFailedOpenTakesNoHeapAndMayBeClosed) {
  HalFile opened = Storage.open("/missing.txt");
  HalFile read;
  HalFile updated;

  EXPECT_FALSE(opened);
  EXPECT_FALSE(Storage.openFileForRead("TEST", "/missing.txt", read));
  EXPECT_FALSE(read);
  EXPECT_FALSE(Storage.openFileForUpdate("TEST", "/missing.txt", updated));
  EXPECT_EQ(heapNews, 0u);
  // As on a closed file: nothing to read, nothing written, and close() is allowed.
  char byte = 0;
  EXPECT_EQ(opened.read(&byte, 1), -1);
  EXPECT_EQ(read.write("x", 1), 0u);
  opened.close();
  read.close();
  updated.close();
}

TEST_F(HalStorageTest, TheEndOfAListingTakesNoHeap) {
  ASSERT_TRUE(vol_.mkdir("/books"));
  put("/books/a.txt", "a");
  put("/books/b.txt", "b");
  HalFile folder = Storage.open("/books");
  ASSERT_TRUE(folder);
  heapNews = 0;

  int listed = 0;
  for (HalFile entry = folder.openNextFile(); entry; entry = folder.openNextFile()) ++listed;

  EXPECT_EQ(listed, 2);
  EXPECT_EQ(heapNews, 2u);  // the entries; the end of the listing took none
}

// What used to abort the device: std::make_unique under -fno-exceptions.
TEST_F(HalStorageTest, AnOpenWhoseImplCannotBeHadFailsAndMayBeClosed) {
  put("/book.txt", "page");
  ASSERT_TRUE(vol_.mkdir("/books"));
  put("/books/a.txt", "a");
  HalFile folder = Storage.open("/books");
  ASSERT_TRUE(folder);
  refuseNothrowNew = true;

  HalFile opened = Storage.open("/book.txt");
  HalFile read;
  HalFile written;
  EXPECT_FALSE(opened);
  EXPECT_FALSE(Storage.openFileForRead("TEST", "/book.txt", read));
  EXPECT_FALSE(read);
  EXPECT_FALSE(Storage.openFileForWrite("TEST", "/new.txt", written));
  EXPECT_FALSE(written);
  EXPECT_FALSE(folder.openNextFile());  // ends the listing
  EXPECT_EQ(written.write("x", 1), 0u);
  opened.close();
  read.close();
  written.close();

  // The files the failed opens had open were closed: once there is heap again, all open as before.
  refuseNothrowNew = false;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/book.txt", read));
  char text[8] = {};
  EXPECT_EQ(read.read(text, sizeof(text)), 4);
  EXPECT_STREQ(text, "page");
  EXPECT_FALSE(Storage.exists("/new.txt"));  // the open for writing failed before it made the file
}

// It used to create or empty the file first and fail after: what an existing file held was lost.
TEST_F(HalStorageTest, AnOpenForWritingWhoseImplCannotBeHadLeavesTheCardAsItWas) {
  put("/book.txt", "page");
  refuseNothrowNew = true;

  HalFile written;
  EXPECT_FALSE(Storage.openFileForWrite("TEST", "/book.txt", written));
  HalFile truncated = Storage.open("/book.txt", O_RDWR | O_TRUNC);
  EXPECT_FALSE(truncated);
  HalFile created = Storage.open("/new.txt", O_RDWR | O_CREAT);
  EXPECT_FALSE(created);
  HalFile alsoCreated;
  EXPECT_FALSE(Storage.openFileForWrite("TEST", "/new-too.txt", alsoCreated));
  written.close();
  truncated.close();
  created.close();
  alsoCreated.close();

  refuseNothrowNew = false;
  EXPECT_EQ(contents("/book.txt"), "page");
  EXPECT_EQ(contents("/new.txt"), "<none>");
  EXPECT_EQ(contents("/new-too.txt"), "<none>");
}

TEST_F(HalStorageTest, AnOpenForWritingHoldsOneImplAndAFailedOneGivesItBack) {
  put("/book.txt", "page");
  HalFile written;
  heapNews = heapDeletes = 0;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", "/book.txt", written));
  EXPECT_EQ(heapNews, 1u);
  EXPECT_EQ(heapDeletes, 0u);
  EXPECT_EQ(written.write("leaf", 4), 4u);
  EXPECT_TRUE(written.close());
  EXPECT_EQ(contents("/book.txt"), "leaf");

  // No such folder: the Impl taken for the open goes back, and the handle holds the shared one.
  HalFile missing;
  heapNews = heapDeletes = 0;
  EXPECT_FALSE(Storage.openFileForWrite("TEST", "/no-folder/book.txt", missing));
  EXPECT_FALSE(missing);
  HalFile created = Storage.open("/no-folder/new.txt", O_RDWR | O_CREAT);
  EXPECT_FALSE(created);
  EXPECT_EQ(heapNews, 2u);
  EXPECT_EQ(heapDeletes, 2u);
  missing.close();
  created.close();
  EXPECT_EQ(heapDeletes, 2u);  // the shared Impl is never freed
}

TEST_F(HalStorageTest, ManyFailedOpensShareOneImplAndNeverFreeIt) {
  for (int i = 0; i < 1000; ++i) {
    HalFile file = Storage.open("/missing.txt");
    HalFile moved = std::move(file);
    file = Storage.open("/missing-too.txt");
    EXPECT_FALSE(moved);
    EXPECT_FALSE(file);
  }
  EXPECT_EQ(heapNews, 0u);
}

// The tripwire stays: a handle no open ever filled is a caller's bug.
TEST_F(HalStorageTest, ClosingAHandleNeverOpenedStillAsserts) {
  EXPECT_DEATH(
      {
        HalFile never;
        never.close();
      },
      "impl != nullptr");
}

// A book a PC marked read-only (#750): SdFat alone will not remove it, the HAL does.
TEST_F(HalStorageTest, AReadOnlyFileIsRemovedAllTheSame) {
  ASSERT_TRUE(vol_.mkdir("/Books"));
  put("/Books/Done.epub", "PK");
  setAttributes("/Books/Done.epub", FS_ATTRIB_READ_ONLY | FS_ATTRIB_ARCHIVE);
  ASSERT_FALSE(vol_.remove("/Books/Done.epub")) << "SdFat removes read-only files now: the HAL's retry is moot";

  EXPECT_TRUE(Storage.remove("/Books/Done.epub"));
  EXPECT_FALSE(vol_.exists("/Books/Done.epub"));
  EXPECT_TRUE(vol_.exists("/Books"));
}

TEST_F(HalStorageTest, ARemoveOfAMissingFileStillFails) {
  EXPECT_FALSE(Storage.remove("/missing.epub"));
  EXPECT_FALSE(Storage.remove("/no-folder/missing.epub"));
}

// remove() is for files: a folder stays, and a read-only one keeps its flag.
TEST_F(HalStorageTest, ARemoveLeavesAFolderAsItWas) {
  ASSERT_TRUE(vol_.mkdir("/Kept"));
  setAttributes("/Kept", FS_ATTRIB_READ_ONLY);

  EXPECT_FALSE(Storage.remove("/Kept"));
  EXPECT_TRUE(vol_.exists("/Kept"));
  EXPECT_EQ(attributes("/Kept") & FS_ATTRIB_READ_ONLY, FS_ATTRIB_READ_ONLY);
}

// --- The at-rest cipher of a book cache folder (lib/CacheCipher/CacheCipherFile.h, D2) -------------------------------

bool testFolderKey(const uint8_t (&salt)[cache_cipher::kSaltBytes], uint8_t (&key)[cache_cipher::kKeyBytes]) {
  for (size_t i = 0; i < sizeof(key); ++i) key[i] = static_cast<uint8_t>(0x3C ^ (i * 7) ^ salt[i % sizeof(salt)]);
  return true;
}

class CacheCipherStorageTest : public HalStorageTest {
 protected:
  static constexpr const char* kFolder = "/.crosspoint/epub_7";
  static constexpr const char* kFile = "/.crosspoint/epub_7/spines/0/html_1.bin";
  static constexpr const char* kText = "Call me Ishmael. Some years ago - never mind how long precisely";

  void SetUp() override {
    HalStorageTest::SetUp();
    Storage.setCacheKeySource(testFolderKey);  // also forgets the folders of the last test's card
    ASSERT_TRUE(vol_.mkdir("/.crosspoint/epub_7/spines/0"));
    ASSERT_TRUE(Storage.markCacheFolderEnciphered(kFolder));
  }
  void TearDown() override {
    Storage.setCacheKeySource(nullptr);
    HalStorageTest::TearDown();
  }

  void write(const char* path, const std::string& text) {
    HalFile file;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", path, file));
    ASSERT_EQ(file.write(text.data(), text.size()), text.size());
    ASSERT_TRUE(file.close());
  }
  std::string read(const char* path) {
    HalFile file;
    if (!Storage.openFileForRead("TEST", path, file)) return "<none>";
    std::string text(file.fileSize(), '\0');
    EXPECT_EQ(file.read(text.data(), text.size()), static_cast<int>(text.size()));
    file.close();
    return text;
  }
};

TEST_F(CacheCipherStorageTest, AFileIsPlainThroughItsHandleAndEncipheredOnTheCard) {
  write(kFile, kText);
  const std::string onCard = contents(kFile);
  ASSERT_EQ(onCard.size(), strlen(kText) + cache_cipher::kHeaderBytes);
  EXPECT_EQ(onCard.compare(0, 4, "WCC1"), 0);
  EXPECT_EQ(onCard.find("Ishmael"), std::string::npos) << "the text is on the card in the clear";
  EXPECT_TRUE(Storage.cacheFolderEnciphered(kFolder));

  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", kFile, file));
  EXPECT_EQ(file.fileSize(), strlen(kText));
  EXPECT_EQ(file.position(), 0u);
  ASSERT_TRUE(file.seek(8));
  EXPECT_EQ(file.position(), 8u);
  EXPECT_EQ(file.available(), static_cast<int>(strlen(kText) - 8));
  char word[8] = {};
  ASSERT_EQ(file.read(word, 7), 7);
  EXPECT_STREQ(word, "Ishmael");
  EXPECT_EQ(file.read(), '.');
  ASSERT_TRUE(file.seekCur(-8));
  EXPECT_EQ(file.read(), 'I');
  file.close();
  EXPECT_EQ(read(kFile), kText);
}

// A patch in place (a section header's offsets) and a rename (every .part file put in place) keep the file readable.
TEST_F(CacheCipherStorageTest, APatchInPlaceAndARenameKeepTheFile) {
  const std::string part = std::string(kFile) + ".part";
  write(part.c_str(), kText);
  ASSERT_TRUE(Storage.rename(part.c_str(), kFile));
  HalFile file;
  ASSERT_TRUE(Storage.openFileForUpdate("TEST", kFile, file));
  ASSERT_TRUE(file.seek(0));
  ASSERT_EQ(file.write("CALL", 4), 4u);
  ASSERT_TRUE(file.close());
  EXPECT_EQ(read(kFile), "CALL" + std::string(kText).substr(4));
}

// The same text written twice is two different files on the card: no keystream serves two writes.
TEST_F(CacheCipherStorageTest, EveryFileWrittenGetsItsOwnKeystream) {
  write(kFile, kText);
  const std::string first = contents(kFile);
  write(kFile, kText);
  const std::string second = contents(kFile);
  EXPECT_NE(first.substr(4, 12), second.substr(4, 12)) << "the nonce was reused";
  EXPECT_NE(first.substr(cache_cipher::kHeaderBytes), second.substr(cache_cipher::kHeaderBytes));
  EXPECT_EQ(read(kFile), kText);
}

// What hands a file to the outside (the web download, the serial transfers) gets the card's bytes.
TEST_F(CacheCipherStorageTest, ARawOpenGivesTheCardsBytes) {
  write(kFile, kText);
  HalFile raw = Storage.openRaw(kFile);
  ASSERT_TRUE(raw);
  std::string bytes(raw.fileSize(), '\0');
  ASSERT_EQ(raw.read(bytes.data(), bytes.size()), static_cast<int>(bytes.size()));
  raw.close();
  EXPECT_EQ(bytes, contents(kFile));
}

TEST_F(CacheCipherStorageTest, TheWholeFileHelpersGoThroughTheCipher) {
  const char* path = "/.crosspoint/epub_7/koreader_docid.txt";
  ASSERT_TRUE(Storage.writeFile(path, String("0123456789abcdef")));
  EXPECT_EQ(contents(path).find("0123456789"), std::string::npos);
  EXPECT_STREQ(Storage.readFile(path).c_str(), "0123456789abcdef");
  char buffer[8];
  EXPECT_EQ(Storage.readFileToBuffer(path, buffer, sizeof(buffer)), 7u);
  EXPECT_STREQ(buffer, "0123456");
}

// Without the folder's key nothing in it opens: not read as if plain, and not emptied by an open for writing.
TEST_F(CacheCipherStorageTest, AFolderWithoutItsKeyOpensNothing) {
  write(kFile, kText);
  const std::string before = contents(kFile);
  Storage.setCacheKeySource(nullptr);
  HalFile file;
  EXPECT_FALSE(Storage.openFileForRead("TEST", kFile, file));
  EXPECT_FALSE(Storage.openFileForWrite("TEST", kFile, file));
  EXPECT_FALSE(Storage.openFileForUpdate("TEST", kFile, file));
  EXPECT_FALSE(Storage.open(kFile, O_RDWR | O_CREAT | O_TRUNC));
  EXPECT_FALSE(Storage.open("/.crosspoint/epub_7/new.bin", O_RDWR | O_CREAT | O_TRUNC));
  EXPECT_EQ(contents(kFile), before) << "a refused open changed the file";
  EXPECT_EQ(contents("/.crosspoint/epub_7/new.bin"), "<none>");
  file.close();
}

// A file in a marked folder that the cipher did not write (copied in from a computer, left by an older firmware) is
// refused, never read as the book's.
TEST_F(CacheCipherStorageTest, AFileWithoutAHeaderIsRefused) {
  put(kFile, kText);
  HalFile file;
  EXPECT_FALSE(Storage.openFileForRead("TEST", kFile, file));
  file.close();
}

// A folder without the marker, and any other folder, are not touched.
TEST_F(CacheCipherStorageTest, APlainFolderStaysPlain) {
  ASSERT_TRUE(vol_.mkdir("/.crosspoint/epub_8"));
  write("/.crosspoint/epub_8/book.bin", kText);
  EXPECT_EQ(contents("/.crosspoint/epub_8/book.bin"), kText);
  EXPECT_FALSE(Storage.cacheFolderEnciphered("/.crosspoint/epub_8"));
}

// A remove through Storage forgets what it knew of the folder (this stub's removeDir() does nothing, so the folder is
// emptied file by file, as BookCache::clear() empties one): without its marker, the same folder is plain again.
TEST_F(CacheCipherStorageTest, ARemovedMarkerIsForgotten) {
  write(kFile, kText);
  EXPECT_TRUE(Storage.cacheFolderEnciphered(kFolder));
  ASSERT_TRUE(Storage.remove(kFile));
  ASSERT_TRUE(Storage.remove("/.crosspoint/epub_7/cipher.bin"));
  EXPECT_FALSE(Storage.cacheFolderEnciphered(kFolder));
  write("/.crosspoint/epub_7/book.bin", kText);
  EXPECT_EQ(contents("/.crosspoint/epub_7/book.bin"), kText);
}

}  // namespace
