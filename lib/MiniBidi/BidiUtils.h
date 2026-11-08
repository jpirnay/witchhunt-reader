#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace BidiUtils {

// Paragraph-level P2/P3: scan the first N strong chars per word to find base direction.
inline constexpr int RTL_PARAGRAPH_PROBE_DEPTH = 5;

bool startsWithRtl(const char* utf8, int maxStrongChars = RTL_PARAGRAPH_PROBE_DEPTH);

int detectParagraphLevel(const char* utf8, int fallbackLevel = 0, int maxStrongChars = 64);

// True for RTL-script non-spacing marks (Hebrew niqqud/cantillation, Arabic
// harakat and Quranic annotation): zero-width for measurement, transparent for
// Arabic joining, and rendered as overlays on the preceding base glyph when
// the active font carries their glyphs (SD fonts; built-in fonts don't).
// Latin combining marks (U+0300-U+036F) intentionally return false — they are
// handled by the utf8IsCombiningMark() rendering path.
bool isTransparentMark(uint32_t cp);

// paragraphLevel: -1 = auto-detect, 0 = LTR, 1 = RTL
// On failure (including workspace OOM), returns false and leaves out unchanged.
// witchhunt: deviation 2 - Arabic letters are reordered but not shaped (no do_shape, no lam-alef ligature).
bool applyBidiVisual(const char* utf8, std::string& out, int paragraphLevel = -1);

// witchhunt: deviation 2 - upstream's behaviour, with Arabic contextual shaping (do_shape). Unused in step 1.
bool applyBidiVisualShaped(const char* utf8, std::string& out, int paragraphLevel = -1);

// witchhunt: deviation 1 - frees the shared workspace; the next call allocates it again. Also unbinds a lent buffer.
void releaseWorkspace();

// witchhunt: deviation 11
inline constexpr size_t kMaxLineCodepoints = 128;  // BIDI_MAX_LINE, for callers without minibidi.h
// witchhunt: deviation 1
inline constexpr size_t kWorkspaceBytes = 1536;  // BIDI_MAX_LINE bidi_chars; static_assert in the .cpp

// witchhunt: deviation 11
// Orders cps[0..count) (logical) at paragraphLevel (0 or 1; no autodetection) with one do_bidi in the shared
// workspace. On true: visual[v] is the logical index drawn at visual position v and drawn[v] the code point drawn
// there (mirrored by L4); each run of transparent marks follows its base, as applyBidiVisual emits it. False, the
// outputs untouched, for count 0, count > kMaxLineCodepoints, or no workspace. Unlike applyBidiVisual it drops
// nothing: ZWJ, ZWNJ and the explicit controls stay in `drawn`, so the caller filters what it does not draw.
bool orderLine(const uint32_t* cps, size_t count, int paragraphLevel, uint16_t* visual, uint32_t* drawn);

// witchhunt: deviation 11
// Whether a code point of utf8[0..len) has bidi class R, AL, RLE, RLO, RLI or FSI (is_rtl_class). A byte scan
// first; only candidates are decoded: lead bytes D6-DF, E0 A0-A3, E2 80 8F/AB/AE, E2 81 A7/A8, EF AC-BB.
bool hasRtlClass(const char* utf8, size_t len);

// witchhunt: deviation 11
// 1 for class R or AL, 0 for L, -1 otherwise.
int strongLevel(uint32_t cp);

// witchhunt: deviation 1
// While bound, `buffer` (kWorkspaceBytes, 4-aligned) is the shared workspace and nothing is allocated; nullptr
// unbinds. A heap workspace allocated earlier stays until releaseWorkspace(). The buffer must outlive the binding:
// unbind (or releaseWorkspace()) before freeing or reusing it.
void bindWorkspace(void* buffer);

// witchhunt: deviation 11
// Whether a heap workspace is allocated (tests: none survives a build).
bool heapWorkspaceHeld();

// witchhunt: whether orderLine would have a workspace now: the bound one, or the heap one, made here if it can be.
// A caller with a short path that needs none asks first, so that it takes the same path as the full one would
// without a workspace (the line is then laid out without bidi, and a form is its text).
bool workspaceAvailable();

// Returns false with empty visualOrder when no order is produced, including OOM.
bool computeVisualWordOrder(const std::vector<std::string>& words, bool paragraphIsRtl,
                            std::vector<uint16_t>& visualOrder);

}  // namespace BidiUtils
