// A Hindi book with the Hindi hyphenation pack (assets/sd-fonts/Hyphenation-Hindi), hyphenation on, laid out narrow
// through the host pipeline with the Devanagari subset as the face: its words break across lines where the patterns
// allow, between syllables, and without a hyphen, as Indic lines break (LanguageHyphenator::insertsHyphen).
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "Epub/hyphenation/LanguageRegistry.h"
#include "GfxRenderer.h"
#include "IndicScripts.h"
#include "PipelineRunner.h"
#include "SyntheticBook.h"
#include "Utf8.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kDevanagariFace = "NotoSansDevanagari-Shaping-Regular.ttf";

// The words the subset face holds (ShapingStatusTest): hindi, patrika, shri, prarthana, krishna, jnana.
const char* const kWords[] = {
    "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80",
    "\xE0\xA4\xAA\xE0\xA4\xA4\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA4\xBF\xE0\xA4\x95\xE0\xA4\xBE",
    "\xE0\xA4\xB6\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA5\x80",
    "\xE0\xA4\xAA\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA4\xBE\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\xA5\xE0\xA4\xA8\xE0\xA4\xBE",
    "\xE0\xA4\x95\xE0\xA5\x83\xE0\xA4\xB7\xE0\xA5\x8D\xE0\xA4\xA3",
    "\xE0\xA4\x9C\xE0\xA5\x8D\xE0\xA4\x9E\xE0\xA4\xBE\xE0\xA4\xA8",
};

// The pack's trie: the file's bytes after its 24-byte CPHY header, which gives the root offset at byte 12.
std::vector<uint8_t> gPack;
bool hindiPack(const char* code, SerializedHyphenationPatterns& out, uint32_t& crc32) {
  if (std::strcmp(code, "hi") != 0 || gPack.size() <= 24) return false;
  const auto le32 = [](const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
  };
  out = {le32(gPack.data() + 12), gPack.data() + 24, gPack.size() - 24};
  crc32 = le32(gPack.data() + 20);
  return true;
}

std::vector<uint32_t> codepoints(const std::string& s) {
  std::vector<uint32_t> out;
  const auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
  while (*p) out.push_back(utf8NextCodepoint(&p));
  return out;
}

}  // namespace

TEST(IndicPackHyphenation, AHindiBookBreaksBetweenSyllablesWithoutAHyphen) {
  const fs::path work = fs::temp_directory_path() / "indic_pack_hyphenation";
  fs::remove_all(work);
  fs::create_directories(work);
  {
    std::ifstream in(std::string(HYPHENATION_PACK_DIR) + "/Hyphenation-Hindi/hyph-hi.cphyph", std::ios::binary);
    gPack.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  ASSERT_GT(gPack.size(), 24u);
  forgetPackPatterns();
  setHyphenationPatternProvider(hindiPack);

  std::string body;
  for (int paragraph = 0; paragraph < 8; ++paragraph) {
    body += "<p>";
    for (int i = 0; i < 10; ++i) {
      for (const char* word : kWords) body += std::string(word) + " ";
    }
    body += "</p>\n";
  }
  const std::shared_ptr<Epub> epub = syntheticBook(work, 0, body, {}, "hi");
  const pipeline_harness::ShapingFaceScope face(kDevanagariFace);
  GfxRenderer renderer;
  Section::BuildParams p;
  p.fontId = 1;
  p.lineCompression = 1.0f;
  p.viewportWidth = 150;  // narrow: lines end inside words
  p.viewportHeight = 760;
  p.inlineFootnotePreviews = false;
  p.hyphenationEnabled = true;
  p.hyphenationLayoutKey = Hyphenator::layoutKey("hi", true);
  EXPECT_NE(p.hyphenationLayoutKey, kNoPatternsLayoutKey);
  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.createSectionFile(p, {}, /*skipEviction=*/true));
  EXPECT_FALSE(section.isShapingDegraded());

  // Every line's words, in order.
  std::vector<std::vector<std::string>> lines;
  for (uint16_t n = 0; n < section.pageCount; ++n) {
    section.currentPage = n;
    const auto page = section.loadPageFromSectionFile();
    ASSERT_TRUE(page);
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const TextBlock& block = *static_cast<const PageLine&>(*element).getBlock();
      std::vector<std::string> words;
      for (uint16_t w = 0; w < block.wordCount(); ++w) words.emplace_back(block.wordText(w), block.wordTextLen(w));
      lines.push_back(words);
    }
  }
  const std::set<std::string> whole(std::begin(kWords), std::end(kWords));
  int splits = 0;
  for (size_t i = 0; i + 1 < lines.size(); ++i) {
    if (lines[i].empty() || lines[i + 1].empty()) continue;
    const std::string& head = lines[i].back();
    EXPECT_NE(head.back(), '-') << "a hyphen at the end of line " << i << ": " << head;
    if (whole.count(head)) continue;
    // A line that ends inside a word: the next line starts with the rest of it.
    const std::string& tail = lines[i + 1].front();
    ASSERT_TRUE(whole.count(head + tail)) << "line " << i << " ends with " << head << ", the next starts with " << tail;
    const std::vector<uint32_t> a = codepoints(head);
    const std::vector<uint32_t> b = codepoints(tail);
    EXPECT_TRUE(indic::syllableBreakAllowed(a.back(), b.front())) << head << " | " << tail;
    ++splits;
  }
  EXPECT_GE(splits, 10);
  std::printf("[ INFO ] %zu lines, %d words broken between syllables\n", lines.size(), splits);

  setHyphenationPatternProvider(nullptr);
  forgetPackPatterns();
  fs::remove_all(work);
}
