#include "TextShaping.h"

#include <Logging.h>
#include <Memory.h>

#include <cstdint>
#include <cstring>
#include <memory>

#include "BidiUtils.h"
#include "HebrewShaper.h"
#include "IndicScripts.h"
#include "Utf8.h"

namespace TextShaping {

uint32_t decodeAt(std::string_view text, size_t& pos) {
  // utf8NextCodepoint reads up to four bytes and stops at a NUL, so it decodes a NUL-padded copy of what the view
  // holds.
  unsigned char padded[5] = {};
  const size_t n = text.size() - pos < 4 ? text.size() - pos : 4;
  memcpy(padded, text.data() + pos, n);
  const unsigned char* cursor = padded;
  const uint32_t cp = utf8NextCodepoint(&cursor);
  const size_t used = static_cast<size_t>(cursor - padded);
  pos += used == 0 ? 1 : used;
  return cp;
}

namespace {

// The last code point of a non-empty view: the lead byte is at most three continuation bytes back.
uint32_t lastCodepoint(std::string_view text) {
  size_t start = text.size() - 1;
  while (start > 0 && text.size() - start < 4 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) start--;
  size_t pos = start;
  const uint32_t cp = decodeAt(text, pos);
  return pos == text.size() ? cp : 0xFFFD;
}

bool holdsMarksOnly(std::string_view text) {
  size_t pos = 0;
  while (pos < text.size()) {
    if (!BidiUtils::isTransparentMark(decodeAt(text, pos))) return false;
  }
  return !text.empty();
}

}  // namespace

bool mergesWithPrevious(const GroupToken& previous, const GroupToken& token, bool indicGroup) {
  if (previous.text.empty() || token.text.empty()) return false;
  if (indicGroup) {
    size_t pos = 0;
    if (previous.styleKey == token.styleKey ||
        !indic::syllableBreakAllowed(lastCodepoint(previous.text), decodeAt(token.text, pos))) {
      return true;
    }
  }
  return holdsMarksOnly(token.text) && HebrewShaper::endsWithName(previous.text.data(), previous.text.size());
}

namespace {

// The build's heap block for shaping calls (buildHeapBlock): builds and the dictionary's pages run under RenderLock,
// one call at a time, so it needs no lock.
std::unique_ptr<uint8_t[]> heapBlock;
size_t heapBlockBytes = 0;
bool heapBlockRefusalLogged = false;  // logged once until releaseBuildMemory()

// A block grows by at least this much, so a build's tokens of rising length do not free and allocate at each one.
constexpr size_t kHeapBlockStep = 512;

}  // namespace

void* buildHeapBlock(const size_t bytes) {
  if (heapBlock && heapBlockBytes >= bytes) return heapBlock.get();
  // The smaller block goes first, so the larger one can take its room.
  heapBlock.reset();
  heapBlockBytes = 0;
  const size_t grown =
      bytes > SIZE_MAX - kHeapBlockStep ? bytes : (bytes + kHeapBlockStep - 1) / kHeapBlockStep * kHeapBlockStep;
  // One shaping call's glyph buffer, its storage, runs, code points and reordered form (IndicShaper::CallMemory, about
  // 6-8 KB): past any task's stack, and kept for the build so each token does not allocate it again.
  heapBlock = makeUniqueNoThrow<uint8_t[]>(grown);
  if (!heapBlock) {
    if (!heapBlockRefusalLogged) {
      heapBlockRefusalLogged = true;
      LOG_ERR("SHP", "No %zu B for a shaping call: its tokens are laid out unshaped", grown);
    }
    return nullptr;
  }
  heapBlockBytes = grown;
  return heapBlock.get();
}

void releaseBuildMemory() {
  heapBlock.reset();
  heapBlockBytes = 0;
  heapBlockRefusalLogged = false;
}

bool buildMemoryHeld() { return static_cast<bool>(heapBlock); }

bool shape(const char* text, size_t len, Unit* units, size_t capacity, size_t& count) {
  // Hebrew letters and points are U+0590-05FF; the lead byte 0xD7 covers U+05C0-05FF, which holds every letter.
  if (text == nullptr || memchr(text, 0xD7, len) == nullptr) return false;
  return HebrewShaper::shape(text, len, units, capacity, count);
}

}  // namespace TextShaping
