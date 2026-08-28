#pragma once

// WitchReader SecureNet — minimal keep-alive HTTP/1.1 over SecureClient.
//
// Adapted from Free-Ink/freeink-sdk (MIT) SecureHttpClient and crosspoint-reader
// PR #2475, extended for WitchReader with: keep-alive session reuse, redirect
// following, streaming (non-buffering) body sink, HTTP basic auth, and plain-http
// passthrough (a plain WiFiClient transport when the scheme is http). It does NOT
// wrap Arduino HTTPClient (which binds a NetworkClient, incompatible with our
// SecureClient's owned-WiFiClient design).
//
// This is the single HTTP engine behind HttpDownloader and KOReaderSync now that
// the mbedtls/esp_http_client stack has been removed.
//
// OPT-IN: https requires -DFREEINK_NET_WOLFSSL=1. With the flag off, https
// connect() fails; plain-http still works over the WiFiClient transport.

#include <Arduino.h>
#include <WiFiClient.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "SecureClient.h"

namespace crosspoint {

class SecureHttpClient {
 public:
  // Body sink: called with each decoded body chunk; return false to abort.
  using BodySink = std::function<bool(const uint8_t* data, size_t len)>;
  // Progress: (downloaded, total); total==0 when Content-Length is unknown.
  // Return false to abort.
  using ProgressFn = std::function<bool(size_t downloaded, size_t total)>;

  SecureHttpClient() = default;
  ~SecureHttpClient() { close(); }

  SecureHttpClient(const SecureHttpClient&) = delete;
  SecureHttpClient& operator=(const SecureHttpClient&) = delete;

  // --- configuration (before request) ---
  void setCACert(const char* rootCA) { _rootCA = rootCA; }
  void setAllowInsecureFallback(bool allow) { _allowInsecureFallback = allow; }
  // See SecureClient::setAllowCertificateDateErrors().
  void setAllowCertificateDateErrors(bool allow) { _allowCertificateDateErrors = allow; }
  void setTimeout(uint32_t ms) { _timeoutMs = ms; }
  void setUserAgent(const std::string& ua) { _userAgent = ua; }
  void setBasicAuth(const std::string& user, const std::string& pass) {
    _user = user;
    _pass = pass;
  }
  // Per-request custom headers (cleared after each request unless keep()).
  void addHeader(const std::string& name, const std::string& value) { _headers.push_back(name + ": " + value); }
  void clearHeaders() { _headers.clear(); }
  // Max redirect hops to follow (default 5; 0 disables following).
  void setMaxRedirects(int n) { _maxRedirects = n; }
  // Allow a redirect to step down from https to http. Off by default, because
  // the downgrade silently drops transport security; when refused, following
  // stops and the caller sees the 3xx status.
  void setAllowRedirectDowngrade(bool allow) { _allowRedirectDowngrade = allow; }

  // --- requests ---
  // Streaming GET: body is delivered to sink in chunks. Returns the final HTTP
  // status (after redirects), or a negative SecureHttpError on transport failure.
  int get(const std::string& url, const BodySink& sink, const ProgressFn& progress = nullptr);

  // Buffered request (GET/POST/PUT): full body accumulated into getBody(). Use
  // only for small responses (KOSync JSON). Returns HTTP status or negative error.
  int request(const char* method, const std::string& url, const std::string& body = "");
  int GET(const std::string& url) { return request("GET", url); }
  int POST(const std::string& url, const std::string& body) { return request("POST", url, body); }
  int PUT(const std::string& url, const std::string& body) { return request("PUT", url, body); }

  const std::string& getBody() const { return _body; }
  int lastStatus() const { return _status; }
  bool lastConnectionWasInsecure() const { return _lastInsecure; }
  // Heap trough sampled across the last TLS handshake (see SecureClient). Only
  // meaningful for https requests; SIZE_MAX if no https handshake occurred.
  size_t lastHandshakeMinFree() const { return _secure.handshakeMinFree(); }
  size_t lastHandshakeMinLargest() const { return _secure.handshakeMinLargest(); }

  // Close the held-open keep-alive connection (if any).
  void close();

  // Negative return codes from get()/request() (distinct from HTTP status).
  enum SecureHttpError {
    ERR_BAD_URL = -1,
    ERR_CONNECT = -2,
    ERR_SEND = -3,
    ERR_TIMEOUT = -4,
    ERR_TOO_MANY_REDIRECTS = -5,
    ERR_ABORTED = -6,
    ERR_TRUNCATED = -7,
  };

 private:
  struct Url {
    std::string scheme;
    std::string host;
    std::string path;
    uint16_t port = 0;
    bool https() const { return scheme == "https"; }
  };
  static bool parseUrl(const std::string& in, Url& out);
  static std::string resolveRedirect(const Url& base, const std::string& location);

  // Per-request trace for the half of a request that was never instrumented.
  //
  // SecureClient breaks the handshake down to the certificate; everything after it was a single
  // elapsed number, so a 5 s ERR_TIMEOUT could not say whether the request left the device,
  // whether bytes came back and we failed to read them, or whether the peer simply never
  // answered. Each field exists to separate one of those:
  //   sendMs/sendBytes   the request actually went out, and how long the socket took to take it
  //   firstByteMs        0 means NOTHING ever arrived -- silence, not a parse problem
  //   readPolls          2 ms idle polls spent waiting; high with firstByteMs 0 is a dead wait
  //   headerBytes        bytes that did arrive, so a partial response is distinguishable
  // Paired with rssi and the modem-sleep mode, which is the other thing that was reasoned about
  // rather than measured.
  struct RequestTrace {
    uint32_t startMs = 0;
    uint32_t sendMs = 0;
    size_t sendBytes = 0;
    uint32_t firstByteMs = 0;
    uint32_t readPolls = 0;
    size_t headerBytes = 0;
    uint8_t attempts = 0;
    bool reusedConnection = false;
  };
  RequestTrace _trace;
  void logRequestTrace(const char* method, const Url& u, int rc) const;

  // Response status line + header fields that drive body framing and reuse.
  struct ResponseMeta {
    int status = 0;
    long contentLength = -1;
    bool chunked = false;
    bool keepAlive = true;
    std::string location;
  };

  // True when the kept-open connection matches (scheme,host,port) and still
  // looks alive. "Looks" is best-effort: the server may have closed it already,
  // which transact() handles with its one-shot retry.
  bool connectionMatches(const Url& u);
  // Ensure a live connection to (scheme,host,port); reuse the kept-open one when
  // it matches, else (re)open. Returns false on connect failure.
  bool ensureConnected(const Url& u);
  // Connect (or reuse), send the request, and read the response headers — with
  // one transparent retry when a reused keep-alive socket turns out to be dead.
  // Returns 0, or a negative SecureHttpError.
  int transact(const char* method, const Url& u, const uint8_t* body, size_t bodyLen, ResponseMeta& meta);
  // Send the request line + headers (+ body for POST/PUT). Returns false on write failure.
  bool sendRequest(const char* method, const Url& u, const uint8_t* body, size_t bodyLen);
  // Read status line + headers. Fills status, and out params for body framing.
  bool readHeaders(int& status, long& contentLength, bool& chunked, bool& keepAlive, std::string& location);
  // Stream the body per framing to sink (with progress). Returns 0 on success,
  // or a negative SecureHttpError.
  int readBody(const BodySink& sink, const ProgressFn& progress, long contentLength, bool chunked, bool keepAlive);

  bool readLine(std::string& line, uint32_t deadline);

  // Transport: one of these is active per connection depending on scheme.
  SecureClient _secure;
  WiFiClient _plain;
  Client* _client = nullptr;  // -> _secure or _plain while connected
  bool _connectedHttps = false;
  std::string _connHost;
  uint16_t _connPort = 0;

  // config
  const char* _rootCA = nullptr;
  bool _allowInsecureFallback = true;
  bool _allowCertificateDateErrors = false;
  uint32_t _timeoutMs = 15000;
  std::string _userAgent = "CrossPoint-ESP32";
  std::string _user;
  std::string _pass;
  std::vector<std::string> _headers;
  int _maxRedirects = 5;
  bool _allowRedirectDowngrade = false;

  // per-request result
  std::string _body;
  int _status = 0;
  bool _lastInsecure = false;

  static constexpr size_t MAX_LINE = 4096;
  // Body read buffer. 2 KB (was 512) so we drain wolfSSL's decrypted TLS records
  // in far fewer read() calls — 512 throttled large downloads to ~30 KB/s and
  // let slow CDNs (Cloudflare) drop the connection mid-stream. Stack-allocated,
  // so kept modest. Matches the legacy esp_http_client READ_CHUNK.
  static constexpr size_t READ_CHUNK = 2048;
};

}  // namespace crosspoint
