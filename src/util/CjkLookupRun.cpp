#include "CjkLookupRun.h"

#include <Utf8.h>

#include <cctype>
#include <cstring>

#include "Dictionary.h"
#include "DictionaryPunctuation.h"

// Dictionary::resolvePrefixes() searches a run's first MAX_PREFIX_RUN_BYTES: the longest run must fit.
static_assert(CjkLookupRun::MAX_CODEPOINTS * 4 <= Dictionary::MAX_PREFIX_RUN_BYTES,
              "Dictionary::resolvePrefixes() must search the whole of the longest run");

namespace {

// Decodes the codepoint at text[i]; false when `length` cuts it off. `len` gets its byte count.
bool decodeAt(const char* text, const size_t length, const size_t i, uint32_t& cp, size_t& len) {
  const auto* p = reinterpret_cast<const unsigned char*>(text) + i;
  len = static_cast<size_t>(utf8CodepointLen(*p));
  if (i + len > length) return false;
  cp = utf8NextCodepoint(&p);
  return true;
}

// Skips what cleanWord()'s leading loop skips: non-alphanumeric ASCII and edge punctuation.
size_t skipLeading(const char* text, const size_t length) {
  size_t i = 0;
  while (i < length) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      if (std::isalnum(c) != 0) break;
      i++;
      continue;
    }
    uint32_t cp = 0;
    size_t len = 0;
    if (!decodeAt(text, length, i, cp, len) || !isDictionaryEdgePunctuation(cp)) break;
    i += len;
  }
  return i;
}

}  // namespace

CjkLookupRun::Script CjkLookupRun::scriptOf(const char* text, const size_t length) {
  if (!text) return Script::Other;
  const size_t i = skipLeading(text, length);
  uint32_t cp = 0;
  size_t len = 0;
  if (i >= length || !decodeAt(text, length, i, cp, len)) return Script::Other;
  if (utf8IsHangul(cp)) return Script::Hangul;
  if (utf8IsUnspacedCjk(cp)) return Script::Unspaced;
  return Script::Other;
}

bool CjkLookupRun::append(const char* text, const size_t length) {
  if (closed_ || !text) return false;
  const uint8_t word = words_;
  if (words_ < 255) words_++;

  size_t i = 0;
  if (!started_) {
    started_ = true;
    script_ = scriptOf(text, length);
    if (script_ == Script::Other) {
      closed_ = true;
      return false;
    }
    i = skipLeading(text, length);
  }

  while (i < length) {
    uint32_t cp = 0;
    size_t len = 0;
    if (!decodeAt(text, length, i, cp, len)) {
      closed_ = true;
      return false;
    }
    const bool keep =
        script_ == Script::Hangul ? utf8IsHangul(cp) : (utf8IsUnspacedCjk(cp) && !isDictionaryEdgePunctuation(cp));
    if (!keep || count_ >= MAX_CODEPOINTS) {
      closed_ = true;
      return false;
    }
    memcpy(text_ + bytes_, text + i, len);
    bytes_ = static_cast<uint8_t>(bytes_ + len);
    text_[bytes_] = '\0';
    end_[count_] = bytes_;
    word_[count_] = word;
    count_++;
    i += len;
  }

  // A Hangul run is the first word's token and takes no second word.
  if (script_ == Script::Hangul || count_ >= MAX_CODEPOINTS) {
    closed_ = true;
    return false;
  }
  return true;
}

uint8_t CjkLookupRun::wordsCovering(const uint8_t runBytes) const {
  if (runBytes == 0) return 0;
  for (uint8_t k = 0; k < count_; k++) {
    if (end_[k] == runBytes) return static_cast<uint8_t>(word_[k] + 1);
  }
  return 0;
}
