#include "SecureClient.h"

#include <Logging.h>
#include <esp_heap_caps.h>

// wolfSSL is only pulled in when explicitly enabled, keeping flag-off builds
// (and host tests) free of the dependency. See SecureClient.h.
#if defined(FREEINK_NET_WOLFSSL)
#include <wolfssl/error-ssl.h>  // VERIFY_CERT_ERROR, DOMAIN_NAME_MISMATCH (SSL-layer codes)
#include <wolfssl/ssl.h>

#include <ctime>  // time() for the clock reported alongside a cert-date load failure

// The Arduino-wolfSSL library's logging.c references this hook, normally defined
// in the library's wolfssl.h sketch glue (not compiled in a PlatformIO lib build),
// so we provide it. Signature must match wolfcrypt/logging.h exactly (int return).
// Routes wolfSSL's internal debug logging to our logger.
extern "C" int wolfSSL_Arduino_Serial_Print(const char* const s) {
  if (s) LOG_DBG("WOLF", "%s", s);
  return 0;
}
#endif

namespace crosspoint {

bool SecureClient::tls13Available() {
#if defined(FREEINK_NET_WOLFSSL)
  return true;
#else
  return false;
#endif
}

SecureClient::~SecureClient() { stop(); }

#if defined(FREEINK_NET_WOLFSSL)

namespace {
// Bridge wolfSSL's I/O to the underlying WiFiClient transport. Non-blocking:
// return WANT_READ/WANT_WRITE when the socket has nothing yet so wolfSSL retries.
int wcSend(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
  auto* t = static_cast<WiFiClient*>(ctx);
  const int n = t->write(reinterpret_cast<const uint8_t*>(buf), sz);
  if (n <= 0) {
    // A dead transport must surface as CONN_CLOSE: mapping it to WANT_WRITE
    // makes the handshake spin until the deadline instead of failing fast.
    // (Ported from Free-Ink/freeink-sdk 43132fc.)
    if (!t->connected()) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
    return WOLFSSL_CBIO_ERR_WANT_WRITE;
  }
  return n;
}
int wcRecv(WOLFSSL* /*ssl*/, char* buf, int sz, void* ctx) {
  auto* t = static_cast<WiFiClient*>(ctx);
  if (!t->connected() && t->available() == 0) return WOLFSSL_CBIO_ERR_CONN_CLOSE;
  if (t->available() == 0) return WOLFSSL_CBIO_ERR_WANT_READ;
  const int n = t->read(reinterpret_cast<uint8_t*>(buf), sz);
  if (n <= 0) return WOLFSSL_CBIO_ERR_WANT_READ;
  return n;
}

// True if the wolfSSL error code is a peer-certificate-verification failure
// (as opposed to a transport/protocol failure). Only these justify the
// verified->insecure fallback: retrying insecurely on a DNS/TCP/version error
// gains nothing and would hide the real problem.
bool isVerificationError(int err) {
  switch (err) {
    case ASN_NO_SIGNER_E:    // no trusted root for the chain
    case ASN_SIG_CONFIRM_E:  // signature check failed
    case ASN_BEFORE_DATE_E:  // notBefore in the future (clock)
    case ASN_AFTER_DATE_E:   // expired
    case ASN_SELF_SIGNED_E:
    case CRL_CERT_DATE_ERR:
    case VERIFY_CERT_ERROR:  // generic cert verification failure
    case DOMAIN_NAME_MISMATCH:
      return true;
    default:
      return false;
  }
}

// Installed as wolfSSL's verify callback only when the caller could not obtain a trustworthy
// clock. wolfSSL calls it for every verification failure; accept precisely the two
// validity-window errors and pass everything else (chain, signature, trust anchor, hostname)
// straight through as the failure it is.
int allowCertificateDateErrors(int preverify, WOLFSSL_X509_STORE_CTX* store) {
  if (preverify == 0 && store != nullptr && (store->error == ASN_BEFORE_DATE_E || store->error == ASN_AFTER_DATE_E)) {
    LOG_INF("TLS", "Ignoring certificate date error %d (no trusted clock); chain and hostname still verified",
            store->error);
    return 1;
  }
  return preverify;
}
}  // namespace

// One handshake attempt at a fixed verification level and TLS method.
int SecureClient::connectWithMethod(const char* host, uint16_t port, void* method, const char* label, bool verifyPeer) {
  stop();
  if (!_transport.connect(host, port)) return 0;

  auto* ctx = wolfSSL_CTX_new(static_cast<WOLFSSL_METHOD*>(method));
  if (!ctx) {
    _transport.stop();
    return 0;
  }
  _ctx = ctx;

  if (verifyPeer && !_insecure && _rootCA) {
    // The roots are date-checked AS THEY LOAD, not only during the handshake: plain
    // load_verify_buffer() passes WOLFSSL_LOAD_VERIFY_DEFAULT_FLAGS (== WOLFSSL_LOAD_FLAG_NONE),
    // so a cold-booted RTC-less board sitting at 1970 sees every curated root's notBefore as
    // "in the future" and loads none of them. WOLFSSL_LOAD_FLAG_DATE_ERR_OKAY switches the parse
    // to VERIFY_SKIP_DATE, which suppresses the notBefore/notAfter test and nothing else — the
    // signature and structural checks are unchanged (wolfcrypt/src/asn.c, `cert->badDate`).
    const word32 loadFlags =
        _allowCertificateDateErrors ? WOLFSSL_LOAD_FLAG_DATE_ERR_OKAY : WOLFSSL_LOAD_VERIFY_DEFAULT_FLAGS;
    const int loadRet = wolfSSL_CTX_load_verify_buffer_ex(ctx, reinterpret_cast<const unsigned char*>(_rootCA),
                                                          strlen(_rootCA), WOLFSSL_FILETYPE_PEM, 0, loadFlags);
    if (loadRet != WOLFSSL_SUCCESS) {
      // Report the wolfSSL code and the epoch together. This failure happens before the
      // handshake, so it leaves no verification error for connect() to classify and used to
      // surface as a bare "connect failed" — with -150/-151 and a 1970 timestamp side by side,
      // "the clock is wrong" is readable straight off the log.
      LOG_ERR("TLS", "load_verify_buffer failed for curated roots: err=%d epoch=%lld (dateErrorsAllowed=%d)", loadRet,
              static_cast<long long>(time(nullptr)), _allowCertificateDateErrors ? 1 : 0);
      if (loadRet == ASN_BEFORE_DATE_E || loadRet == ASN_AFTER_DATE_E) {
        LOG_ERR("TLS", "  -> the device clock is outside the roots' validity window; sync NTP before connecting");
      }
      stop();
      return 0;
    }
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER,
                           _allowCertificateDateErrors ? allowCertificateDateErrors : nullptr);
  } else {
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, nullptr);
  }
  wolfSSL_SetIORecv(ctx, wcRecv);
  wolfSSL_SetIOSend(ctx, wcSend);

  auto* ssl = wolfSSL_new(ctx);
  if (!ssl) {
    stop();
    return 0;
  }
  _ssl = ssl;
  wolfSSL_SetIOReadCtx(ssl, &_transport);
  wolfSSL_SetIOWriteCtx(ssl, &_transport);
  wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host, strlen(host));
  if (verifyPeer && !_insecure) {
    // Also check the hostname against the cert SAN/CN, not just the chain.
    wolfSSL_check_domain_name(ssl, host);
  }

  // Non-blocking recv => retry wolfSSL_connect across handshake round-trips.
  // Sample the heap low-water across the handshake (this is where ECC/RSA bignum
  // allocations peak) so callers can report the handshake's real heap trough,
  // distinct from the all-time ESP.getMinFreeHeap() figure.
  auto sampleHeapTrough = [this]() {
    const size_t freeNow = esp_get_free_heap_size();
    const size_t largestNow = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (freeNow < _handshakeMinFree) _handshakeMinFree = freeNow;
    if (largestNow < _handshakeMinLargest) _handshakeMinLargest = largestNow;
  };
  sampleHeapTrough();
  const uint32_t deadline = millis() + 15000;
  int ret;
  while ((ret = wolfSSL_connect(ssl)) != WOLFSSL_SUCCESS) {
    sampleHeapTrough();
    const int err = wolfSSL_get_error(ssl, ret);
    if (err != WOLFSSL_ERROR_WANT_READ && err != WOLFSSL_ERROR_WANT_WRITE) {
      // Record whether this was a cert-verify failure so the caller can decide
      // on the insecure fallback; keep the log at DBG (connect() logs the WARN).
      _lastConnectErr = err;
      LOG_DBG("TLS", "wolfSSL_connect(%s) failed: err=%d", label, err);
      stop();
      return 0;
    }
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      _lastConnectErr = WOLFSSL_ERROR_WANT_READ;  // treat timeout as transport, not verify
      LOG_INF("TLS", "handshake timeout to %s", host);
      stop();
      return 0;
    }
    delay(5);
  }
  sampleHeapTrough();
  _connected = true;
  return 1;
}

// A connect attempt at one verification level, with the v23->TLS1.2 method retry.
int SecureClient::connectAtVerify(const char* host, uint16_t port, bool verifyPeer) {
  // v23 negotiates the highest mutually supported version (TLS 1.3 when offered,
  // 1.2 otherwise). WOLFSSL_TLS13 is enabled in user_settings.h.
  if (connectWithMethod(host, port, wolfSSLv23_client_method(), "auto", verifyPeer)) return 1;
  // Some TLS-1.2-only servers choke on a 1.3-capable ClientHello; retry 1.2-only,
  // but only if the failure wasn't a cert-verify problem (that won't change).
  if (isVerificationError(_lastConnectErr)) return 0;
  return connectWithMethod(host, port, wolfTLSv1_2_client_method(), "tls1.2", verifyPeer);
}

int SecureClient::connect(const char* host, uint16_t port) {
  _lastWasInsecure = false;
  // Must not leak across connects: a TCP/DNS failure records no handshake
  // error, and a stale verification code from an earlier attempt would
  // misclassify it and trigger a pointless insecure-fallback retry.
  _lastConnectErr = 0;
  _handshakeMinFree = SIZE_MAX;
  _handshakeMinLargest = SIZE_MAX;

  // If explicitly insecure (debug), skip verification outright.
  if (_insecure) {
    const int ok = connectAtVerify(host, port, /*verifyPeer=*/false);
    _lastWasInsecure = ok == 1;
    return ok;
  }

  // Verified-first.
  if (connectAtVerify(host, port, /*verifyPeer=*/true)) return 1;

  // Only fall back to insecure on a verification-class failure, when allowed.
  if (_allowInsecureFallback && isVerificationError(_lastConnectErr)) {
    LOG_INF("TLS", "WARNING: cert verify failed for %s (err=%d); retrying WITHOUT verification", host, _lastConnectErr);
    const int ok = connectAtVerify(host, port, /*verifyPeer=*/false);
    _lastWasInsecure = ok == 1;
    return ok;
  }

  if (!_allowInsecureFallback && isVerificationError(_lastConnectErr)) {
    LOG_ERR("TLS", "cert verify failed for %s (err=%d); insecure fallback disabled -> aborting", host, _lastConnectErr);
  }
  return 0;
}

int SecureClient::connect(IPAddress ip, uint16_t port) {
  char host[16];
  snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return connect(host, port);
}

size_t SecureClient::write(const uint8_t* buf, size_t size) {
  if (!_connected) return 0;
  const int n = wolfSSL_write(static_cast<WOLFSSL*>(_ssl), buf, size);
  return n > 0 ? static_cast<size_t>(n) : 0;
}

int SecureClient::read(uint8_t* buf, size_t size) {
  if (!_connected) return -1;
  auto* ssl = static_cast<WOLFSSL*>(_ssl);
  const int n = wolfSSL_read(ssl, buf, size);
  if (n > 0) return n;

  const int err = wolfSSL_get_error(ssl, n);
  if (err == WOLFSSL_ERROR_WANT_READ || err == WOLFSSL_ERROR_WANT_WRITE) return 0;  // no data yet
  if (err == WOLFSSL_ERROR_ZERO_RETURN) {                                           // peer closed cleanly
    _connected = false;
    return 0;
  }
  _connected = false;
  return -1;
}

int SecureClient::available() {
  if (!_connected) return 0;
  return wolfSSL_pending(static_cast<WOLFSSL*>(_ssl)) + _transport.available();
}

void SecureClient::stop() {
  if (_ssl) {
    wolfSSL_free(static_cast<WOLFSSL*>(_ssl));
    _ssl = nullptr;
  }
  if (_ctx) {
    wolfSSL_CTX_free(static_cast<WOLFSSL_CTX*>(_ctx));
    _ctx = nullptr;
  }
  _transport.stop();
  _connected = false;
}

uint8_t SecureClient::connected() { return _connected && _transport.connected(); }

#else  // !FREEINK_NET_WOLFSSL — inert stub so the firmware builds without wolfSSL.

int SecureClient::connectWithMethod(const char*, uint16_t, void*, const char*, bool) { return 0; }
int SecureClient::connectAtVerify(const char*, uint16_t, bool) { return 0; }
int SecureClient::connect(const char* host, uint16_t port) {
  (void)host;
  (void)port;
  LOG_ERR("TLS", "TLS unavailable: build with -DFREEINK_NET_WOLFSSL=1");
  return 0;
}
int SecureClient::connect(IPAddress ip, uint16_t port) {
  (void)ip;
  (void)port;
  return 0;
}
size_t SecureClient::write(const uint8_t* buf, size_t size) {
  (void)buf;
  (void)size;
  return 0;
}
int SecureClient::read(uint8_t* buf, size_t size) {
  (void)buf;
  (void)size;
  return -1;
}
int SecureClient::available() { return 0; }
void SecureClient::stop() {
  _transport.stop();
  _connected = false;
}
uint8_t SecureClient::connected() { return 0; }

#endif

// --- transport-agnostic single-byte helpers (shared) ---
size_t SecureClient::write(uint8_t b) { return write(&b, 1); }
int SecureClient::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}
int SecureClient::peek() { return -1; }
void SecureClient::flush() {}

}  // namespace crosspoint
