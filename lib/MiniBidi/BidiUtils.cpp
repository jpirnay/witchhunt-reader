#include "BidiUtils.h"

extern "C" {
#include "minibidi.h"
}

#undef when
#undef otherwise

#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <cstring>
#include <mutex>

// Guards the shared workspace in applyBidiVisual() and
// computeVisualWordOrder().  The bidi+shaping pipeline is not reentrant;
// this mutex serialises access so multi-core callers don't corrupt each
// other's intermediate state.
static std::mutex bidiMutex;

namespace {

bool isNaturalDirectionClass(const uchar cls) {
  switch (cls) {
    case L:
    case R:
    case AL:
    case EN:
    case AN:
      return true;
    default:
      return false;
  }
}

struct BidiWorkspace {
  bidi_char line[BIDI_MAX_LINE];
  // witchhunt: deviation 1 - no `shaped` member: the workspace holds the line only (the shaped variant uses a per-call
  // scratch).
};

// witchhunt: deviation 1 - the workspace is exactly BIDI_MAX_LINE bidi_chars, and a build can lend a buffer of that
// size.
static_assert(sizeof(BidiWorkspace) == BidiUtils::kWorkspaceBytes, "kWorkspaceBytes must match BidiWorkspace");
static_assert(BIDI_MAX_LINE == BidiUtils::kMaxLineCodepoints, "kMaxLineCodepoints must match BIDI_MAX_LINE");

// Retained after first use to avoid allocation churn on every RTL token.
std::unique_ptr<BidiWorkspace> bidiWorkspace;

// witchhunt: deviation 1 - a buffer lent by the caller (bindWorkspace); used instead of the heap one while set.
BidiWorkspace* boundWorkspace = nullptr;

// Caller holds bidiMutex, including during allocation and subsequent use.
BidiWorkspace* getWorkspace() {
  if (boundWorkspace) return boundWorkspace;
  if (!bidiWorkspace) {
    bidiWorkspace = makeUniqueNoThrow<BidiWorkspace>();
    if (!bidiWorkspace) {
      LOG_ERR("BIDI", "Failed to allocate %zu-byte workspace", sizeof(BidiWorkspace));
    }
  }
  return bidiWorkspace.get();
}

}  // namespace

namespace BidiUtils {

bool startsWithRtl(const char* utf8, int maxStrongChars) {
  if (!utf8 || maxStrongChars <= 0) return false;

  auto* p = reinterpret_cast<const unsigned char*>(utf8);
  int checked = 0;
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!cp || cp == REPLACEMENT_GLYPH) break;

    const uchar cls = bidi_class(cp);
    if (cls == R || cls == AL) return true;
    if (cls == L) return false;
    checked++;
    if (checked >= maxStrongChars) break;
  }
  return false;
}

int detectParagraphLevel(const char* utf8, const int fallbackLevel, const int maxStrongChars) {
  if (!utf8 || maxStrongChars <= 0) return fallbackLevel & 1;

  auto* p = reinterpret_cast<const unsigned char*>(utf8);
  int checked = 0;
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (!cp || cp == REPLACEMENT_GLYPH) break;

    const uchar cls = bidi_class(cp);
    if (cls == R || cls == AL) return 1;
    if (cls == L) return 0;
    checked++;
    if (checked >= maxStrongChars) break;
  }

  return fallbackLevel & 1;
}

bool isTransparentMark(const uint32_t cp) {
  // RTL-script combining marks: Hebrew niqqud/cantillation and Arabic
  // harakat/Quranic annotation.  Transparent for Arabic joining (do_shape
  // skips them), zero-advance for measurement, and rendered as overlays on
  // the preceding base glyph when the active font carries their glyphs.
  // The cp >= 0x0591 guard keeps Latin combining marks (U+0300-U+036F, also
  // NSM) on their existing utf8IsCombiningMark() rendering path.
  return cp >= 0x0591 && bidi_class(cp) == NSM;
}

namespace {

// witchhunt: deviation 1 - scratch for the shaped output of applyBidiVisualShaped().
struct ShapedBuffer {
  bidi_char chars[BIDI_MAX_LINE];
};

// witchhunt: deviation 2 - one body behind applyBidiVisual() (Shape = false) and applyBidiVisualShaped() (Shape =
// true).
template <bool Shape>
bool applyBidiVisualImpl(const char* utf8, std::string& out, int paragraphLevel) {
  if (!utf8 || !*utf8) return false;
  const std::lock_guard<std::mutex> lock(bidiMutex);

  auto* const workspace = getWorkspace();
  if (!workspace) return false;
  bidi_char* const line = workspace->line;
  int count = 0;
  int lastBase = -1;           // last non-formatter character (mintty's ibase)
  uint8_t pendingJoiners = 0;  // ZWJ/ZWNJ seen since lastBase
  auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p) {
    if (count >= BIDI_MAX_LINE) {
      LOG_DBG("BIDI", "applyBidiVisual: input exceeds BIDI_MAX_LINE (%d chars), returning unprocessed", BIDI_MAX_LINE);
      return false;
    }

    const uint32_t cp = utf8NextCodepoint(&p);
    if (!cp) break;  // witchhunt: deviation 10 (U+FFFD is an ordinary code point; only NUL stops the loop)
    line[count].origwc = line[count].wc = cp;
    line[count].index = static_cast<uint16_t>(count);
    line[count].joiners = 0;

    // Flag Arabic joining formatters mintty-style (termline.c): the ZWJ/ZWNJ
    // goes into the low nibble of the character it follows and the high
    // nibble of the character it precedes.  Flags are assigned in logical
    // order here; do_shape() reads them after reordering.
    if (cp == 0x200C || cp == 0x200D) {
      const uint8_t joiner = (cp == 0x200D) ? ZWJ : ZWNJ;
      if (lastBase >= 0) line[lastBase].joiners |= joiner;
      pendingJoiners |= joiner;
    } else {
      line[count].joiners = pendingJoiners << 4;
      pendingJoiners = 0;
      lastBase = count;
    }
    count++;
  }
  if (!count) return false;

  const bool autodir = (paragraphLevel < 0);
  const int level = autodir ? 0 : (paragraphLevel & 1);

  // Order matters (mintty does the same): do_bidi() first to obtain visual
  // order, then do_shape() — contextual forms are resolved from *visual*
  // adjacency, and shaping presentation forms must never be reordered.
  do_bidi(autodir, level, line, count);
  // witchhunt: deviation 2 - the non-shaping instantiation has no call to do_shape, so the linker can drop it.
  // witchhunt: deviation 1 - the shaped buffer is a per-call scratch, not part of the shared workspace.
  std::unique_ptr<ShapedBuffer> scratch;
  if constexpr (Shape) {
    scratch = makeUniqueNoThrow<ShapedBuffer>();
    if (!scratch) {
      LOG_ERR("BIDI", "Failed to allocate %zu-byte shaping buffer", sizeof(ShapedBuffer));
      return false;
    }
    do_shape(line, scratch->chars, count);
  }
  // witchhunt: deviation 2 - without shaping, the emission loop reads the reordered line directly.
  const bidi_char* const visualLine = Shape ? scratch->chars : line;

  out.clear();
  out.reserve(std::strlen(utf8));
  // Lam-Alef collapse sentinel and zero-width joining formatters have done
  // their job during shaping and have no glyphs to render.
  const auto filtered = [](const uint32_t cp) { return cp == LIGATURE_PLACEHOLDER || cp == 0x200C || cp == 0x200D; };
  for (int i = 0; i < count; i++) {
    const uint32_t cp = visualLine[i].wc;  // witchhunt: deviation 2 - visualLine instead of shaped
    if (filtered(cp)) continue;
    // witchhunt: deviation 3 - upstream's divine-name match (#3834) is removed here; lib/TextShaping's Hebrew
    // module matches it in logical order instead.
    if (!isTransparentMark(cp)) {
      utf8AppendCodepoint(cp, out);
      continue;
    }
    // UAX#9 rule L3: reversing an RTL run leaves combining marks *before*
    // their base character. The renderer overlays a mark on the most
    // recently drawn glyph, so emit the base first, then its marks in
    // logical order. `index` is the original logical position: a base
    // following its marks with a *lower* index means the run was reversed.
    int j = i;  // [i, j) = the run of marks (and filtered entries)
    while (j < count && (filtered(visualLine[j].wc) || isTransparentMark(visualLine[j].wc)))
      j++;                                                         // witchhunt: deviation 2
    if (j < count && visualLine[j].index < visualLine[i].index) {  // witchhunt: deviation 2
      utf8AppendCodepoint(visualLine[j].wc, out);                  // witchhunt: deviation 2
      for (int k = j - 1; k >= i; k--) {
        if (isTransparentMark(visualLine[k].wc)) utf8AppendCodepoint(visualLine[k].wc, out);  // witchhunt: deviation 2
      }
      i = j;  // base already emitted
    } else {
      // Unreversed (or trailing, base-less) marks already follow their base.
      for (int k = i; k < j; k++) {
        if (isTransparentMark(visualLine[k].wc)) utf8AppendCodepoint(visualLine[k].wc, out);  // witchhunt: deviation 2
      }
      i = j - 1;
    }
  }
  return true;
}

}  // namespace

// witchhunt: deviation 2 - the public entry points over the shared template body.
bool applyBidiVisual(const char* utf8, std::string& out, int paragraphLevel) {
  return applyBidiVisualImpl<false>(utf8, out, paragraphLevel);
}

bool applyBidiVisualShaped(const char* utf8, std::string& out, int paragraphLevel) {
  return applyBidiVisualImpl<true>(utf8, out, paragraphLevel);
}

// witchhunt: deviation 1 - release the workspace when a section build ends; getWorkspace() reallocates on demand.
void releaseWorkspace() {
  const std::lock_guard<std::mutex> lock(bidiMutex);
  bidiWorkspace.reset();
  boundWorkspace = nullptr;
}

// witchhunt: deviation 1
void bindWorkspace(void* buffer) {
  const std::lock_guard<std::mutex> lock(bidiMutex);
  boundWorkspace = static_cast<BidiWorkspace*>(buffer);
}

// witchhunt: deviation 11
bool heapWorkspaceHeld() {
  const std::lock_guard<std::mutex> lock(bidiMutex);
  return static_cast<bool>(bidiWorkspace);
}

// witchhunt: deviation 11
int strongLevel(const uint32_t cp) {
  const uchar cls = bidi_class(cp);
  if (cls == R || cls == AL) return 1;
  return cls == L ? 0 : -1;
}

// witchhunt: deviation 11
bool hasRtlClass(const char* utf8, const size_t len) {
  if (!utf8) return false;
  const auto* const s = reinterpret_cast<const unsigned char*>(utf8);
  for (size_t i = 0; i < len; i++) {
    const unsigned char b = s[i];
    bool candidate = false;
    if (b >= 0xD6 && b <= 0xDF) {
      candidate = true;
    } else if (b == 0xE0) {
      candidate = i + 1 < len && s[i + 1] >= 0xA0 && s[i + 1] <= 0xA3;
    } else if (b == 0xE2) {
      if (i + 2 < len) {
        const unsigned char b1 = s[i + 1], b2 = s[i + 2];
        candidate =
            (b1 == 0x80 && (b2 == 0x8F || b2 == 0xAB || b2 == 0xAE)) || (b1 == 0x81 && (b2 == 0xA7 || b2 == 0xA8));
      }
    } else if (b == 0xEF) {
      candidate = i + 1 < len && s[i + 1] >= 0xAC && s[i + 1] <= 0xBB;
    }
    if (!candidate) continue;
    // Decode from a bounded copy: utf8NextCodepoint reads up to the lead byte's length and a NUL stops it.
    char tmp[5] = {};
    for (size_t k = 0; k < 4 && i + k < len; k++) tmp[k] = static_cast<char>(s[i + k]);
    const auto* p = reinterpret_cast<const unsigned char*>(tmp);
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp && is_rtl_class(bidi_class(cp))) return true;
  }
  return false;
}

// witchhunt: deviation 11 - re-states applyBidiVisual's marks-after-base rule (L3) over indices.
bool orderLine(const uint32_t* cps, const size_t count, const int paragraphLevel, uint16_t* visual, uint32_t* drawn) {
  if (!cps || !visual || !drawn || count == 0 || count > kMaxLineCodepoints) return false;
  const std::lock_guard<std::mutex> lock(bidiMutex);

  auto* const workspace = getWorkspace();
  if (!workspace) return false;
  bidi_char* const line = workspace->line;
  const int n = static_cast<int>(count);
  for (int i = 0; i < n; i++) {
    line[i].origwc = line[i].wc = cps[i];
    line[i].index = static_cast<uint16_t>(i);
    line[i].joiners = 0;
  }
  do_bidi(/*autodir=*/false, paragraphLevel & 1, line, n);

  int out = 0;
  for (int i = 0; i < n; i++) {
    if (!isTransparentMark(line[i].wc)) {
      visual[out] = line[i].index;
      drawn[out++] = line[i].wc;
      continue;
    }
    // A run of marks [i, j): when the run was reversed, its base follows it with a lower logical index; emit the base
    // first, then the marks in logical order. Otherwise the marks already follow their base.
    int j = i;
    while (j < n && isTransparentMark(line[j].wc)) j++;
    if (j < n && line[j].index < line[i].index) {
      visual[out] = line[j].index;
      drawn[out++] = line[j].wc;
      for (int k = j - 1; k >= i; k--) {
        visual[out] = line[k].index;
        drawn[out++] = line[k].wc;
      }
      i = j;
    } else {
      for (int k = i; k < j; k++) {
        visual[out] = line[k].index;
        drawn[out++] = line[k].wc;
      }
      i = j - 1;
    }
  }
  return true;
}

bool computeVisualWordOrder(const std::vector<std::string>& words, bool paragraphIsRtl,
                            std::vector<uint16_t>& visualOrder) {
  visualOrder.clear();
  const size_t nWords = words.size();
  if (nWords <= 1 || nWords > BIDI_MAX_LINE) return false;
  const std::lock_guard<std::mutex> lock(bidiMutex);

  auto* const workspace = getWorkspace();
  if (!workspace) return false;
  bidi_char* const line = workspace->line;
  int count = 0;
  bool truncated = false;

  for (size_t w = 0; w < nWords && !truncated; w++) {
    auto* p = reinterpret_cast<const unsigned char*>(words[w].c_str());
    while (*p) {
      if (count >= BIDI_MAX_LINE) {
        truncated = true;
        break;
      }
      const uint32_t cp = utf8NextCodepoint(&p);
      if (!cp || cp == REPLACEMENT_GLYPH) break;
      line[count].origwc = line[count].wc = cp;
      line[count].index = static_cast<uint16_t>(w);
      line[count].joiners = 0;
      count++;
    }

    if (!truncated && w + 1 < nWords) {
      if (count >= BIDI_MAX_LINE) {
        truncated = true;
        break;
      }
      line[count].origwc = line[count].wc = ' ';
      line[count].index = static_cast<uint16_t>(nWords);
      line[count].joiners = 0;
      count++;
    }
  }

  if (truncated || count == 0) return false;

  // Fast-path for homogeneous lines: skip UAX#9 if there's no mixing.
  bool hasL = false, hasR = false;
  for (int i = 0; i < count; i++) {
    uchar bc = bidi_class(line[i].wc);
    if (bc == L || bc == EN || bc == AN)
      hasL = true;
    else if (bc == R || bc == AL)
      hasR = true;
  }

  // Purely LTR line in RTL paragraph: identity order, but we might still need to reorder
  // if some characters are mirrored or neutral resolution differs.
  // Actually, UAX#9 rule L1/L2 says purely LTR in RTL para stays as is (identity).
  // Purely RTL line: just reverse the words.
  if (!hasL && hasR && paragraphIsRtl) {
    visualOrder.reserve(nWords);
    for (int i = static_cast<int>(nWords) - 1; i >= 0; i--) {
      visualOrder.push_back(static_cast<uint16_t>(i));
    }
    return true;
  }
  if (!hasR) {
    if (!paragraphIsRtl) {
      // Pure LTR in LTR paragraph: nothing to do.
      return false;
    }
    // Pure LTR in RTL paragraph: no word reordering, but must use the
    // willReorder (left-to-right) positioning path, not the RTL right-to-left path.
    visualOrder.reserve(nWords);
    for (size_t i = 0; i < nWords; i++) {
      visualOrder.push_back(static_cast<uint16_t>(i));
    }
    return true;
  }

  do_bidi(/*autodir=*/false, paragraphIsRtl ? 1 : 0, line, count);

  uint16_t firstAny[BIDI_MAX_LINE];
  uint16_t firstNatural[BIDI_MAX_LINE];
  for (size_t w = 0; w < nWords; w++) {
    firstAny[w] = UINT16_MAX;
    firstNatural[w] = UINT16_MAX;
  }

  for (int i = 0; i < count; i++) {
    const uint16_t w = line[i].index;
    if (w >= nWords) continue;

    if (firstAny[w] == UINT16_MAX) {
      firstAny[w] = static_cast<uint16_t>(i);
    }

    if (firstNatural[w] == UINT16_MAX && isNaturalDirectionClass(bidi_class(line[i].wc))) {
      firstNatural[w] = static_cast<uint16_t>(i);
    }
  }

  visualOrder.reserve(nWords);
  for (int i = 0; i < count; i++) {
    const uint16_t w = line[i].index;
    if (w >= nWords) continue;

    const uint16_t anchor = firstNatural[w] != UINT16_MAX ? firstNatural[w] : firstAny[w];
    if (anchor == UINT16_MAX) {
      visualOrder.clear();
      return false;
    }
    if (anchor == static_cast<uint16_t>(i)) {
      visualOrder.push_back(w);
    }
  }

  if (visualOrder.size() != nWords) {
    visualOrder.clear();
    return false;
  }

  // Check if the order is exactly the same as the original input
  bool needsReorder = false;
  for (size_t i = 0; i < nWords; i++) {
    if (visualOrder[i] != i) {
      needsReorder = true;
      break;
    }
  }

  if (!needsReorder) {
    visualOrder.clear();
    return false;
  }

  return true;
}

}  // namespace BidiUtils
