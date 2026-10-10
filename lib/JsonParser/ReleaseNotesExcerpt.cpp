#include "ReleaseNotesExcerpt.h"

#include <cstring>

namespace release_notes {

namespace {

constexpr char ELLIPSIS[] = "\xE2\x80\xA6";
constexpr size_t ELLIPSIS_LEN = sizeof(ELLIPSIS) - 1;
constexpr char AUTHOR_TAIL[] = " by @";
constexpr size_t AUTHOR_TAIL_LEN = sizeof(AUTHOR_TAIL) - 1;
constexpr char FULL_CHANGELOG[] = "**Full Changelog**";
constexpr size_t FULL_CHANGELOG_LEN = sizeof(FULL_CHANGELOG) - 1;

struct Writer {
  char* out;
  size_t cap;  // outSize - 1, room for the NUL
  size_t n = 0;
  bool full = false;

  void put(char c) {
    if (n < cap) {
      out[n++] = c;
    } else {
      full = true;
    }
  }
};

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

void trim(const char*& s, size_t& len) {
  while (len > 0 && isSpace(s[0])) {
    ++s;
    --len;
  }
  while (len > 0 && isSpace(s[len - 1])) --len;
}

// A thematic break: three or more of '-', '*', '_', with nothing else but spaces.
bool isRule(const char* s, size_t len) {
  size_t marks = 0;
  for (size_t i = 0; i < len; ++i) {
    if (s[i] == '-' || s[i] == '*' || s[i] == '_') {
      ++marks;
    } else if (s[i] != ' ') {
      return false;
    }
  }
  return marks >= 3;
}

size_t withoutAuthorTail(const char* s, size_t len) {
  for (size_t i = 0; i + AUTHOR_TAIL_LEN <= len; ++i) {
    if (memcmp(s + i, AUTHOR_TAIL, AUTHOR_TAIL_LEN) == 0) return i;
  }
  return len;
}

// The text without backticks and "**"; a "[text](url)" link keeps its text.
void writePlain(Writer& w, const char* s, size_t len, bool links) {
  size_t i = 0;
  while (i < len && !w.full) {
    const char c = s[i];
    if (c == '`') {
      ++i;
      continue;
    }
    if (c == '*' && i + 1 < len && s[i + 1] == '*') {
      i += 2;
      continue;
    }
    if (c == '[' && links) {
      const char* close = static_cast<const char*>(memchr(s + i + 1, ']', len - i - 1));
      const size_t afterClose = close ? static_cast<size_t>(close - s) + 1 : len;
      if (afterClose < len && s[afterClose] == '(') {
        const char* paren = static_cast<const char*>(memchr(s + afterClose + 1, ')', len - afterClose - 1));
        if (paren) {
          writePlain(w, s + i + 1, static_cast<size_t>(close - s) - i - 1, false);
          i = static_cast<size_t>(paren - s) + 1;
          continue;
        }
      }
    }
    w.put(c);
    ++i;
  }
}

// The line overflowed `out`: back off to make room for the ellipsis, to a codepoint and, where the
// line has one, a word boundary. A line with no text left is taken out whole, separator included.
void endWithEllipsis(Writer& w, size_t lineBegin, size_t textBegin) {
  size_t n = w.cap >= ELLIPSIS_LEN ? w.cap - ELLIPSIS_LEN : 0;
  if (n < textBegin) n = textBegin;
  while (n > textBegin && (static_cast<unsigned char>(w.out[n]) & 0xC0) == 0x80) --n;
  if (n > textBegin && w.out[n] != ' ') {
    size_t space = n;
    while (space > textBegin && w.out[space - 1] != ' ') --space;
    if (space > textBegin) n = space;
  }
  while (n > textBegin && w.out[n - 1] == ' ') --n;
  if (n == textBegin) {
    w.n = lineBegin;
    return;
  }
  memcpy(w.out + n, ELLIPSIS, ELLIPSIS_LEN);
  w.n = n + ELLIPSIS_LEN;
}

}  // namespace

size_t excerpt(const char* body, size_t len, bool complete, char* out, size_t outSize, size_t maxLines) {
  Writer w{out, outSize - 1};
  size_t lines = 0;
  size_t pos = 0;
  while (pos < len && lines < maxLines && !w.full) {
    const char* newline = static_cast<const char*>(memchr(body + pos, '\n', len - pos));
    if (!newline && !complete) break;  // cut off mid-line
    const size_t end = newline ? static_cast<size_t>(newline - body) : len;
    const char* s = body + pos;
    size_t lineLen = end - pos;
    pos = end + 1;

    trim(s, lineLen);
    if (lineLen == 0 || s[0] == '#' || isRule(s, lineLen)) continue;
    if (lineLen >= FULL_CHANGELOG_LEN && memcmp(s, FULL_CHANGELOG, FULL_CHANGELOG_LEN) == 0) continue;
    lineLen = withoutAuthorTail(s, lineLen);
    const bool bullet = lineLen >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ';
    if (bullet) {
      s += 2;
      lineLen -= 2;
    }
    trim(s, lineLen);
    if (lineLen == 0) continue;

    const size_t lineBegin = w.n;
    if (lines > 0) w.put('\n');
    if (bullet) {
      w.put('-');
      w.put(' ');
    }
    const size_t textBegin = w.n;
    writePlain(w, s, lineLen, true);
    if (w.full) {
      endWithEllipsis(w, lineBegin, textBegin);
      break;
    }
    if (w.n == textBegin) {
      w.n = lineBegin;  // nothing but markup
      continue;
    }
    ++lines;
  }
  out[w.n] = '\0';
  return w.n;
}

}  // namespace release_notes
