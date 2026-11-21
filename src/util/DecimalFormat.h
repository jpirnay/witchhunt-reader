#pragma once

#include <cstddef>
#include <cstdint>

// num / den written as printf("%.<places>f") writes it, from integers: for a value kept in fixed point (thousandths of
// g, a ratio of seconds), so the text costs no float on the C3, which has no FPU, and no double through newlib's
// printf. Rounded half away from zero, where printf rounds the double's exact binary value half to even: the two can
// differ only on an exact tie. A negative value keeps its '-' when it rounds to zero, as printf writes -0.004 as
// "-0.00"; `plus` writes '+' on the others, like "%+.2f". 0 < den < 2^40 and places <= 6.
int formatDecimal(char* buf, size_t size, int64_t num, int64_t den, int places, bool plus = false);
