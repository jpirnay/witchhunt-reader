#include "DictHtmlNormalize.h"

#include <cctype>
#include <cstring>

namespace {

// Longest element or attribute name kept. Real HTML tops out around
// "figcaption" (10); anything longer is truncated, and because start and end
// tags go through the same truncation they still match each other.
constexpr size_t NAME_BUF_BYTES = 24;

// Staging for the sink. The parser feeds every write straight into the XML
// parser, so this only exists to keep that from happening one byte at a time.
constexpr size_t SINK_BUF_BYTES = 128;

bool isNameChar(const unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '_' || c == ':' || c == '.'; }

char toLowerAscii(const char c) {
  const auto u = static_cast<unsigned char>(c);
  return u >= 'A' && u <= 'Z' ? static_cast<char>(u + ('a' - 'A')) : c;
}

// Buffered writer over the caller's Print. A false return means the sink
// rejected the bytes, which is the parser's way of reporting a parse error, so
// every append is checked and the walk stops at the first failure.
class Sink {
 public:
  explicit Sink(Print& out) : out(out) {}

  bool append(const char c) { return append(&c, 1); }
  bool append(const char* text) { return append(text, strlen(text)); }

  bool append(const char* data, size_t len) {
    while (len > 0) {
      const size_t space = SINK_BUF_BYTES - used;
      const size_t chunk = len < space ? len : space;
      memcpy(buffer + used, data, chunk);
      used += chunk;
      data += chunk;
      len -= chunk;
      if (used == SINK_BUF_BYTES && !flush()) return false;
    }
    return true;
  }

  bool flush() {
    if (used == 0) return true;
    if (out.write(reinterpret_cast<const uint8_t*>(buffer), used) != used) return false;
    used = 0;
    return true;
  }

 private:
  Print& out;
  char buffer[SINK_BUF_BYTES] = {};
  size_t used = 0;
};

// True for a well-formed entity reference at html[pos] ('&'): &name; &#123; or
// &#x1F;. On success *end is the index of the ';'. Well-formed references are
// copied through verbatim -- the parser resolves HTML named entities such as
// &nbsp; through its default handler, so rewriting them would lose them.
bool isEntityRef(const std::string& html, const size_t pos, size_t* end) {
  size_t j = pos + 1;
  const size_t n = html.size();
  if (j < n && html[j] == '#') {
    j++;
    if (j < n && (html[j] == 'x' || html[j] == 'X')) j++;
    const size_t digits = j;
    while (j < n && std::isxdigit(static_cast<unsigned char>(html[j]))) j++;
    if (j == digits) return false;
  } else {
    const size_t letters = j;
    while (j < n && std::isalnum(static_cast<unsigned char>(html[j]))) j++;
    if (j == letters) return false;
  }
  if (j >= n || html[j] != ';') return false;
  *end = j;
  return true;
}

// Copy one character of an attribute value, escaping what XML forbids there.
// `i` advances past a whole entity reference when one starts here.
bool appendAttrChar(const std::string& html, size_t& i, Sink& sink) {
  const char c = html[i];
  if (c == '&') {
    size_t entityEnd = 0;
    if (isEntityRef(html, i, &entityEnd)) {
      if (!sink.append(html.data() + i, entityEnd - i + 1)) return false;
      i = entityEnd + 1;
      return true;
    }
    i++;
    return sink.append("&amp;");
  }
  i++;
  if (c == '<') return sink.append("&lt;");
  if (c == '>') return sink.append("&gt;");
  if (c == '"') return sink.append("&quot;");
  return sink.append(c);
}

// Rewrite the attribute region of a start tag, [pos, end), into quoted XML
// attributes. Names are lowercased; values are quoted whether the source quoted
// them or not; a valueless HTML boolean attribute becomes name="name", the XHTML
// spelling. Junk that is neither is dropped rather than passed through to break
// the parse.
bool writeAttributes(const std::string& html, size_t pos, const size_t end, Sink& sink) {
  while (pos < end) {
    if (std::isspace(static_cast<unsigned char>(html[pos]))) {
      pos++;
      continue;
    }
    if (!isNameChar(static_cast<unsigned char>(html[pos]))) {
      pos++;  // stray '=' , quote or punctuation with no name in front of it
      continue;
    }

    char nameBuf[NAME_BUF_BYTES];
    size_t nameLen = 0;
    while (pos < end && isNameChar(static_cast<unsigned char>(html[pos]))) {
      if (nameLen < NAME_BUF_BYTES - 1) nameBuf[nameLen++] = toLowerAscii(html[pos]);
      pos++;
    }
    if (!sink.append(' ') || !sink.append(nameBuf, nameLen)) return false;

    size_t after = pos;
    while (after < end && std::isspace(static_cast<unsigned char>(html[after]))) after++;
    if (after >= end || html[after] != '=') {
      // Valueless in HTML ("<td nowrap>"); XML has no such thing.
      if (!sink.append("=\"") || !sink.append(nameBuf, nameLen) || !sink.append('"')) return false;
      continue;  // leave pos where it is: the next name starts there
    }

    pos = after + 1;
    while (pos < end && std::isspace(static_cast<unsigned char>(html[pos]))) pos++;
    if (!sink.append("=\"")) return false;
    if (pos < end && (html[pos] == '"' || html[pos] == '\'')) {
      const char quote = html[pos++];
      while (pos < end && html[pos] != quote) {
        if (!appendAttrChar(html, pos, sink)) return false;
      }
      if (pos < end) pos++;  // closing quote
    } else {
      // Unquoted value: runs to the next whitespace.
      while (pos < end && !std::isspace(static_cast<unsigned char>(html[pos]))) {
        if (!appendAttrChar(html, pos, sink)) return false;
      }
    }
    if (!sink.append('"')) return false;
  }
  return true;
}

}  // namespace

bool normalizeDictionaryHtml(const std::string& html, Print& out) {
  Sink sink(out);
  if (!sink.append("<html><body>")) return false;

  const size_t n = html.size();
  size_t i = 0;
  while (i < n) {
    const char c = html[i];

    if (c == '<' && i + 1 < n && (html[i + 1] == '!' || html[i + 1] == '?')) {
      // Comment, doctype or processing instruction: carries nothing the layout
      // wants, and a malformed one would fail the parse.
      const bool isComment = html.compare(i, 4, "<!--") == 0;
      const size_t j = isComment ? html.find("-->", i + 4) : html.find('>', i);
      i = (j == std::string::npos) ? n : j + (isComment ? 3 : 1);
      continue;
    }

    if (c == '<' && i + 1 < n && (html[i + 1] == '/' || std::isalpha(static_cast<unsigned char>(html[i + 1])))) {
      // Find the tag's '>', honouring quoted attribute values so a '>' inside
      // one does not end the tag early.
      size_t j = i + 1;
      char quote = 0;
      while (j < n) {
        const char d = html[j];
        if (quote) {
          if (d == quote) quote = 0;
        } else if (d == '"' || d == '\'') {
          quote = d;
        } else if (d == '>') {
          break;
        }
        j++;
      }
      if (j == n) {  // unterminated tag: the '<' was literal text after all
        if (!sink.append("&lt;")) return false;
        i++;
        continue;
      }

      const bool closing = html[i + 1] == '/';
      size_t nameEnd = i + (closing ? 2 : 1);
      char nameBuf[NAME_BUF_BYTES];
      size_t nameLen = 0;
      while (nameEnd < j && isNameChar(static_cast<unsigned char>(html[nameEnd]))) {
        if (nameLen < NAME_BUF_BYTES - 1) nameBuf[nameLen++] = toLowerAscii(html[nameEnd]);
        nameEnd++;
      }
      if (nameLen == 0) {  // "< 5" or "</>" -- not a tag
        if (!sink.append("&lt;")) return false;
        i++;
        continue;
      }

      if (!sink.append('<')) return false;
      if (closing && !sink.append('/')) return false;
      if (!sink.append(nameBuf, nameLen)) return false;

      if (!closing) {
        // A trailing '/' is the self-close marker, not an attribute. Look past
        // trailing whitespace for it, and keep it out of the attribute region.
        size_t attrEnd = j;
        while (attrEnd > nameEnd && std::isspace(static_cast<unsigned char>(html[attrEnd - 1]))) attrEnd--;
        const bool selfClosed = attrEnd > nameEnd && html[attrEnd - 1] == '/';
        if (selfClosed) attrEnd--;
        if (!writeAttributes(html, nameEnd, attrEnd, sink)) return false;
        if (selfClosed && !sink.append('/')) return false;
      }
      if (!sink.append('>')) return false;
      i = j + 1;
      continue;
    }

    if (c == '<') {  // stray '<' in running text ("x < y")
      if (!sink.append("&lt;")) return false;
      i++;
      continue;
    }

    if (c == '&') {
      size_t entityEnd = 0;
      if (isEntityRef(html, i, &entityEnd)) {
        if (!sink.append(html.data() + i, entityEnd - i + 1)) return false;
        i = entityEnd + 1;
      } else {  // bare ampersand ("Tom & Jerry")
        if (!sink.append("&amp;")) return false;
        i++;
      }
      continue;
    }

    if (!sink.append(c)) return false;
    i++;
  }

  return sink.append("</body></html>") && sink.flush();
}
