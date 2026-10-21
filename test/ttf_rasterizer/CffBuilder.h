#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "TtfBuilder.h"

/// Small CFF tables built in memory for the CFF reader's tests, name-keyed or CID-keyed, wrapped in an OpenType font
/// by the TrueType builder. DICT offsets are 5-byte integers, so a DICT's size does not depend on where things land.
namespace ttf_test {

inline void putCffInt(Bytes& out, const int32_t value) {
  out.push_back(29);
  put32(out, static_cast<uint32_t>(value));
}

/// A DICT real from its text ("-0.001", "2.5E-4", "1E-3"): BCD nibbles, ended by 0xF.
inline void putCffReal(Bytes& out, const std::string& text) {
  std::vector<uint8_t> nibbles;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c >= '0' && c <= '9') {
      nibbles.push_back(static_cast<uint8_t>(c - '0'));
    } else if (c == '.') {
      nibbles.push_back(0xA);
    } else if (c == 'E' && i + 1 < text.size() && text[i + 1] == '-') {
      nibbles.push_back(0xC);
      ++i;
    } else if (c == 'E') {
      nibbles.push_back(0xB);
    } else if (c == '-') {
      nibbles.push_back(0xE);
    }
  }
  nibbles.push_back(0xF);
  if (nibbles.size() % 2) nibbles.push_back(0xF);
  out.push_back(30);
  for (size_t i = 0; i < nibbles.size(); i += 2) out.push_back(static_cast<uint8_t>(nibbles[i] << 4 | nibbles[i + 1]));
}

/// One-byte operators as themselves, two-byte ones (12 x) as 0x100 | x.
inline void putCffOp(Bytes& out, const uint16_t op) {
  if (op >= 0x100) out.push_back(12);
  out.push_back(static_cast<uint8_t>(op));
}

/// A FontMatrix entry from six space-separated numbers: integers as integers, anything else as a real.
inline void putCffMatrix(Bytes& out, const std::string& numbers) {
  size_t at = 0;
  while (at < numbers.size()) {
    size_t end = numbers.find(' ', at);
    if (end == std::string::npos) end = numbers.size();
    const std::string number = numbers.substr(at, end - at);
    if (number.find_first_not_of("-0123456789") == std::string::npos) {
      putCffInt(out, std::stoi(number));
    } else {
      putCffReal(out, number);
    }
    at = end + 1;
  }
  putCffOp(out, 0x107);
}

inline Bytes cffIndex(const std::vector<Bytes>& items) {
  Bytes out;
  put16(out, static_cast<uint32_t>(items.size()));
  if (items.empty()) return out;
  out.push_back(4);
  uint32_t offset = 1;
  put32(out, offset);
  for (const Bytes& item : items) {
    offset += static_cast<uint32_t>(item.size());
    put32(out, offset);
  }
  for (const Bytes& item : items) out.insert(out.end(), item.begin(), item.end());
  return out;
}

/// A font DICT of a CID-keyed font, or (the first) the Private DICT of a name-keyed one.
struct CffFontDict {
  std::string matrix;             ///< the FontMatrix's six numbers; empty: no FontMatrix
  std::vector<Bytes> localSubrs;  ///< empty: no Subrs entry
  std::string subrsReal;          ///< the Subrs offset as this real instead of an integer (it must truncate to it)
};

struct CffSpec {
  std::vector<Bytes> charStrings = {{14}, {14}, {14}};  ///< endchar each
  bool cid = false;
  std::string topMatrix;         ///< the Top DICT's FontMatrix; empty: none
  std::vector<CffFontDict> fds;  ///< CID: the FDArray; name-keyed: at most one, the Top DICT's Private
  Bytes fdSelect;                ///< CID: the FDSelect as is; empty: one format 3 range, every glyph in FD 0
  Bytes charset;                 ///< the charset as is; empty: format 0 with SID (or CID) g for glyph g
  int32_t charsetOffset = -1;    ///< 0 to 2: a predefined charset; -1: `charset`, after the global subroutines
  Bytes encoding;                ///< name-keyed: an Encoding as is, after the charset; empty: none (Standard)
  std::vector<Bytes> globalSubrs;
  std::vector<std::string> names = {"Test"};
  Bytes header = {1, 0, 4, 4};  ///< major, minor, hdrSize, offSize
};

inline Bytes buildCff(const CffSpec& spec) {
  const uint32_t glyphs = static_cast<uint32_t>(spec.charStrings.size());
  std::vector<Bytes> names;
  for (const std::string& name : spec.names) names.emplace_back(name.begin(), name.end());
  const Bytes nameIndex = cffIndex(names);
  const Bytes strings = cffIndex({Bytes{'A', 'd', 'o', 'b', 'e'}, Bytes{'I', 'd', 'e', 'n', 't', 'i', 't', 'y'}});
  const Bytes globalSubrs = cffIndex(spec.globalSubrs);
  Bytes charset = spec.charset;
  if (charset.empty() && spec.charsetOffset < 0) {
    charset.push_back(0);
    for (uint32_t g = 1; g < glyphs; ++g) put16(charset, g);
  }
  Bytes fdSelect = spec.fdSelect;
  if (spec.cid && fdSelect.empty()) {
    fdSelect.push_back(3);
    put16(fdSelect, 1);
    put16(fdSelect, 0);
    fdSelect.push_back(0);
    put16(fdSelect, glyphs);
  }
  const Bytes charStrings = cffIndex(spec.charStrings);

  // Each Private DICT holds defaultWidthX 0, then Subrs when the font DICT has local subroutines, laid out right
  // after the DICT.
  std::vector<Bytes> privates;
  std::vector<uint32_t> privateSizes;
  for (const CffFontDict& fd : spec.fds) {
    Bytes dict;
    putCffInt(dict, 0);
    putCffOp(dict, 20);
    if (!fd.localSubrs.empty()) {
      if (fd.subrsReal.empty()) {
        putCffInt(dict, static_cast<int32_t>(dict.size() + 6));
      } else {
        putCffReal(dict, fd.subrsReal);
      }
      putCffOp(dict, 19);
    }
    privateSizes.push_back(static_cast<uint32_t>(dict.size()));
    if (!fd.localSubrs.empty()) {
      const Bytes subrs = cffIndex(fd.localSubrs);
      dict.insert(dict.end(), subrs.begin(), subrs.end());
    }
    privates.push_back(dict);
  }
  const auto fontDict = [&](const size_t i, const uint32_t privateAt) {
    Bytes dict;
    if (!spec.fds[i].matrix.empty()) putCffMatrix(dict, spec.fds[i].matrix);
    putCffInt(dict, static_cast<int32_t>(privateSizes[i]));
    putCffInt(dict, static_cast<int32_t>(privateAt));
    putCffOp(dict, 18);
    return dict;
  };
  const auto topDict = [&](const uint32_t charsetAt, const uint32_t encodingAt, const uint32_t charStringsAt,
                           const uint32_t fdArrayAt, const uint32_t fdSelectAt, const uint32_t privateAt) {
    Bytes dict;
    if (spec.cid) {
      putCffInt(dict, 391);  // Adobe
      putCffInt(dict, 392);  // Identity
      putCffInt(dict, 0);
      putCffOp(dict, 0x11E);  // ROS
    }
    if (!spec.topMatrix.empty()) putCffMatrix(dict, spec.topMatrix);
    putCffInt(dict, static_cast<int32_t>(charsetAt));
    putCffOp(dict, 15);
    if (!spec.encoding.empty()) {
      putCffInt(dict, static_cast<int32_t>(encodingAt));
      putCffOp(dict, 16);
    }
    putCffInt(dict, static_cast<int32_t>(charStringsAt));
    putCffOp(dict, 17);
    if (spec.cid) {
      putCffInt(dict, static_cast<int32_t>(fdArrayAt));
      putCffOp(dict, 0x124);
      putCffInt(dict, static_cast<int32_t>(fdSelectAt));
      putCffOp(dict, 0x125);
    } else if (!spec.fds.empty()) {
      putCffInt(dict, static_cast<int32_t>(privateSizes[0]));
      putCffInt(dict, static_cast<int32_t>(privateAt));
      putCffOp(dict, 18);
    }
    return dict;
  };

  // Where everything lands: the sizes do not depend on the offsets.
  uint32_t at =
      static_cast<uint32_t>(spec.header.size() + nameIndex.size() + cffIndex({topDict(0, 0, 0, 0, 0, 0)}).size() +
                            strings.size() + globalSubrs.size());
  const uint32_t charsetAt = spec.charsetOffset >= 0 ? static_cast<uint32_t>(spec.charsetOffset) : at;
  at += static_cast<uint32_t>(charset.size());
  const uint32_t encodingAt = at;
  at += static_cast<uint32_t>(spec.encoding.size());
  const uint32_t fdSelectAt = at;
  at += static_cast<uint32_t>(fdSelect.size());
  const uint32_t charStringsAt = at;
  at += static_cast<uint32_t>(charStrings.size());
  const uint32_t fdArrayAt = at;
  if (spec.cid) {
    std::vector<Bytes> sized;
    for (size_t i = 0; i < spec.fds.size(); ++i) sized.push_back(fontDict(i, 0));
    at += static_cast<uint32_t>(cffIndex(sized).size());
  }
  std::vector<uint32_t> privateAt;
  for (const Bytes& priv : privates) {
    privateAt.push_back(at);
    at += static_cast<uint32_t>(priv.size());
  }

  Bytes out = spec.header;
  const auto append = [&out](const Bytes& part) { out.insert(out.end(), part.begin(), part.end()); };
  append(nameIndex);
  append(cffIndex(
      {topDict(charsetAt, encodingAt, charStringsAt, fdArrayAt, fdSelectAt, privateAt.empty() ? 0 : privateAt[0])}));
  append(strings);
  append(globalSubrs);
  append(charset);
  append(spec.encoding);
  append(fdSelect);
  append(charStrings);
  if (spec.cid) {
    std::vector<Bytes> dicts;
    for (size_t i = 0; i < spec.fds.size(); ++i) dicts.push_back(fontDict(i, privateAt[i]));
    append(cffIndex(dicts));
  }
  for (const Bytes& priv : privates) append(priv);
  return out;
}

/// An OpenType font around `cff`: sfnt 'OTTO', with the TrueType builder's head, hhea, hmtx, maxp and cmap (its empty
/// glyf and loca are left in; nothing reads them in a CFF font).
inline Bytes buildOtf(const Bytes& cff, const uint32_t glyphs) {
  Font font;
  font.sfntVersion = tagOf("OTTO");
  font.glyphs.resize(glyphs);
  font.cmap = {{'A', 1}};
  font.extraTables = {{"CFF ", cff}};
  return buildFont(font);
}

inline Bytes buildOtf(const CffSpec& spec) {
  return buildOtf(buildCff(spec), static_cast<uint32_t>(spec.charStrings.size()));
}

}  // namespace ttf_test
