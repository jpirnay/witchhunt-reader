#include "HttpDownloader.h"

#include <Arduino.h>
#include <CrossPointRoots.h>
#include <HalClock.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <base64.h>
#include <esp_heap_caps.h>

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "CrossPointSettings.h"

// All HTTPS runs over the wolfSSL-backed SecureNet stack (verified against the
// curated CrossPointRoots); plain http uses SecureNet's WiFiClient passthrough.
// The former mbedtls/esp_http_client path (with its per-host cert pins and
// crt_bundle workarounds) has been removed — TLS 1.3 support and lower heap use
// were the reasons for the switch. See lib/SecureNet.

namespace {

std::string extractHostFromUrl(const std::string& url) {
  size_t schemeEnd = url.find("://");
  size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
  size_t hostEnd = url.find('/', hostStart);
  if (hostEnd == std::string::npos) hostEnd = url.size();
  return url.substr(hostStart, hostEnd - hostStart);
}

// Clock guard for TLS. The plausibility window and the SNTP retry policy now live in
// HalClock (isPlausibleForTls / ensureUsableForTls) so every TLS entry point shares one rule —
// this used to be a private copy here, which is why the KOReader paths never got it. Returns
// whether full certificate date validation is possible; false means the caller should tolerate
// date errors (and only date errors) for this request.
bool ensureClockForTls() { return HalClock::ensureUsableForTls(SETTINGS.ntpServer); }

// Per-request timeout handed to SecureHttpClient::setTimeout(). 60s gives slow
// servers room to send their first headers; SecureHttpClient reuses it as the
// idle deadline for each body read. The response body streams in
// SecureHttpClient's own READ_CHUNK-sized pieces.
constexpr int HTTP_TIMEOUT_MS = 60000;

struct Sink {
  // Returns false to abort the transfer (e.g. SD write failure or user cancel).
  std::function<bool(const uint8_t*, size_t)> write;
  HttpDownloader::ProgressCallback progress;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Runs once per http call (or once per session for reused sessions): logs
// heap stats and ensures the wall clock is set so TLS cert-date validation
// can succeed. Shared by both TLS backends. Returns false when https was requested and the
// clock could not be established — the caller then permits date errors alone.
bool logPreCallContext(const std::string& url) {
  LOG_DBG("HTTP", "Heap free: %u, largest block: %u", esp_get_free_heap_size(),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
  if (url.compare(0, 8, "https://") == 0) {
    return ensureClockForTls();
  }
  return true;
}

// One-shot streaming GET over SecureNet (wolfSSL). Fills the Sink and emits
// "Phase start"/"Phase open_ok"/"Phase done" heap telemetry. TLS verification
// uses the curated CrossPoint roots; verified-first with insecure fallback
// (except where the caller disables it, e.g. OTA).
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password, Sink& sink, bool allowInsecureFallback) {
  const bool clockReady = logPreCallContext(url);
  const unsigned long startMs = millis();
  LOG_DBG("HTTP", "Phase start @%lums heap=%u largest=%u", millis() - startMs, esp_get_free_heap_size(),
          heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

  crosspoint::SecureHttpClient http;
  http.setCACert(CROSSPOINT_ROOTS_PEM);
  http.setAllowInsecureFallback(allowInsecureFallback);
  http.setAllowCertificateDateErrors(!clockReady);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setUserAgent("WitchReader-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    http.setBasicAuth(username, password);
  }

  bool openLogged = false;
  auto bodySink = [&](const uint8_t* data, size_t len) -> bool {
    if (!openLogged) {
      openLogged = true;
      LOG_DBG("HTTP", "Phase open_ok @%lums heap=%u largest=%u", millis() - startMs, esp_get_free_heap_size(),
              heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    }
    if (!sink.write(data, len)) return false;  // abort
    sink.downloaded += len;
    if (sink.progress && sink.total > 0) {
      if (!sink.progress(sink.downloaded, sink.total)) return false;
    }
    return true;
  };
  auto progress = [&](size_t downloaded, size_t total) -> bool {
    sink.total = total;
    return true;
  };

  const int rc = http.get(url, bodySink, progress);
  // Log the handshake heap trough on EVERY path (incl. early abort via
  // treatAbortAsSuccess) so the TLS-specific low-water is always captured,
  // distinct from the all-time ESP.getMinFreeHeap() figure.
  LOG_DBG("HTTP", "Phase done @%lums rc=%d downloaded=%zu (insecure=%d) handshakeMinFree=%u handshakeMinLargest=%u",
          millis() - startMs, rc, sink.downloaded, static_cast<int>(http.lastConnectionWasInsecure()),
          static_cast<unsigned>(http.lastHandshakeMinFree()), static_cast<unsigned>(http.lastHandshakeMinLargest()));
  if (rc == crosspoint::SecureHttpClient::ERR_ABORTED) {
    return HttpDownloader::ABORTED;
  }
  if (rc < 0) {
    LOG_ERR("HTTP", "SecureNet GET failed: rc=%d url=%s", rc, url.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  if (rc != 200) {
    LOG_ERR("HTTP", "SecureNet unexpected status: %d", rc);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

// Single funnel for every fetchUrl/downloadToFile overload. allowInsecureFallback
// defaults true (browsing paths); OTA passes false to fail closed.
HttpDownloader::DownloadError runGetDispatch(const std::string& url, const std::string& username,
                                             const std::string& password, Sink& sink,
                                             bool allowInsecureFallback = true) {
  return runGetSecure(url, username, password, sink, allowInsecureFallback);
}
}  // namespace

// ---- Session implementation ----

struct HttpDownloader::Session::Impl {
  // SecureNet keeps its own keep-alive connection alive internally (reuses the
  // open SecureClient when host/port match), so the Session just owns one
  // persistent SecureHttpClient across downloadToFile(session, ...) calls.
  std::unique_ptr<crosspoint::SecureHttpClient> http;
  bool preCallLogged = false;
};

HttpDownloader::Session::Session() : impl_(std::make_unique<Impl>()) {}
HttpDownloader::Session::~Session() = default;

namespace {

// SecureNet session GET: reuse one persistent SecureHttpClient. Its internal
// keep-alive reuses the open TLS connection when the host/port match, so
// back-to-back files on the same host share a single handshake (the Session
// heap win). Cross-host requests transparently reopen inside SecureHttpClient.
HttpDownloader::DownloadError runGetSecureOnSession(HttpDownloader::Session& session, const std::string& url,
                                                    const std::string& username, const std::string& password,
                                                    Sink& sink, bool allowInsecureFallback) {
  auto* impl = session.impl();
  // Evaluated on every call, not just when the session is created: a session opened before the
  // clock was set must not keep that verdict for the rest of its life (nor the reverse).
  const bool clockReady = logPreCallContext(url);
  if (!impl->http) {
    impl->http = std::make_unique<crosspoint::SecureHttpClient>();
    impl->http->setCACert(CROSSPOINT_ROOTS_PEM);
    impl->http->setTimeout(HTTP_TIMEOUT_MS);
    impl->http->setUserAgent("WitchReader-ESP32-" CROSSPOINT_VERSION);
  }
  impl->http->setAllowInsecureFallback(allowInsecureFallback);
  impl->http->setAllowCertificateDateErrors(!clockReady);
  impl->http->clearHeaders();
  if (!username.empty() && !password.empty()) {
    impl->http->setBasicAuth(username, password);
  }

  auto bodySink = [&](const uint8_t* data, size_t len) -> bool {
    if (!sink.write(data, len)) return false;
    sink.downloaded += len;
    if (sink.progress && sink.total > 0) {
      if (!sink.progress(sink.downloaded, sink.total)) return false;
    }
    return true;
  };
  auto progress = [&](size_t, size_t total) -> bool {
    sink.total = total;
    return true;
  };

  const int rc = impl->http->get(url, bodySink, progress);
  if (rc == crosspoint::SecureHttpClient::ERR_ABORTED) return HttpDownloader::ABORTED;
  if (rc != 200) {
    LOG_ERR("HTTP", "SecureNet session GET failed: rc=%d url=%s", rc, url.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

HttpDownloader::DownloadError runGetOnSession(HttpDownloader::Session& session, const std::string& url,
                                              const std::string& username, const std::string& password, Sink& sink) {
  return runGetSecureOnSession(session, url, username, password, sink, /*allowInsecureFallback=*/true);
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetDispatch(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  // Preserve historic semantics: callback abort => failure.
  return fetchUrl(url, onData, false, username, password);
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, bool treatAbortAsSuccess,
                              const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Fetching (stream): %s", url.c_str());
  if (!onData) {
    return false;
  }
  Sink sink;
  sink.write = [&onData](const uint8_t* data, size_t len) { return onData(data, len); };
  const DownloadError result = runGetDispatch(url, username, password, sink);
  if (result == OK) {
    return true;
  }
  // Optional mode for parsers that intentionally stop early once they have
  // extracted all required fields from the stream.
  return treatAbortAsSuccess && result == ABORTED;
}

bool HttpDownloader::fetchUrlVerified(const std::string& url, const DataCallback& onData, bool treatAbortAsSuccess,
                                      const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Fetching (stream, verify-only): %s", url.c_str());
  if (!onData) {
    return false;
  }
  Sink sink;
  sink.write = [&onData](const uint8_t* data, size_t len) { return onData(data, len); };
  // allowInsecureFallback=false: fail closed on cert-verify failure (OTA).
  const DownloadError result = runGetDispatch(url, username, password, sink, /*allowInsecureFallback=*/false);
  if (result == OK) {
    return true;
  }
  return treatAbortAsSuccess && result == ABORTED;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetDispatch(url, username, password, sink) == OK;
}

namespace {
// Common file-sink plumbing used by both downloadToFile overloads.
HttpDownloader::DownloadError finishFileDownload(HttpDownloader::DownloadError result, const std::string& destPath,
                                                 FsFile& file, size_t downloaded) {
  // Flush before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.flush();
  file.close();
  if (result != HttpDownloader::OK) {
    Storage.remove(destPath.c_str());
    return result;
  }
  if (downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);
  return HttpDownloader::OK;
}
}  // namespace

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const std::string& username,
                                                             const std::string& password) {
  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing: %s", destPath.c_str());
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGetDispatch(url, username, password, sink);
  return finishFileDownload(result, destPath, file, sink.downloaded);
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(Session& session, const std::string& url,
                                                             const std::string& destPath, ProgressCallback progress,
                                                             const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Downloading (session): %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing: %s", destPath.c_str());
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGetOnSession(session, url, username, password, sink);
  return finishFileDownload(result, destPath, file, sink.downloaded);
}
