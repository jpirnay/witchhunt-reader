// Stub implementations of Logging symbols for host/test builds.
// ENABLE_SERIAL_LOG is not defined, so all LOG_* macros are no-ops and
// logPrintf is never called — but the linker still needs the symbol.
#include <string>

void logPrintf(const char*, const char*, const char*, ...) {}
std::string getLastLogs() { return {}; }
void clearLastLogs() {}
bool sanitizeLogHead() { return false; }
