#include "TtfCff.h"

#include "TtfFace.h"
#include "TtfFixed.h"
#include "TtfRasterizer.h"

namespace ttf {

namespace {

constexpr uint32_t kTopStack = 96;        // CFF_MAX_STACK_DEPTH: a Top or font DICT's operand stack (cff_subfont_load)
constexpr uint32_t kPrivateStack = 97;    // ... and a Private DICT's (cff_load_private_dict)
constexpr uint32_t kNoRegistry = 0xFFFF;  // cff_subfont_load's ROS registry when the DICT has none: not CID-keyed
constexpr uint32_t kMaxFontDicts = 256;   // CFF_MAX_CID_FONTS
constexpr uint32_t kMaxOperands = 6;      // the most any operator read here takes (FontMatrix)

constexpr int32_t kPowerTens[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

// cff_standard_encoding: StandardEncoding's code to SID, 16 codes a row. Every SID fits a byte.
// clang-format off
constexpr uint8_t kStandardEncoding[256] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,  16,
     17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,
     33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,  48,
     49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,
     65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,  80,
     81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,  96,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110,
      0, 111, 112, 113, 114,   0, 115, 116, 117, 118, 119, 120, 121, 122,   0, 123,
      0, 124, 125, 126, 127, 128, 129, 130, 131,   0, 132, 133,   0, 134, 135, 136,
    137,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0, 138,   0, 139,   0,   0,   0,   0, 140, 141, 142, 143,   0,   0,   0,   0,
      0, 144,   0,   0,   0, 145,   0,   0, 146, 147, 148, 149,   0,   0,   0,   0,
};
// clang-format on

// The predefined Expert and ExpertSubset charsets (cff_expert_charset, cff_expertsubset_charset) cut to what a
// StandardEncoding SID (at most 149) can find in them: these SIDs, at these glyphs.
constexpr uint8_t kExpertSids[] = {1, 13, 14, 15, 27, 28, 99, 109, 110};
constexpr uint8_t kExpertGlyphs[] = {1, 12, 13, 14, 26, 27, 15, 46, 47};
constexpr uint8_t kExpertSubsetGlyphs[] = {1, 8, 9, 10, 22, 23, 11, 41, 42};
// The most glyphs a font may have with each predefined charset (cff_charset_load): ISOAdobe, Expert, ExpertSubset.
constexpr uint32_t kPredefinedCharsetGlyphs[] = {229, 166, 87};

// --- Numbers ---

// FT_DivFix, FT_MulDiv and FT_MSB as FreeType computes them with a 64-bit FT_Long: the font matrix is worked out in
// these and must fit 32 bits when it is done.
int64_t divFix64(const int64_t a, const int64_t b) {
  const uint64_t ua = a < 0 ? 0 - static_cast<uint64_t>(a) : static_cast<uint64_t>(a);
  const uint64_t ub = b < 0 ? 0 - static_cast<uint64_t>(b) : static_cast<uint64_t>(b);
  const uint64_t q = ub > 0 ? ((ua << 16) + (ub >> 1)) / ub : 0x7FFFFFFF;
  return (a < 0) != (b < 0) ? static_cast<int64_t>(0 - q) : static_cast<int64_t>(q);
}

int64_t mulDiv64(const int64_t a, const int64_t b, const int64_t c) {
  const uint64_t ua = a < 0 ? 0 - static_cast<uint64_t>(a) : static_cast<uint64_t>(a);
  const uint64_t ub = b < 0 ? 0 - static_cast<uint64_t>(b) : static_cast<uint64_t>(b);
  const uint64_t uc = c < 0 ? 0 - static_cast<uint64_t>(c) : static_cast<uint64_t>(c);
  const uint64_t d = uc > 0 ? (ua * ub + (uc >> 1)) / uc : 0x7FFFFFFF;
  return ((a < 0) != (b < 0)) != (c < 0) ? static_cast<int64_t>(0 - d) : static_cast<int64_t>(d);
}

// ADD_LONG: a sum that wraps rather than overflows.
int64_t addWide(const int64_t a, const int64_t b) {
  return static_cast<int64_t>(static_cast<uint64_t>(a) + static_cast<uint64_t>(b));
}

int32_t msb(uint32_t z) {
  int32_t shift = 0;
  while (z >>= 1) ++shift;
  return shift;
}

int64_t absWide(const int64_t v) { return v < 0 ? -v : v; }

// --- DICTs ---

// cff_parse_integer: the integer at `at`, 0 when its bytes run past the DICT's `end`.
int32_t dictInteger(FontBytes& bytes, const uint32_t at, const uint32_t end) {
  const uint32_t v = bytes.u8(at);
  const uint32_t p = at + 1;  // at most `end`: `at` is inside the DICT
  if (v == 28) return end - p < 2 ? 0 : bytes.i16(p);
  if (v == 29) return end - p < 4 ? 0 : static_cast<int32_t>(bytes.u32(p));
  if (v < 247) return static_cast<int32_t>(v) - 139;
  if (end - p < 1) return 0;
  if (v < 251) return static_cast<int32_t>((v - 247) * 256 + bytes.u8(p) + 108);
  return -static_cast<int32_t>((v - 251) * 256 + bytes.u8(p) + 108);
}

// cff_parse_real: the BCD real at `at` (its 30 byte) in 16.16, rounded as FreeType rounds it. Without `scaling`, the
// value (0x7FFFFFFF when too large); with it, the most precise 16.16 value and *scaling the power of ten it is to
// be multiplied by (cff_parse_fixed_dynamic). 0 when the real runs past the DICT's `end`.
int32_t dictReal(FontBytes& bytes, const uint32_t at, const uint32_t end, int32_t* scaling) {
  if (scaling) *scaling = 0;
  uint32_t p = at;
  uint32_t phase = 4;
  uint32_t nib = 0;
  const auto next = [&]() {  // the next nibble, high one first; false past the DICT
    if (phase) {
      ++p;
      if (p >= end) return false;
    }
    nib = (bytes.u8(p) >> phase) & 0xF;
    phase = 4 - phase;
    return true;
  };

  bool negative = false;
  int32_t number = 0;  // at most 10 digits: below 2^31
  int32_t exponentAdd = 0;
  int32_t integerLength = 0;
  int32_t fractionLength = 0;
  for (;;) {
    if (!next()) return 0;
    if (nib == 0xE) {
      negative = true;
    } else if (nib > 9) {
      break;
    } else if (number >= 0xCCCCCCC) {
      ++exponentAdd;             // no room for the digit
    } else if (nib || number) {  // leading zeros are skipped
      ++integerLength;
      number = number * 10 + static_cast<int32_t>(nib);
    }
  }
  if (nib == 0xA) {
    for (;;) {
      if (!next()) return 0;
      if (nib >= 10) break;
      if (!nib && !number) {
        --exponentAdd;
      } else if (number < 0xCCCCCCC && fractionLength < 9) {
        ++fractionLength;
        number = number * 10 + static_cast<int32_t>(nib);
      }
    }
  }
  bool exponentNegative = false;
  if (nib == 0xC) {
    exponentNegative = true;
    nib = 0xB;
  }
  int32_t exponent = 0;
  bool exponentOverflow = false;
  if (nib == 0xB) {
    for (;;) {
      if (!next()) return 0;
      if (nib >= 10) break;
      if (exponent > 1000) {
        exponentOverflow = true;  // FreeType's arbitrary limit
      } else {
        exponent = exponent * 10 + static_cast<int32_t>(nib);
      }
    }
    if (exponentNegative) exponent = -exponent;
  }
  if (!number) return 0;

  int32_t result = 0;
  if (exponentOverflow) {
    result = exponentNegative ? 0 : 0x7FFFFFFF;
  } else if (scaling) {
    exponent += exponentAdd + integerLength;
    fractionLength += integerLength;  // now every digit kept
    if (fractionLength <= 5) {
      if (number > 0x7FFF) {
        result = divFix(number, 10);
        *scaling = exponent - fractionLength + 1;
      } else {
        if (exponent > 0) {
          const int32_t newLength = exponent < 5 ? exponent : 5;  // makes the scaling as small as possible
          const int32_t shift = newLength - fractionLength;
          if (shift > 0) {
            exponent -= newLength;
            number *= kPowerTens[shift];
            if (number > 0x7FFF) {
              number /= 10;
              exponent += 1;
            }
          } else {
            exponent -= fractionLength;
          }
        } else {
          exponent -= fractionLength;
        }
        result = number * 65536;
        *scaling = exponent;
      }
    } else if (number / kPowerTens[fractionLength - 5] > 0x7FFF) {
      result = divFix(number, kPowerTens[fractionLength - 4]);
      *scaling = exponent - 4;
    } else {
      result = divFix(number, kPowerTens[fractionLength - 5]);
      *scaling = exponent - 5;
    }
  } else {
    exponent += exponentAdd;
    integerLength += exponent;
    fractionLength -= exponent;
    if (integerLength > 5) {
      result = 0x7FFFFFFF;
    } else if (integerLength >= -5) {
      if (integerLength < 0) {  // digits below 16.16's precision
        number /= kPowerTens[-integerLength];
        fractionLength += integerLength;
      }
      if (fractionLength == 10) {
        number /= 10;
        fractionLength -= 1;
      }
      if (fractionLength > 0) {
        if (number / kPowerTens[fractionLength] <= 0x7FFF) result = divFix(number, kPowerTens[fractionLength]);
      } else {
        number *= kPowerTens[-fractionLength];
        result = number > 0x7FFF ? 0x7FFFFFFF : number * 65536;
      }
    }
  }
  return negative ? -result : result;
}

// cff_parse_num: an integer, or a real truncated to one.
int32_t dictNumber(FontBytes& bytes, const uint32_t at, const uint32_t end) {
  return bytes.u8(at) == 30 ? dictReal(bytes, at, end, nullptr) >> 16 : dictInteger(bytes, at, end);
}

// cff_parse_fixed_dynamic: a 16.16 value as precise as it can be and the power of ten it is to be multiplied by. An
// integer's value is FreeType's 64-bit one.
int64_t dictFixedDynamic(FontBytes& bytes, const uint32_t at, const uint32_t end, int32_t* scaling) {
  if (bytes.u8(at) == 30) return dictReal(bytes, at, end, scaling);
  const int32_t number = dictInteger(bytes, at, end);
  if (number <= 0x7FFF) {
    *scaling = 0;
    return int64_t{number} * 65536;
  }
  int32_t length = 5;
  while (length < 10 && number >= kPowerTens[length]) ++length;
  if (number / kPowerTens[length - 5] > 0x7FFF) {
    *scaling = length - 4;
    return divFix(number, kPowerTens[length - 4]);
  }
  *scaling = length - 5;
  return divFix(number, kPowerTens[length - 5]);
}

// A font matrix while it is worked out: FT_Matrix, FT_Vector and units_per_em in FreeType's 64-bit FT_Long.
struct WideMatrix {
  int64_t xx = 0x10000;
  int64_t xy = 0;
  int64_t yx = 0;
  int64_t yy = 0x10000;
  int64_t dx = 0;
  int64_t dy = 0;
  int64_t upm = 0;
};

// FT_Matrix_Check: false for a matrix that is 0, wider than 32 bits, or (nearly) singular.
bool matrixUsable(int64_t xx, int64_t xy, int64_t yx, int64_t yy) {
  const int64_t all = absWide(xx) | absWide(xy) | absWide(yx) | absWide(yy);
  if (!all || all > 0x7FFFFFFF) return false;
  const int32_t shift = msb(static_cast<uint32_t>(all)) - 12;  // keeps the products below 2^27
  if (shift > 0) {
    xx >>= shift;
    xy >>= shift;
    yx >>= shift;
    yy >>= shift;
  }
  const uint64_t determinant = 32u * static_cast<uint64_t>(absWide(xx * yy - xy * yx));
  const uint64_t norm = static_cast<uint64_t>(xx * xx) + static_cast<uint64_t>(xy * xy) +
                        static_cast<uint64_t>(yx * yx) + static_cast<uint64_t>(yy * yy);
  return determinant > norm;
}

// cff_parse_font_matrix over the operator's first six operands: each as precise as it can be read, brought to the
// scale of the most precise, which sets the units per em. Scales far apart, or a degenerate matrix, give the default
// matrix with 1 unit per em.
WideMatrix dictFontMatrix(FontBytes& bytes, const uint32_t (&operand)[kMaxOperands], const uint32_t end) {
  int64_t values[6];
  int32_t scalings[6];
  int64_t maxScaling = INT64_MIN;
  int64_t minScaling = INT64_MAX;
  for (int i = 0; i < 6; ++i) {
    values[i] = dictFixedDynamic(bytes, operand[i], end, &scalings[i]);
    if (values[i]) {
      maxScaling = scalings[i] > maxScaling ? scalings[i] : maxScaling;
      minScaling = scalings[i] < minScaling ? scalings[i] : minScaling;
    }
  }
  WideMatrix matrix;
  matrix.upm = 1;
  if (maxScaling < -9 || maxScaling > 0 || maxScaling - minScaling < 0 || maxScaling - minScaling > 9) return matrix;
  for (int i = 0; i < 6; ++i) {
    if (!values[i]) continue;
    const int64_t divisor = kPowerTens[maxScaling - scalings[i]];
    const int64_t half = divisor >> 1;
    // FreeType guards these sums against FT_Long's limits; values read from 32-bit numbers stay far below them.
    values[i] = values[i] < 0 ? (values[i] - half) / divisor : (values[i] + half) / divisor;
  }
  if (!matrixUsable(values[0], values[2], values[1], values[3])) return matrix;
  matrix.xx = values[0];
  matrix.yx = values[1];
  matrix.xy = values[2];
  matrix.yy = values[3];
  matrix.dx = values[4];
  matrix.dy = values[5];
  matrix.upm = kPowerTens[-maxScaling];
  return matrix;
}

// cff_face_init: the matrix and the offset divided by |yy| (|yx| when yy is 0), so that one is 1.0, and the units per
// em with them; the offset then in whole font units.
void normalize(WideMatrix* m) {
  const int64_t by = m->yy ? absWide(m->yy) : absWide(m->yx);
  if (by != 0x10000) {
    m->upm = divFix64(m->upm, by);
    m->xx = divFix64(m->xx, by);
    m->yx = divFix64(m->yx, by);
    m->xy = divFix64(m->xy, by);
    m->yy = divFix64(m->yy, by);
    m->dx = divFix64(m->dx, by);
    m->dy = divFix64(m->dy, by);
  }
  m->dx >>= 16;
  m->dy >>= 16;
}

// The finished matrix in 32 bits; false when it does not fit (FreeType would go on with 64-bit values the engine's
// arithmetic does not take).
bool narrow(const WideMatrix& m, CffMatrix* out) {
  const auto fits = [](const int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; };
  if (!fits(m.xx) || !fits(m.xy) || !fits(m.yx) || !fits(m.yy) || !fits(m.dx) || !fits(m.dy) || m.upm < 0 ||
      m.upm > UINT32_MAX) {
    return false;
  }
  *out = {static_cast<int32_t>(m.xx),  static_cast<int32_t>(m.xy), static_cast<int32_t>(m.yx),
          static_cast<int32_t>(m.yy),  static_cast<int32_t>(m.dx), static_cast<int32_t>(m.dy),
          static_cast<uint32_t>(m.upm)};
  return true;
}

// A DICT operator and the operands before it: their count, and where the first ones start. FreeType's parser keeps a
// pointer per operand (cff_parser_run); an operator reads its first ones.
struct DictOperator {
  uint16_t code = 0;  // one byte, or 0x100 | the second byte of an escaped (12 x) operator
  uint32_t count = 0;
  uint32_t operand[kMaxOperands] = {};
};

// cff_parser_run over the DICT [start, end): numbers are pushed, each operator is handed to `visit` with them. False on
// a stack overflow (more than `stackSize` entries, the operator counting as one), an escape byte ending the DICT, or
// when `visit` refuses the operator. A real running past the end ends the DICT, without error.
template <typename Visit>
bool runDict(FontBytes& bytes, const uint32_t start, const uint32_t end, const uint32_t stackSize, Visit&& visit) {
  DictOperator op;
  for (uint64_t p = start; p < end; ++p) {
    const uint32_t v = bytes.u8(static_cast<uint32_t>(p));
    if (op.count >= stackSize) return false;
    if (v >= 27 && v != 31 && v != 255) {
      if (op.count < kMaxOperands) op.operand[op.count] = static_cast<uint32_t>(p);
      ++op.count;
      if (v == 30) {
        for (++p;; ++p) {  // to the byte holding the end nibble
          if (p >= end) return true;
          const uint32_t b = bytes.u8(static_cast<uint32_t>(p));
          if ((b >> 4) == 0xF || (b & 0xF) == 0xF) break;
        }
      } else if (v == 28) {
        p += 2;
      } else if (v == 29) {
        p += 4;
      } else if (v > 246) {
        p += 1;
      }
      continue;
    }
    op.code = static_cast<uint16_t>(v);
    if (v == 12) {
      if (++p >= end) return false;
      op.code = static_cast<uint16_t>(0x100 | bytes.u8(static_cast<uint32_t>(p)));
    }
    if (!visit(op)) return false;
    op.count = 0;
  }
  return true;
}

// What cff_subfont_load keeps of a Top DICT or a font DICT (CFF_FontRecDictRec): offsets from the table's start as
// the DICT gives them, and the FontMatrix as read.
struct FontDict {
  uint32_t charset = 0;
  uint32_t encoding = 0;
  uint32_t charStrings = 0;
  uint32_t privateSize = 0;
  uint32_t privateOffset = 0;
  uint32_t fdArray = 0;
  uint32_t fdSelect = 0;
  uint32_t registry = kNoRegistry;
  bool hasMatrix = false;
  WideMatrix matrix;  // without a FontMatrix: identity, 0 units per em
};

// The Top and font DICT operators FreeType knows (cfftoken.h, CFF_CODE_TOPDICT); it skips the others.
bool topDictOperator(const uint16_t code) {
  if (code < 0x100) return code <= 5 || code == 13 || (code >= 15 && code <= 18);
  const uint32_t x = code & 0xFF;
  return x <= 8 || x == 0x14 || x == 0x15 || x == 0x18 || (x >= 0x1E && x <= 0x26);
}

// The Private DICT operators FreeType knows (CFF_CODE_PRIVATE) that are not delta arrays: they need an operand.
bool privateOperatorWithOperand(const uint16_t code) {
  if (code < 0x100) return code == 10 || code == 11 || (code >= 19 && code <= 21);
  const uint32_t x = code & 0xFF;
  return x >= 0x09 && x <= 0x13 && x != 0x0C && x != 0x0D;
}

// One Top or font DICT operator, with the checks FreeType makes: an operator it knows needs an operand, and the
// callbacks (FontBBox, Private, FontMatrix, MultipleMaster, ROS) their own.
bool readFontDictOperator(FontBytes& bytes, const uint32_t end, const DictOperator& op, FontDict* dict) {
  if (!topDictOperator(op.code)) return true;
  if (op.count < 1) return false;
  const auto number = [&](const int i) { return dictNumber(bytes, op.operand[i], end); };
  switch (op.code) {
    case 5:  // FontBBox
      return op.count >= 4;
    case 15:
      dict->charset = static_cast<uint32_t>(number(0));
      return true;
    case 16:
      dict->encoding = static_cast<uint32_t>(number(0));
      return true;
    case 17:
      dict->charStrings = static_cast<uint32_t>(number(0));
      return true;
    case 18: {  // Private: size, offset
      if (op.count < 2) return false;
      const int32_t size = number(0);
      const int32_t offset = number(1);
      if (size < 0 || offset < 0) return false;
      dict->privateSize = static_cast<uint32_t>(size);
      dict->privateOffset = static_cast<uint32_t>(offset);
      return true;
    }
    case 0x107:  // FontMatrix
      if (op.count < 6) return false;
      dict->matrix = dictFontMatrix(bytes, op.operand, end);
      dict->hasMatrix = true;
      return true;
    case 0x118: {  // MultipleMaster
      if (op.count < 5) return false;
      const int32_t designs = number(0);
      return designs >= 2 && designs <= 16;
    }
    case 0x11E:  // ROS
      if (op.count < 3) return false;
      dict->registry = static_cast<uint32_t>(number(0));
      return true;
    case 0x124:
      dict->fdArray = static_cast<uint32_t>(number(0));
      return true;
    case 0x125:
      dict->fdSelect = static_cast<uint32_t>(number(0));
      return true;
    default:
      return true;
  }
}

// cff_subfont_load's DICT: item `item` of the Top DICT INDEX or the FDArray.
bool readFontDict(FontBytes& bytes, const CffIndex& index, const uint32_t item, FontDict* dict) {
  *dict = FontDict{};
  uint32_t offset = 0;
  uint32_t length = 0;
  if (!cffIndexItem(bytes, index, item, &offset, &length)) return false;
  const uint32_t end = offset + length;
  return runDict(bytes, offset, end, kTopStack,
                 [&](const DictOperator& op) { return readFontDictOperator(bytes, end, op, dict); });
}

// The Private DICT of `dict` (cff_load_private_dict) and its local subroutines (cff_subfont_load): none for a DICT
// with ROS, which FreeType stops reading there, or without a Private DICT (an offset or a size of 0). The Subrs offset
// is from the Private DICT's start, summed with wrap-around as FreeType sums it; everything lies inside the table.
bool readPrivate(FontBytes& bytes, const CffFace& cff, const FontDict& dict, CffIndex* localSubrs) {
  *localSubrs = CffIndex{};
  if (dict.registry != kNoRegistry || dict.privateOffset == 0 || dict.privateSize == 0) return true;
  if (dict.privateOffset > cff.tableLength || dict.privateSize > cff.tableLength - dict.privateOffset) return false;
  const uint32_t start = cff.table + dict.privateOffset;
  const uint32_t end = start + dict.privateSize;
  uint32_t subrs = 0;
  const bool read = runDict(bytes, start, end, kPrivateStack, [&](const DictOperator& op) {
    if (!privateOperatorWithOperand(op.code)) return true;
    if (op.count < 1) return false;
    if (op.code == 19) subrs = static_cast<uint32_t>(dictNumber(bytes, op.operand[0], end));
    return true;
  });
  if (!read) return false;
  if (subrs == 0) return true;
  const uint32_t at = dict.privateOffset + subrs;
  return at < cff.tableLength && readCffIndex(bytes, cff.table + at, cff.table + cff.tableLength, localSubrs);
}

// A font DICT's matrix as cff_face_init leaves it: its own FontMatrix concatenated with the Top DICT's (as normalized)
// when both have one, its own alone, or the Top DICT's when it has none; then normalized.
WideMatrix fontDictMatrix(const CffFace& cff, const FontDict& dict) {
  const CffMatrix& top = cff.matrix;
  WideMatrix m = dict.matrix;
  if (!dict.hasMatrix) {
    m = {top.xx, top.xy, top.yx, top.yy, top.dx, top.dy, top.upm};
  } else if (cff.hasFontMatrix) {
    // FT_Matrix_Multiply_Scaled and FT_Vector_Transform_Scaled at the smaller units per em.
    const int64_t topUpm = top.upm;
    const int64_t scaling = topUpm > 1 && m.upm > 1 ? (topUpm < m.upm ? topUpm : m.upm) : 1;
    const int64_t by = 0x10000 * scaling;
    const WideMatrix own = m;
    m.xx = addWide(mulDiv64(top.xx, own.xx, by), mulDiv64(top.xy, own.yx, by));
    m.xy = addWide(mulDiv64(top.xx, own.xy, by), mulDiv64(top.xy, own.yy, by));
    m.yx = addWide(mulDiv64(top.yx, own.xx, by), mulDiv64(top.yy, own.yx, by));
    m.yy = addWide(mulDiv64(top.yx, own.xy, by), mulDiv64(top.yy, own.yy, by));
    m.dx = addWide(mulDiv64(own.dx, top.xx, by), mulDiv64(own.dy, top.xy, by));
    m.dy = addWide(mulDiv64(own.dx, top.yx, by), mulDiv64(own.dy, top.yy, by));
    m.upm = mulDiv64(own.upm, topUpm, scaling);
  }
  normalize(&m);
  return m;
}

// A CID-keyed font's font DICTs, each with its matrix and its Private DICT, as cff_font_load reads them all: 1 to 256
// of them. FreeType takes a font without any, or with more than it holds, as having none, and draws every glyph with
// the Top DICT, whose Private DICT it never reads; the engine refuses both.
bool fontDictsFit(FontBytes& bytes, const CffFace& cff) {
  if (cff.fdArray.count == 0 || cff.fdArray.count > kMaxFontDicts) return false;
  for (uint32_t fd = 0; fd < cff.fdArray.count; ++fd) {
    FontDict dict;
    CffMatrix matrix;
    CffIndex localSubrs;
    if (!readFontDict(bytes, cff.fdArray, fd, &dict) || !narrow(fontDictMatrix(cff, dict), &matrix) ||
        !readPrivate(bytes, cff, dict, &localSubrs)) {
      return false;
    }
  }
  return true;
}

// A CID-keyed glyph's FD (cff_fd_select_get). FreeType scans the format 3 ranges in order for the first that holds
// the glyph; with the ranges in ascending order, as the CFF spec has them, that is the last starting at or before it,
// found here by bisection. A glyph before the first range or past the sentinel is in FD 0.
uint32_t glyphFd(FontBytes& bytes, const CffFace& cff, const uint32_t glyph) {
  if (cff.fdSelectFormat == 0) return bytes.u8(cff.fdSelect + 1 + glyph);
  const uint32_t ranges = bytes.u16(cff.fdSelect + 1);
  const uint32_t first = cff.fdSelect + 3;  // range i: its first glyph (2 bytes), its FD; then the sentinel
  uint32_t lo = 0;
  uint32_t hi = ranges;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (bytes.u16(first + 3 * mid) <= glyph) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return 0;
  const uint32_t range = first + 3 * (lo - 1);
  return glyph < bytes.u16(range + 3) ? bytes.u8(range + 2) : 0;
}

// CFF_Load_FD_Select: format 0, a byte per glyph, or format 3, at least one range; other formats are refused.
bool readFdSelect(FontBytes& bytes, const uint32_t offset, CffFace* cff) {
  if (offset >= cff->tableLength) return false;
  const uint32_t at = cff->table + offset;
  const uint32_t room = cff->tableLength - offset;
  const uint8_t format = bytes.u8(at);
  uint64_t size = 0;
  if (format == 0) {
    size = 1 + uint64_t{cff->charStrings.count};
  } else if (format == 3) {
    if (room < 3) return false;
    const uint32_t ranges = bytes.u16(at + 1);
    if (ranges == 0) return false;
    size = 3 + 3 * uint64_t{ranges} + 2;
  } else {
    return false;
  }
  if (size > room) return false;
  cff->fdSelect = at;
  cff->fdSelectFormat = format;
  return true;
}

// cff_charset_load's checks: a predefined charset no smaller than the font, or format 0, 1 or 2 with an entry for
// every glyph but .notdef inside the table.
bool charsetFits(FontBytes& bytes, const CffFace& cff, const uint32_t offset) {
  const uint32_t glyphs = cff.charStrings.count;
  if (offset <= 2) return glyphs <= kPredefinedCharsetGlyphs[offset];
  if (offset >= cff.tableLength) return false;
  const uint32_t at = cff.table + offset;
  const uint64_t end = uint64_t{cff.table} + cff.tableLength;
  const uint8_t format = bytes.u8(at);
  if (format == 0) return uint64_t{at} + 1 + 2 * (uint64_t{glyphs} - 1) <= end;
  if (format != 1 && format != 2) return false;
  const uint32_t rangeBytes = format == 2 ? 4 : 3;
  uint64_t p = uint64_t{at} + 1;
  for (uint32_t g = 1; g < glyphs; p += rangeBytes) {
    if (p + rangeBytes > end) return false;
    const uint32_t first = bytes.u16(static_cast<uint32_t>(p));
    uint32_t left = format == 2 ? bytes.u16(static_cast<uint32_t>(p) + 2) : bytes.u8(static_cast<uint32_t>(p) + 2);
    if (first > 0xFFFF - left) left = 0xFFFF - first;  // FreeType keeps the SIDs below 0xFFFF
    g += left + 1;
  }
  return true;
}

// cff_encoding_load's checks (name-keyed fonts): a predefined encoding (0, 1), or format 0 or 1 with the codes its
// count says, then the supplements its high bit announces.
bool encodingFits(FontBytes& bytes, const CffFace& cff, const uint32_t offset) {
  if (offset <= 1) return true;
  if (offset >= cff.tableLength) return false;
  const uint32_t at = cff.table + offset;
  const uint32_t room = cff.tableLength - offset;
  if (room < 2) return false;
  const uint8_t format = bytes.u8(at);
  const uint32_t count = bytes.u8(at + 1);
  uint32_t size = 2;
  if ((format & 0x7F) == 0) {
    size += count;
  } else if ((format & 0x7F) == 1) {
    size += 2 * count;
  } else {
    return false;
  }
  if (format & 0x80) {
    if (size >= room) return false;
    size += 1 + 3 * static_cast<uint32_t>(bytes.u8(at + size));
  }
  return size <= room;
}

// The first glyph whose charset entry is `sid` (at most 149), or -1. Entry 0 is .notdef's 0 in every charset.
int32_t charsetGlyph(FontBytes& bytes, const CffFace& cff, const uint32_t sid) {
  const uint32_t glyphs = cff.charStrings.count;
  if (sid == 0) return 0;
  if (cff.charset == 0) return sid < glyphs ? static_cast<int32_t>(sid) : -1;  // ISOAdobe: SID n at glyph n
  if (cff.charset <= 2) {
    const uint8_t* at = cff.charset == 1 ? kExpertGlyphs : kExpertSubsetGlyphs;
    for (uint32_t i = 0; i < sizeof kExpertSids; ++i) {
      if (kExpertSids[i] == sid) return at[i] < glyphs ? at[i] : -1;
    }
    return -1;
  }
  const uint8_t format = bytes.u8(cff.charset);
  const uint32_t entries = cff.charset + 1;
  if (format == 0) {
    for (uint32_t g = 1; g < glyphs; ++g) {
      if (bytes.u16(entries + 2 * (g - 1)) == sid) return static_cast<int32_t>(g);
    }
    return -1;
  }
  const uint32_t rangeBytes = format == 2 ? 4 : 3;
  uint32_t p = entries;
  for (uint32_t g = 1; g < glyphs; p += rangeBytes) {
    const uint32_t first = bytes.u16(p);
    uint32_t left = format == 2 ? bytes.u16(p + 2) : bytes.u8(p + 2);
    if (first > 0xFFFF - left) left = 0xFFFF - first;
    if (sid >= first && sid - first <= left) {
      const uint32_t glyph = g + (sid - first);
      return glyph < glyphs ? static_cast<int32_t>(glyph) : -1;
    }
    g += left + 1;
  }
  return -1;
}

uint32_t readOffset(FontBytes& bytes, const uint32_t at, const uint32_t size) {
  uint32_t value = 0;
  for (uint32_t i = 0; i < size; ++i) value = value << 8 | bytes.u8(at + i);
  return value;
}

}  // namespace

bool readCffIndex(FontBytes& bytes, const uint32_t at, const uint32_t limit, CffIndex* out) {
  *out = CffIndex{};
  if (at > limit || limit - at < 2) return false;
  const uint16_t count = bytes.u16(at);
  if (count == 0) {
    out->start = at;
    out->dataStart = out->end = at + 2;
    return true;
  }
  if (limit - at < 3) return false;
  const uint8_t offSize = bytes.u8(at + 2);
  if (offSize < 1 || offSize > 4) return false;
  const uint64_t dataStart = uint64_t{at} + 3 + (uint64_t{count} + 1) * offSize;
  if (dataStart > limit) return false;
  const uint32_t last = readOffset(bytes, static_cast<uint32_t>(dataStart) - offSize, offSize);
  if (last == 0 || dataStart + last - 1 > limit) return false;
  out->start = at;
  out->count = count;
  out->offSize = offSize;
  out->dataStart = static_cast<uint32_t>(dataStart);
  out->end = static_cast<uint32_t>(dataStart + last - 1);
  return true;
}

bool cffIndexItem(FontBytes& bytes, const CffIndex& index, uint32_t item, uint32_t* offset, uint32_t* length) {
  *offset = 0;
  *length = 0;
  if (item >= index.count) return false;
  const uint32_t offsets = index.start + 3;
  const uint32_t start = readOffset(bytes, offsets + item * index.offSize, index.offSize);
  uint32_t end = 0;
  if (start != 0) {
    do {
      ++item;
      end = readOffset(bytes, offsets + item * index.offSize, index.offSize);
    } while (end == 0 && item < index.count);
  }
  // FreeType clamps the end to the end of the file (cff_index_access_element), and a subroutine's to the end of the
  // INDEX (cff_index_get_pointers); here every item ends inside its INDEX.
  const uint32_t room = index.end - index.dataStart + 1;
  if (end > room) end = room;
  if (start != 0 && end > start) {
    *offset = index.dataStart + start - 1;
    *length = end - start;
  }
  return true;
}

bool readCffTable(FontBytes& bytes, const uint32_t table, const uint32_t length, const uint16_t unitsPerEm,
                  CffFace* out) {
  *out = CffFace{};
  out->table = table;
  out->tableLength = length;
  const uint32_t end = table + length;
  // The header (cff_font_load): major version 1, at least 4 bytes, an offset size of at most 4.
  if (length < 4 || bytes.u8(table) != 1 || bytes.u8(table + 3) > 4) return false;
  const uint32_t headerSize = bytes.u8(table + 2);
  if (headerSize < 4 || headerSize > length) return false;
  // One name, as FreeType wants in an OpenType font, and its Top DICT; then the strings and global subroutines.
  CffIndex names;
  CffIndex topDicts;
  CffIndex strings;
  if (!readCffIndex(bytes, table + headerSize, end, &names) || names.count != 1 ||
      !readCffIndex(bytes, names.end, end, &topDicts) || topDicts.count < 1 ||
      !readCffIndex(bytes, topDicts.end, end, &strings) || !readCffIndex(bytes, strings.end, end, &out->globalSubrs)) {
    return false;
  }
  FontDict top;
  if (!readFontDict(bytes, topDicts, 0, &top)) return false;
  if (top.charStrings == 0 || top.charStrings >= length ||
      !readCffIndex(bytes, table + top.charStrings, end, &out->charStrings) || out->charStrings.count == 0) {
    return false;
  }

  // The Top DICT's matrix (cff_face_init); without a FontMatrix its units per em are head's.
  if (!top.hasMatrix) top.matrix.upm = unitsPerEm;
  normalize(&top.matrix);
  if (!narrow(top.matrix, &out->matrix)) return false;
  out->hasFontMatrix = top.hasMatrix;

  out->cid = top.registry != kNoRegistry;
  if (out->cid) {
    if (top.fdArray >= length || !readCffIndex(bytes, table + top.fdArray, end, &out->fdArray) ||
        !fontDictsFit(bytes, *out) || !readFdSelect(bytes, top.fdSelect, out)) {
      return false;
    }
  } else {
    if (!readPrivate(bytes, *out, top, &out->localSubrs)) return false;
    if (top.privateOffset != 0 && top.privateSize != 0) {
      out->privateDict = table + top.privateOffset;
      out->privateSize = top.privateSize;
    }
    if (!encodingFits(bytes, *out, top.encoding)) return false;
  }
  if (!charsetFits(bytes, *out, top.charset)) return false;
  out->charset = top.charset <= 2 ? top.charset : table + top.charset;
  return !bytes.outOfRange();
}

Status cffGlyphSource(FontBytes& bytes, const Face& face, const uint16_t glyph, CffGlyphSource* out) {
  *out = CffGlyphSource{};
  const CffFace& cff = face.cff;
  const auto failed = [&]() {
    *out = CffGlyphSource{};
    return bytes.ioError() ? Status::Transient : Status::Malformed;
  };
  if (face.outlines != Face::Outlines::Cff || glyph >= cff.charStrings.count) return failed();
  if (!cffIndexItem(bytes, cff.charStrings, glyph, &out->offset, &out->length)) return failed();
  if (!cff.cid) {
    out->localSubrs = cff.localSubrs;
    out->matrix = cff.matrix;
  } else {
    const uint32_t fd = glyphFd(bytes, cff, glyph);
    FontDict dict;
    if (fd >= cff.fdArray.count || !readFontDict(bytes, cff.fdArray, fd, &dict) ||
        !narrow(fontDictMatrix(cff, dict), &out->matrix) || !readPrivate(bytes, cff, dict, &out->localSubrs)) {
      return failed();
    }
    // cff_slot_load scales by the Top DICT's units per em over the font DICT's.
    if (out->matrix.upm != cff.matrix.upm) {
      out->scaleNum = cff.matrix.upm;
      out->scaleDen = out->matrix.upm;
    }
  }
  if (bytes.ioError() || bytes.outOfRange()) return failed();
  return Status::Ok;
}

int32_t cffStandardCharGlyph(FontBytes& bytes, const Face& face, const int32_t code) {
  if (face.outlines != Face::Outlines::Cff || code < 0 || code > 255) return -1;
  return charsetGlyph(bytes, face.cff, kStandardEncoding[code]);
}

}  // namespace ttf
