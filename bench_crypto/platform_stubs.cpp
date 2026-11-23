// Link-time stubs this benchmark needs because it inherits [base]'s flags and lib_deps while
// compiling none of src/ — the definitions the firmware normally supplies live there.
//
// Same job as bench/platform_stubs.c, plus the wolfSSL logging hook: Arduino-wolfSSL's
// logging.c references wolfSSL_Arduino_Serial_Print, which the library's sketch glue would
// normally define and a PlatformIO lib build does not. The firmware defines it in
// lib/SecureNet/src/SecureClient.cpp; here it goes straight to Serial.

#include <Arduino.h>
#include <stdint.h>

#include "esp_err.h"

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);
void __real_panic_arch_fill_info(void* frame, void* info);
void __real_panic_soc_fill_info(void* frame, void* info);

void __wrap_panic_abort(const char* message) { __real_panic_abort(message); }

void __wrap_panic_print_backtrace(const void* frame, int core) { __real_panic_print_backtrace(frame, core); }

void __wrap_panic_arch_fill_info(void* frame, void* info) { __real_panic_arch_fill_info(frame, info); }

void __wrap_panic_soc_fill_info(void* frame, void* info) { __real_panic_soc_fill_info(frame, info); }

esp_err_t __wrap_bootloader_common_check_efuse_blk_validity(uint32_t min_rev_full, uint32_t max_rev_full) {
  (void)min_rev_full;
  (void)max_rev_full;
  return 0;  // ESP_OK
}

// Signature must match wolfcrypt/logging.h exactly (int return).
int wolfSSL_Arduino_Serial_Print(const char* const s) {
  if (s) Serial.println(s);
  return 0;
}

}  // extern "C"
