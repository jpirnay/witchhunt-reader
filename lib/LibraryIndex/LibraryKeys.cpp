#include "LibraryKeys.h"

#include <Utf8.h>

#include <cctype>

#include "LibraryFormat.h"

namespace {

constexpr uint32_t FNV_BASIS = 2166136261u;
constexpr uint32_t FNV_PRIME = 16777619u;

uint32_t fnv1a(uint32_t hash, const void* data, const size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// The base letter of each code point U+00C0..U+017F. '*' marks one that folds to two letters
// (expansion() handles those first) or has no base letter at all (the multiplication and division
// signs), which is kept as it is.
constexpr char kLatinBase[] =
    "aaaaaa*ceeeeiiiidnooooo*ouuuuy**"                                     // U+00C0..U+00DF
    "aaaaaa*ceeeeiiiidnooooo*ouuuuy*y"                                     // U+00E0..U+00FF
    "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiii**jjkkkllllllllll"  // U+0100..U+0142
    "nnnnnnnnnoooooo**rrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";       // U+0143..U+017F
static_assert(sizeof(kLatinBase) - 1 == 0x180 - 0xC0, "one entry per code point U+00C0..U+017F");

const char* expansion(const uint32_t cp) {
  switch (cp) {
    case 0xC6:
    case 0xE6:
      return "ae";
    case 0xDE:
    case 0xFE:
      return "th";
    case 0xDF:
      return "ss";
    case 0x132:
    case 0x133:
      return "ij";
    case 0x152:
    case 0x153:
      return "oe";
    default:
      return nullptr;
  }
}

bool isSpace(const uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0; }

void appendUtf8(std::string& out, const uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Runs of whitespace as one space, none at either end; nothing else changed.
std::string tidySpaces(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!out.empty() && out.back() != ' ') out += ' ';
      continue;
    }
    out += c;
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

}  // namespace

namespace LibraryKeys {

std::string fold(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  bool space = false;
  const auto* p = reinterpret_cast<const unsigned char*>(text.c_str());
  for (uint32_t cp = utf8NextCodepoint(&p); cp != 0; cp = utf8NextCodepoint(&p)) {
    if (isSpace(cp)) {
      space = !out.empty();
      continue;
    }
    if (cp >= 0x300 && cp <= 0x36F) continue;  // combining marks
    if (space) {
      out += ' ';
      space = false;
    }
    if (cp < 0x80) {
      out += static_cast<char>(std::tolower(static_cast<int>(cp)));
    } else if (const char* letters = expansion(cp)) {
      out += letters;
    } else if (cp >= 0xC0 && cp < 0x180 && kLatinBase[cp - 0xC0] != '*') {
      out += kLatinBase[cp - 0xC0];
    } else {
      appendUtf8(out, cp);
    }
  }
  return out;
}

std::string authorFilingName(const std::string& name, const std::string& fileAs) {
  std::string filing = tidySpaces(fileAs);
  if (!filing.empty()) return filing;
  filing = tidySpaces(name);
  const size_t lastSpace = filing.rfind(' ');
  if (filing.find(',') != std::string::npos || lastSpace == std::string::npos) return filing;
  return filing.substr(lastSpace + 1) + ", " + filing.substr(0, lastSpace);
}

std::string filingKey(const std::string& filing) {
  const std::string key = fold(filing);
  std::string out;
  out.reserve(key.size());
  for (const char c : key) {
    if (c == ',' || c == '.') continue;
    if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
    out += c;
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string authorSortKey(const std::string& name, const std::string& fileAs) {
  return filingKey(authorFilingName(name, fileAs));
}

uint32_t authorHash(const std::string& name) {
  const std::string folded = fold(name);
  if (folded.empty()) return library::AUTHOR_UNKNOWN;
  uint32_t hash = fnv1a(FNV_BASIS, folded.data(), folded.size());
  if (hash == library::AUTHOR_UNKNOWN) hash = 1;
  if (hash == library::AUTHOR_PENDING) hash = library::AUTHOR_PENDING - 1;
  return hash;
}

uint32_t bookIdentity(const char* fileName, const uint32_t size) {
  uint32_t hash = FNV_BASIS;
  for (const char* c = fileName; *c != '\0'; ++c) {
    const auto lower = static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(*c)));
    hash = fnv1a(hash, &lower, 1);
  }
  const uint8_t sizeBytes[4] = {static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8),
                                static_cast<uint8_t>(size >> 16), static_cast<uint8_t>(size >> 24)};
  return fnv1a(hash, sizeBytes, sizeof(sizeBytes));
}

}  // namespace LibraryKeys
