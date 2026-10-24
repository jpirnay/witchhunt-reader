#pragma once

#include <cstdint>

#include "hyphenation/HyphenationCommon.h"

// Ported from crosspoint-reader PR #2892 ("fix: hyphenate focus-split words and break after visible
// hyphens", Uri Tauber), commit 25cf99b2: the two predicates below are theirs, verbatim; Boundary and the
// predicates over it, at the end, are ours.
// Upstream ParsedText stores each boundary in two bit-packed vectors. Together the bits distinguish
// ordinary word gaps, CJK-style stretchable zero-width gaps, unbreakable attachment, and
// breakable attachment after visible hyphens/dashes.
namespace TokenBoundary {

// A continuation normally forbids a line break. noSpaceBefore marks the fourth state where the
// text stays attached when unbroken but the line breaker may still stop at the boundary.
constexpr bool allowsBreak(const bool continues, const bool noSpaceBefore) { return !continues || noSpaceBefore; }

// Soft hyphens become visible only when their conditional break is taken. U+2011 exists to forbid
// a break. Every other character classified by isExplicitHyphen is a visible break opportunity.
inline bool allowsBreakAfterExplicitHyphen(const uint32_t cp) {
  constexpr uint32_t NON_BREAKING_HYPHEN_CP = 0x2011;
  return isExplicitHyphen(cp) && !isSoftHyphen(cp) && cp != NON_BREAKING_HYPHEN_CP;
}

// Ours: ParsedText keeps one of these per token instead of upstream's two bit vectors, so the two
// halves of a boundary travel together through every insert, erase and copy of the word lists.
// The part a token plays in a ruby group (a base with its ruby text above it): the first token of the group leads,
// the rest follow. The ruby text itself is kept by ParsedText, one per leader, in token order.
enum class RubyRole : uint8_t { None = 0, Leader = 1, Follower = 2 };

struct Boundary {
  bool continues = false;      // glued to the previous token: no space, and no break
  bool noSpaceBefore = false;  // no space either; with continues=false, a breakable CJK gap
  uint8_t annotation = 0;      // bits 0-1 RubyRole; bits 2-5 the emphasis mark (not used yet)
};

constexpr bool allowsBreak(const Boundary b) { return allowsBreak(b.continues, b.noSpaceBefore); }

// True for an ordinary word gap, the only kind measured as a space.
constexpr bool hasSpace(const Boundary b) { return !b.continues && !b.noSpaceBefore; }

constexpr RubyRole rubyRole(const Boundary b) { return static_cast<RubyRole>(b.annotation & 0x03); }

constexpr Boundary withRubyRole(Boundary b, const RubyRole role) {
  b.annotation = static_cast<uint8_t>((b.annotation & ~0x03) | static_cast<uint8_t>(role));
  return b;
}

}  // namespace TokenBoundary
