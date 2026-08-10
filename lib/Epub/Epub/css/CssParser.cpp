#include "CssParser.h"

#include <Arduino.h>
#include <BuildArena.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <new>
#include <string_view>

// Sparse resident-style codec (defined below, used by lookupRule before its definition).
static size_t compressStyle(const CssStyle& s, uint8_t* out);
static void decompressStyle(const uint8_t* in, CssStyle& out);

namespace {

// Stack-allocated string buffer to avoid heap reallocations during parsing
// Provides string-like interface with fixed capacity
struct StackBuffer {
  static constexpr size_t CAPACITY = 1024;
  char data[CAPACITY];
  size_t len = 0;
  bool overflowed = false;  // set once content exceeds CAPACITY so callers can reject the whole token

  void push_back(char c) {
    if (len < CAPACITY - 1) {
      data[len++] = c;
    } else {
      overflowed = true;  // dropping this char would silently truncate — flag it instead
    }
  }

  void clear() {
    len = 0;
    overflowed = false;
  }
  bool empty() const { return len == 0; }
  size_t size() const { return len; }

  // Get string view of current content (zero-copy)
  std::string_view view() const { return std::string_view(data, len); }

  // Convert to string for passing to functions (single allocation)
  std::string str() const { return std::string(data, len); }
};

// Buffer size for reading CSS files
constexpr size_t READ_BUFFER_SIZE = 512;

// Maximum number of CSS rules to store in the selector map
// Prevents unbounded memory growth from pathological CSS files
constexpr size_t MAX_RULES = 1500;

// Minimum free heap required to apply CSS during rendering
// If below this threshold, we skip CSS to avoid display artifacts.
#ifndef CSS_MIN_FREE_HEAP_FOR_CSS
#define CSS_MIN_FREE_HEAP_FOR_CSS (40 * 1024)
#endif

constexpr size_t MIN_FREE_HEAP_FOR_CSS = CSS_MIN_FREE_HEAP_FOR_CSS;

// Lean-mode floor (setLeanResolve): the index lives off-heap in the arena and the hot cache
// is disabled, so resolveStyle allocates only a few small transient strings. The real fault
// zone is ~13-15 KB free (see EpubReaderActivity RESIDENT_BUILD_ABORT_*), so 24 KB keeps a
// comfortable margin while letting a borrowed build (~40 KB free) resolve without degrading.
#ifndef CSS_LEAN_MIN_FREE_HEAP_FOR_CSS
#define CSS_LEAN_MIN_FREE_HEAP_FOR_CSS (24 * 1024)
#endif

constexpr size_t LEAN_MIN_FREE_HEAP_FOR_CSS = CSS_LEAN_MIN_FREE_HEAP_FOR_CSS;

// In-memory CSS rule cache sizing for disk-backed lookup mode.
// Keeps memory bounded on large books while retaining hot selectors.
#ifndef CSS_HOT_RULE_CACHE_SIZE
#define CSS_HOT_RULE_CACHE_SIZE 128
#endif

#ifndef CSS_NEGATIVE_CACHE_SIZE
#define CSS_NEGATIVE_CACHE_SIZE 256
#endif

constexpr size_t HOT_RULE_CACHE_SIZE = CSS_HOT_RULE_CACHE_SIZE;
constexpr size_t NEGATIVE_CACHE_SIZE = CSS_NEGATIVE_CACHE_SIZE;

// Maximum length for a single selector string
// Prevents parsing of extremely long or malformed selectors
constexpr size_t MAX_SELECTOR_LENGTH = 256;

constexpr size_t CSS_LENGTH_FIELD_COUNT = 11;
constexpr size_t CSS_LENGTH_BYTES = sizeof(float) + sizeof(uint8_t);
// Layout: 4 enum bytes + 11 lengths + display byte + definedBits uint16 + 2 vertAlign bytes + cssFloat byte
//         + smallCaps byte + fontSizeMultiplier float + fontSize flags byte + block flags byte
//         (listStyleNone / pageBreakBefore / pageBreakAfter value+defined pairs)
constexpr size_t CSS_FIXED_STYLE_BYTES = 4 * sizeof(uint8_t) + (CSS_LENGTH_FIELD_COUNT * CSS_LENGTH_BYTES) +
                                         sizeof(uint8_t) + sizeof(uint16_t) + 2 * sizeof(uint8_t) + sizeof(uint8_t) +
                                         sizeof(uint8_t) + sizeof(float) + sizeof(uint8_t) + sizeof(uint8_t);
static_assert(CSS_FIXED_STYLE_BYTES == 72,
              "style payload layout changed — update read/writeCssStylePayload and bump CSS_CACHE_VERSION");

// Cache file name (version is CssParser::CSS_CACHE_VERSION)
constexpr char rulesCache[] = "/css_rules.cache";
constexpr char compileTempRulesCache[] = "/css_rules.compile.tmp";

// Check if character is CSS whitespace
bool isCssWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

// Resolver supports: tag, .class, tag.class, #id, tag#id
bool isSelectorUsableByResolver(std::string_view selector) {
  if (selector.empty()) {
    return false;
  }

  // Reject combinators, pseudo-classes, wildcards, and whitespace
  if (selector.find_first_of("+>:[~* ") != std::string_view::npos) {
    return false;
  }

  const size_t hashPos = selector.find('#');
  const size_t dotPos = selector.find('.');

  // Reject selectors with both # and . (too complex)
  if (hashPos != std::string_view::npos && dotPos != std::string_view::npos) {
    return false;
  }

  if (hashPos != std::string_view::npos) {
    // #id  (hashPos == 0, must have content after)
    if (hashPos == 0) {
      return selector.size() > 1 && selector.find('#', 1) == std::string_view::npos;
    }
    // tag#id (no second #, must have content after #)
    return hashPos + 1 < selector.size() && selector.find('#', hashPos + 1) == std::string_view::npos;
  }

  if (dotPos == std::string_view::npos) {
    return true;  // tag
  }

  if (dotPos == 0) {
    return selector.size() > 1 && selector.find('.', 1) == std::string_view::npos;  // .class only
  }

  // tag.class only, no additional dots
  return dotPos + 1 < selector.size() && selector.find('.', dotPos + 1) == std::string_view::npos;
}

template <typename Fn>
void forEachNormalizedClassToken(const std::string& classAttr, std::string& normalizedBuf, Fn&& fn) {
  size_t i = 0;
  while (i < classAttr.size()) {
    while (i < classAttr.size() && isCssWhitespace(classAttr[i])) {
      ++i;
    }
    if (i >= classAttr.size()) {
      break;
    }

    const size_t start = i;
    while (i < classAttr.size() && !isCssWhitespace(classAttr[i])) {
      ++i;
    }

    normalizedBuf.clear();
    normalizedBuf.reserve(i - start);
    for (size_t j = start; j < i; ++j) {
      normalizedBuf.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(classAttr[j]))));
    }
    if (!normalizedBuf.empty()) {
      fn(normalizedBuf);
    }
  }
}

std::string_view stripTrailingImportant(std::string_view value) {
  constexpr std::string_view IMPORTANT = "!important";

  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }

  if (value.size() < IMPORTANT.size()) {
    return value;
  }

  const size_t suffixPos = value.size() - IMPORTANT.size();
  if (value.substr(suffixPos) != IMPORTANT) {
    return value;
  }

  value.remove_suffix(IMPORTANT.size());
  while (!value.empty() && isCssWhitespace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

}  // anonymous namespace

// FNV-1a 32-bit hash — no heap, good distribution over short selector strings.
// 32 bits keep the selector index at 8 bytes/entry; collisions are resolved by
// verifying the on-disk selector string in readRuleFromDiskAtOffset().
uint32_t CssParser::selectorHash(std::string_view s) {
  uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

// String utilities implementation

std::string CssParser::normalized(const std::string& s) {
  std::string result;
  result.reserve(s.size());

  bool inSpace = true;  // Start true to skip leading space
  for (const char c : s) {
    if (isCssWhitespace(c)) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      inSpace = false;
    }
  }

  // Remove trailing space
  while (!result.empty() && (result.back() == ' ' || result.back() == '\n')) {
    result.pop_back();
  }
  return result;
}

void CssParser::normalizedInto(const std::string_view s, std::string& out) {
  out.clear();
  out.reserve(s.size());

  bool inSpace = true;  // Start true to skip leading space
  for (const char c : s) {
    if (isCssWhitespace(c)) {
      if (!inSpace) {
        out.push_back(' ');
        inSpace = true;
      }
    } else {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      inSpace = false;
    }
  }

  if (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
}

std::vector<std::string> CssParser::splitWhitespace(const std::string& s) {
  std::vector<std::string> parts;
  size_t start = 0;
  bool inWord = false;

  for (size_t i = 0; i <= s.size(); ++i) {
    const bool isSpace = i == s.size() || isCssWhitespace(s[i]);
    if (isSpace && inWord) {
      parts.push_back(s.substr(start, i - start));
      inWord = false;
    } else if (!isSpace && !inWord) {
      start = i;
      inWord = true;
    }
  }
  return parts;
}

// Property value interpreters
//
// These take a std::string_view and do NOT normalize. Every caller already passes normalized
// text: parseDeclarationIntoStyle fills propValueBuf via normalizedInto(), splitOnChar()
// normalizes each part it emits, and splitWhitespace() splits an already-normalized string on
// single spaces. normalized() is idempotent (lowercase + collapse whitespace + trim), so the
// re-normalization these used to do was provably a no-op that cost one heap string per call.
//
// That waste is not free even though CSS compiles once per book: on a no-compaction heap the
// holes a short-lived string leaves are permanent for the session, so first-open churn is still
// there when a later chapter needs a contiguous block.

CssTextAlign CssParser::interpretAlignment(const std::string_view v) {
  if (v == "left" || v == "start") return CssTextAlign::Left;
  if (v == "right" || v == "end") return CssTextAlign::Right;
  if (v == "center") return CssTextAlign::Center;
  if (v == "justify") return CssTextAlign::Justify;

  return CssTextAlign::Left;
}

CssFontStyle CssParser::interpretFontStyle(const std::string_view v) {
  if (v == "italic" || v == "oblique") return CssFontStyle::Italic;
  return CssFontStyle::Normal;
}

CssFontWeight CssParser::interpretFontWeight(const std::string_view v) {
  // Named values
  if (v == "bold" || v == "bolder") return CssFontWeight::Bold;
  if (v == "normal" || v == "lighter") return CssFontWeight::Normal;

  // Numeric values: 100-900
  // CSS spec: 400 = normal, 700 = bold
  // We use: 0-400 = normal, 700+ = bold, 500-600 = normal (conservative)
  // Parsed digit-by-digit rather than via strtol so no NUL-terminated copy is needed; the
  // range is 3 digits, so overflow is not a concern once the length is bounded.
  if (v.empty() || v.size() > 4) return CssFontWeight::Normal;
  long numericWeight = 0;
  for (const char c : v) {
    if (c < '0' || c > '9') return CssFontWeight::Normal;  // not a pure number (e.g. "inherit")
    numericWeight = numericWeight * 10 + (c - '0');
  }
  return numericWeight >= 700 ? CssFontWeight::Bold : CssFontWeight::Normal;
}

CssTextDecoration CssParser::interpretDecoration(const std::string_view v) {
  // text-decoration can have multiple space-separated values
  const bool underline = v.find("underline") != std::string_view::npos;
  const bool lineThrough = v.find("line-through") != std::string_view::npos;
  uint8_t result = 0;
  if (underline) result |= static_cast<uint8_t>(CssTextDecoration::Underline);
  if (lineThrough) result |= static_cast<uint8_t>(CssTextDecoration::LineThrough);
  return static_cast<CssTextDecoration>(result);
}

CssLength CssParser::interpretLength(const std::string_view val) {
  CssLength result;
  tryInterpretLength(val, result);
  return result;
}

bool CssParser::tryInterpretLength(const std::string_view v, CssLength& out) {
  // Was three heap strings per call — normalized(val) plus a substr for each of the number and
  // unit halves — on a function invoked for every margin/padding/indent in the sheet (and four
  // times over for each shorthand). Now zero: the input is already normalized (see the note
  // above the interpreters) and both halves are views into it.
  if (v.empty()) {
    out = CssLength{};
    return false;
  }

  size_t unitStart = v.size();
  for (size_t i = 0; i < v.size(); ++i) {
    const char c = v[i];
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '+') {
      unitStart = i;
      break;
    }
  }

  const std::string_view numPart = v.substr(0, unitStart);
  const std::string_view unitPart = v.substr(unitStart);

  // strtof needs a NUL-terminated buffer and string_view gives no guarantee of one. The number
  // half of a CSS length is short by construction, so copy it into a small stack buffer rather
  // than heap-allocating; anything longer than the buffer is not a valid length anyway.
  char numBuf[24];
  if (numPart.empty() || numPart.size() >= sizeof(numBuf)) {
    out = CssLength{};
    return false;
  }
  std::memcpy(numBuf, numPart.data(), numPart.size());
  numBuf[numPart.size()] = '\0';

  char* endPtr = nullptr;
  const float numericValue = std::strtof(numBuf, &endPtr);
  if (endPtr == numBuf) {
    out = CssLength{};
    return false;  // No number parsed (e.g. auto, inherit, initial)
  }

  auto unit = CssUnit::Pixels;
  if (unitPart == "em") {
    unit = CssUnit::Em;
  } else if (unitPart == "rem") {
    unit = CssUnit::Rem;
  } else if (unitPart == "pt") {
    unit = CssUnit::Points;
  } else if (unitPart == "%") {
    unit = CssUnit::Percent;
  }

  out = CssLength{numericValue, unit};
  return true;
}

// Declaration parsing

void CssParser::parseDeclarationIntoStyle(const std::string_view decl, CssStyle& style, std::string& propNameBuf,
                                          std::string& propValueBuf) {
  const size_t colonPos = decl.find(':');
  if (colonPos == std::string::npos || colonPos == 0) return;

  // substr on a string_view is a view, not a copy: the two halves are normalized straight into
  // the caller's scratchpad, so a declaration costs zero heap blocks once those have grown.
  normalizedInto(decl.substr(0, colonPos), propNameBuf);
  normalizedInto(decl.substr(colonPos + 1), propValueBuf);

  if (propNameBuf.empty() || propValueBuf.empty()) return;

  if (propNameBuf == "text-align") {
    style.textAlign = interpretAlignment(propValueBuf);
    style.defined.textAlign = 1;
  } else if (propNameBuf == "font-style") {
    style.fontStyle = interpretFontStyle(propValueBuf);
    style.defined.fontStyle = 1;
  } else if (propNameBuf == "font-weight") {
    style.fontWeight = interpretFontWeight(propValueBuf);
    style.defined.fontWeight = 1;
  } else if (propNameBuf == "text-decoration" || propNameBuf == "text-decoration-line") {
    style.textDecoration = interpretDecoration(propValueBuf);
    style.defined.textDecoration = 1;
  } else if (propNameBuf == "text-indent") {
    style.textIndent = interpretLength(propValueBuf);
    style.defined.textIndent = 1;
  } else if (propNameBuf == "margin-top") {
    style.marginTop = interpretLength(propValueBuf);
    style.defined.marginTop = 1;
  } else if (propNameBuf == "margin-bottom") {
    style.marginBottom = interpretLength(propValueBuf);
    style.defined.marginBottom = 1;
  } else if (propNameBuf == "margin-left") {
    style.marginLeft = interpretLength(propValueBuf);
    style.defined.marginLeft = 1;
  } else if (propNameBuf == "margin-right") {
    style.marginRight = interpretLength(propValueBuf);
    style.defined.marginRight = 1;
  } else if (propNameBuf == "margin") {
    const auto values = splitWhitespace(propValueBuf);
    if (!values.empty()) {
      style.marginTop = interpretLength(values[0]);
      style.marginRight = values.size() >= 2 ? interpretLength(values[1]) : style.marginTop;
      style.marginBottom = values.size() >= 3 ? interpretLength(values[2]) : style.marginTop;
      style.marginLeft = values.size() >= 4 ? interpretLength(values[3]) : style.marginRight;
      style.defined.marginTop = style.defined.marginRight = style.defined.marginBottom = style.defined.marginLeft = 1;
    }
  } else if (propNameBuf == "padding-top") {
    style.paddingTop = interpretLength(propValueBuf);
    style.defined.paddingTop = 1;
  } else if (propNameBuf == "padding-bottom") {
    style.paddingBottom = interpretLength(propValueBuf);
    style.defined.paddingBottom = 1;
  } else if (propNameBuf == "padding-left") {
    style.paddingLeft = interpretLength(propValueBuf);
    style.defined.paddingLeft = 1;
  } else if (propNameBuf == "padding-right") {
    style.paddingRight = interpretLength(propValueBuf);
    style.defined.paddingRight = 1;
  } else if (propNameBuf == "padding") {
    const auto values = splitWhitespace(propValueBuf);
    if (!values.empty()) {
      style.paddingTop = interpretLength(values[0]);
      style.paddingRight = values.size() >= 2 ? interpretLength(values[1]) : style.paddingTop;
      style.paddingBottom = values.size() >= 3 ? interpretLength(values[2]) : style.paddingTop;
      style.paddingLeft = values.size() >= 4 ? interpretLength(values[3]) : style.paddingRight;
      style.defined.paddingTop = style.defined.paddingRight = style.defined.paddingBottom = style.defined.paddingLeft =
          1;
    }
  } else if (propNameBuf == "height") {
    CssLength len;
    if (tryInterpretLength(propValueBuf, len)) {
      style.imageHeight = len;
      style.defined.imageHeight = 1;
    }
  } else if (propNameBuf == "width") {
    CssLength len;
    if (tryInterpretLength(propValueBuf, len)) {
      style.imageWidth = len;
      style.defined.imageWidth = 1;
    }
  } else if (propNameBuf == "display") {
    const std::string_view displayValue = stripTrailingImportant(propValueBuf);
    style.display = (displayValue == "none") ? CssDisplay::None : CssDisplay::Block;
    style.defined.display = 1;
  } else if (propNameBuf == "vertical-align") {
    const std::string_view va = stripTrailingImportant(propValueBuf);
    if (va == "super") {
      style.verticalAlign = CssVerticalAlign::Super;
      style.defined.verticalAlign = 1;
    } else if (va == "sub") {
      style.verticalAlign = CssVerticalAlign::Sub;
      style.defined.verticalAlign = 1;
    } else if (va == "baseline") {
      style.verticalAlign = CssVerticalAlign::Baseline;
      style.defined.verticalAlign = 1;
    }
  } else if (propNameBuf == "font-variant" || propNameBuf == "font-variant-caps") {
    const std::string_view val = stripTrailingImportant(propValueBuf);
    if (val == "small-caps" || val == "all-small-caps") {
      style.smallCaps = true;
      style.defined.smallCaps = 1;
    } else if (val == "normal" || val == "none") {
      style.smallCaps = false;
      style.defined.smallCaps = 1;
    }
  } else if (propNameBuf == "float") {
    const std::string_view val = stripTrailingImportant(propValueBuf);
    if (val == "left") {
      style.cssFloat = CssFloat::Left;
      style.defined.cssFloat = 1;
    } else if (val == "right") {
      style.cssFloat = CssFloat::Right;
      style.defined.cssFloat = 1;
    } else if (val == "none") {
      style.cssFloat = CssFloat::None;
      style.defined.cssFloat = 1;
    }
  } else if (propNameBuf == "list-style-type" || propNameBuf == "list-style") {
    const std::string_view val = stripTrailingImportant(propValueBuf);
    if (val == "none") {
      style.listStyleNone = true;
      style.defined.listStyleNone = 1;
    }
  } else if (propNameBuf == "page-break-before" || propNameBuf == "break-before") {
    const std::string_view val = stripTrailingImportant(propValueBuf);
    if (val == "always" || val == "page" || val == "left" || val == "right") {
      style.pageBreakBefore = true;
      style.defined.pageBreakBefore = 1;
    }
  } else if (propNameBuf == "page-break-after" || propNameBuf == "break-after") {
    const std::string_view val = stripTrailingImportant(propValueBuf);
    if (val == "always" || val == "page" || val == "left" || val == "right") {
      style.pageBreakAfter = true;
      style.defined.pageBreakAfter = 1;
    }
  } else if (propNameBuf == "line-height") {
    const std::string_view val = stripTrailingImportant(propValueBuf);
    if (val != "normal" && val != "inherit" && val != "initial" && val != "unset") {
      // Parse unitless, %, or em. Normalise to a multiplier relative to default y_advance.
      // Base = 1.5 (typical body line-height). Result range clamped to [0.7, 2.0].
      static constexpr float kBase = 1.5f;
      float parsed = 0.0f;
      bool ok = false;
      if (!val.empty() && val.back() == '%') {
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v / 100.0f / kBase;
          ok = true;
        }
      } else if (val.size() > 2 && val.substr(val.size() - 2) == "em") {
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v / kBase;
          ok = true;
        }
      } else {
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p && *end == '\0') {
          parsed = v / kBase;
          ok = true;
        }
      }
      if (ok && parsed > 0.0f) {
        style.lineHeightMultiplier = std::max(0.7f, std::min(2.0f, parsed));
        style.defined.lineHeight = 1;
      }
    }
  } else if (propNameBuf == "font-size") {
    const std::string_view val = stripTrailingImportant(propValueBuf);
    if (val != "inherit" && val != "initial" && val != "unset") {
      float parsed = 0.0f;
      bool ok = false;
      if (!val.empty() && val.back() == '%') {
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v / 100.0f;
          ok = true;
        }
      } else if (val.size() > 3 && val.substr(val.size() - 3) == "rem") {
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v;
          ok = true;
        }
      } else if (val.size() > 2 && val.substr(val.size() - 2) == "em") {
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v;
          ok = true;
        }
      } else if (val.size() > 2 && val.substr(val.size() - 2) == "pt") {
        // Absolute points, normalised against a 12 pt nominal body size — the
        // convention print-derived EPUBs assume (CSS medium == 16 px == 12 pt).
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v / 12.0f;
          ok = true;
        }
      } else if (val.size() > 2 && val.substr(val.size() - 2) == "px") {
        // Absolute pixels, normalised against the CSS default body size of 16 px.
        const char* p = val.data();
        char* end = nullptr;
        float v = std::strtof(p, &end);
        if (end != p) {
          parsed = v / 16.0f;
          ok = true;
        }
      } else {
        // CSS absolute-size / relative-size keywords, mapped to the same multiplier
        // steps microreader uses (smaller/larger fold onto small/large).
        if (val == "xx-small") {
          parsed = 0.6f;
        } else if (val == "x-small") {
          parsed = 0.75f;
        } else if (val == "small" || val == "smaller") {
          parsed = 0.8f;
        } else if (val == "medium") {
          parsed = 1.0f;
        } else if (val == "large" || val == "larger") {
          parsed = 1.2f;
        } else if (val == "x-large") {
          parsed = 1.4f;
        } else if (val == "xx-large") {
          parsed = 1.6f;
        }
        ok = parsed > 0.0f;
      }
      if (ok && parsed > 0.0f) {
        style.fontSizeMultiplier = parsed;
        style.defined.fontSizeMultiplier = 1;
      }
    }
  }
}

CssStyle CssParser::parseDeclarations(const std::string_view declBlock) {
  // One scratchpad for the whole process, not two fresh std::strings per rule block. This is a
  // static function called from two hot paths — every rule block during a compile, and every
  // inline style="" during a render — so the buffers used to be built and destroyed thousands
  // of times. Function-local statics keep their capacity for the life of the process: after the
  // first few blocks these never allocate again.
  //
  // Safe as statics because the parser is single-threaded (parse runs on the build/render task,
  // never both at once) and neither buffer outlives the call.
  static std::string propNameBuf;
  static std::string propValueBuf;

  CssStyle style;
  size_t start = 0;
  for (size_t i = 0; i <= declBlock.size(); ++i) {
    if (i == declBlock.size() || declBlock[i] == ';') {
      if (i > start) {
        // A view, not a substr copy — parseDeclarationIntoStyle slices it further and
        // normalizes straight into the scratchpad, so the declaration text is never
        // materialised on the heap at all.
        const std::string_view decl = declBlock.substr(start, i - start);
        if (!decl.empty()) {
          parseDeclarationIntoStyle(decl, style, propNameBuf, propValueBuf);
        }
      }
      start = i + 1;
    }
  }

  return style;
}

// Rule processing

void CssParser::processRuleBlockWithStyle(const std::string& selectorGroup, const CssStyle& style) {
  // Inspired by crosspoint-reader#2604: a selector with no renderer-supported
  // declarations only consumes scarce rule-map and cache-index capacity.
  if (!style.defined.anySet()) {
    return;
  }

  // Check if we've reached the rule limit before processing
  if (rulesBySelector_.size() >= MAX_RULES) {
    LOG_DBG("CSS", "Reached max rules limit (%zu), stopping CSS parsing", MAX_RULES);
    return;
  }

  // Handle comma-separated selectors.
  //
  // Was splitOnChar(), which materialised a std::vector<std::string> of every selector in the
  // group — one heap string per selector, each of them ALREADY normalized inside the split —
  // and then normalized each one a second time into `key` below. For a 119-candidate sheet that
  // is ~240 short-lived heap blocks whose holes outlive the parse: the heap never compacts, so
  // first-open CSS churn is still fragmenting the address space when a later chapter needs a
  // contiguous block.
  //
  // Now: walk the group in place and normalize each part straight into one reused buffer. Zero
  // allocations per selector after the buffer's first growth.
  size_t partStart = 0;
  for (size_t i = 0; i <= selectorGroup.size(); ++i) {
    if (i != selectorGroup.size() && selectorGroup[i] != ',') continue;
    const std::string_view rawPart(selectorGroup.data() + partStart, i - partStart);
    partStart = i + 1;

    normalizedInto(rawPart, selectorKeyBuf_);
    if (selectorKeyBuf_.empty()) continue;  // splitOnChar dropped empties too

    totalSelectorCandidates_++;
    // Validate selector length before processing
    if (selectorKeyBuf_.size() > MAX_SELECTOR_LENGTH) {
      LOG_DBG("CSS", "Selector too long (%zu > %zu), skipping", selectorKeyBuf_.size(), MAX_SELECTOR_LENGTH);
      unsupportedSelectorSkips_++;
      continue;
    }

    // Already normalized into selectorKeyBuf_ above — no second pass, no second allocation.
    const std::string& key = selectorKeyBuf_;

    if (!isSelectorUsableByResolver(key)) {
      unsupportedSelectorSkips_++;
      continue;
    }

    // Skip if this would exceed the rule limit
    const size_t ruleCount = compileModeActive_ ? compileSelectorOffsets_.size() : rulesBySelector_.size();
    if (ruleCount >= MAX_RULES) {
      // In compile mode, falling through to a persisted cache here would look complete
      // (hasCache() true) while silently and permanently dropping every selector past the
      // cap — the same failure signature as the old crosspoint-reader's heap-triggered
      // truncation, just triggered by selector count instead of heap size. Fail the compile
      // instead: endCacheCompile() then discards the temp file and writes no cache, so the
      // book re-parses (and hits this same cap) on the next open rather than losing styles
      // forever.
      if (compileModeActive_) {
        if (!compileModeFailed_) {
          LOG_ERR("CSS", "Reached max rules limit (%zu) mid-compile, aborting CSS cache for this book", MAX_RULES);
        }
        compileModeFailed_ = true;
      } else {
        LOG_DBG("CSS", "Reached max rules limit, stopping selector processing");
      }
      return;
    }

    if (compileModeActive_) {
      if (!compileTempFile_) {
        compileModeFailed_ = true;
        continue;
      }

      compileTempFile_.flush();
      CssStyle merged = style;
      auto existingOffsetIt = compileSelectorOffsets_.find(key);
      if (existingOffsetIt != compileSelectorOffsets_.end()) {
        CssStyle existing;
        FsFile tempRead;
        if (Storage.openFileForRead("CSS", compileTempPath_, tempRead) && tempRead.seek(existingOffsetIt->second) &&
            readCssStylePayload(tempRead, existing)) {
          existing.applyOver(merged);
          merged = existing;
        } else {
          LOG_ERR("CSS", "Failed to read compiled style for selector '%s' at offset %u", key.c_str(),
                  existingOffsetIt->second);
        }
        if (tempRead) {
          tempRead.close();
        }
      }

      const uint32_t styleOffset = compileTempFile_.position();
      writeCssStylePayload(compileTempFile_, merged);
      compileSelectorOffsets_[key] = styleOffset;
      continue;
    }

    // Store or merge with existing (non-compile mode)
    auto it = rulesBySelector_.find(key);
    if (it != rulesBySelector_.end()) {
      it->second.applyOver(style);
    } else {
      rulesBySelector_[key] = style;
    }
  }
}

// Main parsing entry point

bool CssParser::loadFromStream(FsFile& source) {
  if (!source) {
    LOG_ERR("CSS", "Cannot read from invalid file");
    return false;
  }

  size_t totalRead = 0;

  // Use stack-allocated buffers for parsing to avoid heap reallocations
  StackBuffer selector;
  StackBuffer declBuffer;
  // Keep these as std::string since they're passed by reference to parseDeclarationIntoStyle
  std::string propNameBuf;
  std::string propValueBuf;

  bool inComment = false;
  bool maybeSlash = false;
  bool prevStar = false;

  bool inAtRule = false;
  int atDepth = 0;

  int bodyDepth = 0;
  bool skippingRule = false;
  CssStyle currentStyle;

  auto handleChar = [&](const char c) {
    if (inAtRule) {
      if (c == '{') {
        ++atDepth;
      } else if (c == '}') {
        if (atDepth > 0) --atDepth;
        if (atDepth == 0) inAtRule = false;
      } else if (c == ';' && atDepth == 0) {
        inAtRule = false;
      }
      return;
    }

    if (bodyDepth == 0) {
      if (selector.empty() && isCssWhitespace(c)) {
        return;
      }
      if (c == '@' && selector.empty()) {
        inAtRule = true;
        atDepth = 0;
        return;
      }
      if (c == '{') {
        bodyDepth = 1;
        currentStyle = CssStyle{};
        declBuffer.clear();
        // A selector group that overflowed the StackBuffer was silently truncated; the
        // truncated tail could otherwise be parsed as a bogus rule (e.g. a cut class name
        // accidentally matching a real one). Skip the entire rule instead.
        if (selector.overflowed) {
          skippingRule = true;
        }
        return;
      }
      selector.push_back(c);
      return;
    }

    // bodyDepth > 0
    if (c == '{') {
      ++bodyDepth;
      return;
    }
    if (c == '}') {
      --bodyDepth;
      if (bodyDepth == 0) {
        // A truncated (overflowed) trailing declaration is dropped rather than parsed as garbage.
        if (!skippingRule && !declBuffer.empty() && !declBuffer.overflowed) {
          parseDeclarationIntoStyle(declBuffer.str(), currentStyle, propNameBuf, propValueBuf);
        }
        if (!skippingRule) {
          processRuleBlockWithStyle(selector.str(), currentStyle);
        }
        selector.clear();
        declBuffer.clear();
        skippingRule = false;
        return;
      }
      return;
    }
    if (bodyDepth > 1) {
      return;
    }
    if (!skippingRule) {
      if (c == ';') {
        // clear() also resets the overflow flag, so a single oversized declaration
        // is dropped without poisoning the declarations that follow it in the block.
        if (!declBuffer.empty() && !declBuffer.overflowed) {
          parseDeclarationIntoStyle(declBuffer.str(), currentStyle, propNameBuf, propValueBuf);
        }
        declBuffer.clear();
      } else {
        declBuffer.push_back(c);
      }
    }
  };

  char buffer[READ_BUFFER_SIZE];
  while (source.available()) {
    int bytesRead = source.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) break;

    totalRead += static_cast<size_t>(bytesRead);

    for (int i = 0; i < bytesRead; ++i) {
      const char c = buffer[i];

      if (inComment) {
        if (prevStar && c == '/') {
          inComment = false;
          prevStar = false;
          continue;
        }
        prevStar = c == '*';
        continue;
      }

      if (maybeSlash) {
        if (c == '*') {
          inComment = true;
          maybeSlash = false;
          prevStar = false;
          continue;
        }
        handleChar('/');
        maybeSlash = false;
        // fall through to process current char
      }

      if (c == '/') {
        maybeSlash = true;
        continue;
      }

      handleChar(c);
    }
  }

  if (maybeSlash) {
    handleChar('/');
  }

  if (compileModeActive_) {
    LOG_DBG("CSS", "Parsed %zu usable selectors from %zu bytes (compile mode)", compileSelectorOffsets_.size(),
            totalRead);
  } else {
    LOG_DBG("CSS", "Parsed %zu rules from %zu bytes", rulesBySelector_.size(), totalRead);
  }
  return true;
}

bool CssParser::beginCacheCompile() {
  clear();
  compileTempPath_ = cachePath + compileTempRulesCache;
  Storage.remove(compileTempPath_.c_str());
  if (!Storage.openFileForWrite("CSS", compileTempPath_, compileTempFile_)) {
    return false;
  }
  compileSelectorOffsets_.clear();
  compileModeActive_ = true;
  compileModeFailed_ = false;
  return true;
}

bool CssParser::appendCompiledFromStream(FsFile& source) {
  if (!compileModeActive_) {
    return false;
  }
  if (!loadFromStream(source)) {
    compileModeFailed_ = true;
    return false;
  }
  return !compileModeFailed_;
}

void CssParser::abortCacheCompile() {
  if (!compileModeActive_) {
    return;
  }
  // Reuse the failed-compile teardown: closes and removes the temp staging file and leaves no
  // rules cache behind (hasCache() stays false, so the next open re-parses the stylesheets).
  compileModeFailed_ = true;
  endCacheCompile();
}

bool CssParser::endCacheCompile() {
  if (!compileModeActive_) {
    return false;
  }

  compileModeActive_ = false;
  compileTempFile_.close();

  if (compileModeFailed_) {
    Storage.remove(compileTempPath_.c_str());
    compileSelectorOffsets_.clear();
    return false;
  }

  FsFile outFile;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, outFile)) {
    Storage.remove(compileTempPath_.c_str());
    compileSelectorOffsets_.clear();
    return false;
  }

  outFile.write(CssParser::CSS_CACHE_VERSION);
  const auto ruleCount = static_cast<uint16_t>(compileSelectorOffsets_.size());
  outFile.write(reinterpret_cast<const uint8_t*>(&ruleCount), sizeof(ruleCount));
  outFile.write(reinterpret_cast<const uint8_t*>(&totalSelectorCandidates_), sizeof(totalSelectorCandidates_));
  outFile.write(reinterpret_cast<const uint8_t*>(&unsupportedSelectorSkips_), sizeof(unsupportedSelectorSkips_));
  // v10: index immediately after the 11-byte header — write zeroed placeholder, patch below.

  FsFile tempFile;
  if (!Storage.openFileForRead("CSS", compileTempPath_, tempFile)) {
    outFile.close();
    Storage.remove((cachePath + rulesCache).c_str());
    Storage.remove(compileTempPath_.c_str());
    compileSelectorOffsets_.clear();
    return false;
  }

  std::vector<SelectorEntry> indexEntries;
  indexEntries.reserve(ruleCount);
  {
    constexpr SelectorEntry kZero{};
    for (uint16_t i = 0; i < ruleCount; ++i) {
      outFile.write(reinterpret_cast<const uint8_t*>(&kZero), sizeof(kZero));
    }
  }

  std::array<uint8_t, CSS_FIXED_STYLE_BYTES> styleBytes{};
  for (const auto& it : compileSelectorOffsets_) {
    const uint32_t ruleOffset = outFile.position();
    const auto selectorLen = static_cast<uint16_t>(it.first.size());
    outFile.write(reinterpret_cast<const uint8_t*>(&selectorLen), sizeof(selectorLen));
    outFile.write(reinterpret_cast<const uint8_t*>(it.first.data()), selectorLen);

    if (!tempFile.seek(it.second)) {
      tempFile.close();
      outFile.close();
      Storage.remove((cachePath + rulesCache).c_str());
      Storage.remove(compileTempPath_.c_str());
      compileSelectorOffsets_.clear();
      return false;
    }
    if (tempFile.read(styleBytes.data(), styleBytes.size()) != static_cast<int>(styleBytes.size())) {
      tempFile.close();
      outFile.close();
      Storage.remove((cachePath + rulesCache).c_str());
      Storage.remove(compileTempPath_.c_str());
      compileSelectorOffsets_.clear();
      return false;
    }
    outFile.write(styleBytes.data(), styleBytes.size());
    indexEntries.push_back({selectorHash(it.first), ruleOffset});
  }

  tempFile.close();

  // Sort and patch the index placeholder at position 11.
  std::sort(indexEntries.begin(), indexEntries.end(),
            [](const SelectorEntry& a, const SelectorEntry& b) { return a.hash < b.hash; });
  outFile.seek(11);
  for (const auto& entry : indexEntries) {
    outFile.write(reinterpret_cast<const uint8_t*>(&entry), sizeof(entry));
  }

  outFile.close();
  Storage.remove(compileTempPath_.c_str());

  compileSelectorOffsets_.clear();

  rulesBySelector_.clear();
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();
  dropIndex();

  return ensureCacheIndexLoaded();
}

bool CssParser::empty() const { return ruleCount() == 0; }

size_t CssParser::ruleCount() const {
  if (!rulesBySelector_.empty()) {
    return rulesBySelector_.size();
  }
  if (cacheIndexLoaded_) {
    return cachedRuleCount_;
  }
  return 0;
}

void CssParser::clearCaches(const bool evictEverything) {
  rulesBySelector_.clear();
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();
  // Arena-backed ruleset (resident or index): the memory belongs to the build arena, which is
  // reset per build, so never try to retain it across sections — drop the view and reload from
  // disk on the next resolve (the arena is re-filled fresh). The 10 KB heap-retain heuristic
  // below is meaningless here (nothing is on the heap).
  if (indexArena_) {
    dropIndex();
    return;
  }
  if (evictEverything) {
    // Defragmentation eviction: swap the unordered containers down so their bucket
    // arrays are freed too (clear() keeps them), and drop the retained disk index.
    // Everything here reloads lazily from SD on the next resolve.
    std::unordered_map<std::string, CssStyle>().swap(rulesBySelector_);
    std::unordered_map<std::string, std::pair<CssStyle, std::list<std::string>::iterator>>().swap(hotRuleCache_);
    std::unordered_set<std::string>().swap(negativeRuleCache_);
    std::vector<SelectorEntry>().swap(cacheRuleOffsets_);
    cacheIndexLoaded_ = false;
    cachedRuleCount_ = 0;
    return;
  }
  // Retain the sorted disk index if it fits in 10 KB — avoids a cold SD re-read
  // (~240 ms) at the start of each section build. Evict if larger to protect heap.
  if (cacheRuleOffsets_.size() * CSS_INDEX_BYTES_PER_RULE > 10 * 1024) {
    cacheRuleOffsets_.clear();
    cacheIndexLoaded_ = false;
    cachedRuleCount_ = 0;
  }
}

void CssParser::clear() {
  if (compileTempFile_) {
    compileTempFile_.flush();
    compileTempFile_.close();
  }
  if (!compileTempPath_.empty()) {
    Storage.remove(compileTempPath_.c_str());
    compileTempPath_.clear();
  }
  rulesBySelector_.clear();
  dropIndex();
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();
  resolveStats_ = {};
  compileModeActive_ = false;
  compileModeFailed_ = false;
  compileSelectorOffsets_.clear();
  totalSelectorCandidates_ = 0;
  unsupportedSelectorSkips_ = 0;
  // Reset Phase-2 arena config: clear() ends a build, so the shared per-epub parser must not
  // carry the lean flag or a now-dangling arena pointer into the next (possibly heap-backed) one.
  indexArena_ = nullptr;
  leanResolve_ = false;
}

void CssParser::resetResolveStats() const { resolveStats_ = {}; }

CssParser::ResolveStats CssParser::getResolveStats() const { return resolveStats_; }

void CssParser::logResolveStats(const char* context) const {
  const auto s = getResolveStats();
  LOG_DBG("CSS",
          "resolve stats[%s]: calls=%lu lowHeapSkips=%lu lowHeapRescuedHits=%lu lowHeapDiskBypasses=%lu "
          "mapHits=%lu hotHits=%lu diskHits=%lu misses=%lu negativeHits=%lu "
          "unsupportedSelectorsSkipped=%lu totalSelectorCandidates=%lu hotSize=%u indexSize=%u",
          context ? context : "n/a", s.resolveCalls, s.lowHeapSkips, s.lowHeapRescuedHits, s.lowHeapDiskBypasses,
          s.mapHits, s.hotHits, s.diskHits, s.misses, s.negativeHits,
          static_cast<unsigned long>(unsupportedSelectorSkips_), static_cast<unsigned long>(totalSelectorCandidates_),
          static_cast<unsigned>(hotRuleCache_.size()), static_cast<unsigned>(cachedRuleCount_));
  (void)context;
  (void)s;
}

bool CssParser::readCssStylePayload(FsFile& file, CssStyle& style) {
  uint8_t enumVal;
  if (file.read(&enumVal, 1) != 1) {
    return false;
  }
  style.textAlign = static_cast<CssTextAlign>(enumVal);

  if (file.read(&enumVal, 1) != 1) {
    return false;
  }
  style.fontStyle = static_cast<CssFontStyle>(enumVal);

  if (file.read(&enumVal, 1) != 1) {
    return false;
  }
  style.fontWeight = static_cast<CssFontWeight>(enumVal);

  if (file.read(&enumVal, 1) != 1) {
    return false;
  }
  style.textDecoration = static_cast<CssTextDecoration>(enumVal);

  auto readLength = [&file](CssLength& len) -> bool {
    if (file.read(&len.value, sizeof(len.value)) != sizeof(len.value)) {
      return false;
    }
    uint8_t unitVal;
    if (file.read(&unitVal, 1) != 1) {
      return false;
    }
    len.unit = static_cast<CssUnit>(unitVal);
    return true;
  };

  if (!readLength(style.textIndent) || !readLength(style.marginTop) || !readLength(style.marginBottom) ||
      !readLength(style.marginLeft) || !readLength(style.marginRight) || !readLength(style.paddingTop) ||
      !readLength(style.paddingBottom) || !readLength(style.paddingLeft) || !readLength(style.paddingRight) ||
      !readLength(style.imageHeight) || !readLength(style.imageWidth)) {
    return false;
  }

  uint8_t displayVal;
  if (file.read(&displayVal, 1) != 1) {
    return false;
  }
  style.display = static_cast<CssDisplay>(displayVal);

  uint16_t definedBits = 0;
  if (file.read(&definedBits, sizeof(definedBits)) != sizeof(definedBits)) {
    return false;
  }
  style.defined.textAlign = (definedBits & 1 << 0) != 0;
  style.defined.fontStyle = (definedBits & 1 << 1) != 0;
  style.defined.fontWeight = (definedBits & 1 << 2) != 0;
  style.defined.textDecoration = (definedBits & 1 << 3) != 0;
  style.defined.textIndent = (definedBits & 1 << 4) != 0;
  style.defined.marginTop = (definedBits & 1 << 5) != 0;
  style.defined.marginBottom = (definedBits & 1 << 6) != 0;
  style.defined.marginLeft = (definedBits & 1 << 7) != 0;
  style.defined.marginRight = (definedBits & 1 << 8) != 0;
  style.defined.paddingTop = (definedBits & 1 << 9) != 0;
  style.defined.paddingBottom = (definedBits & 1 << 10) != 0;
  style.defined.paddingLeft = (definedBits & 1 << 11) != 0;
  style.defined.paddingRight = (definedBits & 1 << 12) != 0;
  style.defined.imageHeight = (definedBits & 1 << 13) != 0;
  style.defined.imageWidth = (definedBits & 1 << 14) != 0;
  style.defined.display = (definedBits & 1 << 15) != 0;
  uint8_t vertAlignVal = 0;
  uint8_t vertAlignDefined = 0;
  if (file.read(&vertAlignVal, 1) != 1 || file.read(&vertAlignDefined, 1) != 1) {
    return false;
  }
  style.verticalAlign = static_cast<CssVerticalAlign>(vertAlignVal);
  style.defined.verticalAlign = vertAlignDefined != 0 ? 1 : 0;
  uint8_t cssFloatVal = 0;
  if (file.read(&cssFloatVal, 1) != 1) {
    return false;
  }
  style.cssFloat = static_cast<CssFloat>(cssFloatVal);
  style.defined.cssFloat = (cssFloatVal != static_cast<uint8_t>(CssFloat::None)) ? 1 : 0;
  uint8_t smallCapsVal = 0;
  if (file.read(&smallCapsVal, 1) != 1) {
    return false;
  }
  // bit 0 = value, bit 1 = defined (distinguishes explicit "normal" from unset)
  style.smallCaps = (smallCapsVal & 0x1) != 0;
  style.defined.smallCaps = (smallCapsVal & 0x2) != 0 ? 1 : 0;
  float fontSizeMul = 1.0f;
  uint8_t fontSizeFlags = 0;
  if (file.read(&fontSizeMul, sizeof(fontSizeMul)) != sizeof(fontSizeMul) || file.read(&fontSizeFlags, 1) != 1) {
    return false;
  }
  if ((fontSizeFlags & 0x1) != 0) {
    style.fontSizeMultiplier = fontSizeMul;
    style.defined.fontSizeMultiplier = 1;
  }
  uint8_t blockFlags = 0;
  if (file.read(&blockFlags, 1) != 1) {
    return false;
  }
  style.listStyleNone = (blockFlags & 0x01) != 0;
  style.defined.listStyleNone = (blockFlags & 0x02) != 0 ? 1 : 0;
  style.pageBreakBefore = (blockFlags & 0x04) != 0;
  style.defined.pageBreakBefore = (blockFlags & 0x08) != 0 ? 1 : 0;
  style.pageBreakAfter = (blockFlags & 0x10) != 0;
  style.defined.pageBreakAfter = (blockFlags & 0x20) != 0 ? 1 : 0;
  return true;
}

void CssParser::writeCssStylePayload(FsFile& file, const CssStyle& style) {
  file.write(static_cast<uint8_t>(style.textAlign));
  file.write(static_cast<uint8_t>(style.fontStyle));
  file.write(static_cast<uint8_t>(style.fontWeight));
  file.write(static_cast<uint8_t>(style.textDecoration));

  auto writeLength = [&file](const CssLength& len) {
    file.write(reinterpret_cast<const uint8_t*>(&len.value), sizeof(len.value));
    file.write(static_cast<uint8_t>(len.unit));
  };

  writeLength(style.textIndent);
  writeLength(style.marginTop);
  writeLength(style.marginBottom);
  writeLength(style.marginLeft);
  writeLength(style.marginRight);
  writeLength(style.paddingTop);
  writeLength(style.paddingBottom);
  writeLength(style.paddingLeft);
  writeLength(style.paddingRight);
  writeLength(style.imageHeight);
  writeLength(style.imageWidth);
  file.write(static_cast<uint8_t>(style.display));

  uint16_t definedBits = 0;
  if (style.defined.textAlign) definedBits |= 1 << 0;
  if (style.defined.fontStyle) definedBits |= 1 << 1;
  if (style.defined.fontWeight) definedBits |= 1 << 2;
  if (style.defined.textDecoration) definedBits |= 1 << 3;
  if (style.defined.textIndent) definedBits |= 1 << 4;
  if (style.defined.marginTop) definedBits |= 1 << 5;
  if (style.defined.marginBottom) definedBits |= 1 << 6;
  if (style.defined.marginLeft) definedBits |= 1 << 7;
  if (style.defined.marginRight) definedBits |= 1 << 8;
  if (style.defined.paddingTop) definedBits |= 1 << 9;
  if (style.defined.paddingBottom) definedBits |= 1 << 10;
  if (style.defined.paddingLeft) definedBits |= 1 << 11;
  if (style.defined.paddingRight) definedBits |= 1 << 12;
  if (style.defined.imageHeight) definedBits |= 1 << 13;
  if (style.defined.imageWidth) definedBits |= 1 << 14;
  if (style.defined.display) definedBits |= 1 << 15;
  file.write(reinterpret_cast<const uint8_t*>(&definedBits), sizeof(definedBits));
  file.write(static_cast<uint8_t>(style.verticalAlign));
  file.write(static_cast<uint8_t>(style.defined.verticalAlign));
  file.write(static_cast<uint8_t>(style.cssFloat));
  // bit 0 = value, bit 1 = defined (distinguishes explicit "normal" from unset)
  uint8_t smallCapsVal = (style.smallCaps ? 0x1 : 0x0) | (style.defined.smallCaps ? 0x2 : 0x0);
  file.write(smallCapsVal);
  // font-size multiplier: float + flags byte (bit 0 = defined)
  file.write(reinterpret_cast<const uint8_t*>(&style.fontSizeMultiplier), sizeof(style.fontSizeMultiplier));
  const uint8_t fontSizeFlags = style.defined.fontSizeMultiplier ? 0x1 : 0x0;
  file.write(fontSizeFlags);
  // Block flags byte: value/defined pairs, same convention as smallCaps above.
  const uint8_t blockFlags = (style.listStyleNone ? 0x01 : 0x00) | (style.defined.listStyleNone ? 0x02 : 0x00) |
                             (style.pageBreakBefore ? 0x04 : 0x00) | (style.defined.pageBreakBefore ? 0x08 : 0x00) |
                             (style.pageBreakAfter ? 0x10 : 0x00) | (style.defined.pageBreakAfter ? 0x20 : 0x00);
  file.write(blockFlags);
}

void CssParser::touchHotRule(const std::string& selector) const {
  auto it = hotRuleCache_.find(selector);
  if (it == hotRuleCache_.end()) {
    return;
  }
  hotRuleLru_.erase(it->second.second);
  hotRuleLru_.push_front(selector);
  it->second.second = hotRuleLru_.begin();
}

void CssParser::cacheHotRule(const std::string& selector, const CssStyle& style) const {
  // Lean/arena builds skip the hot LRU entirely: its ~100 B/entry heap growth is exactly what
  // the CSS floor guarded against, and arena builds resolve from the resident/index store
  // instead (FreeInkBook keeps no such cache). This is a no-op call from the arena index-only
  // path; it is never reached in RESIDENT mode.
  if (leanResolve_) {
    return;
  }
  auto it = hotRuleCache_.find(selector);
  if (it != hotRuleCache_.end()) {
    it->second.first = style;
    touchHotRule(selector);
    return;
  }

  hotRuleLru_.push_front(selector);
  hotRuleCache_.emplace(selector, std::make_pair(style, hotRuleLru_.begin()));
  if (hotRuleCache_.size() > HOT_RULE_CACHE_SIZE) {
    const std::string& evictKey = hotRuleLru_.back();
    hotRuleCache_.erase(evictKey);
    hotRuleLru_.pop_back();
  }
}

// Reads the rule record at ruleOffset and returns its style only if the stored
// selector matches `selector` exactly. A mismatch is not an error — it means the
// 32-bit index hash collided and the caller should try the next candidate.
bool CssParser::readRuleFromDiskAtOffset(const uint32_t ruleOffset, const std::string& selector,
                                         CssStyle& outStyle) const {
  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  // 256 B stack buffer (bounded by MAX_SELECTOR_LENGTH): render-path code, so no
  // heap; not live at the same time as the index-load buffer — both are reached
  // sequentially from lookupRule, so peak stack on this chain stays ~256 B.
  char selectorBuf[MAX_SELECTOR_LENGTH];
  uint16_t selectorLen = 0;
  bool ok = file.seek(ruleOffset) && file.read(&selectorLen, sizeof(selectorLen)) == sizeof(selectorLen) &&
            selectorLen == selector.size() && selectorLen <= MAX_SELECTOR_LENGTH &&
            file.read(selectorBuf, selectorLen) == selectorLen &&
            std::string_view(selectorBuf, selectorLen) == selector;
  ok = ok && readCssStylePayload(file, outStyle);
  file.close();
  return ok;
}

bool CssParser::lookupRule(const std::string& selector, CssStyle& outStyle, const bool allowDiskLookup) const {
  auto mapIt = rulesBySelector_.find(selector);
  if (mapIt != rulesBySelector_.end()) {
    outStyle = mapIt->second;
    resolveStats_.mapHits++;
    return true;
  }

  auto hotIt = hotRuleCache_.find(selector);
  if (hotIt != hotRuleCache_.end()) {
    outStyle = hotIt->second.first;
    touchHotRule(selector);
    resolveStats_.hotHits++;
    return true;
  }

  if (negativeRuleCache_.find(selector) != negativeRuleCache_.end()) {
    resolveStats_.negativeHits++;
    return false;
  }

  const auto byHash = [](const SelectorEntry& e, uint32_t key) { return e.hash < key; };

  // --- Arena mode (Phase 2) ---
  // The ruleset lives in arena memory (a heap-free fill), so load it even under heap pressure.
  if (indexArena_) {
    if (!ensureCacheIndexLoaded()) {
      return false;
    }
    const uint32_t h = selectorHash(selector);

    // RESIDENT: the whole ruleset is in RAM — resolve by hash with no disk read and no caching
    // (FreeInkBook's model). A hash collision between two distinct selectors is astronomically
    // unlikely at these rule counts and costs at most one wrong style; upstream accepts that
    // rather than paying a disk read to verify the selector string, and so do we. No low-heap
    // guard applies — nothing here touches SD or grows the heap.
    if (arenaResident_) {
      const ResidentEntry* begin = arenaResident_;
      const ResidentEntry* end = arenaResident_ + cachedRuleCount_;
      auto it = std::lower_bound(begin, end, h, [](const ResidentEntry& e, uint32_t key) { return e.hash < key; });
      if (it != end && it->hash == h) {
        decompressStyle(arenaStylePool_ + it->styleOff + 1, outStyle);  // +1 skips the length prefix
        resolveStats_.diskHits++;                                       // resolved from the (in-RAM) ruleset
        return true;
      }
      return false;  // cheap re-scan next time; no negative cache needed in RAM
    }

    // Index-only fallback: payloads are on disk, so honor the low-heap bypass.
    if (!allowDiskLookup) {
      resolveStats_.lowHeapDiskBypasses++;
      return false;
    }
    const SelectorEntry* begin = arenaIndex_;
    const SelectorEntry* end = arenaIndex_ + cachedRuleCount_;
    for (auto it = std::lower_bound(begin, end, h, byHash); it != end && it->hash == h; ++it) {
      if (readRuleFromDiskAtOffset(it->offset, selector, outStyle)) {
        resolveStats_.diskHits++;
        return true;
      }
    }
    if (negativeRuleCache_.size() >= NEGATIVE_CACHE_SIZE) {
      negativeRuleCache_.clear();
    }
    negativeRuleCache_.insert(selector);
    return false;
  }

  // --- Heap mode (disk index + hot cache) ---
  if (!allowDiskLookup) {
    resolveStats_.lowHeapDiskBypasses++;
    return false;
  }
  if (!ensureCacheIndexLoaded()) {
    return false;
  }
  const uint32_t h = selectorHash(selector);
  auto it = std::lower_bound(cacheRuleOffsets_.begin(), cacheRuleOffsets_.end(), h, byHash);
  // Equal hashes are adjacent after the sort; probe each candidate until the
  // on-disk selector matches (collisions are expected to be rare).
  for (; it != cacheRuleOffsets_.end() && it->hash == h; ++it) {
    if (readRuleFromDiskAtOffset(it->offset, selector, outStyle)) {
      cacheHotRule(selector, outStyle);
      resolveStats_.diskHits++;
      return true;
    }
  }

  if (negativeRuleCache_.size() >= NEGATIVE_CACHE_SIZE) {
    negativeRuleCache_.clear();
  }
  negativeRuleCache_.insert(selector);
  return false;
}

bool CssParser::ensureCacheIndexLoaded() const {
  if (cacheIndexLoaded_) {
    return true;
  }

  if (cachePath.empty()) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != CssParser::CSS_CACHE_VERSION) {
    file.close();
    Storage.remove((cachePath + rulesCache).c_str());
    return false;
  }

  uint16_t ruleCount = 0;
  if (file.read(&ruleCount, sizeof(ruleCount)) != sizeof(ruleCount) || ruleCount > MAX_RULES) {
    file.close();
    return false;
  }

  uint32_t totalCandidates = 0;
  uint32_t unsupportedSkips = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&totalCandidates), sizeof(totalCandidates)) != sizeof(totalCandidates) ||
      file.read(reinterpret_cast<uint8_t*>(&unsupportedSkips), sizeof(unsupportedSkips)) != sizeof(unsupportedSkips)) {
    file.close();
    return false;
  }

  // v10: index is immediately after the 11-byte header — read sequentially, no seek.
  dropIndex();  // clears the heap vector and any arena view; resets cachedRuleCount_
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();

  // Arena mode (Phase 2): prefer the resident {hash, CssStyle} ruleset (in-RAM resolve, no
  // disk reads — see loadArenaResident); fall back to an arena-backed offset index when the
  // resident array won't fit. loadArenaResident leaves the file open for that fallback read.
  if (indexArena_) {
    if (loadArenaResident(file, ruleCount, totalCandidates, unsupportedSkips)) {
      file.close();
      return true;
    }
    // Resident didn't fit — rewind to just past the 11-byte header and read the offset index
    // into the arena instead. Payloads stay on disk (readRuleFromDiskAtOffset).
    const size_t indexBytes = static_cast<size_t>(ruleCount) * sizeof(SelectorEntry);
    if (ruleCount > 0) {
      auto* idx = static_cast<SelectorEntry*>(indexArena_->alloc(indexBytes, alignof(SelectorEntry)));
      if (!idx || !file.seek(11) ||
          file.read(reinterpret_cast<uint8_t*>(idx), indexBytes) != static_cast<int>(indexBytes)) {
        file.close();
        return false;  // arena exhausted or short read — caller falls back to a released build
      }
      arenaIndex_ = idx;
    }
    file.close();
    cachedRuleCount_ = ruleCount;
    totalSelectorCandidates_ = totalCandidates;
    unsupportedSelectorSkips_ = unsupportedSkips;
    cacheIndexLoaded_ = true;
    LOG_DBG("CSS", "Loaded CSS index (arena, disk payloads): %u selectors", static_cast<unsigned>(ruleCount));
    return true;
  }

  if (ruleCount > 0) {
    cacheRuleOffsets_.resize(ruleCount);
    const size_t indexBytes = static_cast<size_t>(ruleCount) * sizeof(SelectorEntry);
    if (file.read(reinterpret_cast<uint8_t*>(cacheRuleOffsets_.data()), indexBytes) != static_cast<int>(indexBytes)) {
      file.close();
      cacheRuleOffsets_.clear();
      return false;
    }
  }

  file.close();

  cachedRuleCount_ = cacheRuleOffsets_.size();
  totalSelectorCandidates_ = totalCandidates;
  unsupportedSelectorSkips_ = unsupportedSkips;
  cacheIndexLoaded_ = true;
  LOG_DBG("CSS", "Loaded CSS index: %u selectors (hot cache size=%u, unsupported=%lu/%lu)",
          static_cast<unsigned>(cachedRuleCount_), static_cast<unsigned>(HOT_RULE_CACHE_SIZE),
          static_cast<unsigned long>(unsupportedSelectorSkips_), static_cast<unsigned long>(totalSelectorCandidates_));
  return true;
}

// --- Sparse CssStyle compression for the resident pool ---
// A style is serialized to a canonical, SPARSE record: only the fields the 'defined' mask
// flags are written (a real rule sets a handful of 24 properties). Because the encoding is
// canonical, two equivalent styles produce identical bytes, so dedup is a memcmp — and lookup
// decompresses back to a full CssStyle. Values are copied with memcpy so the byte-packed
// (unaligned) records are safe to read on the C3.

// Worst case: 4 (mask) + 2 (packed enums) + 11 lengths * 5 + 2 multipliers * 4 = 69 bytes.
constexpr size_t kMaxCompressedStyle = 4 + 2 + 11 * 5 + 2 * 4;

// Canonical 24-bit "which properties are set" mask. Bit order is the pool's dedup key and must
// stay stable within a build (the pool is rebuilt each load, so it never persists to disk).
static uint32_t packDefinedMask(const CssPropertyFlags& d) {
  return (d.textAlign << 0) | (d.fontStyle << 1) | (d.fontWeight << 2) | (d.textDecoration << 3) | (d.textIndent << 4) |
         (d.marginTop << 5) | (d.marginBottom << 6) | (d.marginLeft << 7) | (d.marginRight << 8) | (d.paddingTop << 9) |
         (d.paddingBottom << 10) | (d.paddingLeft << 11) | (d.paddingRight << 12) | (d.imageHeight << 13) |
         (d.imageWidth << 14) | (d.display << 15) | (d.verticalAlign << 16) | (d.listStyleNone << 17) |
         (d.pageBreakBefore << 18) | (d.pageBreakAfter << 19) | (d.lineHeight << 20) | (d.fontSizeMultiplier << 21) |
         (d.cssFloat << 22) | (d.smallCaps << 23);
}

// Serialize `s` into `out` (>= kMaxCompressedStyle bytes); returns the record length.
static size_t compressStyle(const CssStyle& s, uint8_t* out) {
  uint8_t* p = out;
  const uint32_t mask = packDefinedMask(s.defined);
  std::memcpy(p, &mask, 4);
  p += 4;
  uint16_t enums = 0;
  const CssPropertyFlags& d = s.defined;
  if (d.textAlign) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.textAlign) & 7u) << 0);
  if (d.fontStyle) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.fontStyle) & 1u) << 3);
  if (d.fontWeight) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.fontWeight) & 1u) << 4);
  if (d.textDecoration) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.textDecoration) & 3u) << 5);
  if (d.display) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.display) & 1u) << 7);
  if (d.verticalAlign) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.verticalAlign) & 3u) << 8);
  if (d.listStyleNone) enums |= static_cast<uint16_t>((s.listStyleNone ? 1u : 0u) << 10);
  if (d.pageBreakBefore) enums |= static_cast<uint16_t>((s.pageBreakBefore ? 1u : 0u) << 11);
  if (d.pageBreakAfter) enums |= static_cast<uint16_t>((s.pageBreakAfter ? 1u : 0u) << 12);
  if (d.cssFloat) enums |= static_cast<uint16_t>((static_cast<unsigned>(s.cssFloat) & 3u) << 13);
  if (d.smallCaps) enums |= static_cast<uint16_t>((s.smallCaps ? 1u : 0u) << 15);
  std::memcpy(p, &enums, 2);
  p += 2;
  const auto putLen = [&](bool def, const CssLength& l) {
    if (def) {
      std::memcpy(p, &l.value, 4);
      p += 4;
      *p++ = static_cast<uint8_t>(l.unit);
    }
  };
  putLen(d.textIndent, s.textIndent);
  putLen(d.marginTop, s.marginTop);
  putLen(d.marginBottom, s.marginBottom);
  putLen(d.marginLeft, s.marginLeft);
  putLen(d.marginRight, s.marginRight);
  putLen(d.paddingTop, s.paddingTop);
  putLen(d.paddingBottom, s.paddingBottom);
  putLen(d.paddingLeft, s.paddingLeft);
  putLen(d.paddingRight, s.paddingRight);
  putLen(d.imageHeight, s.imageHeight);
  putLen(d.imageWidth, s.imageWidth);
  if (d.lineHeight) {
    std::memcpy(p, &s.lineHeightMultiplier, 4);
    p += 4;
  }
  if (d.fontSizeMultiplier) {
    std::memcpy(p, &s.fontSizeMultiplier, 4);
    p += 4;
  }
  return static_cast<size_t>(p - out);
}

// Inverse of compressStyle: rebuild a full CssStyle from a compressed record.
static void decompressStyle(const uint8_t* in, CssStyle& out) {
  out.reset();
  const uint8_t* p = in;
  uint32_t mask = 0;
  std::memcpy(&mask, p, 4);
  p += 4;
  uint16_t enums = 0;
  std::memcpy(&enums, p, 2);
  p += 2;
  const auto isDefined = [&](int index) { return (mask >> index) & 1u; };
  CssPropertyFlags& d = out.defined;
  d.textAlign = isDefined(0);
  d.fontStyle = isDefined(1);
  d.fontWeight = isDefined(2);
  d.textDecoration = isDefined(3);
  d.textIndent = isDefined(4);
  d.marginTop = isDefined(5);
  d.marginBottom = isDefined(6);
  d.marginLeft = isDefined(7);
  d.marginRight = isDefined(8);
  d.paddingTop = isDefined(9);
  d.paddingBottom = isDefined(10);
  d.paddingLeft = isDefined(11);
  d.paddingRight = isDefined(12);
  d.imageHeight = isDefined(13);
  d.imageWidth = isDefined(14);
  d.display = isDefined(15);
  d.verticalAlign = isDefined(16);
  d.listStyleNone = isDefined(17);
  d.pageBreakBefore = isDefined(18);
  d.pageBreakAfter = isDefined(19);
  d.lineHeight = isDefined(20);
  d.fontSizeMultiplier = isDefined(21);
  d.cssFloat = isDefined(22);
  d.smallCaps = isDefined(23);
  if (d.textAlign) out.textAlign = static_cast<CssTextAlign>(enums & 7u);
  if (d.fontStyle) out.fontStyle = static_cast<CssFontStyle>((enums >> 3) & 1u);
  if (d.fontWeight) out.fontWeight = static_cast<CssFontWeight>((enums >> 4) & 1u);
  if (d.textDecoration) out.textDecoration = static_cast<CssTextDecoration>((enums >> 5) & 3u);
  if (d.display) out.display = static_cast<CssDisplay>((enums >> 7) & 1u);
  if (d.verticalAlign) out.verticalAlign = static_cast<CssVerticalAlign>((enums >> 8) & 3u);
  if (d.listStyleNone) out.listStyleNone = ((enums >> 10) & 1u) != 0;
  if (d.pageBreakBefore) out.pageBreakBefore = ((enums >> 11) & 1u) != 0;
  if (d.pageBreakAfter) out.pageBreakAfter = ((enums >> 12) & 1u) != 0;
  if (d.cssFloat) out.cssFloat = static_cast<CssFloat>((enums >> 13) & 3u);
  if (d.smallCaps) out.smallCaps = ((enums >> 15) & 1u) != 0;
  const auto getLen = [&](bool def, CssLength& l) {
    if (def) {
      std::memcpy(&l.value, p, 4);
      p += 4;
      l.unit = static_cast<CssUnit>(*p++);
    }
  };
  getLen(d.textIndent, out.textIndent);
  getLen(d.marginTop, out.marginTop);
  getLen(d.marginBottom, out.marginBottom);
  getLen(d.marginLeft, out.marginLeft);
  getLen(d.marginRight, out.marginRight);
  getLen(d.paddingTop, out.paddingTop);
  getLen(d.paddingBottom, out.paddingBottom);
  getLen(d.paddingLeft, out.paddingLeft);
  getLen(d.paddingRight, out.paddingRight);
  getLen(d.imageHeight, out.imageHeight);
  getLen(d.imageWidth, out.imageWidth);
  if (d.lineHeight) {
    std::memcpy(&out.lineHeightMultiplier, p, 4);
    p += 4;
  }
  if (d.fontSizeMultiplier) {
    std::memcpy(&out.fontSizeMultiplier, p, 4);
    p += 4;
  }
}

// Stream the whole ruleset into the arena as a sorted {hash, styleOff} index plus a pool of
// DISTINCT, sparse-COMPRESSED styles so resolveStyle runs entirely in RAM (FreeInkBook's model,
// adapted, with dedup + compression). One sequential pass over the payload block; each style is
// compressed (compressStyle) and, being canonical, deduplicated by a memcmp against the existing
// length-prefixed pool records. Identical styles (Calibre) collapse to one record, and each is
// far smaller than a flat CssStyle, so a large/repetitive sheet that a flat array couldn't hold
// still fits. Returns false without disturbing arena state when even this won't fit; the caller
// then tries the smaller offset index.
bool CssParser::loadArenaResident(FsFile& file, const uint16_t ruleCount, const uint32_t totalCandidates,
                                  const uint32_t unsupportedSkips) const {
  // Reserve a block so a failed attempt (won't fit, or a stream error) leaves the arena
  // cursor exactly where it was — the caller then reuses that space for the smaller offset
  // index. The file stays open; the caller (ensureCacheIndexLoaded) owns closing it.
  auto block = indexArena_->reserveBlock();
  ResidentEntry* index = ruleCount > 0 ? indexArena_->allocArray<ResidentEntry>(ruleCount) : nullptr;
  if (ruleCount > 0 && index == nullptr) {
    indexArena_->release(block);
    return false;  // even the index won't fit — caller falls back to the offset index
  }

  // Compressed distinct-style pool: contiguous [u8 len][len bytes] records bump-allocated right
  // after the index (only these allocations happen during the loop, so the pool stays contiguous
  // and self-delimiting for the dedup scan). styleOff addresses each record's length prefix.
  uint8_t* poolBase = nullptr;
  size_t poolBytes = 0;
  uint16_t poolCount = 0;

  if (ruleCount > 0) {
    // Skip the sorted offset index (11-byte header + ruleCount * 8) to reach the payload block,
    // then stream it in one sequential pass (fixed-size payloads → no per-record seeking).
    if (!file.seek(static_cast<uint32_t>(11 + static_cast<size_t>(ruleCount) * sizeof(SelectorEntry)))) {
      indexArena_->release(block);
      return false;
    }
    char selectorBuf[MAX_SELECTOR_LENGTH];
    uint8_t rec[kMaxCompressedStyle];
    for (uint16_t i = 0; i < ruleCount; ++i) {
      uint16_t selectorLen = 0;
      if (file.read(&selectorLen, sizeof(selectorLen)) != sizeof(selectorLen) || selectorLen > MAX_SELECTOR_LENGTH ||
          file.read(selectorBuf, selectorLen) != selectorLen) {
        indexArena_->release(block);
        return false;
      }
      CssStyle style;
      if (!readCssStylePayload(file, style)) {
        indexArena_->release(block);
        return false;
      }
      const size_t recLen = compressStyle(style, rec);

      // Intern: reuse an identical record if we've already stored one (memcmp over the canonical
      // compressed bytes), else append a new [len][bytes] record to the pool.
      uint32_t off = 0;
      bool found = false;
      for (size_t scan = 0; scan < poolBytes;) {
        const size_t existingLen = poolBase[scan];
        if (existingLen == recLen && std::memcmp(poolBase + scan + 1, rec, recLen) == 0) {
          off = static_cast<uint32_t>(scan);
          found = true;
          break;
        }
        scan += 1 + existingLen;
      }
      if (!found) {
        auto* slot = static_cast<uint8_t*>(indexArena_->alloc(1 + recLen, 1));
        if (slot == nullptr) {  // pool grew past the arena even after dedup — fall back to index
          indexArena_->release(block);
          return false;
        }
        if (poolBase == nullptr) poolBase = slot;  // first record anchors the pool
        off = static_cast<uint32_t>(slot - poolBase);
        slot[0] = static_cast<uint8_t>(recLen);
        std::memcpy(slot + 1, rec, recLen);
        poolBytes = static_cast<size_t>(off) + 1 + recLen;
        ++poolCount;
      }
      index[i] = ResidentEntry{selectorHash(std::string_view(selectorBuf, selectorLen)), off};
    }
    // Sort by hash so lookupRule can binary-search, matching the on-disk index ordering.
    std::sort(index, index + ruleCount, [](const ResidentEntry& a, const ResidentEntry& b) { return a.hash < b.hash; });
    arenaResident_ = index;
    arenaStylePool_ = poolBase;
    arenaStyleCount_ = poolCount;
    arenaPoolBytes_ = static_cast<uint32_t>(poolBytes);
  }

  indexArena_->commit(block);
  cachedRuleCount_ = ruleCount;
  totalSelectorCandidates_ = totalCandidates;
  unsupportedSelectorSkips_ = unsupportedSkips;
  cacheIndexLoaded_ = true;
  LOG_DBG("CSS", "Loaded CSS RESIDENT in arena: %u selectors -> %u distinct styles, %u index + %u pool bytes",
          static_cast<unsigned>(ruleCount), static_cast<unsigned>(poolCount),
          static_cast<unsigned>(static_cast<size_t>(ruleCount) * sizeof(ResidentEntry)),
          static_cast<unsigned>(poolBytes));
  return true;
}

// Forget the current ruleset view — heap vector or either arena layout — and reset the
// loaded flag so ensureCacheIndexLoaded() reloads. Arena memory is owned by the arena
// (reset per build), so we only drop the non-owning pointers here.
void CssParser::dropIndex() const {
  cacheRuleOffsets_.clear();
  arenaResident_ = nullptr;
  arenaStylePool_ = nullptr;
  arenaStyleCount_ = 0;
  arenaPoolBytes_ = 0;
  arenaIndex_ = nullptr;
  cacheIndexLoaded_ = false;
  cachedRuleCount_ = 0;
}

CssParser::ResidentFootprint CssParser::getResidentFootprint() const {
  ResidentFootprint f;
  if (arenaResident_ == nullptr) return f;  // only the RESIDENT layout has a pooled footprint
  f.ruleCount = static_cast<uint16_t>(cachedRuleCount_);
  f.distinctStyles = arenaStyleCount_;
  f.indexBytes = static_cast<uint32_t>(cachedRuleCount_) * sizeof(ResidentEntry);
  f.poolBytes = arenaPoolBytes_;
  return f;
}

// Style resolution

CssStyle CssParser::resolveStyle(const std::string& tagName, const std::string& classAttr,
                                 const std::string& idAttr) const {
  static bool lowHeapWarningLogged = false;
  resolveStats_.resolveCalls++;
  const uint32_t freeHeap = ESP.getFreeHeap();
  // Lean/arena builds keep the resolver's footprint off the heap (index in arena, hot cache
  // disabled), so they stay correct at the lower LEAN_MIN_FREE_HEAP_FOR_CSS floor. RESIDENT
  // lookups ignore lowHeapMode entirely inside lookupRule — they touch no SD.
  const bool lowHeapMode = freeHeap < (leanResolve_ ? LEAN_MIN_FREE_HEAP_FOR_CSS : MIN_FREE_HEAP_FOR_CSS);
  if (lowHeapMode) {
    if (!lowHeapWarningLogged) {
      lowHeapWarningLogged = true;
      // Report the floor that ACTUALLY applied, and whether the skip can cost anything. The
      // old message hardcoded MIN_FREE_HEAP_FOR_CSS even in lean mode, so a 960-byte dip under
      // the 24 KB lean floor read as "17 KB below 40960" — which misdiagnosed this exact bug
      // once already. Resident rulesets serve every lookup from the arena, so a skip there is
      // harmless: say so rather than warning about missing styles that cannot happen.
      LOG_DBG("CSS", "Low heap (%u bytes) below %s floor (%u), skipping disk CSS lookups (%s)", freeHeap,
              leanResolve_ ? "LEAN" : "full", static_cast<unsigned>(leanResolve_ ? LEAN_MIN_FREE_HEAP_FOR_CSS
                                                                                 : MIN_FREE_HEAP_FOR_CSS),
              arenaResident_ ? "harmless: ruleset is arena-resident" : "styles may be missing");
    }
    resolveStats_.lowHeapSkips++;
  }
  CssStyle result;
  const std::string tag = normalized(tagName);

  // 1. Apply element-level style (lowest priority)
  {
    CssStyle tagStyle;
    if (lookupRule(tag, tagStyle, !lowHeapMode)) {
      if (lowHeapMode) {
        resolveStats_.lowHeapRescuedHits++;
      }
      result.applyOver(tagStyle);
    }
  }

  // TODO: Support combinations of classes (e.g. style on .class1.class2)
  // 2. Apply class styles (medium priority)
  if (!classAttr.empty()) {
    std::string classToken;
    std::string classKey;
    classKey.reserve(32);
    std::string combinedKey;
    combinedKey.reserve(tag.size() + 1 + 32);

    forEachNormalizedClassToken(classAttr, classToken, [&](const std::string& cls) {
      classKey.clear();
      classKey.push_back('.');
      classKey.append(cls);

      CssStyle classStyle;
      if (lookupRule(classKey, classStyle, !lowHeapMode)) {
        if (lowHeapMode) {
          resolveStats_.lowHeapRescuedHits++;
        }
        result.applyOver(classStyle);
      }

      combinedKey.clear();
      combinedKey.append(tag);
      combinedKey.push_back('.');
      combinedKey.append(cls);

      CssStyle combinedStyle;
      if (lookupRule(combinedKey, combinedStyle, !lowHeapMode)) {
        if (lowHeapMode) {
          resolveStats_.lowHeapRescuedHits++;
        }
        result.applyOver(combinedStyle);
      }
    });
  }

  // 3. Apply ID styles (highest priority: #id < tag#id)
  if (!idAttr.empty()) {
    std::string idKey;
    idKey.reserve(1 + idAttr.size());
    idKey.push_back('#');
    for (const char c : idAttr) {
      idKey.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    CssStyle idStyle;
    if (lookupRule(idKey, idStyle, !lowHeapMode)) {
      if (lowHeapMode) resolveStats_.lowHeapRescuedHits++;
      result.applyOver(idStyle);
    }

    std::string tagIdKey;
    tagIdKey.reserve(tag.size() + 1 + idAttr.size());
    tagIdKey.append(tag);
    tagIdKey.push_back('#');
    for (const char c : idAttr) {
      tagIdKey.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    CssStyle tagIdStyle;
    if (lookupRule(tagIdKey, tagIdStyle, !lowHeapMode)) {
      if (lowHeapMode) resolveStats_.lowHeapRescuedHits++;
      result.applyOver(tagIdStyle);
    }
  }

  if (!result.defined.anySet()) {
    resolveStats_.misses++;
  }

  return result;
}

// Inline style parsing (static - doesn't need rule database)

CssStyle CssParser::parseInlineStyle(const std::string& styleValue) { return parseDeclarations(styleValue); }

// Cache serialization

bool CssParser::hasCache() const { return Storage.exists((cachePath + rulesCache).c_str()); }

void CssParser::deleteCache() const {
  if (hasCache()) Storage.remove((cachePath + rulesCache).c_str());
}

bool CssParser::saveToCache() const {
  if (cachePath.empty()) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForWrite("CSS", cachePath + rulesCache, file)) {
    return false;
  }

  file.write(CssParser::CSS_CACHE_VERSION);
  const auto ruleCount = static_cast<uint16_t>(rulesBySelector_.size());
  file.write(reinterpret_cast<const uint8_t*>(&ruleCount), sizeof(ruleCount));
  file.write(reinterpret_cast<const uint8_t*>(&totalSelectorCandidates_), sizeof(totalSelectorCandidates_));
  file.write(reinterpret_cast<const uint8_t*>(&unsupportedSelectorSkips_), sizeof(unsupportedSelectorSkips_));
  // v10: index lives immediately after the 11-byte header (before rule payloads).
  // Write zeroed placeholder entries now; patch with sorted data below.
  // ensureCacheIndexLoaded() reads header + index sequentially — no seek over payloads.
  std::vector<SelectorEntry> indexEntries;
  indexEntries.reserve(ruleCount);
  {
    constexpr SelectorEntry kZero{};
    for (uint16_t i = 0; i < ruleCount; ++i) {
      file.write(reinterpret_cast<const uint8_t*>(&kZero), sizeof(kZero));
    }
  }

  // Write each rule: selector string (length-prefixed) + style payload.
  // Record {hash, ruleOffset} pairs as we go so we can build the sorted index.
  for (const auto& pair : rulesBySelector_) {
    const uint32_t ruleOffset = file.position();
    const auto selectorLen = static_cast<uint16_t>(pair.first.size());
    file.write(reinterpret_cast<const uint8_t*>(&selectorLen), sizeof(selectorLen));
    file.write(reinterpret_cast<const uint8_t*>(pair.first.data()), selectorLen);
    writeCssStylePayload(file, pair.second);
    indexEntries.push_back({selectorHash(pair.first), ruleOffset});
  }

  // Sort and patch the index placeholder at position 11.
  std::sort(indexEntries.begin(), indexEntries.end(),
            [](const SelectorEntry& a, const SelectorEntry& b) { return a.hash < b.hash; });
  file.seek(11);
  for (const auto& entry : indexEntries) {
    file.write(reinterpret_cast<const uint8_t*>(&entry), sizeof(entry));
  }

  LOG_DBG("CSS", "Saved %u rules to cache", ruleCount);
  file.close();
  return true;
}

bool CssParser::loadFromCache() {
  if (cachePath.empty()) {
    return false;
  }

  // Drop parse-time in-memory rules and LRU caches.
  rulesBySelector_.clear();
  hotRuleCache_.clear();
  hotRuleLru_.clear();
  negativeRuleCache_.clear();
  // Arena mode: any view from a previous build points into an arena that has since been reset,
  // so force a fresh load into the current arena. Heap mode: if clearCaches() retained the disk
  // index, ensureCacheIndexLoaded() short-circuits — don't evict it here.
  if (indexArena_) {
    dropIndex();
  } else if (!cacheIndexLoaded_) {
    cacheRuleOffsets_.clear();
    cachedRuleCount_ = 0;
  }

  if (!ensureCacheIndexLoaded()) {
    return false;
  }

  LOG_DBG("CSS", "Loaded %u rules from cache index", static_cast<unsigned>(cachedRuleCount_));
  return true;
}
