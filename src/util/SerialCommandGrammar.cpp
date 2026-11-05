#include "SerialCommandGrammar.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

namespace SerialCommandGrammar {

namespace {

bool equalsIgnoreCase(const std::string_view a, const std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'a' && x <= 'z') x = static_cast<char>(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z') y = static_cast<char>(y - 'a' + 'A');
    if (x != y) return false;
  }
  return true;
}

bool needsQuotes(const std::string_view value) {
  if (value.empty()) return true;
  for (const char c : value) {
    if (c == ' ' || c == '"' || c == '\\' || c == '=' || static_cast<unsigned char>(c) < 0x20) return true;
  }
  return false;
}

}  // namespace

bool parseButtonArg(const std::string_view arg, std::string_view& name, PressKind& kind) {
  const size_t colon = arg.find(':');
  name = arg.substr(0, colon);
  kind = PressKind::Short;
  if (name.empty()) return false;
  if (colon == std::string_view::npos) return true;
  const std::string_view modifier = arg.substr(colon + 1);
  if (modifier == "LONG") {
    kind = PressKind::Long;
    return true;
  }
  if (modifier == "DOUBLE") {
    kind = PressKind::Double;
    return true;
  }
  return false;
}

bool parseReaderTarget(const std::string_view arg, ReaderTarget& out) {
  out = ReaderTarget{};
  out.path = arg;
  const size_t at = arg.rfind('@');
  if (at == std::string_view::npos) return !arg.empty();
  const std::string_view position = arg.substr(at + 1);

  if (!position.empty() && position.back() == '%') {
    int percent = 0;
    if (!parseInt(position.substr(0, position.size() - 1), percent)) return !arg.empty();
    out.path = arg.substr(0, at);
    out.kind = ReaderTarget::Kind::Percent;
    out.percent = percent;
    return !out.path.empty() && percent >= 0 && percent <= 100;
  }

  const size_t colon = position.find(':');
  int spine = 0;
  int page = 0;
  if (colon == std::string_view::npos || !parseInt(position.substr(0, colon), spine) ||
      !parseInt(position.substr(colon + 1), page)) {
    return !arg.empty();  // not a position: the '@' belongs to the file name
  }
  out.path = arg.substr(0, at);
  out.kind = ReaderTarget::Kind::SpinePage;
  out.spine = spine;
  out.page = page;
  return !out.path.empty() && spine >= 1 && page >= 1;
}

bool splitKeyValue(const std::string_view arg, std::string_view& key, std::string_view& value) {
  const size_t eq = arg.find('=');
  if (eq == std::string_view::npos || eq == 0) return false;
  key = arg.substr(0, eq);
  value = arg.substr(eq + 1);
  return true;
}

bool parseInt(const std::string_view text, int& out) {
  size_t i = 0;
  bool negative = false;
  if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
    negative = text[0] == '-';
    i = 1;
  }
  if (i >= text.size()) return false;
  // Checked before the step, not after it: `long` is 32 bits on the device, so value * 10 + digit
  // could wrap back into range first ("9999999999" became 1410065407 and was accepted).
  constexpr uint32_t kMax = 0x7fffffffu;
  uint32_t value = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    const uint32_t digit = static_cast<uint32_t>(c - '0');
    if (value > (kMax - digit) / 10) return false;
    value = value * 10 + digit;
  }
  out = negative ? -static_cast<int>(value) : static_cast<int>(value);
  return true;
}

bool parseIntList(std::string_view text, int* out, const size_t count) {
  for (size_t i = 0; i < count; ++i) {
    const size_t comma = text.find(',');
    const bool last = i + 1 == count;
    // The last number takes the rest; a comma left in it means too many numbers.
    if (last != (comma == std::string_view::npos)) return false;
    if (!parseInt(text.substr(0, comma), out[i])) return false;
    if (!last) text.remove_prefix(comma + 1);
  }
  return true;
}

namespace {
// "<key>=<a>" or "<key>=<a>,<b>"; `numbers[1]` keeps its value when there is no second number.
bool parseKeyedPair(const std::string_view arg, const std::string_view wantedKey, int (&numbers)[2]) {
  std::string_view key;
  std::string_view value;
  if (!splitKeyValue(arg, key, value) || key != wantedKey) return false;
  const bool split = value.find(',') != std::string_view::npos;
  return parseIntList(value, numbers, split ? 2 : 1);
}
}  // namespace

bool parseHeapHold(const std::string_view arg, int& bytes, int& blocks, const int maxBlocks) {
  int numbers[2] = {0, 1};
  if (!parseKeyedPair(arg, "HOLD", numbers)) return false;
  if (numbers[0] <= 0 || numbers[1] < 1 || numbers[1] > maxBlocks || numbers[0] < numbers[1]) return false;
  bytes = numbers[0];
  blocks = numbers[1];
  return true;
}

bool parseHeapRefuse(const std::string_view arg, int& decodes, int& reloads) {
  int numbers[2] = {0, 0};
  if (!parseKeyedPair(arg, "REFUSE", numbers)) return false;
  if (numbers[0] < 0 || numbers[0] > 255 || numbers[1] < 0 || numbers[1] > 255) return false;
  decodes = numbers[0];
  reloads = numbers[1];
  return true;
}

bool parseLogLevel(const std::string_view text, uint8_t& level) {
  // Tag and long name per level, in level order: "ERR" or "ERROR" is 0.
  static constexpr const char* kNames[] = {"ERR", "ERROR", "INF", "INFO", "DBG", "DEBUG", "TRC", "TRACE"};
  for (uint8_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
    if (equalsIgnoreCase(text, kNames[i])) {
      level = i / 2;
      return true;
    }
  }
  return false;
}

bool parseLanguage(const std::string_view text, const char* const* codes, const char* const* names, const size_t count,
                   size_t& index) {
  for (size_t i = 0; i < count; ++i) {
    if (equalsIgnoreCase(text, codes[i]) || text == names[i]) {
      index = i;
      return true;
    }
  }
  int number = 0;
  if (!parseInt(text, number) || number < 0 || static_cast<size_t>(number) >= count) return false;
  index = static_cast<size_t>(number);
  return true;
}

namespace {

// A decimal byte count that fits a FAT32 file: digits only, 0 .. 4294967295.
bool parseSize32(const std::string_view text, uint32_t& out) {
  if (text.empty() || text.size() > 10) return false;
  uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  if (value > 0xFFFFFFFFull) return false;
  out = static_cast<uint32_t>(value);
  return true;
}

// 1 to 8 hex digits, any case.
bool parseHex32(const std::string_view text, uint32_t& out) {
  if (text.empty() || text.size() > 8) return false;
  uint32_t value = 0;
  for (const char c : text) {
    uint32_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  out = value;
  return true;
}

}  // namespace

bool parseFsPath(const std::string_view arg, std::string_view& path) {
  if (arg.empty() || arg.front() != '/' || arg.find(':') != std::string_view::npos) return false;
  path = arg;
  return true;
}

bool parseFsPut(const std::string_view arg, std::string_view& path, uint32_t& size, uint32_t& crc) {
  const size_t crcColon = arg.rfind(':');
  if (crcColon == std::string_view::npos || crcColon == 0) return false;
  const size_t sizeColon = arg.rfind(':', crcColon - 1);
  if (sizeColon == std::string_view::npos) return false;
  uint32_t parsedSize = 0;
  uint32_t parsedCrc = 0;
  if (!parseSize32(arg.substr(sizeColon + 1, crcColon - sizeColon - 1), parsedSize) ||
      !parseHex32(arg.substr(crcColon + 1), parsedCrc) || !parseFsPath(arg.substr(0, sizeColon), path)) {
    return false;
  }
  size = parsedSize;
  crc = parsedCrc;
  return true;
}

bool parseFsMove(const std::string_view arg, std::string_view& from, std::string_view& to) {
  const size_t colon = arg.find(':');
  if (colon == std::string_view::npos) return false;
  return parseFsPath(arg.substr(0, colon), from) && parseFsPath(arg.substr(colon + 1), to);
}

bool parseTtfShape(std::string_view arg, std::string_view& language, std::string_view& text) {
  constexpr std::string_view kPrefix = "SHAPE:";
  constexpr size_t kMaxLanguage = 15;
  if (arg.substr(0, kPrefix.size()) != kPrefix) return false;
  arg.remove_prefix(kPrefix.size());
  const size_t colon = arg.find(':');
  if (colon == std::string_view::npos || colon > kMaxLanguage) return false;
  const std::string_view tag = arg.substr(0, colon);
  const std::string_view body = arg.substr(colon + 1);
  if (body.empty() || body.size() > kMaxTtfShapeText) return false;
  for (const char c : tag) {
    const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!alnum && c != '-') return false;
  }
  language = tag;
  text = body;
  return true;
}

namespace {

// The characters CMD:PAGE writes as \u{XXXX} although they are valid UTF-8: controls, and the marks
// that take no room on the page, so a bidi control or a zero-width space shows in the dump.
bool escapedCodepoint(const uint32_t cp) {
  return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0xAD || cp == 0x61C || (cp >= 0x200B && cp <= 0x200F) ||
         (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2069) || cp == 0xFEFF;
}

// The length of the valid UTF-8 sequence at the start of `text`, and its codepoint; 0 when the first
// byte starts none (RFC 3629: no overlong forms, no surrogates, nothing above U+10FFFF).
size_t decodeUtf8(const std::string_view text, uint32_t& cp) {
  const auto byte = [&text](const size_t i) { return static_cast<uint8_t>(text[i]); };
  const uint8_t lead = byte(0);
  if (lead < 0x80) {
    cp = lead;
    return 1;
  }
  size_t length = 0;
  uint8_t low = 0x80;  // the range the second byte must fall in
  uint8_t high = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    cp = lead & 0x1F;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    cp = lead & 0x0F;
    if (lead == 0xE0) low = 0xA0;   // overlong
    if (lead == 0xED) high = 0x9F;  // a surrogate
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    cp = lead & 0x07;
    if (lead == 0xF0) low = 0x90;   // overlong
    if (lead == 0xF4) high = 0x8F;  // above U+10FFFF
  } else {
    return 0;
  }
  if (text.size() < length || byte(1) < low || byte(1) > high) return 0;
  for (size_t i = 1; i < length; ++i) {
    if ((byte(i) & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (byte(i) & 0x3F);
  }
  return length;
}

}  // namespace

size_t escapeTextUnit(const std::string_view text, size_t& pos, char* out) {
  constexpr char kHex[] = "0123456789ABCDEF";
  const std::string_view rest = text.substr(pos);
  uint32_t cp = 0;
  const size_t length = decodeUtf8(rest, cp);
  if (length == 0) {
    const auto b = static_cast<uint8_t>(rest[0]);
    pos += 1;
    const char escape[] = {'\\', 'x', '{', kHex[b >> 4], kHex[b & 0x0F], '}'};
    std::copy(std::begin(escape), std::end(escape), out);
    return sizeof(escape);
  }
  pos += length;
  if (escapedCodepoint(cp)) {
    const char escape[] = {
        '\\', 'u', '{', kHex[(cp >> 12) & 0x0F], kHex[(cp >> 8) & 0x0F], kHex[(cp >> 4) & 0x0F], kHex[cp & 0x0F], '}'};
    std::copy(std::begin(escape), std::end(escape), out);
    return sizeof(escape);
  }
  if (cp == '"' || cp == '\\') {
    out[0] = '\\';
    out[1] = static_cast<char>(cp);
    return 2;
  }
  std::copy(rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(length), out);
  return length;
}

void appendField(std::string& out, const char* key, const std::string_view value) {
  out += ' ';
  out += key;
  out += '=';
  appendValue(out, value);
}

void appendValue(std::string& out, const std::string_view value) {
  if (!needsQuotes(value)) {
    out.append(value.data(), value.size());
    return;
  }
  out += '"';
  for (const char c : value) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  out += '"';
}

void appendField(std::string& out, const char* key, const long value) {
  char digits[24];
  snprintf(digits, sizeof(digits), "%ld", value);
  appendField(out, key, std::string_view(digits));
}

}  // namespace SerialCommandGrammar
