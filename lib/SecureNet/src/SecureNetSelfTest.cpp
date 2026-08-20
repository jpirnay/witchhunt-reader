// Optional boot-time self-test to (a) force wolfSSL to link so PR1 can MEASURE
// its real flash cost, and (b) sanity-check a verified TLS 1.3 GET to each
// first-party host once WiFi is up. Compiled only under -DSECURENET_SELFTEST;
// it is NOT part of the shipping firmware and has no callers otherwise.
#if defined(SECURENET_SELFTEST)

#include <HalClock.h>
#include <Logging.h>

#include <ctime>

#include "CrossPointRoots.h"
#include "SecureHttpClient.h"

namespace crosspoint {

void secureNetSelfTest();

}  // namespace crosspoint

// C-linkage anchor so the measurement env can force-keep this TU via
// -Wl,--undefined=secureNetSelfTest_anchor without name mangling.
extern "C" void secureNetSelfTest_anchor() { crosspoint::secureNetSelfTest(); }

namespace crosspoint {

void secureNetSelfTest() {
  static const char* kHosts[] = {
      "https://api.github.com/repos/jpirnay/witchhunt-reader/releases/latest",
      "https://sync.koreader.rocks/",
      "https://timeapi.io/api/time/current/zone?timeZone=UTC",
  };
  // The measurement env can be run straight off a cold boot, where an RTC-less board sits at
  // 1970 and the curated roots fail to LOAD (their notBefore reads as "in the future"). Without
  // this every host below reports a connect failure that says nothing about reachability — the
  // thing this self-test exists to measure.
  const bool clockReady = HalClock::ensureUsableForTls();
  if (!clockReady) {
    LOG_INF("SNTST", "No trusted clock (epoch %lld); tolerating certificate date errors only",
            static_cast<long long>(time(nullptr)));
  }

  for (const char* url : kHosts) {
    SecureHttpClient http;
    http.setCACert(CROSSPOINT_ROOTS_PEM);
    http.setAllowCertificateDateErrors(!clockReady);
    http.setTimeout(15000);
    const int status = http.GET(url);
    LOG_INF("SNTST", "GET %s -> status=%d insecure=%d bodyLen=%u", url, status,
            static_cast<int>(http.lastConnectionWasInsecure()), static_cast<unsigned>(http.getBody().size()));
    http.close();
  }
}

}  // namespace crosspoint

#endif  // SECURENET_SELFTEST
