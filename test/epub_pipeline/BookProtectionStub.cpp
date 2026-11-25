// lib/BookProtection's entry points for the pipeline harness, which builds Epub without the key
// store, NVS and AES: every book reads as plain. EpubProtectedPipelineTest links the real ones
// (ProtectedBookPipelineTest.cpp).
#include <BookSession.h>
#include <CacheAtRest.h>
#include <ProtectionManifest.h>

namespace book_protection {
OpenResult openBook(const std::string&, const std::string&, uint64_t, bool, BuildArena*) { return OpenResult::Plain; }
void closeBook(const std::string&) {}
std::string manifestPath(const std::string& cacheDir) { return cacheDir + "/protect.bin"; }
// Every book is plain here, so nothing calls it; a test that marks a folder itself does so through Storage.
bool encipherCache(const std::string&, const std::string&) { return true; }
ManifestProbe probeManifest(const std::string&) {
  ManifestProbe probe;
  probe.status = ManifestStatus::Plain;
  return probe;
}
}  // namespace book_protection
