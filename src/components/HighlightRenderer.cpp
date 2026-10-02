#include "HighlightRenderer.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <string>

#include "HighlightStore.h"

namespace HighlightRenderer {

namespace {
// Shortest overlap accepted for a highlight continuing from/onto a neighbouring page: short
// enough for a sentence's tail, long enough not to fire on a common word.
constexpr size_t MIN_EDGE_OVERLAP = 12;
constexpr size_t MAX_PAGE_TOKENS = 1200;

struct TokenRef {
  const PageLine* line;
  uint16_t word;
};

// Appends `text` with whitespace, ASCII hyphens, soft hyphens and no-break spaces dropped.
// `owner` receives, for every byte appended, the token it came from.
void appendCompact(const char* text, const size_t length, const uint16_t token, std::string& out,
                   std::vector<uint16_t>* owner) {
  const auto* b = reinterpret_cast<const uint8_t*>(text);
  for (size_t i = 0; i < length; i++) {
    const uint8_t c = b[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '-') continue;
    if (c == 0xC2 && i + 1 < length && (b[i + 1] == 0xAD || b[i + 1] == 0xA0)) {
      i++;
      continue;
    }
    out.push_back(static_cast<char>(c));
    if (owner) owner->push_back(token);
  }
}

// [begin, end) of the page's compact text covered by `needle`, or false.
bool locate(const std::string& page, const std::string& needle, size_t& begin, size_t& end) {
  if (needle.size() < 4 || page.size() < 4) return false;
  const size_t whole = page.find(needle);
  if (whole != std::string::npos) {
    begin = whole;
    end = whole + needle.size();
    return true;
  }
  if (page.size() < MIN_EDGE_OVERLAP || needle.size() < MIN_EDGE_OVERLAP) return false;
  // The highlight's tail opens this page.
  const std::string head = page.substr(0, MIN_EDGE_OVERLAP);
  for (size_t s = needle.find(head); s != std::string::npos; s = needle.find(head, s + 1)) {
    const size_t k = needle.size() - s;
    if (k <= page.size() && page.compare(0, k, needle, s, k) == 0) {
      begin = 0;
      end = k;
      return true;
    }
  }
  // The highlight's head closes this page.
  const std::string tail = page.substr(page.size() - MIN_EDGE_OVERLAP);
  for (size_t e = needle.find(tail); e != std::string::npos; e = needle.find(tail, e + 1)) {
    const size_t k = e + MIN_EDGE_OVERLAP;
    if (k <= page.size() && page.compare(page.size() - k, k, needle, 0, k) == 0) {
      begin = page.size() - k;
      end = page.size();
      return true;
    }
  }
  return false;
}
}  // namespace

void drawUnderlines(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                    const int marginTop, const std::vector<const Highlight*>& highlights) {
  if (highlights.empty()) return;

  std::vector<TokenRef> tokens;
  std::string compact;
  std::vector<uint16_t> owner;
  tokens.reserve(256);
  compact.reserve(2048);
  owner.reserve(2048);
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;
    for (uint16_t i = 0; i < block->wordCount() && tokens.size() < MAX_PAGE_TOKENS; i++) {
      appendCompact(block->wordText(i), block->wordTextLen(i), static_cast<uint16_t>(tokens.size()), compact, &owner);
      tokens.push_back({line, i});
    }
  }
  if (compact.empty()) return;

  std::vector<bool> marked(tokens.size(), false);
  bool any = false;
  std::string needle;
  for (const Highlight* h : highlights) {
    needle.clear();
    appendCompact(h->text.data(), h->text.size(), 0, needle, nullptr);
    size_t begin = 0, end = 0;
    if (!locate(compact, needle, begin, end)) continue;
    for (size_t i = begin; i < end; i++) marked[owner[i]] = true;
    any = true;
  }
  if (!any) return;

  // One underline per run of marked words on a line, so the gaps between words are covered.
  int runX0 = 0, runX1 = 0, runY = -1;
  auto flush = [&] {
    if (runY >= 0 && runX1 > runX0) renderer.fillRect(runX0, runY, runX1 - runX0, 2, true);
    runY = -1;
  };
  for (size_t t = 0; t < tokens.size(); t++) {
    if (!marked[t]) {
      flush();
      continue;
    }
    const auto& block = tokens[t].line->getBlock();
    const TextBlock::WordBox box = block->wordBox(renderer, tokens[t].word, fontId, tokens[t].line->xPos + marginLeft,
                                                  tokens[t].line->yPos + marginTop);
    if (box.width <= 0 || box.height <= 0) continue;
    const int y = box.y + box.height + 1;
    if (y != runY) {
      flush();
      runX0 = box.x;
      runY = y;
    }
    runX1 = box.x + box.width;
  }
  flush();
}

}  // namespace HighlightRenderer
