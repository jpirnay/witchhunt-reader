#pragma once

#include <cstddef>
#include <cstdint>

// Decimal fixed point: an integer that counts 10^-decimals of a unit (tenths of a degree, millionths of a degree).
// The C3 has no FPU, so the firmware keeps such values in integers (docs/design/integer-math.md); these turn them
// into text and back, and take in a float that a library hands over (ArduinoJson's numbers) once, where it arrives.
// Values are int32; decimals go up to 9.
namespace fixed_decimal {

// How a value exactly halfway between two results rounds. AwayFromZero is lround's rule; ToEven is what printf's
// "%.*f" does with a value it holds exactly.
enum class Tie : uint8_t { AwayFromZero, ToEven };

constexpr int32_t pow10(const unsigned n) {
  int32_t p = 1;
  for (unsigned i = 0; i < n; i++) p *= 10;
  return p;
}

// `value`, in 10^-from units, in 10^-to units (to <= from), rounded to nearest; a tie as `tie` says.
int32_t rescale(int32_t value, unsigned from, unsigned to, Tie tie);

// `value`, in 10^-decimals units, with `shown` decimals (shown <= decimals): "-12.35". Like printf's "%.*f" of the
// value, also in keeping the sign of a negative value that rounds to zero ("-0.0"). Returns what snprintf returns.
int format(char* out, size_t outSize, int32_t value, unsigned decimals, unsigned shown, Tie tie);

// "[spaces][+|-]digits[.digits]" (digits on at least one side of the point) in 10^-decimals units, rounded to nearest
// past the last kept decimal, a tie away from zero. False for anything else, exponents included, or past int32.
bool parse(const char* text, unsigned decimals, int32_t& out);

// The float `value` in 10^-decimals units, rounded to nearest, a tie as `tie` says. Worked out from the float's bits,
// so exact and without soft-float: value * 10^6 for a latitude needs 44 bits, which a float product would round to a
// multiple of 4. 0 for NaN; held to the int32 range.
int32_t fromFloat(float value, unsigned decimals, Tie tie);

}  // namespace fixed_decimal
