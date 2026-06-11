// Stub logging symbols for ZipFile host test.
// ENABLE_SERIAL_LOG is not defined so LOG_* macros are no-ops;
// logPrintf is never called but the linker requires the symbol.
#include <cstdint>
#include <string>

void logPrintf(const char*, const char*, const char*, ...) {}
std::string getLastLogs() { return {}; }
void clearLastLogs() {}
bool sanitizeLogHead() { return false; }

// uzlib_uncompress_chksum (compiled into tinflate.o but never called by
// InflateReader) references these checksum helpers. Provide stubs so the
// host test links without pulling in a full checksum implementation.
extern "C" {
uint32_t uzlib_adler32(const void*, unsigned int, uint32_t prev) { return prev; }
uint32_t uzlib_crc32(const void*, unsigned int, uint32_t crc) { return crc; }
}
