#pragma once
#include <HalStorage.h>

#include <functional>
#include <memory>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. Built on the
 * wolfSSL-backed SecureNet stack: https is verified against the curated
 * CrossPoint root set (TLS 1.3); plain http uses a WiFiClient passthrough
 * (transport is chosen from the URL scheme). Every https caller picks a
 * TlsPolicy — see the enum: the default browsing posture, fail-closed, or
 * explicitly unverified. Use fetchUrlVerified() for fail-closed streaming.
 */
class HttpDownloader {
 public:
  // Progress callback. Return false to abort the transfer.
  using ProgressCallback = std::function<bool(unsigned int downloaded, unsigned int total)>;
  // Called with each response chunk as it arrives; return false to abort.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * TLS verification posture for an https transfer.
   *
   * Verified — verify against the curated roots and abort if that fails, unless
   *   the user has turned SETTINGS.skipHttpsValidation on. The default: there is
   *   no "verify, then retry unverified" middle any more, because an on-path
   *   attacker presenting a bad certificate was simply accepted on the retry —
   *   it authenticated nothing while paying for the chain verification twice.
   *   Reaching a private-CA or self-signed server is now an explicit,
   *   user-visible choice instead of a silent per-call downgrade.
   * Strict — always verified; the skip setting does not apply. For firmware,
   *   which the device executes, so it is not the user's to downgrade.
   * Unverified — never verified: no roots loaded, no hostname check, one
   *   handshake. For public read-only data where there is no secret to leak and
   *   nothing to authenticate; also materially cheaper, since the ECDSA chain
   *   verify dominates a handshake on this chip.
   */
  enum class TlsPolicy {
    Verified,
    Strict,
    Unverified,
  };

  /**
   * Reusable HTTP+TLS session. Holding one of these across multiple
   * downloadToFile() calls keeps a single SecureHttpClient (and its wolfSSL
   * connection) alive, so the TLS handshake — the heap/CPU spike from the ECC
   * key exchange and RSA/ECDSA chain verify — runs once instead of per-file.
   * This is the structural fix for back-to-back HTTPS calls failing on a
   * fragmented heap.
   *
   * Usage: construct one, pass to downloadToFile(session, …) for every file
   * served by the same host. Destroying it closes the connection.
   *
   * Cross-host reuse is transparent (SecureHttpClient reopens when host/port
   * change) but defeats the heap win — group calls by host.
   */
  class Session {
   public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    struct Impl;
    Impl* impl() const { return impl_.get(); }

   private:
    std::unique_ptr<Impl> impl_;
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "", TlsPolicy tls = TlsPolicy::Verified);

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "", TlsPolicy tls = TlsPolicy::Verified);

  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Streaming fetch variant with explicit abort semantics.
   *
   * When treatAbortAsSuccess is false (default behavior), a callback abort
   * (onData returns false) is treated as failure.
   *
   * When treatAbortAsSuccess is true, a callback abort maps to success.
   * This is used by metadata parsers that intentionally stop once required
   * fields are found, to avoid depending on full-body tail reads.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, bool treatAbortAsSuccess,
                       const std::string& username = "", const std::string& password = "");

  /**
   * Streaming fetch that ALWAYS fails closed on TLS verification failure — it
   * never falls back to an unverified connection. Use for security-critical
   * transfers (OTA firmware download) where an on-path attacker must not be able
   * to force a downgrade by presenting a bad certificate.
   */
  static bool fetchUrlVerified(const std::string& url, const DataCallback& onData, bool treatAbortAsSuccess = false,
                               const std::string& username = "", const std::string& password = "");

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, const std::string& username = "",
                                      const std::string& password = "", TlsPolicy tls = TlsPolicy::Verified);

  /**
   * Session-based variant. The first call on a fresh session opens the
   * connection (TLS handshake, cert verification, etc.); subsequent calls to
   * URLs on the same host reuse the open client and skip the handshake.
   */
  static DownloadError downloadToFile(Session& session, const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, const std::string& username = "",
                                      const std::string& password = "", TlsPolicy tls = TlsPolicy::Verified);
};
