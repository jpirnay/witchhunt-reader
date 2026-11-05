#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The argument grammar of the serial test commands (CMD:..., see SerialTestApi.h and
// docs/serial-test-api.md), kept apart from the dispatcher so it can be tested on the host. Nothing
// here touches the device: every function reads a view of the command line and fills plain values.
namespace SerialCommandGrammar {

// How CMD:BTN:<name>[:LONG|:DOUBLE] presses the button.
enum class PressKind : uint8_t { Short, Long, Double };

// Splits "<NAME>[:LONG|:DOUBLE]". False for any other suffix, so a typo is an error rather than a
// short press of a button called "BACK:LONGG".
bool parseButtonArg(std::string_view arg, std::string_view& name, PressKind& kind);

// Where CMD:ACT:READER:<path>[@<spine>:<page>|@<percent>%] opens the book. Spine and page are
// 1-based, as CMD:STATE reports them.
struct ReaderTarget {
  enum class Kind : uint8_t { Saved, SpinePage, Percent };
  std::string_view path;
  Kind kind = Kind::Saved;
  int spine = 0;
  int page = 0;
  int percent = 0;
};

// The position is the text after the LAST '@', and only when all of it fits one of the two forms:
// a file name may contain '@' itself, and "/books/a@b.epub" must still open that book.
// False when a position is given but out of range (spine or page below 1, percent above 100).
bool parseReaderTarget(std::string_view arg, ReaderTarget& out);

// Splits "<key>=<value>" at the first '='. False when there is no '=' or the key is empty.
bool splitKeyValue(std::string_view arg, std::string_view& key, std::string_view& value);

// A decimal integer, optionally negative, with nothing else around it.
bool parseInt(std::string_view text, int& out);

// Exactly `count` comma-separated integers ("x,y" for a tap, "x1,y1,x2,y2" for a swipe).
bool parseIntList(std::string_view text, int* out, size_t count);

// CMD:HEAP's ballast: "HOLD=<bytes>" or "HOLD=<bytes>,<blocks>", the bytes split over 1..maxBlocks equal blocks.
bool parseHeapHold(std::string_view arg, int& bytes, int& blocks, int maxBlocks);
// CMD:HEAP's refusals: "REFUSE=<decodes>" or "REFUSE=<decodes>,<reloads>", each 0..255.
bool parseHeapRefuse(std::string_view arg, int& decodes, int& reloads);

// The log levels of lib/Logging (0 = ERR .. 3 = TRC), by their tag or its long name:
// ERR|ERROR, INF|INFO, DBG|DEBUG, TRC|TRACE. Case-insensitive.
bool parseLogLevel(std::string_view text, uint8_t& level);

// A UI language for CMD:SET:language=, out of `count` languages given by their codes (EN, DE, ZH...)
// and native names: its code in any case, its name exactly as the picker shows it, or its index.
bool parseLanguage(std::string_view text, const char* const* codes, const char* const* names, size_t count,
                   size_t& index);

// The file commands' arguments (CMD:FS:...). A FAT name cannot hold ':', so a ':' always separates
// fields, and every card path starts with '/'.
//
// CMD:FS:GET, HASH and MKDIR: one card path. False when it does not start with '/' or holds a ':'.
bool parseFsPath(std::string_view arg, std::string_view& path);
// CMD:FS:PUT:<path>:<size>:<crc32hex>: the last two fields are the size (decimal, 0 to 4294967295, what
// a FAT32 file can hold) and the zlib CRC32 (1 to 8 hex digits, any case); the path is all before them.
bool parseFsPut(std::string_view arg, std::string_view& path, uint32_t& size, uint32_t& crc);
// CMD:FS:MV:<from>:<to>: two card paths and the one ':' between them.
bool parseFsMove(std::string_view arg, std::string_view& from, std::string_view& to);

// CMD:TTF's "SHAPE:<bcp47>:<utf8>": the language is empty or 1-15 of [A-Za-z0-9-], the text 1-192 bytes (it may hold
// ':', so it is all after the second one).
inline constexpr size_t kMaxTtfShapeText = 192;
bool parseTtfShape(std::string_view arg, std::string_view& language, std::string_view& text);

// CMD:PAGE writes a word's text in double quotes, one unit at a time: a UTF-8 character, or a byte
// that starts none. escapeTextUnit() writes the unit of `text` at `pos` to `out`, which has room for
// kMaxEscapedUnit bytes, moves `pos` past it and returns the bytes written:
//  - printable UTF-8 as it is, '"' and '\' with a backslash before them;
//  - the C0 controls, DEL, the C1 controls and the invisible marks a reader of the text would want to
//    see -- U+00AD, U+061C, U+200B-U+200F, U+2028-U+202E, U+2060-U+2069 and U+FEFF, the bidi
//    controls among them -- as \u{XXXX};
//  - a byte that is not part of a valid UTF-8 sequence (overlong, a surrogate, above U+10FFFF, cut
//    short) as \x{XX}, and the next unit starts at the byte after it.
inline constexpr size_t kMaxEscapedUnit = 8;  // "\u{XXXX}"
size_t escapeTextUnit(std::string_view text, size_t& pos, char* out);

// Appends " <key>=<value>" to a CMD:STATE line. A value that holds a space, a quote, a backslash or
// an '=' is written in double quotes with '"' and '\' escaped by a backslash, so a host can split
// the line on spaces outside quotes; control characters (a newline in a popup's text) become
// spaces, since the line must stay one line.
void appendField(std::string& out, const char* key, std::string_view value);
void appendField(std::string& out, const char* key, long value);
// Just the value half of appendField(), quoted by the same rule.
void appendValue(std::string& out, std::string_view value);

}  // namespace SerialCommandGrammar
