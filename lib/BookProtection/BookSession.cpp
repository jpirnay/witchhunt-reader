#include "BookSession.h"

#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <ZipFile.h>

#include <cstring>
#include <memory>

#include "BookKey.h"
#include "ProtectionManifest.h"

namespace book_protection {
namespace {

// A loan's state against the loan clock (HalClock::loanNow(), which a clock set back cannot move
// back). `expires`: epoch seconds, 0 for no due date. A clock never set cannot judge a date.
OpenResult loanState(const int64_t expires) {
  if (expires == 0) return OpenResult::Protected;
  const int64_t now = HalClock::loanNow();
  if (now == 0) return OpenResult::NeedsTime;
  return now > expires ? OpenResult::Expired : OpenResult::Protected;
}

const char* describe(const OpenResult result) {
  switch (result) {
    case OpenResult::Plain:
      return "plain";
    case OpenResult::Protected:
      return "protected";
    case OpenResult::NoKey:
      return "no key";
    case OpenResult::OtherDevice:
      return "key not for this device";
    case OpenResult::Expired:
      return "loan expired";
    case OpenResult::NeedsTime:
      return "loan needs the time";
    case OpenResult::Unreadable:
      return "unreadable";
  }
  return "?";
}

}  // namespace

std::string manifestPath(const std::string& cacheDir) { return cacheDir + "/protect.bin"; }

OpenResult openBook(const std::string& epubPath, const std::string& cacheDir, const uint64_t fingerprint,
                    const bool build, BuildArena* scratch) {
  uint8_t key[bookkey::MAX_KEY_LEN] = {};
  int64_t expires = 0;
  const size_t keyLen = bookkey::read(epubPath, key, &expires);
  const std::string manifest = manifestPath(cacheDir);
  std::unique_ptr<ZipFile::Protection> protection;
  ManifestStatus status = openManifest(manifest, fingerprint, key, keyLen, &protection);

  OpenResult result = OpenResult::Unreadable;
  if (status == ManifestStatus::Plain) {
    result = OpenResult::Plain;
  } else if (keyLen == 0) {
    // Without a key a missing manifest is built only when there is nothing to decrypt.
    if (status == ManifestStatus::Missing && probeManifest(epubPath).status == ManifestStatus::Plain) {
      result = buildManifest(epubPath, fingerprint, nullptr, 0, manifest, scratch) == ManifestStatus::Plain
                   ? OpenResult::Plain
                   : OpenResult::Unreadable;
    } else {
      result = Storage.exists(bookkey::keyPath(epubPath).c_str()) ? OpenResult::OtherDevice : OpenResult::NoKey;
    }
  } else if ((result = loanState(expires)) == OpenResult::Protected) {
    if (status == ManifestStatus::Missing && build) {
      status = buildManifest(epubPath, fingerprint, key, keyLen, manifest, scratch);
      if (status == ManifestStatus::Protected) status = openManifest(manifest, fingerprint, key, keyLen, &protection);
    }
    if (status == ManifestStatus::Plain) {
      result = OpenResult::Plain;
    } else if (status != ManifestStatus::Protected || !ZipFile::protect(epubPath, std::move(protection))) {
      result = OpenResult::Unreadable;
    }
  }
  memset(key, 0, sizeof(key));
  if (result != OpenResult::Plain) LOG_INF("PRT", "%s: %s", epubPath.c_str(), describe(result));
  return result;
}

void closeBook(const std::string& epubPath) { ZipFile::unprotect(epubPath); }

}  // namespace book_protection
