#include "TtfCffCharString.h"

#include <cstring>

#include "TtfFace.h"
#include "TtfFixed.h"
#include "TtfGrays.h"  // FT_Vector and the curve tags
#include "TtfRasterizer.h"

namespace ttf {

namespace {

constexpr uint32_t kMaxHints = 96;     // CF2_MAX_HINTS: the stems a hint mask holds
constexpr int32_t kUnitScale = 0x400;  // cf2_getScaleAndHintFlag unhinted: 1/64 in 16.16, cff_slot_load scales later

// Type 2 operators (psintrp.c's cf2_cmd* and cf2_esc*).
enum : uint8_t {
  kHstem = 1,
  kVstem = 3,
  kVmoveto = 4,
  kRlineto = 5,
  kHlineto = 6,
  kVlineto = 7,
  kRrcurveto = 8,
  kCallsubr = 10,
  kReturn = 11,
  kEscape = 12,
  kEndchar = 14,
  kHstemhm = 18,
  kHintmask = 19,
  kCntrmask = 20,
  kRmoveto = 21,
  kHmoveto = 22,
  kVstemhm = 23,
  kRcurveline = 24,
  kRlinecurve = 25,
  kVvcurveto = 26,
  kHhcurveto = 27,
  kShortint = 28,
  kCallgsubr = 29,
  kVhcurveto = 30,
  kHvcurveto = 31,
};
enum : uint8_t {
  kAnd = 3,
  kOr = 4,
  kNot = 5,
  kAbs = 9,
  kAdd = 10,
  kSub = 11,
  kDiv = 12,
  kNeg = 14,
  kEq = 15,
  kCallothersubr = 16,
  kPop = 17,
  kDrop = 18,
  kPut = 20,
  kGet = 21,
  kIfelse = 22,
  kRandom = 23,
  kMul = 24,
  kSqrt = 26,
  kDup = 27,
  kExch = 28,
  kIndex = 29,
  kRoll = 30,
  kHflex = 34,
  kFlex = 35,
  kHflex1 = 36,
  kFlex1 = 37,
};

// CffDecoder::queuedOp and CffDecoder::phase.
enum : uint8_t { kQueuedNone, kQueuedLine, kQueuedCurve };
enum : uint8_t { kGlyph, kAccent, kBase };

// cf2_doFlex's readFromStack: which of dx1 dy1 ... dx6 dy6 each flex operator reads.
constexpr bool kHflexReads[12] = {true, false, true, true, true, false, true, false, true, false, true, false};
constexpr bool kFlexReads[12] = {true, true, true, true, true, true, true, true, true, true, true, true};
constexpr bool kHflex1Reads[12] = {true, true, true, true, true, false, true, false, true, true, true, false};
constexpr bool kFlex1Reads[12] = {true, true, true, true, true, true, true, true, true, true, false, false};

// cf2_intToFixed, wrapping as FreeType's unsigned shift does.
constexpr int32_t intToFixed(const int32_t i) { return static_cast<int32_t>(static_cast<uint32_t>(i) << 16); }
// ADD_INT32, SUB_INT32, NEG_INT32: 32-bit arithmetic that wraps.
constexpr int32_t add32(const int32_t a, const int32_t b) {
  return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
constexpr int32_t sub32(const int32_t a, const int32_t b) {
  return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
constexpr int32_t neg32(const int32_t a) { return static_cast<int32_t>(0u - static_cast<uint32_t>(a)); }
// cf2_fixedAbs.
constexpr int32_t abs32(const int32_t a) { return a < 0 ? neg32(a) : a; }

// cff_compute_bias: what a subroutine number is offset by, from the count of its INDEX.
uint32_t subrBias(const int32_t charstringType, const uint32_t count) {
  if (charstringType == 1) return 0;
  if (count < 1240) return 107;
  if (count < 33900) return 1131;
  return 32768;
}

using Point = CffDecoder::Point;

// One glyph's run: cf2_interpT2CharString with the glyph path (cf2_glyphpath_*) and the outline builder (psft.c's
// callbacks, ps_builder_*) it drives. Every value lives in the decoder; this holds references only. As in cf2, an
// error is recorded and checked once per operator: until then the operator runs on with the defaults FreeType's
// stack hands out (0), which keeps every access in bounds.
class Interpreter {
 public:
  Interpreter(FontBytes& bytes, const Face& face, const CffIndex& localSubrs, CffDecoder& decoder, CffOutline& out)
      : bytes_(bytes),
        face_(face),
        localSubrs_(localSubrs),
        d_(decoder),
        out_(out),
        localBias_(subrBias(face.cff.charstringType, localSubrs.count)),
        globalBias_(subrBias(face.cff.charstringType, face.cff.globalSubrs.count)) {}

  Status run(uint32_t offset, uint32_t length);

 private:
  bool ok() const { return status_ == Status::Ok; }
  void fail(const Status status) {
    if (ok()) status_ = status;
  }

  // --- Reading (cf2_buf_readByte: past the end of a charstring or subroutine, 0 without an error) ---
  uint8_t readByte() { return d_.region.at < d_.region.end ? bytes_.u8(d_.region.at++) : 0; }

  // --- The operand stack (psstack.c) ---
  bool isFixed(const uint32_t i) const { return (d_.fixedOperands[i >> 5] >> (i & 31)) & 1; }
  void setFixed(const uint32_t i, const bool fixed) {
    uint32_t& word = d_.fixedOperands[i >> 5];
    const uint32_t bit = uint32_t{1} << (i & 31);
    word = fixed ? word | bit : word & ~bit;
  }
  void push(int32_t value, bool fixed);
  void pushInt(const int32_t value) { push(value, false); }
  void pushFixed(const int32_t value) { push(value, true); }
  int32_t popInt();
  int32_t popFixed();
  int32_t real(uint32_t i);  // cf2_stack_getReal
  void clear() { d_.operandCount = 0; }
  void roll(int32_t count, int32_t shift);

  // --- Operators (psintrp.c) ---
  void begin(CffDecoder::Region region, int32_t x, int32_t y);
  void step(uint8_t op);
  void number(uint8_t op);
  void escape(uint8_t op);
  void doStems();
  void hintMask(bool counter);
  void callSubr(const CffIndex& subrs, uint32_t bias);
  void flex(const bool (&reads)[12], bool conditionalLast);
  void endchar();
  void startComponent(int32_t code, int32_t x, int32_t y, uint8_t phase);

  // --- The glyph path (pshints.c), unhinted and undarkened ---
  void moveTo();
  void lineTo(int32_t x, int32_t y);
  void curveTo(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3);
  void closeOpenPath();
  void pushMove(Point p0);
  void pushQueued();
  void buildMap();

  // --- The outline builder (psft.c, psobjs.c) ---
  bool checkPoints(uint32_t count);
  void addPoint(Point p, bool on);
  bool addPoint1(Point p);
  bool startPoint(Point p);
  void builderMoveTo();
  void builderLineTo(Point p0, Point p1);
  void builderCubeTo(Point p0, Point p1, Point p2, Point p3);
  void closeContour();

  FontBytes& bytes_;
  const Face& face_;
  const CffIndex& localSubrs_;
  CffDecoder& d_;
  CffOutline& out_;
  const uint32_t localBias_;
  const uint32_t globalBias_;
  Status status_ = Status::Ok;
  bool finished_ = false;
};

// --- The operand stack ---

void Interpreter::push(const int32_t value, const bool fixed) {
  if (d_.operandCount == kCffStackSize) {
    fail(Status::Malformed);  // Stack_Overflow
    return;
  }
  d_.operands[d_.operandCount] = value;
  setFixed(d_.operandCount, fixed);
  ++d_.operandCount;
}

// cf2_stack_popInt: only an integer; a 16.16 value is a type error, and stays.
int32_t Interpreter::popInt() {
  if (d_.operandCount == 0 || isFixed(d_.operandCount - 1u)) {
    fail(Status::Malformed);  // Stack_Underflow, Syntax_Error
    return 0;
  }
  return d_.operands[--d_.operandCount];
}

// cf2_stack_popFixed: either type, an integer converted.
int32_t Interpreter::popFixed() {
  if (d_.operandCount == 0) {
    fail(Status::Malformed);  // Stack_Underflow
    return 0;
  }
  --d_.operandCount;
  const int32_t value = d_.operands[d_.operandCount];
  return isFixed(d_.operandCount) ? value : intToFixed(value);
}

int32_t Interpreter::real(const uint32_t i) {
  if (i >= d_.operandCount) {
    fail(Status::Malformed);  // Stack_Overflow (a bounds error)
    return 0;
  }
  return isFixed(i) ? d_.operands[i] : intToFixed(d_.operands[i]);
}

// cf2_stack_roll: rotates the BOTTOM `count` entries by `shift`, types and all, along the chains FreeType walks.
void Interpreter::roll(const int32_t count, int32_t shift) {
  if (count < 2) return;
  if (static_cast<uint32_t>(count) > d_.operandCount) {
    fail(Status::Malformed);  // Stack_Overflow
    return;
  }
  // Integers come from the charstring's numbers or the logic operators: far from INT32_MIN, so -shift is safe.
  shift = shift < 0 ? -((-shift) % count) : shift % count;
  if (shift == 0) return;
  int32_t start = -1;
  int32_t index = -1;
  int32_t last = 0;
  bool lastFixed = false;
  for (int32_t i = 0; i < count; ++i) {
    if (start == index) {
      ++start;
      index = start;
      last = d_.operands[index];
      lastFixed = isFixed(static_cast<uint32_t>(index));
    }
    index += shift;
    if (index >= count) {
      index -= count;
    } else if (index < 0) {
      index += count;
    }
    const int32_t value = d_.operands[index];
    const bool fixed = isFixed(static_cast<uint32_t>(index));
    d_.operands[index] = last;
    setFixed(static_cast<uint32_t>(index), lastFixed);
    last = value;
    lastFixed = fixed;
  }
}

// --- Operators ---

// A fresh cf2_interpT2CharString call for the glyph or one of seac's components: its own stack, transient array,
// subroutine stack, hints and glyph path, its current point at (x, y). The builder's outline, the operator count and
// the seac state carry over.
void Interpreter::begin(const CffDecoder::Region region, const int32_t x, const int32_t y) {
  std::memset(d_.storage, 0, sizeof d_.storage);
  d_.fixedOperands[0] = 0;
  d_.fixedOperands[1] = 0;
  d_.operandCount = 0;
  d_.region = region;
  d_.depth = 0;
  d_.stems = 0;
  d_.maskValid = false;
  d_.maskNew = false;
  d_.mapValid = false;
  d_.current = {x, y};
  d_.currentCS = {0, 0};
  d_.start = {0, 0};
  d_.currentDS = {0, 0};
  d_.queuedOp = kQueuedNone;
  d_.pathOpen = false;
  d_.pathClosing = false;
}

Status Interpreter::run(const uint32_t offset, const uint32_t length) {
  std::memset(&d_, 0, sizeof d_);
  d_.phase = kGlyph;
  begin({offset, offset + length}, 0, 0);
  while (ok() && !finished_) {
    if (bytes_.ioError()) return status_;  // the caller reports it as Transient
    // At the end of a subroutine cf2 runs `return`, at the end of the charstring `endchar`.
    uint8_t op = d_.depth > 0 ? kReturn : kEndchar;
    if (d_.region.at < d_.region.end) op = readByte();
    if (++d_.operators > kCffMaxOperators) {
      fail(Status::Unsupported);
      break;
    }
    step(op);
  }
  if (ok()) closeContour();  // cf2_outline_close
  return status_;
}

// One operator or number of cf2_interpT2CharString's main loop. Operators that end with `break` there clear the stack
// after them; those that `continue` (numbers, arithmetic, subroutine calls, drawing operators that cleared it already)
// return here before the clear.
void Interpreter::step(const uint8_t op) {
  CffDecoder::Point& current = d_.current;
  switch (op) {
    case kHstem:
    case kHstemhm:
    case kVstem:
    case kVstemhm:
      if (!d_.maskValid) doStems();  // stems after the mask is in force are ignored
      break;

    case kVmoveto:
      current.y = add32(current.y, popFixed());
      moveTo();
      break;
    case kRmoveto: {
      const int32_t dy = popFixed();
      const int32_t dx = popFixed();
      current.y = add32(current.y, dy);
      current.x = add32(current.x, dx);
      moveTo();
      break;
    }
    case kHmoveto:
      current.x = add32(current.x, popFixed());
      moveTo();
      break;

    case kRlineto: {
      const uint32_t count = d_.operandCount;
      for (uint32_t i = 0; i < count; i += 2) {
        current.x = add32(current.x, real(i));
        current.y = add32(current.y, real(i + 1));
        lineTo(current.x, current.y);
      }
      break;
    }
    case kHlineto:
    case kVlineto: {
      const uint32_t count = d_.operandCount;
      bool isX = op == kHlineto;
      for (uint32_t i = 0; i < count; ++i) {
        const int32_t v = real(i);
        if (isX) {
          current.x = add32(current.x, v);
        } else {
          current.y = add32(current.y, v);
        }
        isX = !isX;
        lineTo(current.x, current.y);
      }
      break;
    }

    case kRrcurveto:
    case kRcurveline: {
      const uint32_t count = d_.operandCount;
      uint32_t i = 0;
      for (; i + 6 <= count; i += 6) {
        const int32_t x1 = add32(real(i), current.x);
        const int32_t y1 = add32(real(i + 1), current.y);
        const int32_t x2 = add32(real(i + 2), x1);
        const int32_t y2 = add32(real(i + 3), y1);
        const int32_t x3 = add32(real(i + 4), x2);
        const int32_t y3 = add32(real(i + 5), y2);
        curveTo(x1, y1, x2, y2, x3, y3);
        current = {x3, y3};
      }
      if (op == kRcurveline) {
        current.x = add32(current.x, real(i));
        current.y = add32(current.y, real(i + 1));
        lineTo(current.x, current.y);
      }
      break;
    }
    case kRlinecurve: {
      const uint32_t count = d_.operandCount;
      uint32_t i = 0;
      for (; i + 6 < count; i += 2) {
        current.x = add32(current.x, real(i));
        current.y = add32(current.y, real(i + 1));
        lineTo(current.x, current.y);
      }
      for (; i < count; i += 6) {
        const int32_t x1 = add32(real(i), current.x);
        const int32_t y1 = add32(real(i + 1), current.y);
        const int32_t x2 = add32(real(i + 2), x1);
        const int32_t y2 = add32(real(i + 3), y1);
        const int32_t x3 = add32(real(i + 4), x2);
        const int32_t y3 = add32(real(i + 5), y2);
        curveTo(x1, y1, x2, y2, x3, y3);
        current = {x3, y3};
      }
      break;
    }
    case kVvcurveto:
    case kHhcurveto: {
      // A count not of the form 4n or 4n+1 loses its two bottom arguments (count &= ~2).
      const uint32_t count = d_.operandCount & ~2u;
      for (uint32_t i = d_.operandCount - count; i < count; i += 4) {
        int32_t x1 = current.x;
        int32_t y1 = current.y;
        int32_t x2 = 0;
        int32_t y2 = 0;
        int32_t x3 = 0;
        int32_t y3 = 0;
        if (op == kVvcurveto) {
          if ((count - i) & 1) {
            x1 = add32(real(i), current.x);
            ++i;
          }
          y1 = add32(real(i), current.y);
          x2 = add32(real(i + 1), x1);
          y2 = add32(real(i + 2), y1);
          x3 = x2;
          y3 = add32(real(i + 3), y2);
        } else {
          if ((count - i) & 1) {
            y1 = add32(real(i), current.y);
            ++i;
          }
          x1 = add32(real(i), current.x);
          x2 = add32(real(i + 1), x1);
          y2 = add32(real(i + 2), y1);
          x3 = add32(real(i + 3), x2);
          y3 = y2;
        }
        curveTo(x1, y1, x2, y2, x3, y3);
        current = {x3, y3};
      }
      break;
    }
    case kVhcurveto:
    case kHvcurveto: {
      // A count not of the form 8n, 8n+1, 8n+4 or 8n+5 loses its two bottom arguments.
      const uint32_t count = d_.operandCount & ~2u;
      bool horizontal = op == kHvcurveto;
      for (uint32_t i = d_.operandCount - count; i < count; i += 4) {
        int32_t x1 = current.x;
        int32_t y1 = current.y;
        int32_t x2 = 0;
        int32_t y2 = 0;
        int32_t x3 = 0;
        int32_t y3 = 0;
        if (horizontal) {
          x1 = add32(real(i), current.x);
          x2 = add32(real(i + 1), x1);
          y2 = add32(real(i + 2), y1);
          y3 = add32(real(i + 3), y2);
          x3 = x2;
          if (count - i == 5) {
            x3 = add32(real(i + 4), x2);
            ++i;
          }
        } else {
          y1 = add32(real(i), current.y);
          x2 = add32(real(i + 1), x1);
          y2 = add32(real(i + 2), y1);
          x3 = add32(real(i + 3), x2);
          y3 = y2;
          if (count - i == 5) {
            y3 = add32(real(i + 4), y2);
            ++i;
          }
        }
        horizontal = !horizontal;
        curveTo(x1, y1, x2, y2, x3, y3);
        current = {x3, y3};
      }
      break;
    }

    case kCallsubr:
      callSubr(localSubrs_, localBias_);
      return;
    case kCallgsubr:
      callSubr(face_.cff.globalSubrs, globalBias_);
      return;
    case kReturn:
      if (d_.depth < 1) {
        fail(Status::Malformed);  // no return from the glyph's own charstring
        return;
      }
      d_.region = d_.callers[--d_.depth];
      return;

    case kEscape:
      escape(readByte());
      return;
    case kEndchar:
      endchar();
      return;
    case kHintmask:
    case kCntrmask:
      hintMask(op == kCntrmask);
      break;

    case kShortint: {
      const int32_t high = readByte();
      const int32_t low = readByte();
      pushInt(static_cast<int16_t>(high << 8 | low));
      return;
    }
    default:
      if (op >= 32) {
        number(op);
        return;
      }
      break;  // reserved, Type 1 only (closepath, hsbw) or CFF2 only (vsindex, blend): ignored
  }
  clear();
}

void Interpreter::number(const uint8_t op) {
  if (op <= 246) {
    pushInt(op - 139);
  } else if (op <= 250) {
    pushInt((op - 247) * 256 + readByte() + 108);
  } else if (op <= 254) {
    pushInt(-((op - 251) * 256 + readByte()) - 108);
  } else {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value = value << 8 | readByte();
    pushFixed(static_cast<int32_t>(value));  // 16.16
  }
}

void Interpreter::escape(const uint8_t op) {
  switch (op) {
    case kHflex:
      flex(kHflexReads, false);
      return;
    case kFlex:
      flex(kFlexReads, false);
      break;
    case kHflex1:
      flex(kHflex1Reads, false);
      return;
    case kFlex1:
      flex(kFlex1Reads, true);
      return;

    case kAnd: {
      const int32_t b = popFixed();
      const int32_t a = popFixed();
      pushInt(a && b);
      return;
    }
    case kOr: {
      const int32_t b = popFixed();
      const int32_t a = popFixed();
      pushInt(a || b);
      return;
    }
    case kNot:
      pushInt(!popFixed());
      return;
    case kAbs: {
      const int32_t a = popFixed();
      pushFixed(a == INT32_MIN ? INT32_MAX : (a < 0 ? -a : a));
      return;
    }
    case kAdd: {
      const int32_t b = popFixed();
      const int32_t a = popFixed();
      pushFixed(add32(a, b));
      return;
    }
    case kSub: {
      const int32_t b = popFixed();
      const int32_t a = popFixed();
      pushFixed(sub32(a, b));
      return;
    }
    case kDiv: {
      const int32_t divisor = popFixed();
      const int32_t dividend = popFixed();
      pushFixed(divFix(dividend, divisor));
      return;
    }
    case kNeg: {
      const int32_t a = popFixed();
      pushFixed(a == INT32_MIN ? INT32_MAX : -a);
      return;
    }
    case kEq: {
      const int32_t b = popFixed();
      const int32_t a = popFixed();
      pushInt(a == b);
      return;
    }
    case kCallothersubr:
    case kPop:
      return;  // Type 1 only, and unlike the others cf2 passes them over without clearing the stack
    case kDrop:
      popFixed();
      return;
    case kPut: {
      const uint32_t index = static_cast<uint32_t>(popInt());
      const int32_t value = popFixed();
      if (index < kCffStorageSize) d_.storage[index] = value;
      return;
    }
    case kGet: {
      const uint32_t index = static_cast<uint32_t>(popInt());
      if (index < kCffStorageSize) pushFixed(d_.storage[index]);
      return;
    }
    case kIfelse: {
      const int32_t condition2 = popFixed();
      const int32_t condition1 = popFixed();
      const int32_t b = popFixed();
      const int32_t a = popFixed();
      pushFixed(condition1 <= condition2 ? a : b);
      return;
    }
    case kRandom:
      fail(Status::Unsupported);  // FreeType seeds it from memory addresses: no outline to match
      return;
    case kMul: {
      const int32_t b = popFixed();
      const int32_t a = popFixed();
      pushFixed(mulFix(a, b));
      return;
    }
    case kSqrt: {
      const int32_t a = popFixed();
      pushFixed(a > 0 ? static_cast<int32_t>(sqrtFixed(static_cast<uint32_t>(a))) : 0);
      return;
    }
    case kDup: {
      const int32_t a = popFixed();
      pushFixed(a);
      pushFixed(a);
      return;
    }
    case kExch: {
      const int32_t b = popFixed();
      const int32_t a = popFixed();
      pushFixed(b);
      pushFixed(a);
      return;
    }
    case kIndex: {
      // From the top: below 0 the top itself, past the bottom the bottom.
      const int32_t index = popInt();
      const uint32_t size = d_.operandCount;
      if (size > 0) {
        const uint32_t from = index < 0                              ? size - 1
                              : static_cast<uint32_t>(index) >= size ? 0
                                                                     : size - 1 - static_cast<uint32_t>(index);
        pushFixed(real(from));
      }
      return;
    }
    case kRoll: {
      const int32_t shift = popInt();
      const int32_t count = popInt();
      roll(count, shift);
      return;
    }
    default:
      break;  // dotsection, reserved, or Type 1 only (vstem3, hstem3, seac, sbw, setcurrentpoint): ignored
  }
  clear();
}

// cf2_doStems: a stem per pair of arguments, after the width when their count is odd.
void Interpreter::doStems() {
  d_.stems += d_.operandCount / 2u;
  clear();
}

// hintmask and cntrmask: arguments are an implied vstemhm, then a bit per stem follows, rounded up to whole bytes
// (cf2_hintmask_read). With a mask already in force, a mask with arguments is ignored, its bytes unread.
void Interpreter::hintMask(const bool counter) {
  if (d_.operandCount > 1 && d_.maskValid) return;
  doStems();
  if (d_.stems > kMaxHints) {
    fail(Status::Malformed);  // cf2_hintmask_setCounts: Invalid_Glyph_Format
    return;
  }
  const uint32_t maskBytes = (d_.stems + 7) / 8;
  const uint32_t room = d_.region.end - d_.region.at;
  d_.region.at += maskBytes < room ? maskBytes : room;
  // A counter mask builds a counter hint map from its own mask, which hints alone use.
  if (!counter) {
    d_.maskValid = true;
    d_.maskNew = true;
  }
}

// callsubr and callgsubr: 16 levels below the glyph's charstring (cf2_arrstack_getPointer refuses the 17th), and a
// subroutine number that, biased, is in the INDEX. The subroutine is found from its two offsets (cffSubroutine), a
// constant cost per call: the face's subroutine INDEXes are plain (readCffTable), so this is FreeType's own table
// (cff_index_get_pointers), and the operator budget bounds the reads.
void Interpreter::callSubr(const CffIndex& subrs, const uint32_t bias) {
  if (d_.depth >= kCffMaxSubrDepth) {
    fail(Status::Malformed);
    return;
  }
  const int32_t number = popInt();
  if (!ok()) return;
  const uint32_t index = static_cast<uint32_t>(number + static_cast<int32_t>(bias));
  uint32_t offset = 0;
  uint32_t length = 0;
  if (!cffSubroutine(bytes_, subrs, index, &offset, &length)) {
    fail(Status::Malformed);
    return;
  }
  d_.callers[d_.depth++] = d_.region;
  d_.region = {offset, offset + length};
}

// cf2_doFlex: two curves from the deltas `reads` names; flex1 takes its last delta on the axis the first five travel
// further along (strictly), and every flex ends where the others' shapes say. The depth argument is never read.
void Interpreter::flex(const bool (&reads)[12], const bool conditionalLast) {
  const Point current = d_.current;
  int32_t vals[14];
  vals[0] = current.x;
  vals[1] = current.y;
  uint32_t index = 0;
  const bool horizontal = !reads[9];
  const int top = horizontal ? 9 : 10;
  for (int i = 0; i < top; ++i) {
    vals[i + 2] = vals[i];
    if (reads[i]) vals[i + 2] = add32(vals[i + 2], real(index++));
  }
  if (horizontal) vals[11] = current.y;
  if (conditionalLast) {
    const bool lastIsX = abs32(sub32(vals[10], current.x)) > abs32(sub32(vals[11], current.y));
    const int32_t last = real(index);
    vals[12] = lastIsX ? add32(vals[10], last) : current.x;
    vals[13] = lastIsX ? current.y : add32(vals[11], last);
  } else {
    vals[12] = reads[10] ? add32(vals[10], real(index++)) : current.x;
    vals[13] = reads[11] ? add32(vals[11], real(index)) : current.y;
  }
  for (int j = 0; j < 2; ++j) {
    curveTo(vals[j * 6 + 2], vals[j * 6 + 3], vals[j * 6 + 4], vals[j * 6 + 5], vals[j * 6 + 6], vals[j * 6 + 7]);
  }
  clear();
  d_.current = {vals[12], vals[13]};
}

// endchar, given or at the end of the charstring: the open path closes; four more arguments make it seac, which
// draws its accent at (adx, ady), then its base, each as a fresh charstring with the glyph's local subroutines
// (cf2_getSeacComponent; a nested seac fails).
void Interpreter::endchar() {
  const uint32_t count = d_.operandCount;
  closeOpenPath();
  if (count <= 1) {
    if (d_.phase == kAccent) {
      startComponent(d_.seacBase, 0, 0, kBase);
    } else {
      finished_ = true;
    }
    return;
  }
  if (d_.phase != kGlyph) {
    fail(Status::Malformed);  // nested seac
    return;
  }
  const int32_t accent = popInt();
  const int32_t base = popInt();
  const int32_t y = popFixed();
  const int32_t x = popFixed();
  if (!ok()) return;
  d_.seacBase = base;
  startComponent(accent, x, y, kAccent);
}

void Interpreter::startComponent(const int32_t code, const int32_t x, const int32_t y, const uint8_t phase) {
  const int32_t glyph = cffStandardCharGlyph(bytes_, face_, code);
  uint32_t offset = 0;
  uint32_t length = 0;
  if (glyph < 0 || !cffIndexItem(bytes_, face_.cff.charStrings, static_cast<uint32_t>(glyph), &offset, &length)) {
    fail(Status::Malformed);  // cff_lookup_glyph_by_stdcharcode: no glyph for the code
    return;
  }
  d_.phase = phase;
  begin({offset, offset + length}, x, y);
}

// --- The glyph path ---
//
// cf2_glyphpath without hints or darkening: offsets are 0 and every hint map maps y by the same scale, so an element
// always joins the next (no intersection, no connecting line) and the first and current maps draw alike. What is left
// is the queue of one element, which delays each element's output until the next one comes; the moves, which reach
// the builder when a subpath first draws; and the hint state, which decides when FreeType counts the stems and when a
// line of no length is still passed on. Device space is 16.16 at 1/64: FT_MulFix(x, 0x400) (cf2_glyphpath_hintPoint
// with outerTransform the identity and no translation).

Point toDevice(const Point p) { return {mulFix(p.x, kUnitScale), mulFix(p.y, kUnitScale)}; }

// cf2_hintmap_build, for what an unhinted outline sees of it: a map without a mask makes the mask of every stem
// (cf2_hintmask_setAll), which more than 96 stems fail; then the map is valid and the mask no longer new.
void Interpreter::buildMap() {
  if (d_.stems > kMaxHints) {
    fail(Status::Malformed);
    return;
  }
  d_.maskValid = true;
  d_.maskNew = false;
  d_.mapValid = true;
}

// cf2_glyphpath_moveTo: closes the open subpath and starts the next one where it is; the builder hears of it only
// when the subpath draws.
void Interpreter::moveTo() {
  closeOpenPath();
  d_.currentCS = d_.current;
  d_.start = d_.current;
  if (!d_.mapValid || d_.maskNew) buildMap();
}

// cf2_glyphpath_lineTo: a line of no length is dropped, unless a new hint mask came before it (and it is not the
// closing line).
void Interpreter::lineTo(const int32_t x, const int32_t y) {
  const bool newHintMap = d_.maskNew && !d_.pathClosing;
  if (d_.currentCS.x == x && d_.currentCS.y == y && !newHintMap) return;
  if (!d_.pathOpen) {
    pushMove(d_.currentCS);
    d_.pathOpen = true;
  }
  if (d_.queuedOp != kQueuedNone) pushQueued();
  d_.queuedOp = kQueuedLine;
  d_.queued[0] = {x, y};
  if (newHintMap) buildMap();
  d_.currentCS = {x, y};
}

// cf2_glyphpath_curveTo: curves are kept whatever their length.
void Interpreter::curveTo(const int32_t x1, const int32_t y1, const int32_t x2, const int32_t y2, const int32_t x3,
                          const int32_t y3) {
  if (!d_.pathOpen) {
    pushMove(d_.currentCS);
    d_.pathOpen = true;
  }
  if (d_.queuedOp != kQueuedNone) pushQueued();
  d_.queuedOp = kQueuedCurve;
  d_.queued[0] = {x1, y1};
  d_.queued[1] = {x2, y2};
  d_.queued[2] = {x3, y3};
  if (d_.maskNew) buildMap();
  d_.currentCS = {x3, y3};
}

// cf2_glyphpath_closeOpenPath: a closing line to the subpath's start (dropped when of no length), then the queued
// element goes out.
void Interpreter::closeOpenPath() {
  if (!d_.pathOpen) return;
  d_.pathClosing = true;
  lineTo(d_.start.x, d_.start.y);
  if (d_.queuedOp != kQueuedNone) pushQueued();
  d_.pathOpen = false;
  d_.pathClosing = false;
  d_.queuedOp = kQueuedNone;
}

// cf2_glyphpath_pushMove: the move reaches the builder. A path that never moved first builds its hint map here (the
// moveTo cf2 synthesizes; it changes nothing else that lasts).
void Interpreter::pushMove(const Point p0) {
  if (!d_.mapValid) buildMap();
  builderMoveTo();
  d_.currentDS = toDevice(p0);
}

// cf2_glyphpath_pushPrevElem: the queued element goes to the builder. A line of no length in device space does not.
void Interpreter::pushQueued() {
  if (d_.queuedOp == kQueuedLine) {
    const Point p1 = toDevice(d_.queued[0]);
    if (p1.x != d_.currentDS.x || p1.y != d_.currentDS.y) {
      builderLineTo(d_.currentDS, p1);
      d_.currentDS = p1;
    }
    return;
  }
  const Point p3 = toDevice(d_.queued[2]);
  builderCubeTo(d_.currentDS, toDevice(d_.queued[0]), toDevice(d_.queued[1]), p3);
  d_.currentDS = p3;
}

// --- The outline builder ---

bool Interpreter::checkPoints(const uint32_t count) {
  if (uint32_t{out_.numPoints} + count > out_.maxPoints) {
    fail(Status::Unsupported);  // more points than the scratch arrays hold
    return false;
  }
  return true;
}

// ps_builder_add_point: cf2's 16.16 device coordinates at 1/64, `>> 10`, are integer font units.
void Interpreter::addPoint(const Point p, const bool on) {
  FT_Vector& point = out_.points[out_.numPoints];
  point.x = p.x >> 10;
  point.y = p.y >> 10;
  out_.tags[out_.numPoints] = on ? FT_CURVE_TAG_ON : FT_CURVE_TAG_CUBIC;
  ++out_.numPoints;
}

bool Interpreter::addPoint1(const Point p) {
  if (!checkPoints(1)) return false;
  addPoint(p, true);
  return true;
}

// ps_builder_start_point and ps_builder_add_contour: a contour begins with the point the path left from; the one
// before it ends at the last point.
bool Interpreter::startPoint(const Point p) {
  d_.pathBegun = true;
  if (out_.numContours >= out_.maxContours) {
    fail(Status::Unsupported);  // more contours than the scratch arrays hold
    return false;
  }
  if (out_.numContours > 0) out_.contours[out_.numContours - 1] = static_cast<unsigned short>(out_.numPoints - 1);
  ++out_.numContours;
  return addPoint1(p);
}

// cf2_builder_moveTo: two moves in a row close the contour twice.
void Interpreter::builderMoveTo() {
  closeContour();
  d_.pathBegun = false;
}

// cf2_builder_lineTo.
void Interpreter::builderLineTo(const Point p0, const Point p1) {
  if (!d_.pathBegun && !startPoint(p0)) return;
  addPoint1(p1);
}

// cf2_builder_cubeTo: two cubic control points and an on-curve end.
void Interpreter::builderCubeTo(const Point p0, const Point p1, const Point p2, const Point p3) {
  if (!d_.pathBegun && !startPoint(p0)) return;
  if (!checkPoints(3)) return;
  addPoint(p1, false);
  addPoint(p2, false);
  addPoint(p3, true);
}

// ps_builder_close_contour: a contour that got no point is dropped; the last point goes when it is on the curve and
// equal to the first; a contour left with one point is dropped.
void Interpreter::closeContour() {
  const int first = out_.numContours <= 1 ? 0 : out_.contours[out_.numContours - 2] + 1;
  if (out_.numContours > 0 && first == out_.numPoints) {
    --out_.numContours;
    return;
  }
  if (out_.numPoints > 1) {
    const FT_Vector& p1 = out_.points[first];
    const FT_Vector& p2 = out_.points[out_.numPoints - 1];
    if (p1.x == p2.x && p1.y == p2.y && out_.tags[out_.numPoints - 1] == FT_CURVE_TAG_ON) --out_.numPoints;
  }
  if (out_.numContours > 0) {
    if (first == out_.numPoints - 1) {
      --out_.numContours;
      --out_.numPoints;
    } else {
      out_.contours[out_.numContours - 1] = static_cast<unsigned short>(out_.numPoints - 1);
    }
  }
}

}  // namespace

Status runCharString(FontBytes& bytes, const Face& face, const CffGlyphSource& glyph, CffDecoder* decoder,
                     CffOutline* out) {
  out->numPoints = 0;
  out->numContours = 0;
  if (face.outlines != Face::Outlines::Cff) return Status::Malformed;
  Interpreter interpreter(bytes, face, glyph.localSubrs, *decoder, *out);
  Status status = interpreter.run(glyph.offset, glyph.length);
  if (bytes.ioError()) {
    status = Status::Transient;
  } else if (status == Status::Ok && bytes.outOfRange()) {
    status = Status::Malformed;  // a read past the font
  }
  if (status != Status::Ok) {
    out->numPoints = 0;
    out->numContours = 0;
  }
  return status;
}

}  // namespace ttf
