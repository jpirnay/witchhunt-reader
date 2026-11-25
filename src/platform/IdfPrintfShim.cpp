#include "IdfPrintfShim.h"

#include <cstdarg>
#include <cstring>

namespace idf_printf_shim {

int writeFormatted(FILE* stream, const char* format, va_list args) {
  char line[kLineBytes];
  const int length = vsnprintf(line, sizeof(line), format, args);
  if (length <= 0) return length;
  size_t count = static_cast<size_t>(length);
  if (count >= sizeof(line)) {
    count = sizeof(line) - 1;
    // A cut line keeps its line end, as the firmware's own log lines do (Logging.cpp), so it does not run into the
    // next one. Only a format that ends in one has a line end to keep.
    const size_t formatLength = strlen(format);
    if (formatLength > 0 && format[formatLength - 1] == '\n') line[count - 1] = '\n';
  }
  return static_cast<int>(fwrite(line, 1, count, stream));
}

}  // namespace idf_printf_shim

// The linker sends the prebuilt libraries' calls here (-Wl,--wrap in platformio.ini); nothing in the firmware calls
// these names itself.
extern "C" {

int __wrap_vprintf(const char* format, va_list args) { return idf_printf_shim::writeFormatted(stdout, format, args); }

int __wrap_printf(const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int written = idf_printf_shim::writeFormatted(stdout, format, args);
  va_end(args);
  return written;
}

int __wrap_fprintf(FILE* stream, const char* format, ...) {
  va_list args;
  va_start(args, format);
  const int written = idf_printf_shim::writeFormatted(stream, format, args);
  va_end(args);
  return written;
}

}  // extern "C"
