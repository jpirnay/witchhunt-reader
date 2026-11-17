// The files a book's cache holds once every spine item is laid out, pinned by path, size and FNV-1a of their bytes
// (goldens/<book>_<profile>.cachefiles.txt). The layout goldens pin what a page shows; this pins what the card holds,
// file by file, including the section files' names, which carry the variant key no dump prints. A layout change meant
// for some books only (a heading-only spine file laid out with the chapter after it) must leave every other book's
// cache as it was, and the books here have no such file: test_heading_near_miss.epub holds the files that come
// closest to one.
//
// The books are built in two cache directories and must agree, as the layout goldens are. Bytes that name where the
// test ran are normalised: a serialized String (u32 length, then its bytes) that starts with the book's cache
// directory or with the book's own path is hashed as if it started with "<cache>" or "<book>" (an image's path and
// its book, ImageBlock::serialize).
#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Epub.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

struct Case {
  const char* book;
  const char* profile;
};

pipeline_harness::Profile profileFor(const std::string& name) {
  pipeline_harness::Profile profile;
  if (name == "murphy") {
    // The Murphy M3's text area at the owner's settings, from a section file's header: 214 x 358, line spacing 0.95,
    // extra paragraph spacing on. The stub renderer gives every font the same metrics, so the font is not modelled.
    profile.viewportWidth = 214;
    profile.viewportHeight = 358;
    profile.lineCompression = 0.95f;
    profile.extraParagraphSpacing = true;
  }
  return profile;
}

std::string stem(const std::string& book) { return fs::path(book).stem().string(); }

uint32_t fnv1a(const std::string& bytes) {
  uint32_t hash = 2166136261u;
  for (const char c : bytes) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 16777619u;
  }
  return hash;
}

// `bytes` with every serialized String that starts with a `from` rewritten to start with its `to`.
std::string normalised(const std::string& bytes, const std::vector<std::pair<std::string, std::string>>& subs) {
  std::string out;
  out.reserve(bytes.size());
  size_t i = 0;
  while (i < bytes.size()) {
    bool rewritten = false;
    if (i + 4 <= bytes.size()) {
      uint32_t len = 0;
      memcpy(&len, bytes.data() + i, sizeof(len));
      for (const auto& [from, to] : subs) {
        if (len < from.size() || i + 4 + len > bytes.size() || bytes.compare(i + 4, from.size(), from) != 0) continue;
        const auto n = static_cast<uint32_t>(len - from.size() + to.size());
        out.append(reinterpret_cast<const char*>(&n), sizeof(n));
        out += to;
        out.append(bytes, i + 4 + from.size(), len - from.size());
        i += 4 + len;
        rewritten = true;
        break;
      }
    }
    if (!rewritten) out += bytes[i++];
  }
  return out;
}

std::string readFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream bytes;
  bytes << in.rdbuf();
  return bytes.str();
}

// One line per file under the book's cache directory, in path order: the path below it, the size and the FNV-1a of
// the normalised bytes.
std::string manifest(const std::string& bookCacheDir, const std::string& bookPath) {
  std::vector<std::string> lines;
  const std::vector<std::pair<std::string, std::string>> subs = {{bookCacheDir, "<cache>"}, {bookPath, "<book>"}};
  for (const auto& entry : fs::recursive_directory_iterator(bookCacheDir)) {
    if (!entry.is_regular_file()) continue;
    const std::string bytes = normalised(readFile(entry.path()), subs);
    char line[512];
    snprintf(line, sizeof(line), "%s %zu %08x", fs::relative(entry.path(), bookCacheDir).generic_string().c_str(),
             bytes.size(), fnv1a(bytes));
    lines.emplace_back(line);
  }
  std::sort(lines.begin(), lines.end());
  std::string out;
  for (const std::string& line : lines) out += line + "\n";
  return out;
}

std::string freshDir(const Case& c, const char* tag) {
  const fs::path dir = fs::temp_directory_path() / ("cache_identity_" + std::to_string(getpid()) + "_" + stem(c.book) +
                                                    "_" + c.profile + "_" + tag);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

std::string buildAndList(const Case& c, const char* tag) {
  const std::string bookPath = std::string(CORPUS_DIR) + "/" + c.book;
  const std::string cacheDir = freshDir(c, tag);
  std::ostringstream dump;
  EXPECT_TRUE(pipeline_harness::runAndDump(bookPath, cacheDir, profileFor(c.profile), dump)) << dump.str();
  // The book's cache directory as the build named it (Epub keys it on the book's path).
  const std::string bookCacheDir = Epub(bookPath, cacheDir).getCachePath();
  std::string listed = manifest(bookCacheDir, bookPath);
  fs::remove_all(cacheDir);
  return listed;
}

class CacheFileIdentityTest : public testing::TestWithParam<Case> {};

TEST_P(CacheFileIdentityTest, MatchesPinnedFiles) {
  const Case c = GetParam();
  const std::string listed = buildAndList(c, "a");
  ASSERT_EQ(listed, buildAndList(c, "b")) << "two cold builds of " << c.book << " left different cache files";
  const fs::path goldenPath = fs::path(GOLDEN_DIR) / (stem(c.book) + "_" + std::string(c.profile) + ".cachefiles.txt");
  if (std::getenv("UPDATE_GOLDENS")) {
    std::ofstream(goldenPath, std::ios::binary) << listed;
    GTEST_SKIP() << "golden regenerated: " << goldenPath;
  }
  ASSERT_TRUE(fs::exists(goldenPath)) << "missing golden " << goldenPath << " -- run with UPDATE_GOLDENS=1";
  EXPECT_EQ(readFile(goldenPath), listed) << "the cache files of " << c.book << " (" << c.profile
                                          << ") changed: a book without the pattern must keep them byte for byte";
}

const Case kCases[] = {
    {"test_heading_near_miss.epub", "default"}, {"test_heading_near_miss.epub", "murphy"},
    {"test_display_none.epub", "default"},      {"test_display_none.epub", "murphy"},
    {"test_keep_together.epub", "default"},     {"test_keep_together.epub", "murphy"},
};

std::string caseName(const testing::TestParamInfo<Case>& info) {
  std::string name = stem(info.param.book) + "_" + info.param.profile;
  std::replace_if(
      name.begin(), name.end(), [](const char ch) { return !isalnum(static_cast<unsigned char>(ch)); }, '_');
  return name;
}

INSTANTIATE_TEST_SUITE_P(BooksWithoutTheJoin, CacheFileIdentityTest, testing::ValuesIn(kCases), caseName);

}  // namespace
