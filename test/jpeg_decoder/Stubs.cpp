// Stubs for device-only symbols referenced by JpegToBmpConverter.cpp / BitmapHelpers.cpp.
// Compiled only for host unit tests.
#include <cstdarg>
#include <cstdint>
#include <string>

// Logging (LOG_* macros expand to logPrintf at the build's LOG_LEVEL).
void logPrintf(const char*, const char*, const char*, ...) {}
std::string getLastLogs() { return {}; }
void clearLastLogs() {}
bool sanitizeLogHead() { return false; }

// Note: `ESP` (heap accessor) is provided as an inline global by the HalDisplay.h
// stub in this directory, since the converter reaches it through that header.
