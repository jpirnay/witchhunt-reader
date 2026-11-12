// The argument grammar of the serial test commands (docs/serial-test-api.md): what a host may send
// after CMD:BTN, CMD:ACT:READER, CMD:SET, CMD:TOUCH/SWIPE, CMD:LOG and CMD:FS, how CMD:STATE writes its
// key=value pairs so a host can split them again, how CMD:PAGE escapes a word's text, and the CRC32
// the file commands check.

#include <SerialTransfer/SerialTransferProtocol.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "util/ButtonPressQueue.h"
#include "util/SerialCommandGrammar.h"

using SerialCommandGrammar::PressKind;
using SerialCommandGrammar::ReaderTarget;

// What SerialTestApi passes.
constexpr int kMaxHoldMs = ButtonPressQueue::MAX_HOLD_MS;

namespace {

// Splits a STATE line the way the doc tells a host to: on spaces outside double quotes, with
// backslash escapes inside them. The inverse of appendField().
std::vector<std::pair<std::string, std::string>> splitFields(const std::string& line) {
  std::vector<std::pair<std::string, std::string>> fields;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    if (i >= line.size()) break;
    const size_t eq = line.find('=', i);
    std::string key = line.substr(i, eq - i);
    i = eq + 1;
    std::string value;
    if (i < line.size() && line[i] == '"') {
      ++i;
      while (i < line.size() && line[i] != '"') {
        if (line[i] == '\\' && i + 1 < line.size()) ++i;
        value += line[i++];
      }
      ++i;  // closing quote
    } else {
      while (i < line.size() && line[i] != ' ') value += line[i++];
    }
    fields.emplace_back(key, value);
  }
  return fields;
}

}  // namespace

TEST(SerialCommandGrammar, ButtonNamesAndModifiers) {
  std::string_view name;
  PressKind kind = PressKind::Long;
  int holdMs = 0;
  ASSERT_TRUE(SerialCommandGrammar::parseButtonArg("BACK", name, kind, holdMs, kMaxHoldMs));
  EXPECT_EQ(name, "BACK");
  EXPECT_EQ(kind, PressKind::Short);
  ASSERT_TRUE(SerialCommandGrammar::parseButtonArg("CONFIRM:LONG", name, kind, holdMs, kMaxHoldMs));
  EXPECT_EQ(name, "CONFIRM");
  EXPECT_EQ(kind, PressKind::Long);
  ASSERT_TRUE(SerialCommandGrammar::parseButtonArg("POWER:DOUBLE", name, kind, holdMs, kMaxHoldMs));
  EXPECT_EQ(name, "POWER");
  EXPECT_EQ(kind, PressKind::Double);
}

TEST(SerialCommandGrammar, ATouchPointTakesTheSameSuffix) {
  std::string_view point;
  PressKind kind = PressKind::Short;
  int holdMs = 0;
  ASSERT_TRUE(SerialCommandGrammar::parseButtonArg("120,300", point, kind, holdMs, kMaxHoldMs));
  EXPECT_EQ(point, "120,300");
  EXPECT_EQ(kind, PressKind::Short);
  ASSERT_TRUE(SerialCommandGrammar::parseButtonArg("120,300:LONG", point, kind, holdMs, kMaxHoldMs));
  EXPECT_EQ(point, "120,300");
  EXPECT_EQ(kind, PressKind::Long);
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("120,300:LONGER", point, kind, holdMs, kMaxHoldMs));
}

TEST(SerialCommandGrammar, ANumberIsAHeldPressOfThatManyMilliseconds) {
  std::string_view name;
  PressKind kind = PressKind::Short;
  int holdMs = 0;
  ASSERT_TRUE(SerialCommandGrammar::parseButtonArg("DOWN:1500", name, kind, holdMs, kMaxHoldMs));
  EXPECT_EQ(name, "DOWN");
  EXPECT_EQ(kind, PressKind::Hold);
  EXPECT_EQ(holdMs, 1500);
  ASSERT_TRUE(SerialCommandGrammar::parseButtonArg("BACK:1", name, kind, holdMs, kMaxHoldMs));
  EXPECT_EQ(holdMs, 1);
  ASSERT_TRUE(SerialCommandGrammar::parseButtonArg("BACK:10000", name, kind, holdMs, kMaxHoldMs));
  EXPECT_EQ(holdMs, 10000);
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("BACK:10001", name, kind, holdMs, kMaxHoldMs)) << "past the cap";
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("BACK:0", name, kind, holdMs, kMaxHoldMs));
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("BACK:-5", name, kind, holdMs, kMaxHoldMs));
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("BACK:+5", name, kind, holdMs, kMaxHoldMs));
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("BACK:15OO", name, kind, holdMs, kMaxHoldMs));
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("BACK:99999999999", name, kind, holdMs, kMaxHoldMs));
}

TEST(SerialCommandGrammar, ATypoInTheModifierIsAnErrorNotAShortPress) {
  std::string_view name;
  PressKind kind = PressKind::Short;
  int holdMs = 0;
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("BACK:LONGG", name, kind, holdMs, kMaxHoldMs));
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("BACK:", name, kind, holdMs, kMaxHoldMs));
  EXPECT_FALSE(SerialCommandGrammar::parseButtonArg("", name, kind, holdMs, kMaxHoldMs));
}

TEST(SerialCommandGrammar, ReaderTargetWithoutAPositionOpensAtTheSavedOne) {
  ReaderTarget target;
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/test/The Anarchy - Dalrymple, William.epub", target));
  EXPECT_EQ(target.kind, ReaderTarget::Kind::Saved);
  EXPECT_EQ(target.path, "/test/The Anarchy - Dalrymple, William.epub");
}

TEST(SerialCommandGrammar, ReaderTargetSpineAndPage) {
  ReaderTarget target;
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/test/a.epub@3:12", target));
  EXPECT_EQ(target.kind, ReaderTarget::Kind::SpinePage);
  EXPECT_EQ(target.path, "/test/a.epub");
  EXPECT_EQ(target.spine, 3);
  EXPECT_EQ(target.page, 12);
}

TEST(SerialCommandGrammar, ReaderTargetPercent) {
  ReaderTarget target;
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/test/a.epub@50%", target));
  EXPECT_EQ(target.kind, ReaderTarget::Kind::Percent);
  EXPECT_EQ(target.path, "/test/a.epub");
  EXPECT_EQ(target.percent, 50);
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/test/a.epub@100%", target));
  EXPECT_EQ(target.percent, 100);
}

// An '@' in a file name is not a position unless all of what follows the last one is one.
TEST(SerialCommandGrammar, AnAtSignInTheFileNameStaysInThePath) {
  ReaderTarget target;
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/books/me@home.epub", target));
  EXPECT_EQ(target.kind, ReaderTarget::Kind::Saved);
  EXPECT_EQ(target.path, "/books/me@home.epub");
  ASSERT_TRUE(SerialCommandGrammar::parseReaderTarget("/books/me@home.epub@2:1", target));
  EXPECT_EQ(target.kind, ReaderTarget::Kind::SpinePage);
  EXPECT_EQ(target.path, "/books/me@home.epub");
}

TEST(SerialCommandGrammar, ReaderPositionsOutOfRangeAreRefused) {
  ReaderTarget target;
  EXPECT_FALSE(SerialCommandGrammar::parseReaderTarget("/a.epub@0:1", target)) << "spines count from 1";
  EXPECT_FALSE(SerialCommandGrammar::parseReaderTarget("/a.epub@1:0", target)) << "pages count from 1";
  EXPECT_FALSE(SerialCommandGrammar::parseReaderTarget("/a.epub@101%", target));
  EXPECT_FALSE(SerialCommandGrammar::parseReaderTarget("@3:4", target)) << "no path";
  EXPECT_FALSE(SerialCommandGrammar::parseReaderTarget("", target));
}

TEST(SerialCommandGrammar, KeyValueSplitsAtTheFirstEquals) {
  std::string_view key;
  std::string_view value;
  ASSERT_TRUE(SerialCommandGrammar::splitKeyValue("opdsUrl=http://x/?a=b", key, value));
  EXPECT_EQ(key, "opdsUrl");
  EXPECT_EQ(value, "http://x/?a=b");
  ASSERT_TRUE(SerialCommandGrammar::splitKeyValue("name=", key, value));
  EXPECT_EQ(value, "");
  EXPECT_FALSE(SerialCommandGrammar::splitKeyValue("=1", key, value));
  EXPECT_FALSE(SerialCommandGrammar::splitKeyValue("fontSize", key, value));
}

TEST(SerialCommandGrammar, IntegersAndLists) {
  int value = 0;
  EXPECT_TRUE(SerialCommandGrammar::parseInt("42", value));
  EXPECT_EQ(value, 42);
  EXPECT_TRUE(SerialCommandGrammar::parseInt("-7", value));
  EXPECT_EQ(value, -7);
  EXPECT_FALSE(SerialCommandGrammar::parseInt("", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("-", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("12a", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("99999999999", value));
  // The limits, and numbers whose last step overflows 32 bits: on the device `long` was 32 bits
  // and they wrapped back into range before the range check (the host's 64-bit long hid that).
  EXPECT_TRUE(SerialCommandGrammar::parseInt("2147483647", value));
  EXPECT_EQ(value, 2147483647);
  EXPECT_TRUE(SerialCommandGrammar::parseInt("-2147483647", value));
  EXPECT_EQ(value, -2147483647);
  EXPECT_FALSE(SerialCommandGrammar::parseInt("2147483648", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("9999999999", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("4294967296", value));
  EXPECT_FALSE(SerialCommandGrammar::parseInt("999999999999", value));

  int xy[2] = {};
  ASSERT_TRUE(SerialCommandGrammar::parseIntList("120,640", xy, 2));
  EXPECT_EQ(xy[0], 120);
  EXPECT_EQ(xy[1], 640);
  int swipe[4] = {};
  ASSERT_TRUE(SerialCommandGrammar::parseIntList("10,700,10,100", swipe, 4));
  EXPECT_EQ(swipe[3], 100);
  EXPECT_FALSE(SerialCommandGrammar::parseIntList("1,2,3", xy, 2)) << "too many";
  EXPECT_FALSE(SerialCommandGrammar::parseIntList("1", xy, 2)) << "too few";
  EXPECT_FALSE(SerialCommandGrammar::parseIntList("1,", xy, 2));
}

TEST(SerialCommandGrammar, HeapBallastInBytesAndBlocks) {
  int bytes = 0;
  int blocks = 0;
  ASSERT_TRUE(SerialCommandGrammar::parseHeapHold("HOLD=20000", bytes, blocks, 16));
  EXPECT_EQ(20000, bytes);
  EXPECT_EQ(1, blocks);
  ASSERT_TRUE(SerialCommandGrammar::parseHeapHold("HOLD=24000,6", bytes, blocks, 16));
  EXPECT_EQ(24000, bytes);
  EXPECT_EQ(6, blocks);
  EXPECT_FALSE(SerialCommandGrammar::parseHeapHold("HOLD=0", bytes, blocks, 16));
  EXPECT_FALSE(SerialCommandGrammar::parseHeapHold("HOLD=-5", bytes, blocks, 16));
  EXPECT_FALSE(SerialCommandGrammar::parseHeapHold("HOLD=4000,17", bytes, blocks, 16)) << "more blocks than held";
  EXPECT_FALSE(SerialCommandGrammar::parseHeapHold("HOLD=4000,0", bytes, blocks, 16));
  EXPECT_FALSE(SerialCommandGrammar::parseHeapHold("HOLD=3,4", bytes, blocks, 16)) << "less than a byte a block";
  EXPECT_FALSE(SerialCommandGrammar::parseHeapHold("HOLD=4000,2,1", bytes, blocks, 16));
  EXPECT_FALSE(SerialCommandGrammar::parseHeapHold("KEEP=4000", bytes, blocks, 16));
  EXPECT_FALSE(SerialCommandGrammar::parseHeapHold("HOLD", bytes, blocks, 16));
}

TEST(SerialCommandGrammar, HeapRefusalsOfDecodesAndReloads) {
  int decodes = -1;
  int reloads = -1;
  ASSERT_TRUE(SerialCommandGrammar::parseHeapRefuse("REFUSE=2", decodes, reloads));
  EXPECT_EQ(2, decodes);
  EXPECT_EQ(0, reloads);
  ASSERT_TRUE(SerialCommandGrammar::parseHeapRefuse("REFUSE=2,1", decodes, reloads));
  EXPECT_EQ(2, decodes);
  EXPECT_EQ(1, reloads);
  ASSERT_TRUE(SerialCommandGrammar::parseHeapRefuse("REFUSE=0", decodes, reloads));  // none: clears them
  EXPECT_EQ(0, decodes);
  EXPECT_FALSE(SerialCommandGrammar::parseHeapRefuse("REFUSE=256", decodes, reloads));
  EXPECT_FALSE(SerialCommandGrammar::parseHeapRefuse("REFUSE=-1", decodes, reloads));
  EXPECT_FALSE(SerialCommandGrammar::parseHeapRefuse("HOLD=2", decodes, reloads));
}

TEST(SerialCommandGrammar, LogLevelsByTagOrName) {
  uint8_t level = 9;
  ASSERT_TRUE(SerialCommandGrammar::parseLogLevel("ERR", level));
  EXPECT_EQ(level, 0);
  ASSERT_TRUE(SerialCommandGrammar::parseLogLevel("info", level));
  EXPECT_EQ(level, 1);
  ASSERT_TRUE(SerialCommandGrammar::parseLogLevel("DEBUG", level));
  EXPECT_EQ(level, 2);
  ASSERT_TRUE(SerialCommandGrammar::parseLogLevel("trc", level));
  EXPECT_EQ(level, 3);
  EXPECT_FALSE(SerialCommandGrammar::parseLogLevel("WARN", level)) << "the firmware has no WARN level";
}

TEST(SerialCommandGrammar, LanguagesByCodeNameOrIndex) {
  const char* const codes[] = {"EN", "DE", "ZH"};
  const char* const names[] = {"English", "Deutsch", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87"};
  size_t index = 9;
  ASSERT_TRUE(SerialCommandGrammar::parseLanguage("ZH", codes, names, 3, index));
  EXPECT_EQ(index, 2u);
  ASSERT_TRUE(SerialCommandGrammar::parseLanguage("de", codes, names, 3, index)) << "a code in any case";
  EXPECT_EQ(index, 1u);
  ASSERT_TRUE(
      SerialCommandGrammar::parseLanguage("\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87", codes, names, 3, index));
  EXPECT_EQ(index, 2u);
  ASSERT_TRUE(SerialCommandGrammar::parseLanguage("0", codes, names, 3, index));
  EXPECT_EQ(index, 0u);
  EXPECT_FALSE(SerialCommandGrammar::parseLanguage("3", codes, names, 3, index)) << "past the last language";
  EXPECT_FALSE(SerialCommandGrammar::parseLanguage("-1", codes, names, 3, index));
  EXPECT_FALSE(SerialCommandGrammar::parseLanguage("FR", codes, names, 3, index));
  EXPECT_FALSE(SerialCommandGrammar::parseLanguage("english", codes, names, 3, index)) << "a name matches exactly";
  EXPECT_FALSE(SerialCommandGrammar::parseLanguage("", codes, names, 3, index));
}

TEST(SerialCommandGrammar, PlainValuesAreWrittenBare) {
  std::string line = "STATE:activity=Home";
  SerialCommandGrammar::appendField(line, "sel", 3L);
  SerialCommandGrammar::appendField(line, "page", "4/12");
  EXPECT_EQ(line, "STATE:activity=Home sel=3 page=4/12");
}

TEST(SerialCommandGrammar, ValuesWithSpacesOrQuotesAreQuotedAndRoundTrip) {
  std::string line;
  SerialCommandGrammar::appendField(line, "book", "/test/The Anarchy - Dalrymple, William.epub");
  SerialCommandGrammar::appendField(line, "label", R"(Say "hi" \ bye)");
  SerialCommandGrammar::appendField(line, "popup", "Nicht\ngefunden");
  SerialCommandGrammar::appendField(line, "stack", "");
  SerialCommandGrammar::appendField(line, "eq", "a=b");
  EXPECT_EQ(line.find('\n'), std::string::npos) << "a reply is one line";
  const auto fields = splitFields(line);
  ASSERT_EQ(fields.size(), 5u);
  EXPECT_EQ(fields[0].second, "/test/The Anarchy - Dalrymple, William.epub");
  EXPECT_EQ(fields[1].second, R"(Say "hi" \ bye)");
  EXPECT_EQ(fields[2].second, "Nicht gefunden");
  EXPECT_EQ(fields[3].first, "stack");
  EXPECT_EQ(fields[3].second, "");
  EXPECT_EQ(fields[4].second, "a=b");
}

TEST(SerialCommandGrammar, NonAsciiTextIsLeftAlone) {
  std::string line;
  SerialCommandGrammar::appendField(line, "label", "Übertragung");
  EXPECT_EQ(line, " label=Übertragung");
}

// ---- CMD:FS:PUT, GET, HASH, MKDIR, MV ----------------------------------------------------------

TEST(SerialFsGrammar, PutTakesTheLastTwoFieldsOffAPathWithSpaces) {
  std::string_view path;
  uint32_t size = 0;
  uint32_t crc = 0;
  ASSERT_TRUE(SerialCommandGrammar::parseFsPut("/test/My Book - Vol 1.epub:123456:cbf43926", path, size, crc));
  EXPECT_EQ(path, "/test/My Book - Vol 1.epub");
  EXPECT_EQ(size, 123456u);
  EXPECT_EQ(crc, 0xCBF43926u);
  ASSERT_TRUE(SerialCommandGrammar::parseFsPut("/empty.txt:0:0", path, size, crc)) << "an empty file";
  EXPECT_EQ(size, 0u);
  EXPECT_EQ(crc, 0u);
  ASSERT_TRUE(SerialCommandGrammar::parseFsPut("/big.bin:4294967295:FFFFFFFF", path, size, crc)) << "any case";
  EXPECT_EQ(size, 4294967295u);
  EXPECT_EQ(crc, 0xFFFFFFFFu);
  ASSERT_TRUE(SerialCommandGrammar::parseFsPut("/a.bin:7:1a", path, size, crc)) << "fewer than 8 digits";
  EXPECT_EQ(crc, 0x1Au);
}

TEST(SerialFsGrammar, PutRefusesMissingFieldsABadSizeAndBadHex) {
  std::string_view path;
  uint32_t size = 0;
  uint32_t crc = 0;
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("", path, size, crc));
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin", path, size, crc)) << "no size, no CRC";
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin:12", path, size, crc)) << "no CRC";
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin::cbf43926", path, size, crc)) << "an empty size";
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin:12:", path, size, crc)) << "an empty CRC";
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut(":12:cbf43926", path, size, crc)) << "no path";
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("a.bin:12:cbf43926", path, size, crc)) << "a path starts with /";
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin:-1:cbf43926", path, size, crc));
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin:+5:cbf43926", path, size, crc));
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin:1e3:cbf43926", path, size, crc));
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin:4294967296:cbf43926", path, size, crc))
      << "a FAT32 file stays below 4 GiB";
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin:99999999999999999999:0", path, size, crc));
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin:12:cbf4392g", path, size, crc)) << "not hex";
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin:12:0x1234", path, size, crc));
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin:12:123456789", path, size, crc)) << "nine digits";
  EXPECT_FALSE(SerialCommandGrammar::parseFsPut("/a.bin:12: cbf43926", path, size, crc));
}

TEST(SerialFsGrammar, APathStartsWithASlashAndHoldsNoColon) {
  std::string_view path;
  ASSERT_TRUE(SerialCommandGrammar::parseFsPath("/test/The Anarchy - Dalrymple, William.epub", path));
  EXPECT_EQ(path, "/test/The Anarchy - Dalrymple, William.epub");
  ASSERT_TRUE(SerialCommandGrammar::parseFsPath("/", path));
  EXPECT_FALSE(SerialCommandGrammar::parseFsPath("", path));
  EXPECT_FALSE(SerialCommandGrammar::parseFsPath("test/a.epub", path));
  EXPECT_FALSE(SerialCommandGrammar::parseFsPath("/a.bin:/b.bin", path)) << "':' cannot be in a FAT name";
}

TEST(SerialFsGrammar, MoveTakesTwoPathsAroundOneColon) {
  std::string_view from;
  std::string_view to;
  ASSERT_TRUE(SerialCommandGrammar::parseFsMove("/test/old name.epub:/books/new name.epub", from, to));
  EXPECT_EQ(from, "/test/old name.epub");
  EXPECT_EQ(to, "/books/new name.epub");
  EXPECT_FALSE(SerialCommandGrammar::parseFsMove("", from, to));
  EXPECT_FALSE(SerialCommandGrammar::parseFsMove("/a.epub", from, to)) << "no target";
  EXPECT_FALSE(SerialCommandGrammar::parseFsMove("/a.epub:", from, to));
  EXPECT_FALSE(SerialCommandGrammar::parseFsMove(":/b.epub", from, to));
  EXPECT_FALSE(SerialCommandGrammar::parseFsMove("/a.epub:b.epub", from, to)) << "both are card paths";
  EXPECT_FALSE(SerialCommandGrammar::parseFsMove("a.epub:/b.epub", from, to));
  EXPECT_FALSE(SerialCommandGrammar::parseFsMove("/a:/b:/c", from, to)) << "':' cannot be in a FAT name";
}

TEST(SerialCommandGrammar, TtfShapeTakesALanguageAndAWord) {
  std::string_view language;
  std::string_view text;
  ASSERT_TRUE(SerialCommandGrammar::parseTtfShape("SHAPE:hi:\xE0\xA4\xB9\xE0\xA4\xBF", language, text));
  EXPECT_EQ(language, "hi");
  EXPECT_EQ(text, "\xE0\xA4\xB9\xE0\xA4\xBF");
  ASSERT_TRUE(SerialCommandGrammar::parseTtfShape("SHAPE::\xE0\xA4\x95\xE0\xA4\xBF", language, text));
  EXPECT_TRUE(language.empty());
  EXPECT_EQ(text, "\xE0\xA4\x95\xE0\xA4\xBF");
  ASSERT_TRUE(SerialCommandGrammar::parseTtfShape("SHAPE:ar:\xD9\x84\xD8\xA7", language, text)) << "Arabic: lam, alef";
  EXPECT_EQ(language, "ar");
  EXPECT_EQ(text, "\xD9\x84\xD8\xA7");
  ASSERT_TRUE(SerialCommandGrammar::parseTtfShape("SHAPE:zh-Hant-TW:x", language, text));
  EXPECT_EQ(language, "zh-Hant-TW");
  EXPECT_FALSE(SerialCommandGrammar::parseTtfShape("SHAPE:hi:", language, text)) << "no text";
  EXPECT_FALSE(SerialCommandGrammar::parseTtfShape("SHAPE:h!:x", language, text)) << "not a language tag";
  EXPECT_FALSE(SerialCommandGrammar::parseTtfShape("SHAPE:abcdefghijklmnop:x", language, text)) << "16 characters";
  EXPECT_FALSE(SerialCommandGrammar::parseTtfShape("SHAPE:hi", language, text)) << "no second colon";
  EXPECT_FALSE(SerialCommandGrammar::parseTtfShape("FULL", language, text));
  const std::string longest = "SHAPE:hi:" + std::string(192, 'a');
  EXPECT_TRUE(SerialCommandGrammar::parseTtfShape(longest, language, text));
  EXPECT_EQ(text.size(), 192u);
  EXPECT_FALSE(SerialCommandGrammar::parseTtfShape(longest + "a", language, text)) << "193 bytes";
}

// ---- CMD:PAGE's text="..." --------------------------------------------------------------------

namespace {

// The whole of `text` escaped, one unit at a time, as the device writes a word.
std::string escaped(const std::string_view text) {
  std::string out;
  size_t pos = 0;
  char unit[SerialCommandGrammar::kMaxEscapedUnit];
  while (pos < text.size()) {
    const size_t before = pos;
    const size_t n = SerialCommandGrammar::escapeTextUnit(text, pos, unit);
    EXPECT_GT(pos, before) << "every call moves on";
    EXPECT_LE(n, SerialCommandGrammar::kMaxEscapedUnit);
    out.append(unit, n);
  }
  return out;
}

}  // namespace

TEST(PageTextEscape, AsciiAndHebrewAreWrittenAsTheyAre) {
  EXPECT_EQ(escaped("Hello, world."), "Hello, world.");
  // שלום, and a Hebrew word with points (U+05B8 qamats, U+05BC dagesh): marks are printable text.
  EXPECT_EQ(escaped("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"), "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D");
  EXPECT_EQ(escaped("\xD7\xA9\xD6\xB8\xD7\x9C\xD7\x95\xD6\xBC\xD7\x9D"),
            "\xD7\xA9\xD6\xB8\xD7\x9C\xD7\x95\xD6\xBC\xD7\x9D");
  EXPECT_EQ(escaped("\xF0\x9F\x98\x80"), "\xF0\x9F\x98\x80") << "a four-byte character";
}

TEST(PageTextEscape, BidiControlsAndInvisibleMarksBecomeCodepoints) {
  EXPECT_EQ(escaped("\xE2\x80\x8F"), "\\u{200F}") << "RLM";
  EXPECT_EQ(escaped("a\xE2\x80\x8Ez"), "a\\u{200E}z") << "LRM";
  EXPECT_EQ(escaped("\xD8\x9C"), "\\u{061C}") << "ALM";
  EXPECT_EQ(escaped("\xE2\x80\xAB\xD7\x90\xE2\x80\xAC"), "\\u{202B}\xD7\x90\\u{202C}") << "RLE, PDF";
  EXPECT_EQ(escaped("\xE2\x81\xA6x\xE2\x81\xA9"), "\\u{2066}x\\u{2069}") << "LRI, PDI";
  EXPECT_EQ(escaped("\xE2\x80\x8B\xE2\x80\x8D\xE2\x81\xA0"), "\\u{200B}\\u{200D}\\u{2060}") << "ZWSP, ZWJ, WJ";
  EXPECT_EQ(escaped("\xEF\xBB\xBF"), "\\u{FEFF}") << "BOM";
  EXPECT_EQ(escaped("hy\xC2\xADphen"), "hy\\u{00AD}phen") << "soft hyphen";
  EXPECT_EQ(escaped("\xE2\x80\xA8\xE2\x80\xA9"), "\\u{2028}\\u{2029}") << "line and paragraph separators";
}

TEST(PageTextEscape, ControlCharactersBecomeCodepoints) {
  EXPECT_EQ(escaped(std::string_view("\x00", 1)), "\\u{0000}");
  EXPECT_EQ(escaped("\x01"), "\\u{0001}");
  EXPECT_EQ(escaped("a\tb\n"), "a\\u{0009}b\\u{000A}");
  EXPECT_EQ(escaped("\x7F"), "\\u{007F}") << "DEL";
  EXPECT_EQ(escaped("\xC2\x85"), "\\u{0085}") << "a C1 control";
}

TEST(PageTextEscape, AQuoteAndABackslashTakeABackslash) {
  EXPECT_EQ(escaped(R"(say "hi")"), R"(say \"hi\")");
  EXPECT_EQ(escaped(R"(C:\books)"), R"(C:\\books)");
}

TEST(PageTextEscape, InvalidBytesAreWrittenOneByOneAsHex) {
  EXPECT_EQ(escaped("\xFF"), "\\x{FF}");
  EXPECT_EQ(escaped("a\xD7"), "a\\x{D7}") << "a sequence cut short at the end";
  EXPECT_EQ(escaped("\xD7Z"), "\\x{D7}Z") << "a lead byte without its continuation";
  EXPECT_EQ(escaped("\x80"), "\\x{80}") << "a lone continuation byte";
  EXPECT_EQ(escaped("\xC0\x80"), "\\x{C0}\\x{80}") << "overlong";
  EXPECT_EQ(escaped("\xE0\x80\xAF"), "\\x{E0}\\x{80}\\x{AF}") << "overlong";
  EXPECT_EQ(escaped("\xED\xA0\x80"), "\\x{ED}\\x{A0}\\x{80}") << "a surrogate";
  EXPECT_EQ(escaped("\xF4\x90\x80\x80"), "\\x{F4}\\x{90}\\x{80}\\x{80}") << "above U+10FFFF";
}

TEST(PageTextEscape, TheNeighboursOfTheEscapedRangesStayText) {
  EXPECT_EQ(escaped("\xE2\x80\x8A"), "\xE2\x80\x8A") << "U+200A hair space";
  EXPECT_EQ(escaped("\xE2\x80\x90"), "\xE2\x80\x90") << "U+2010 hyphen";
  EXPECT_EQ(escaped("\xE2\x80\xAF"), "\xE2\x80\xAF") << "U+202F narrow no-break space";
  EXPECT_EQ(escaped("\xE2\x81\xAA"), "\xE2\x81\xAA") << "U+206A";
  EXPECT_EQ(escaped("\xC2\xA0\xC2\xAC"), "\xC2\xA0\xC2\xAC") << "U+00A0, U+00AC";
  EXPECT_EQ(escaped("\xD8\x9B\xD8\x9D"), "\xD8\x9B\xD8\x9D") << "U+061B, U+061D";
}

// ---- The CRC32 the FS commands use: serialtransfer::crc32Update, zlib's ----------------------

TEST(SerialFsCrc, MatchesZlibCrc32FedInFourKilobyteChunks) {
  using serialtransfer::crc32Update;
  const std::string fox = "The quick brown fox jumps over the lazy dog";
  // zlib.crc32(b"The quick brown fox jumps over the lazy dog")
  EXPECT_EQ(crc32Update(0, reinterpret_cast<const uint8_t*>(fox.data()), fox.size()), 0x414FA339u);
  const std::string shalom = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";
  // zlib.crc32("שלום".encode())
  EXPECT_EQ(crc32Update(0, reinterpret_cast<const uint8_t*>(shalom.data()), shalom.size()), 0xA098974Du);
  // zlib.crc32(bytes(i & 0xff for i in range(10000))), fed as FS:PUT receives it: 4096, 4096, 1808.
  std::vector<uint8_t> data(10000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i & 0xFF);
  uint32_t crc = 0;
  for (size_t at = 0; at < data.size(); at += 4096) {
    crc = crc32Update(crc, data.data() + at, std::min<size_t>(4096, data.size() - at));
  }
  EXPECT_EQ(crc, 0xD1FFC4FCu);
}
