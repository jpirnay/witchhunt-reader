#include "TtfFamily.h"

#include <Logging.h>
#include <Memory.h>
#include <OtPlan.h>

#include <algorithm>
#include <cstring>

#include "TtfFace.h"

namespace ttf {
namespace {

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 |
         p[3];
}
constexpr uint32_t tag(const char (&t)[5]) {
  return static_cast<uint32_t>(static_cast<uint8_t>(t[0])) << 24 |
         static_cast<uint32_t>(static_cast<uint8_t>(t[1])) << 16 |
         static_cast<uint32_t>(static_cast<uint8_t>(t[2])) << 8 | static_cast<uint8_t>(t[3]);
}

// The tables parseFace() needs; a file without one of them can never draw a glyph. The first five serve both outline
// kinds; glyf and loca are TrueType's, "CFF " an OpenType CFF font's.
constexpr uint32_t kRequired[] = {tag("cmap"), tag("head"), tag("hhea"), tag("hmtx"),
                                  tag("maxp"), tag("glyf"), tag("loca"), tag("CFF ")};
constexpr uint32_t kRequiredShared = 0x1F;
constexpr uint32_t kRequiredTrueType = kRequiredShared | 1u << 5 | 1u << 6;
constexpr uint32_t kRequiredCff = kRequiredShared | 1u << 7;
constexpr uint32_t kVersionCff = tag("OTTO");
// What an SD-mode block lists: every table the engine reads (parseFace(), the layout readers), all copied but the
// outline table, which the block points past (glyf) or rebuilds (CFF). A font cannot draw without the tables before
// `firstOptional`; the others it draws without, as parseFace() takes them absent when their record lies outside the
// file.
struct SdTables {
  uint32_t tags[13];
  uint8_t count;
  uint8_t outlines;
  uint8_t firstOptional;
};
constexpr SdTables kSdTrueType = {
    {tag("cmap"), tag("head"), tag("hhea"), tag("hmtx"), tag("loca"), tag("maxp"), tag("glyf"), tag("OS/2"),
     tag("GPOS"), tag("GSUB"), tag("kern"), tag("post"), tag("GDEF")},
    13,
    6,
    7};
constexpr SdTables kSdCff = {{tag("cmap"), tag("head"), tag("hhea"), tag("hmtx"), tag("maxp"), tag("CFF "), tag("OS/2"),
                              tag("GPOS"), tag("GSUB"), tag("kern"), tag("post"), tag("GDEF")},
                             12,
                             5,
                             6};
// The made table that holds the lookup filters (ot::FilterTableWriter's): not in the file, so in neither list above.
constexpr uint32_t kCpacTag = tag("CPac");

// Which of `tables` a record is, or -1, and whether it is its tag's first record: parseFace() reads only that one.
// `seen` remembers the tags met.
int sdTable(const SdTables& tables, const uint32_t tableTag, uint16_t& seen) {
  for (int k = 0; k < tables.count; ++k) {
    if (tableTag != tables.tags[k]) continue;
    const bool first = (seen >> k & 1u) == 0;
    seen |= static_cast<uint16_t>(1u << k);
    return first ? k : -1;
  }
  return -1;
}
// What SD mode copies of a table: all of it, but post's header alone (kPostHeaderBytes); the glyph names after it, tens
// of KB in a TrueType font, are never read.
uint32_t sdCopyBytes(const uint32_t tableTag, const uint32_t length) {
  return tableTag == tag("post") ? std::min(length, kPostHeaderBytes) : length;
}
// An SD-mode block's bytes: the sfnt header, a record per copied table, one for glyf and one for a CPac of
// `cpacBytes` (0: none), then the tables and the CPac room.
constexpr uint64_t sdBlockBytes(const uint32_t copied, const uint64_t tableBytes, const uint32_t cpacBytes) {
  return 12 + 16 * (copied + 1 + (cpacBytes > 0 ? 1 : 0)) + tableBytes + glyphstore::align4(cpacBytes);
}
constexpr uint16_t kMaxDirectory = 256;  // real fonts have under 40 tables; this bounds a hostile directory's reads
constexpr uint32_t kHeadBytes = 54;
constexpr uint32_t kHeadMagic = 0x5F0F3CF5;
constexpr uint32_t kMacStyleOffset = 44;
constexpr uint32_t kWeightClassOffset = 4;  // OS/2 usWeightClass

void putLe(uint8_t* out, const uint32_t value, const int bytes) {
  for (int i = 0; i < bytes; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

// A table directory record as the file has it.
struct Record {
  uint32_t tag;
  uint32_t checksum;
  uint32_t offset;
  uint32_t length;
};

// What forEachTable() found: every record visited, a file it refuses, or a read that failed.
enum class Walk : uint8_t { Done, Refused, ReadFailed };

// Calls `visit(ctx, record)` for each record of `file`'s table directory, read eight records at a time. Refused when
// the file is not an sfnt of TrueType or CFF outlines (a collection 'ttcf'), its directory does not fit it, or `visit`
// returns false. `version` gets the sfnt version. A function, not a template: the registry's probe and SD mode's
// block share one copy.
Walk forEachTable(const FileSource& file, uint32_t* version, bool (*visit)(void* ctx, const Record& record),
                  void* ctx) {
  uint8_t header[12];
  if (!file.read || file.size < sizeof header) return Walk::Refused;
  if (!file.read(file.ctx, 0, header, sizeof header)) return Walk::ReadFailed;
  *version = be32(header);
  if (*version != 0x00010000 && *version != tag("true") && *version != kVersionCff) return Walk::Refused;
  const uint16_t numTables = be16(header + 4);
  if (numTables == 0 || numTables > kMaxDirectory || sizeof header + 16u * numTables > file.size) return Walk::Refused;
  uint8_t records[8 * 16];
  for (uint16_t first = 0; first < numTables; first += 8) {
    const uint16_t n = std::min<uint16_t>(8, static_cast<uint16_t>(numTables - first));
    if (!file.read(file.ctx, sizeof header + 16u * first, records, 16u * n)) return Walk::ReadFailed;
    for (uint16_t i = 0; i < n; ++i) {
      const uint8_t* raw = records + 16 * i;
      if (!visit(ctx, {be32(raw), be32(raw + 4), be32(raw + 8), be32(raw + 12)})) return Walk::Refused;
    }
  }
  return Walk::Done;
}

bool inFile(const FileSource& file, const uint32_t offset, const uint32_t length) {
  return offset <= file.size && length <= file.size - offset;
}

}  // namespace

namespace {

// What familyIdentity() adds for a file that kerns by GDEF class.
constexpr uint8_t kGdefKerningMarker = 'G';

// LookupFlag bits the GPOS reader skips glyphs by (Gpos.cpp): IgnoreBaseGlyphs, IgnoreLigatures, IgnoreMarks,
// UseMarkFilteringSet and MarkAttachmentType.
constexpr uint16_t kSkippingFlags = 0x0002 | 0x0004 | 0x0008 | 0x0010 | 0xFF00;

}  // namespace

bool isOutlineFontFileName(const char* name) {
  const size_t len = std::strlen(name);
  if (len <= 4 || name[len - 4] != '.') return false;
  const char* ext = name + len - 3;
  const int first = ext[0] | 0x20;
  return (first == 't' || first == 'o') && (ext[1] | 0x20) == 't' && (ext[2] | 0x20) == 'f';
}

namespace {

// A GSUB's or GPOS's header: u16 major and minor version, then the u16 offsets of its ScriptList (4), FeatureList (6)
// and LookupList (8).
constexpr uint32_t kLayoutHeaderBytes = 10;

// What walkLookupHeaders() ended with: every header visited, `visit` stopping it, the read budget spent, or a read that
// failed.
enum class HeaderWalk : uint8_t { Done, Stopped, OverBudget, ReadFailed };

// Calls `visit(ctx, at, header)` with the 6-byte header (type, flag, subtable count) of each of the `lookups` lookups
// of the LookupList at `listAt` in the table at `offset`, `length` bytes, `at` its offset in the list: in pieces of 64
// offsets, each piece in offset order, one read serving every header within 128 bytes of its first. A null or
// out-of-list offset, or one with no room for a subtable, is no header. `reads` counts read calls (those before the
// walk included); past `budget` the walk stops (OverBudget). `visit` returns false to stop (Stopped).
HeaderWalk walkLookupHeaders(const FileSource& file, const uint32_t offset, const uint32_t length,
                             const uint32_t listAt, const uint32_t lookups, const uint32_t budget, uint32_t& reads,
                             bool (*visit)(void* ctx, uint32_t at, const uint8_t* header), void* ctx) {
  const uint32_t listLength = length - listAt;
  const uint32_t listed = std::min(lookups, (listLength - 2) / 2);
  // One buffer, two uses: a piece of the list's offsets, decoded into `offsets` before anything else is read into it,
  // then the window of lookup headers. 256 B of buffers in all.
  uint8_t raw[128];        // 128 B: 64 of the list's offsets, then the lookup headers read so far
  uint16_t offsets[64];    // 128 B: the piece, decoded and sorted
  uint32_t windowAt = 0;   // the list offset raw[0] holds as a window
  uint32_t windowEnd = 0;  // the list offset past the window's last byte (== windowAt: no window held)
  for (uint32_t first = 0; first < listed; first += 64) {
    const uint32_t n = std::min<uint32_t>(64, listed - first);
    if (++reads > budget) break;
    if (!file.read(file.ctx, offset + listAt + 2 + 2 * first, raw, 2 * n)) return HeaderWalk::ReadFailed;
    for (uint32_t i = 0; i < n; ++i) offsets[i] = be16(raw + 2 * i);
    windowEnd = windowAt;  // the piece took the window's bytes
    // In offset order, so lookups packed one after another share a window.
    std::sort(offsets, offsets + n);
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t at = offsets[i];
      if (at == 0 || at >= listLength || listLength - at < 8) continue;  // under 8 bytes, no subtable fits
      if (at < windowAt || at + 6 > windowEnd) {
        // A window from this header on, as much as the list holds (at least the 8 bytes checked above).
        const uint32_t len = std::min<uint32_t>(sizeof raw, listLength - at);
        if (++reads > budget) break;
        if (!file.read(file.ctx, offset + listAt + at, raw, len)) return HeaderWalk::ReadFailed;
        windowAt = at;
        windowEnd = at + len;
      }
      if (!visit(ctx, at, raw + (at - windowAt))) return HeaderWalk::Stopped;
    }
    if (reads > budget) break;
  }
  return reads > budget ? HeaderWalk::OverBudget : HeaderWalk::Done;
}

// What kernsByGdefClass() looks for among a GPOS's lookup headers: a pair adjustment lookup (type 2, or an Extension,
// type 9, wrapping one) whose flags skip glyphs by GDEF class, with a first subtable the reader would read. Its own
// reads (a first subtable's offset, an Extension's header) count against the walk's budget.
struct FlagScan {
  const FileSource& file;
  uint32_t listFrom;  // where the LookupList lies in the file
  uint32_t listLength;
  uint32_t budget;
  uint32_t& reads;
  bool found = false;
  bool failed = false;
  bool over = false;

  // The `n` bytes at `at` of the list; false past the list, past the budget (`over`) or when the read failed.
  bool read(const uint32_t at, uint8_t* dst, const uint32_t n) {
    if (at > listLength || n > listLength - at) return false;
    if (++reads > budget) {
      over = true;
      return false;
    }
    failed = !file.read(file.ctx, listFrom + at, dst, n);
    return !failed;
  }
};

bool skipsByClass(void* ctx, const uint32_t at, const uint8_t* header) {
  auto* scan = static_cast<FlagScan*>(ctx);
  const uint16_t type = be16(header);
  if ((type != 2 && type != 9) || (be16(header + 2) & kSkippingFlags) == 0 || be16(header + 4) == 0) return true;
  uint8_t first[2];
  if (!scan->read(at + 6, first, sizeof first)) return !scan->failed && !scan->over;
  const uint32_t subtable = at + be16(first);
  if (be16(first) == 0 || subtable >= scan->listLength) return true;
  if (type == 2) {
    scan->found = true;
    return false;
  }
  // Extension format 1 wrapping a pair adjustment, at a 32-bit offset.
  uint8_t extension[8];
  if (!scan->read(subtable, extension, sizeof extension)) return !scan->failed && !scan->over;
  const uint64_t wrapped = uint64_t{subtable} + be32(extension + 4);
  scan->found =
      be16(extension) == 1 && be16(extension + 2) == 2 && be32(extension + 4) != 0 && wrapped < scan->listLength;
  return !scan->found;
}

// countLookups() of a table whose header (`header`, kLayoutHeaderBytes) was read already.
bool countLookupsAfter(const FileSource& file, const uint32_t offset, const uint32_t length,
                       const uint8_t (&header)[kLayoutHeaderBytes], LayoutCounts* out) {
  *out = {};
  // As Face reads the table: a major version of 1, the LookupList at u16 offset 8, each lookup at a u16 offset of the
  // list; a null or out-of-table offset is a lookup of no subtables, and a lookup declares at most the subtables that
  // fit its table. The lookups are as many as the list declares, whatever fits.
  const uint32_t listAt = be16(header + 8);
  if (be16(header) != 1 || listAt == 0 || listAt >= length) return true;
  const uint32_t listLength = length - listAt;
  if (listLength < 2) return true;
  uint8_t countBytes[2];
  if (!file.read(file.ctx, offset + listAt, countBytes, sizeof countBytes)) return false;
  const uint32_t lookups = be16(countBytes);
  if (lookups > kCpacMaxLookups) {
    out->capped = true;
    return true;
  }
  out->lookups = lookups;
  // Read calls so far: the two above, then a piece of offsets or a window of headers each. Past kMaxCountingReads the
  // list is crafted (a real one packs its lookups, a window serving a dozen or more): the table counts as capped, so
  // its font reserves no CPac, which costs only the computed filters (slower, never wrong).
  uint32_t reads = 2;
  struct Sum {
    uint32_t listLength;
    uint64_t subtables;
  } sum{listLength, 0};
  const auto add = [](void* ctx, const uint32_t at, const uint8_t* lookupHeader) {
    auto* total = static_cast<Sum*>(ctx);
    total->subtables += std::min<uint32_t>(be16(lookupHeader + 4), (total->listLength - at - 6) / 2);
    // Past the cap the rest need not be read.
    return total->subtables <= kCpacMaxSubtables;
  };
  switch (walkLookupHeaders(file, offset, length, listAt, lookups, kMaxCountingReads, reads, add, &sum)) {
    case HeaderWalk::ReadFailed:
      return false;
    case HeaderWalk::Stopped:
      out->capped = true;
      return true;
    case HeaderWalk::OverBudget:
      *out = {};
      out->capped = true;
      return true;
    case HeaderWalk::Done:
      break;
  }
  out->subtables = static_cast<uint32_t>(sum.subtables);
  return true;
}

}  // namespace

bool countLookups(const FileSource& file, const uint32_t offset, const uint32_t length, LayoutCounts* out) {
  *out = {};
  if (!inFile(file, offset, length) || length < kLayoutHeaderBytes) return true;  // a table Face does not read
  uint8_t header[kLayoutHeaderBytes];
  if (!file.read(file.ctx, offset, header, sizeof header)) return false;
  return countLookupsAfter(file, offset, length, header, out);
}

bool kernsByGdefClass(const FileSource& file, bool* kerns) {
  *kerns = false;
  struct Tables {
    Record gpos{};
    Record gdef{};
    bool seenGpos = false;
    bool seenGdef = false;
  } tables;
  uint32_t version = 0;
  const auto visit = [](void* ctx, const Record& record) {
    auto* found = static_cast<Tables*>(ctx);
    // The first record of a tag, the one parseFace() reads.
    if (record.tag == tag("GPOS") && !found->seenGpos) {
      found->gpos = record;
      found->seenGpos = true;
    } else if (record.tag == tag("GDEF") && !found->seenGdef) {
      found->gdef = record;
      found->seenGdef = true;
    }
    return true;
  };
  const Walk walk = forEachTable(file, &version, visit, &tables);
  if (walk == Walk::ReadFailed) return false;
  // As parseFace() takes the tables: in the file, with room for their headers; GDEF as the reader takes it, major
  // version 1. A font without either kerns as before.
  if (walk == Walk::Refused || !tables.seenGpos || !tables.seenGdef ||
      !inFile(file, tables.gpos.offset, tables.gpos.length) || tables.gpos.length < 10 ||
      !inFile(file, tables.gdef.offset, tables.gdef.length) || tables.gdef.length < 12) {
    return true;
  }
  uint8_t major[2];
  if (!file.read(file.ctx, tables.gdef.offset, major, sizeof major)) return false;
  if (be16(major) != 1) return true;
  // GPOS's LookupList, as the reader takes it: at u16 offset 8, whatever the version.
  const uint32_t at = tables.gpos.offset;
  const uint32_t length = tables.gpos.length;
  uint8_t header[kLayoutHeaderBytes];
  if (!file.read(file.ctx, at, header, sizeof header)) return false;
  const uint32_t listAt = be16(header + 8);
  if (listAt == 0 || listAt >= length || length - listAt < 2) return true;
  uint8_t count[2];
  if (!file.read(file.ctx, at + listAt, count, sizeof count)) return false;
  uint32_t reads = 2;
  FlagScan scan{file, at + listAt, length - listAt, kMaxKernFlagReads, reads};
  const HeaderWalk headers =
      walkLookupHeaders(file, at, length, listAt, be16(count), kMaxKernFlagReads, reads, skipsByClass, &scan);
  if (headers == HeaderWalk::ReadFailed || scan.failed) return false;
  // Past the budget: kerning by class, an extra install rather than a stale pair.
  *kerns = scan.found || scan.over || headers == HeaderWalk::OverBudget;
  return true;
}

namespace {

// The first read of a GSUB or GPOS takes this much of its start: the header and, where fonts put it, the ScriptList
// right behind it (its count and 19 records), so a font's scripts usually cost one read a table.
constexpr uint32_t kScriptWindowBytes = 128;
// Reads of a ScriptList past the first, 21 records each: 187 records in all behind the header, 168 elsewhere, more
// scripts than OpenType has. A longer list names none here, which costs its font only the computed filters (slower,
// never wrong).
constexpr uint32_t kMaxScriptReads = 8;

// A script whose fonts get a CPac: one the shaper handles (ot::isShapedScriptTag(): the Indic tags, old and new, and
// Sinhala's), and Arabic, whose joining shaper comes next (deviation 11), so its fonts need no second install then.
bool isShapedTag(const uint32_t scriptTag) { return ot::isShapedScriptTag(scriptTag) || scriptTag == tag("arab"); }

// Whether the ScriptList of the GSUB or GPOS table at `offset`, `length` bytes, names a script whose fonts get a CPac
// (isShapedTag()). Read as Face reads it: a major version of 1, the list at u16 offset 4, its u16 count, then 6-byte
// records of a tag and an offset, as many as fit the table. The first read takes the table's first kScriptWindowBytes,
// so `header` gets the table's header from it too. A table Face does not read names none. False: a read failed.
bool namesShapedScript(const FileSource& file, const uint32_t offset, const uint32_t length,
                       uint8_t (&header)[kLayoutHeaderBytes], bool* names) {
  *names = false;
  uint8_t window[kScriptWindowBytes];  // 128 B: the table's bytes [windowAt, windowEnd)
  uint32_t windowAt = 0;
  uint32_t windowEnd = std::min(kScriptWindowBytes, length);
  if (!file.read(file.ctx, offset, window, windowEnd)) return false;
  std::memcpy(header, window, sizeof header);
  const uint32_t listAt = be16(window + 4);
  if (be16(window) != 1 || listAt == 0 || listAt >= length || length - listAt < 2) return true;
  // The `n` bytes at `at` of the table, from the window, else from a window read from `at` on; nullptr past the read
  // budget or when a read failed (`failed`).
  bool failed = false;
  uint32_t reads = 0;
  const auto bytesAt = [&](const uint32_t at, const uint32_t n) -> const uint8_t* {
    if (at < windowAt || at + n > windowEnd) {
      if (++reads > kMaxScriptReads) return nullptr;
      windowAt = at;
      windowEnd = at + std::min(kScriptWindowBytes, length - at);
      if (!file.read(file.ctx, offset + at, window, windowEnd - windowAt)) {
        failed = true;
        return nullptr;
      }
    }
    return window + (at - windowAt);
  };
  const uint8_t* count = bytesAt(listAt, 2);
  if (!count) return !failed;
  const uint32_t scripts = std::min<uint32_t>(be16(count), (length - listAt - 2) / 6);
  for (uint32_t i = 0; i < scripts; ++i) {
    const uint8_t* scriptTag = bytesAt(listAt + 2 + 6 * i, 4);
    if (!scriptTag) return !failed;
    if (isShapedTag(be32(scriptTag))) {
      *names = true;
      return true;
    }
  }
  return true;
}

// The first GSUB and the first GPOS record of a directory, the ones parseFace() and SdBlock read.
struct LayoutRecords {
  Record record[2] = {};
  bool seen[2] = {false, false};

  void note(const Record& found) {
    for (int k = 0; k < 2; ++k) {
      if (found.tag != (k == 0 ? tag("GSUB") : tag("GPOS")) || seen[k]) continue;
      seen[k] = true;
      record[k] = found;
    }
  }
  // Table k (GSUB, GPOS) as Face could read it: present, in the file, with room for its header.
  bool readable(const FileSource& file, const int k) const {
    return seen[k] && inFile(file, record[k].offset, record[k].length) && record[k].length >= kLayoutHeaderBytes;
  }
  // The CPac room of the two tables: 0 unless one of them names a script whose fonts get a CPac (namesShapedScript()),
  // for no other font shapes from its filters. The lookups are counted only then. False: a read failed.
  bool cpacBytes(const FileSource& file, uint32_t* bytes) const {
    *bytes = 0;
    uint8_t headers[2][kLayoutHeaderBytes] = {};
    bool headerRead[2] = {false, false};
    bool shapes = false;
    for (int k = 0; k < 2 && !shapes; ++k) {
      if (!readable(file, k)) continue;
      if (!namesShapedScript(file, record[k].offset, record[k].length, headers[k], &shapes)) return false;
      headerRead[k] = true;
    }
    if (!shapes) return true;
    LayoutCounts counts[2];
    for (int k = 0; k < 2; ++k) {
      if (!readable(file, k)) continue;
      if (!headerRead[k] && !file.read(file.ctx, record[k].offset, headers[k], kLayoutHeaderBytes)) return false;
      if (!countLookupsAfter(file, record[k].offset, record[k].length, headers[k], &counts[k])) return false;
    }
    *bytes = cpacBytesFor(counts[0], counts[1]);
    return true;
  }
};

}  // namespace

bool filterTableBytes(const FileSource& file, uint32_t* bytes) {
  LayoutRecords layout;
  uint32_t version = 0;
  const auto visit = [](void* ctx, const Record& record) {
    static_cast<LayoutRecords*>(ctx)->note(record);
    return true;
  };
  *bytes = 0;
  const Walk walk = forEachTable(file, &version, visit, &layout);
  if (walk == Walk::ReadFailed) return false;
  if (walk == Walk::Refused) return true;
  return layout.cpacBytes(file, bytes);
}

bool assignStyles(const FileProbe* files, const size_t count, int (&out)[4]) {
  // Lower is better: a variable font ranks after every static one, then the distance from the style's weight, then
  // a file declaring the style before one that only weighs as it (below).
  const auto rank = [&](const size_t i, const int style) {
    const int target = (style & 1) ? 700 : 400;
    const int distance = files[i].weight > target ? files[i].weight - target : target - files[i].weight;
    return (files[i].variable ? 0x10000 : 0) + 2 * distance + (files[i].style != style ? 1 : 0);
  };
  const auto offer = [&](const size_t i, const int style) {
    if (out[style] < 0 || rank(i, style) < rank(static_cast<size_t>(out[style]), style)) {
      out[style] = static_cast<int>(i);
    }
  };
  for (int& file : out) file = -1;
  for (size_t i = 0; i < count; ++i) {
    if (files[i].style >= 0 && files[i].style <= 3) offer(i, files[i].style);
  }
  // A bold weight whose macStyle says regular, as some exports mark a Bold only by its OS/2 weight class (Inter's
  // static Inter-Bold.ttf, embedded book fonts made from it), may serve the bold style too: not when it already
  // serves the regular one, since a single heavy face is a regular with no bold.
  for (size_t i = 0; i < count; ++i) {
    const int style = files[i].style;
    if ((style == 0 || style == 2) && files[i].weight >= 600 && out[style] != static_cast<int>(i)) offer(i, style | 1);
  }
  // No regular face (a script that is only italic, a display face only bold, as books embed them): the nearest style
  // serves as the regular, and the slot it leaves falls back to it. Moved, not shared, so the store holds it once.
  for (const int style : {2, 1, 3}) {
    if (out[0] >= 0) break;
    out[0] = out[style];
    out[style] = -1;
  }
  return out[0] >= 0;
}

bool fitsStore(const uint32_t* fileBytes, const uint32_t* sdBytes, const uint8_t count, const uint32_t usableBytes,
               const uint32_t freeMapPages) {
  if (count == 0 || count > glyphstore::kMaxStyles) return false;
  glyphstore::PlanInput in{};
  in.styleCount = count;
  in.usableBytes = usableBytes;
  in.freeMapPages = freeMapPages;
  glyphstore::Layout layout{};
  for (const uint8_t mode : {glyphstore::kModeFlash, glyphstore::kModeSd}) {
    if (mode == glyphstore::kModeSd) {
      // A style without an SD-mode block (UINT32_MAX, a CFF file that can have none) cannot be mapped from the card.
      // One whose block the probe could not plan for now (kSdBytesUnknown) leaves the family listed: its load plans
      // the blocks again, and refuses the family if they do not fit. Neither size may reach pagesFor(): it would wrap.
      if (std::find(sdBytes, sdBytes + count, UINT32_MAX) != sdBytes + count) continue;
      if (std::find(sdBytes, sdBytes + count, kSdBytesUnknown) != sdBytes + count) return true;
    }
    for (uint8_t s = 0; s < count; ++s) {
      in.styleMode[s] = mode;
      in.styleBytes[s] = mode == glyphstore::kModeFlash ? fileBytes[s] : sdBytes[s];
    }
    if (glyphstore::planLayout(in, layout)) return true;
  }
  return false;
}

namespace {

// Whether parseFace() reads `file` as the load reads a style, through a page cache on the heap for the call (CardFace,
// about 4.5 KB). True without that memory: the file stays listed and its load decides, as before the probe asked. A
// read that fails is as any other of the probe's: not a font for now.
bool faceParses(const FileSource& file) {
  const auto work = makeUniqueNoThrow<CardFace>(file);
  if (!work) {
    LOG_ERR("TTF", "probe: no %u B to read the face with; listed for its load to decide",
            static_cast<unsigned>(sizeof(CardFace)));
    return true;
  }
  return parseFace(work->file.bytes, &work->face) == FaceStatus::Ok;
}

}  // namespace

FileProbe probeFile(const FileSource& file) {
  struct Scan {
    const FileSource& file;
    FileProbe probe;
    uint32_t version = 0;  ///< the sfnt version, known before the first record
    uint32_t found = 0;
    uint32_t headOffset = 0;
    uint32_t os2Offset = 0;
    bool hasOs2Weight = false;
    bool cff2 = false;
    uint16_t sdSeen = 0;
    uint32_t sdTables = 0;
    uint64_t sdTableBytes = 0;
    LayoutRecords layout;  ///< GSUB's and GPOS's records, counted once the head is checked
  } scan{file};
  const auto visit = [](void* ctx, const Record& record) {
    Scan& scan = *static_cast<Scan*>(ctx);
    const uint32_t needed = scan.version == kVersionCff ? kRequiredCff : kRequiredTrueType;
    if (record.tag == tag("fvar")) scan.probe.variable = true;
    scan.layout.note(record);
    const int sd = sdTable(kSdTrueType, record.tag, scan.sdSeen);
    if (sd >= 0 && sd != kSdTrueType.outlines && inFile(scan.file, record.offset, record.length)) {
      ++scan.sdTables;
      scan.sdTableBytes += glyphstore::align4(sdCopyBytes(record.tag, record.length));
    }
    // parseFace() refuses a CFF2 record that is not empty and lies in the file (as FreeType reads it before any CFF
    // table); one it skips leaves the font to its CFF table.
    if (record.tag == tag("CFF2") && record.length > 0 && inFile(scan.file, record.offset, record.length)) {
      scan.cff2 = true;
    }
    if (record.tag == tag("OS/2") && inFile(scan.file, record.offset, record.length) && record.length >= 6) {
      scan.os2Offset = record.offset;
      scan.hasOs2Weight = true;
    }
    for (uint32_t k = 0; k < sizeof kRequired / sizeof kRequired[0]; ++k) {
      if (record.tag != kRequired[k]) continue;
      // The other kind's tables are not read: parseFace() never looks up a record of the other kind's tags, inside the
      // file or outside it.
      if (!(needed >> k & 1u)) continue;
      if (!inFile(scan.file, record.offset, record.length)) return false;
      if (record.tag == tag("head")) {
        if (record.length < kHeadBytes) return false;
        scan.headOffset = record.offset;
      }
      scan.found |= 1u << k;
    }
    return true;
  };
  if (forEachTable(file, &scan.version, visit, &scan) != Walk::Done) return {};
  const bool cff = scan.version == kVersionCff;
  const uint32_t needed = cff ? kRequiredCff : kRequiredTrueType;
  if ((scan.found & needed) != needed || (cff && scan.cff2)) return {};
  FileProbe& probe = scan.probe;
  const uint32_t headOffset = scan.headOffset;
  uint8_t head[kHeadBytes];
  if (!file.read(file.ctx, headOffset, head, sizeof head) || be32(head + 12) != kHeadMagic) return {};
  // Counted after the head check, so a file refused for it pays nothing; a read failing here is as any other of the
  // probe's: not a font for now.
  if (!scan.layout.cpacBytes(file, &probe.cpacBytes)) return {};
  probe.flashBytes = flashBlockBytes(file.size, probe.cpacBytes);
  if (!cff) {
    // As SdBlock::plan() lays it out, by the same rule (sdTable()).
    const uint64_t sdBytes = sdBlockBytes(scan.sdTables, scan.sdTableBytes, probe.cpacBytes);
    probe.sdBytes = sdBytes < kSdBytesUnknown ? static_cast<uint32_t>(sdBytes) : UINT32_MAX;
  }
  probe.style = be16(head + kMacStyleOffset) & 3;
  probe.weight = (probe.style & 1) ? 700 : 400;
  uint8_t weight[2];
  if (scan.hasOs2Weight && file.read(file.ctx, scan.os2Offset + kWeightClassOffset, weight, sizeof weight) &&
      be16(weight) != 0) {
    probe.weight = be16(weight);
  }
  bool parsed = false;
  if (cff) {
    // The rebuilt CFF table's size takes reading the table: the block planned as an install plans it. Without the
    // memory, or with a read failing, its size is not known for now (fitsStore() then lists the family).
    const auto block = makeUniqueNoThrow<SdBlock>();
    const SdPlan planned = block ? block->plan(file, &probe.cpacBytes) : SdPlan::Transient;
    probe.sdBytes = planned == SdPlan::Ok ? block->bytes() : planned == SdPlan::NoBlock ? UINT32_MAX : kSdBytesUnknown;
    parsed = planned == SdPlan::Ok;  // CffBlock::plan() read the face with parseFace()
  }
  // The load reads every style with parseFace() and leaves out one it refuses: a family of such files was installed
  // at every pick, then fell back. Listed only what parseFace() reads.
  if (!parsed && !faceParses(file)) return {};
  return probe;
}

// --- SD mode's style block ---

SdPlan SdBlock::plan(const FileSource& file, const uint32_t* knownCpacBytes) {
  file_ = file;
  seen_ = 0;
  count_ = 0;
  bytes_ = 0;
  outlinesAt_ = 0;
  outlinesLength_ = 0;
  cpacAt_ = 0;
  cpacBytes_ = 0;
  const auto visit = [](void* ctx, const Record& record) { return static_cast<SdBlock*>(ctx)->addTable(&record); };
  const Walk walk = forEachTable(file, &version_, visit, this);
  if (walk != Walk::Done) return walk == Walk::ReadFailed ? SdPlan::Transient : SdPlan::NoBlock;
  const bool cff = version_ == kVersionCff;
  if (cff) {
    const SdPlan table = cff_.plan(file);
    if (table != SdPlan::Ok) return table;
  }
  uint32_t cpac = 0;
  if (knownCpacBytes) {
    cpac = *knownCpacBytes;
  } else if (!filterTableBytes(file, &cpac)) {
    return SdPlan::Transient;
  }
  const SdTables& kept = cff ? kSdCff : kSdTrueType;
  // The directory (a record more for the CPac), then the tables, each 4-byte aligned, the CPac room after the last,
  // the outline table last: glyf past them all (sdBlockBytes()), the rebuilt CFF table ending the block, its
  // charstring data past it.
  uint64_t end = 12 + 16u * (count_ + (cpac > 0 ? 1 : 0));
  int outlines = -1;
  for (uint8_t i = 0; i < count_; ++i) {
    if (tables_[i].tag == kept.tags[kept.outlines]) {
      outlines = i;
      continue;
    }
    tables_[i].blockOffset = static_cast<uint32_t>(end);
    end += glyphstore::align4(tables_[i].length);
    // The block and the outlines past it stay in 32 bits, whatever the tables.
    if (end > UINT32_MAX - file.size) return SdPlan::NoBlock;
  }
  if (outlines < 0) return SdPlan::NoBlock;
  if (cpac > 0) {
    cpacAt_ = static_cast<uint32_t>(end);
    cpacBytes_ = cpac;
    end += glyphstore::align4(cpac);
    if (end > UINT32_MAX - file.size) return SdPlan::NoBlock;
    // A made table: nothing of the file, its checksum the install's to leave 0.
    tables_[count_++] = {kCpacTag, 0, 0, cpac, cpacAt_};
  }
  Table& table = tables_[outlines];
  table.blockOffset = static_cast<uint32_t>(end);
  if (cff) {
    end += cff_.bytes();
    if (end > UINT32_MAX - file.size) return SdPlan::NoBlock;
    table.length = cff_.bytes() + cff_.dataLength();
    outlinesAt_ = cff_.dataOffset();
    outlinesLength_ = cff_.dataLength();
  } else {
    outlinesAt_ = table.fileOffset;
    outlinesLength_ = table.length;
  }
  bytes_ = static_cast<uint32_t>(end);
  return SdPlan::Ok;
}

// A directory record (a Record): kept when SD mode copies its table, or it is the outline table.
bool SdBlock::addTable(const void* record) {
  const Record& table = *static_cast<const Record*>(record);
  const SdTables& kept = version_ == kVersionCff ? kSdCff : kSdTrueType;
  const int k = sdTable(kept, table.tag, seen_);
  if (k < 0) return true;
  // A table the font needs outside the file: it cannot be drawn. An optional one: absent, as parseFace() takes it.
  if (!inFile(file_, table.offset, table.length)) return k >= kept.firstOptional;
  // In file order, so the install reads the file forward.
  uint8_t at = count_++;
  for (; at > 0 && tables_[at - 1].fileOffset > table.offset; --at) tables_[at] = tables_[at - 1];
  tables_[at] = {table.tag, table.checksum, table.offset, sdCopyBytes(table.tag, table.length), 0};
  return true;
}

// Word `index` of the block's table directory: the sfnt header (nothing reads its search fields, left 0), then a
// record per table.
uint32_t SdBlock::directoryWord(const uint32_t index) const {
  if (index == 0) return version_;
  if (index == 1) return static_cast<uint32_t>(count_) << 16;
  if (index == 2) return 0;
  const Table& table = tables_[(index - 3) / 4];
  switch ((index - 3) % 4) {
    case 0:
      return table.tag;
    case 1:
      return table.checksum;
    case 2:
      return table.blockOffset;
    default:
      return table.length;
  }
}

bool SdBlock::read(uint32_t offset, uint8_t* dst, uint32_t len) const {
  if (offset > bytes_ || len > bytes_ - offset) return false;
  const uint32_t directory = 12 + 16u * count_;
  while (len > 0) {
    uint32_t n = 1;
    if (offset < directory) {
      *dst = static_cast<uint8_t>(directoryWord(offset / 4) >> (24 - 8 * (offset % 4)));
    } else {
      // The table this byte is in (glyf, past the block, never is), else the padding before the next one.
      const Table* in = nullptr;
      uint32_t next = bytes_;
      for (uint8_t i = 0; i < count_; ++i) {
        const Table& table = tables_[i];
        if (offset >= table.blockOffset && offset - table.blockOffset < table.length) in = &table;
        if (table.blockOffset > offset && table.blockOffset < next) next = table.blockOffset;
      }
      if (in && in->tag == kCpacTag) {
        // The CPac room reads as erased flash: the install writes the table there.
        n = std::min(len, in->blockOffset + in->length - offset);
        std::memset(dst, 0xFF, n);
      } else if (in) {
        // The CFF table runs past the block (its charstring data), and is made, not copied.
        n = std::min(len, in->blockOffset + in->length - offset);
        const uint32_t at = offset - in->blockOffset;
        const bool rebuilt = version_ == kVersionCff && in->tag == tag("CFF ");
        if (!(rebuilt ? cff_.read(file_, at, dst, n) : file_.read(file_.ctx, in->fileOffset + at, dst, n))) {
          return false;
        }
      } else {
        n = std::min(len, next - offset);
        std::memset(dst, 0, n);
      }
    }
    dst += n;
    offset += n;
    len -= n;
  }
  return true;
}

bool SdBlock::sameAs(const uint8_t* block, const uint32_t size) const {
  if (block == nullptr || size != bytes_) return false;
  for (uint32_t word = 0; word < 3 + 4u * count_; ++word) {
    if (directoryWord(word) != be32(block + 4 * word)) return false;
  }
  return true;
}

bool SdBlock::readSource(void* ctx, const uint32_t offset, void* dst, const uint32_t len) {
  return static_cast<const SdBlock*>(ctx)->read(offset, static_cast<uint8_t*>(dst), len);
}

void familyIdentity(const FileStamp (&styles)[4], uint8_t out[glyphstore::kIdentityBytes]) {
  for (uint32_t lane = 0; lane < glyphstore::kIdentityBytes / 4; ++lane) {
    uint32_t hash = 2166136261u ^ (lane * 0x9E3779B9u);
    for (uint8_t style = 0; style < 4; ++style) {
      const FileStamp& file = styles[style];
      const uint8_t marker = file.name ? style : static_cast<uint8_t>(0x80 | style);
      hash = glyphstore::fnv1a(&marker, 1, hash);
      if (!file.name) continue;
      hash = glyphstore::fnv1a(file.name, std::strlen(file.name) + 1, hash);
      uint8_t fields[8];
      putLe(fields, file.size, 4);
      putLe(fields + 4, file.date, 2);
      putLe(fields + 6, file.time, 2);
      hash = glyphstore::fnv1a(fields, sizeof fields, hash);
      // Only a file whose kerning changed adds it: every other family keeps the identity its store was installed under.
      if (file.kernsByGdefClass) hash = glyphstore::fnv1a(&kGdefKerningMarker, 1, hash);
    }
    hash = glyphstore::fnv1a(&kRenderVersion, 1, hash);
    putLe(out + 4 * lane, hash, 4);
  }
}

void setIdentity(const uint8_t (*families)[glyphstore::kIdentityBytes], const uint8_t count,
                 uint8_t out[glyphstore::kIdentityBytes]) {
  for (uint32_t lane = 0; lane < glyphstore::kIdentityBytes / 4; ++lane) {
    uint32_t hash = glyphstore::fnv1a(&count, 1, 2166136261u ^ (lane * 0x9E3779B9u));
    for (uint8_t family = 0; family < count; ++family) {
      hash = glyphstore::fnv1a(families[family], glyphstore::kIdentityBytes, hash);
    }
    putLe(out + 4 * lane, hash, 4);
  }
}

}  // namespace ttf
