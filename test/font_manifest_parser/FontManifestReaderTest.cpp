// FontManifestReader: the glue the Font Manager and the web font API share -- the manifest file read
// in two streamed passes into one exactly sized FontCatalog, checked family by family.

#include <FontManifestParser.h>
#include <FontManifestReader.h>
#include <gtest/gtest.h>

#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "FailingArrayNew.h"

namespace {

// Stand-ins for FontInstaller's checks, with the same shape: a family name is 1-31 characters of
// [A-Za-z0-9_-]; a file name is 1-60 characters with no "..".
bool familyNameOk(const char* name) {
  const std::string n(name);
  if (n.empty() || n.size() > 31) return false;
  for (const char c : n) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
  }
  return true;
}

bool fileNameOk(const char* name) {
  const std::string n(name);
  return !n.empty() && n.size() <= 60 && n.find("..") == std::string::npos;
}

bool anyName(const char*) { return true; }

// What an earlier read left in the catalog: every read replaces it.
void fillStale(FontCatalog& catalog) {
  FontCatalog::Builder counter;
  counter.beginFamily();
  counter.keepFamily("Stale", 5);
  ASSERT_TRUE(catalog.allocate(counter.size()));
  FontCatalog::Builder filler(catalog);
  filler.beginFamily();
  filler.keepFamily("Stale", 5);
  ASSERT_EQ(catalog.count(), 1u);
}

struct Outcome {
  FontManifestStatus status = FontManifestStatus::Invalid;
  FontCatalog catalog;
  std::string baseUrl = "untouched";
  std::string failure;
  int version = -1;
};

Outcome read(const std::string& json, FontManifestReader::NameCheck familyCheck = familyNameOk,
             FontManifestReader::NameCheck fileCheck = fileNameOk) {
  HalFile file = HalFile::fromString(json);
  FontManifestReader reader("TEST", familyCheck, fileCheck);
  Outcome out;
  fillStale(out.catalog);
  out.status = reader.read(file, out.catalog, out.baseUrl);
  out.failure = reader.failure() ? reader.failure() : "";
  out.version = reader.version();
  return out;
}

std::vector<std::string> names(const FontCatalog& catalog) {
  std::vector<std::string> out;
  for (size_t i = 0; i < catalog.count(); ++i) out.push_back(catalog.name(i));
  return out;
}

// Every string the catalog holds, with its terminator: with the records, all its block may hold.
size_t stringBytes(const FontCatalog& catalog) {
  size_t bytes = 0;
  for (size_t i = 0; i < catalog.count(); ++i) {
    bytes += strlen(catalog.name(i)) + 1 + strlen(catalog.description(i)) + 1;
    for (size_t j = 0; j < catalog.fileCount(i); ++j) bytes += strlen(catalog.fileName(i, j)) + 1;
  }
  return bytes;
}

const std::string kManifest = R"({
  "version": 2,
  "baseUrl": "https://example.com/fonts/",
  "families": [
    {"name": "Alpha", "description": "First", "styles": ["regular"],
     "files": [{"name": "Alpha/Alpha_10.cpfont", "size": 1000, "crc32": 11},
               {"name": "Alpha/Alpha_12.cpfont", "size": 2000, "crc32": 22}]},
    {"name": "Beta", "description": "Second",
     "files": [{"name": "Beta/Beta_10.cpfont", "size": 3000}]}
  ]
})";

}  // namespace

TEST(FontManifestReaderTest, BuildsEachFamilyWithItsFilesAndTotal) {
  const Outcome out = read(kManifest);
  ASSERT_EQ(out.status, FontManifestStatus::Ok) << out.failure;
  EXPECT_EQ(out.version, 2);
  EXPECT_EQ(out.baseUrl, "https://example.com/fonts/");
  const FontCatalog& catalog = out.catalog;
  ASSERT_EQ(catalog.count(), 2u);

  EXPECT_STREQ(catalog.name(0), "Alpha");
  EXPECT_STREQ(catalog.description(0), "First");
  ASSERT_EQ(catalog.fileCount(0), 2u);
  EXPECT_STREQ(catalog.fileName(0, 0), "Alpha/Alpha_10.cpfont");
  EXPECT_EQ(catalog.fileSize(0, 0), 1000u);
  EXPECT_TRUE(catalog.fileHasCrc32(0, 0));
  EXPECT_EQ(catalog.fileCrc32(0, 0), 11u);
  EXPECT_EQ(catalog.fileCrc32(0, 1), 22u);
  EXPECT_EQ(catalog.totalSize(0), 3000u);

  EXPECT_STREQ(catalog.name(1), "Beta");
  EXPECT_STREQ(catalog.description(1), "Second");
  ASSERT_EQ(catalog.fileCount(1), 1u);
  EXPECT_FALSE(catalog.fileHasCrc32(1, 0));
  EXPECT_EQ(catalog.fileCrc32(1, 0), 0u);
  EXPECT_EQ(catalog.totalSize(1), 3000u);

  for (size_t i = 0; i < catalog.count(); ++i) {  // set by the callers afterwards
    EXPECT_FALSE(catalog.installed(i));
    EXPECT_FALSE(catalog.hasUpdate(i));
    EXPECT_FALSE(catalog.hasResumableDownload(i));
  }
}

// The first pass counts what the families that pass their checks hold, so the block is allocated once
// at exactly that size: the entries that fail take no room at all.
TEST(FontManifestReaderTest, SizesTheBlockForTheFamiliesThatPassTheirChecks) {
  const Outcome out = read(R"({"version":2,"families":[{"name":"A"},{"name":"bad name"},{"name":"C"}]})");
  ASSERT_EQ(out.status, FontManifestStatus::Ok);
  EXPECT_EQ(names(out.catalog), (std::vector<std::string>{"A", "C"}));
  // "A", "C" and an empty description each.
  EXPECT_EQ(out.catalog.blockBytes(), FontCatalog::blockBytesFor({2, 0, 2 * (2 + 1)}));
}

// A family whose name or any file name fails its check is left out; the rest still load.
TEST(FontManifestReaderTest, LeavesOutFamiliesThatFailTheirChecks) {
  const std::string longName(200, 'x');
  const Outcome out = read(R"({"version":2,"families":[
    {"name":"Good","files":[{"name":"Good/Good_10.cpfont","size":1}]},
    {"name":"../evil","files":[{"name":"x.cpfont","size":1}]},
    {"name":"BadFile","files":[{"name":"ok.cpfont","size":1},{"name":"../../etc","size":1}]},
    {"name":"TooLong","files":[{"name":")" +
                           longName + R"(","size":1}]},
    {"name":"NotAnObject","files":[{"name":"ok.cpfont","size":1}, 42]},
    7,
    {"files":[{"name":"NoName/x.cpfont","size":1}]},
    {"name":"AlsoGood"}
  ]})");
  ASSERT_EQ(out.status, FontManifestStatus::Ok) << out.failure;
  EXPECT_EQ(names(out.catalog), (std::vector<std::string>{"Good", "AlsoGood"}));
  EXPECT_EQ(out.catalog.fileCount(0), 1u);
  EXPECT_STREQ(out.catalog.fileName(0, 0), "Good/Good_10.cpfont");
  EXPECT_EQ(out.catalog.fileCount(1), 0u);
  EXPECT_EQ(out.catalog.blockBytes(), FontCatalog::blockBytesFor({2, 1, stringBytes(out.catalog)}));
}

// A family that is rejected after its description and files went into the block is taken back out,
// in the filling pass too, where the block has no room to spare: what follows it is intact.
TEST(FontManifestReaderTest, ARejectedFamilyLeavesTheNextIntact) {
  const Outcome out = read(R"({"version":2,"families":[
    {"description":")" + std::string(300, 'd') +
                           R"(","files":[{"name":"Bad/a.cpfont","size":5},{"name":"Bad/b.cpfont","size":6}],
     "name":"bad name"},
    {"name":"Kept","description":"Here","files":[{"name":"Kept/k.cpfont","size":7,"crc32":9}]}
  ]})");
  ASSERT_EQ(out.status, FontManifestStatus::Ok) << out.failure;
  ASSERT_EQ(out.catalog.count(), 1u);
  EXPECT_STREQ(out.catalog.name(0), "Kept");
  EXPECT_STREQ(out.catalog.description(0), "Here");
  ASSERT_EQ(out.catalog.fileCount(0), 1u);
  EXPECT_STREQ(out.catalog.fileName(0, 0), "Kept/k.cpfont");
  EXPECT_EQ(out.catalog.fileCrc32(0, 0), 9u);
  EXPECT_EQ(out.catalog.totalSize(0), 7u);
}

// No key order is assumed: a name after the files still names the family they belong to.
TEST(FontManifestReaderTest, TheNameMayComeLast) {
  const Outcome out =
      read(R"({"families":[{"files":[{"size":4,"name":"Late/l.cpfont"}],"description":"D","name":"Late"}],
               "version":1})");
  ASSERT_EQ(out.status, FontManifestStatus::Ok) << out.failure;
  ASSERT_EQ(out.catalog.count(), 1u);
  EXPECT_STREQ(out.catalog.name(0), "Late");
  EXPECT_STREQ(out.catalog.description(0), "D");
  EXPECT_STREQ(out.catalog.fileLocalName(0, 0), "l.cpfont");
  EXPECT_EQ(out.catalog.totalSize(0), 4u);
}

// A file name over the parser's buffer is rejected even when the check alone would pass it.
TEST(FontManifestReaderTest, AnOverflowedFileNameRejectsItsFamily) {
  const std::string longName(FontManifestParser::FILE_NAME_BUF_SIZE + 10, 'y');
  const Outcome out =
      read(R"({"version":2,"families":[{"name":"F","files":[{"name":")" + longName + R"(","size":1}]},{"name":"G"}]})",
           familyNameOk, anyName);
  ASSERT_EQ(out.status, FontManifestStatus::Ok);
  EXPECT_EQ(names(out.catalog), (std::vector<std::string>{"G"}));
}

// So is a family name too long for the reader to hold, which no family name check would pass anyway.
TEST(FontManifestReaderTest, AFamilyNameTooLongToHoldRejectsItsFamily) {
  const std::string fits(FontManifestReader::FAMILY_NAME_BUF_SIZE - 1, 'f');
  const std::string tooLong(FontManifestReader::FAMILY_NAME_BUF_SIZE, 't');
  const Outcome out =
      read(R"({"version":2,"families":[{"name":")" + tooLong + R"("},{"name":")" + fits + R"("}]})", anyName, anyName);
  ASSERT_EQ(out.status, FontManifestStatus::Ok);
  EXPECT_EQ(names(out.catalog), (std::vector<std::string>{fits}));
}

// Whatever goes wrong, nothing half-built is left behind -- not even what the catalog held before.
TEST(FontManifestReaderTest, AnInvalidDocumentLeavesNothingBehind) {
  for (const std::string& json : {
           kManifest.substr(0, kManifest.size() / 2),                        // cut short
           std::string(R"({"version":2,"families":[{"name":nope}]})"),       // not JSON
           std::string(""),                                                  // empty
           R"({"version":2,"baseUrl":")" + std::string(300, 'u') + R"("})",  // base URL too long
       }) {
    SCOPED_TRACE(json.substr(0, 60));
    const Outcome out = read(json);
    EXPECT_EQ(out.status, FontManifestStatus::Invalid);
    EXPECT_FALSE(out.failure.empty());
    EXPECT_TRUE(out.catalog.empty());
    EXPECT_EQ(out.baseUrl, "");
  }
}

TEST(FontManifestReaderTest, AnUnreadableFileIsInvalid) {
  HalFile file;  // no backing data: seek and read fail
  FontManifestReader reader("TEST", familyNameOk, fileNameOk);
  FontCatalog catalog;
  std::string baseUrl;
  EXPECT_EQ(reader.read(file, catalog, baseUrl), FontManifestStatus::Invalid);
  EXPECT_NE(reader.failure(), nullptr);
}

// Only versions 1 and 2 are read; any other leaves nothing behind and reports the version found.
TEST(FontManifestReaderTest, OnlyVersions1And2AreAccepted) {
  Outcome out = read(R"({"version":3,"families":[{"name":"A"}]})");
  EXPECT_EQ(out.status, FontManifestStatus::UnsupportedVersion);
  EXPECT_EQ(out.version, 3);
  EXPECT_TRUE(out.catalog.empty());
  EXPECT_EQ(out.baseUrl, "");

  out = read(R"({"families":[{"name":"A"}]})");
  EXPECT_EQ(out.status, FontManifestStatus::UnsupportedVersion);
  EXPECT_EQ(out.version, 0);

  out = read(R"({"version":1,"families":[{"name":"A"}]})");
  EXPECT_EQ(out.status, FontManifestStatus::Ok);
  EXPECT_EQ(names(out.catalog), (std::vector<std::string>{"A"}));
}

// A manifest whose every family fails its checks loads as an empty list, as before: no block, no error.
TEST(FontManifestReaderTest, NoFamilyThatPassesIsAnEmptyList) {
  const Outcome out = read(R"({"version":2,"baseUrl":"https://x/","families":[{"name":"bad name"}]})");
  ASSERT_EQ(out.status, FontManifestStatus::Ok) << out.failure;
  EXPECT_TRUE(out.catalog.empty());
  EXPECT_EQ(out.catalog.blockBytes(), 0u);
  EXPECT_EQ(out.baseUrl, "https://x/");
}

// The heap cannot give the block: out of memory, said so, and nothing left behind.
TEST(FontManifestReaderTest, ABlockTheHeapCannotGiveIsOutOfMemory) {
  HalFile file = HalFile::fromString(kManifest);
  FontManifestReader reader("TEST", familyNameOk, fileNameOk);
  FontCatalog catalog;
  fillStale(catalog);
  std::string baseUrl = "untouched";

  const FailingArrayNew noHeap(1);
  EXPECT_EQ(reader.read(file, catalog, baseUrl), FontManifestStatus::OutOfMemory);
  ASSERT_NE(reader.failure(), nullptr);
  EXPECT_STREQ(reader.failure(), "out of memory");
  EXPECT_TRUE(catalog.empty());
  EXPECT_EQ(baseUrl, "");
}

// The shipped manifest, end to end: every family loads, each with the sum of its file sizes, in one
// block that holds its records and strings and nothing else. Structural checks only, so regenerating
// the manifest does not break them.
TEST(FontManifestReaderTest, ReadsTheShippedManifest) {
  std::ifstream in(FONT_MANIFEST_PATH, std::ios::binary);
  ASSERT_TRUE(in.good()) << FONT_MANIFEST_PATH;
  const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  const Outcome out = read(json);
  ASSERT_EQ(out.status, FontManifestStatus::Ok) << out.failure;
  EXPECT_EQ(out.version, 2);
  EXPECT_EQ(out.baseUrl, "https://raw.githubusercontent.com/jpirnay/witchhunt-reader/master/assets/sd-fonts/");
  const FontCatalog& catalog = out.catalog;
  ASSERT_GT(catalog.count(), 0u);
  EXPECT_GT(catalog.totalFiles(), 0u);
  size_t files = 0;
  for (size_t i = 0; i < catalog.count(); ++i) {
    size_t sum = 0;
    for (size_t j = 0; j < catalog.fileCount(i); ++j) {
      sum += catalog.fileSize(i, j);
      EXPECT_TRUE(catalog.fileHasCrc32(i, j)) << catalog.fileName(i, j);
    }
    EXPECT_EQ(catalog.totalSize(i), sum) << catalog.name(i);
    files += catalog.fileCount(i);
  }
  EXPECT_EQ(files, catalog.totalFiles());
  for (size_t i = 0; i < catalog.count(); ++i) {
    EXPECT_NE(*catalog.name(i), ' ') << i;
    for (size_t j = 0; j < catalog.fileCount(i); ++j) {
      EXPECT_NE(*catalog.fileName(i, j), ' ') << catalog.name(i);
      EXPECT_NE(*catalog.fileLocalName(i, j), ' ') << catalog.name(i);
    }
  }

  EXPECT_EQ(catalog.blockBytes(),
            FontCatalog::blockBytesFor({catalog.count(), catalog.totalFiles(), stringBytes(catalog)}));
  RecordProperty("blockBytes", static_cast<int>(catalog.blockBytes()));
  // A memory budget with real headroom, not an exact value (the block is ~8 KB today).
  EXPECT_LE(catalog.blockBytes(), 16u * 1024u);
}
