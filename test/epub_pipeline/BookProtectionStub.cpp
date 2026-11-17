// lib/BookProtection's entry points for the pipeline harness, which builds Epub without the key
// store, NVS and AES: every book reads as plain. A protected book's run through the whole pipeline
// is still to be written (docs/future_work/protected-content.md, D1 step 7).
#include <BookSession.h>
#include <ProtectionManifest.h>

namespace book_protection {
OpenResult openBook(const std::string&, const std::string&, uint64_t, bool, BuildArena*) { return OpenResult::Plain; }
void closeBook(const std::string&) {}
std::string manifestPath(const std::string& cacheDir) { return cacheDir + "/protect.bin"; }
ManifestProbe probeManifest(const std::string&) {
  ManifestProbe probe;
  probe.status = ManifestStatus::Plain;
  return probe;
}
}  // namespace book_protection
