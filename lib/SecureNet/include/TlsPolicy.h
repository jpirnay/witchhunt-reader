#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Which https transfers verify the server, and what happens when its certificate cannot be
// verified. The policy is chosen at each call site (HttpDownloader::TlsPolicy); planTls() turns it
// into what one request does. Kept apart from the TLS code so it can be host-tested.
namespace crosspoint {

enum class TlsPolicy : uint8_t {
  // Verify against the curated roots and abort if that fails, unless the user has turned "Skip
  // HTTPS validation (risky)" on. For anything that carries credentials or has no reason to accept
  // an unverified server: OPDS catalogs with a login, KOSync, plugins, fonts.
  Verified,
  // Always verified; the skip setting does not apply. For firmware, which the device executes, and
  // the release check that names it.
  Strict,
  // Never verified: no roots, no hostname check, one handshake. For public read-only data with no
  // secret to leak and nothing to authenticate (weather, geocoding, timezone, the hash-pinned web
  // bundle); also much cheaper, since the chain verify dominates a handshake on this chip.
  Unverified,
  // Verified first; a certificate that cannot be verified (no trusted root, a bad signature, a
  // name or date mismatch -- not a timeout, DNS or other connection failure) is retried once
  // unverified, and logged. Only for a request that carries no credentials: planTls() falls back
  // to Verified for one that does. For OPDS catalogs without a login, whose feeds and books are
  // public and which are often served from certificates the curated roots do not cover.
  VerifiedThenUnverified,
};

// What one request does under a policy.
struct TlsPlan {
  bool verify = true;                  // load the curated roots and verify the peer
  bool fallbackOnVerifyError = false;  // retry once unverified if the certificate cannot be verified
};

// skipValidation: the user's "Skip HTTPS validation (risky)" setting. sendsCredentials: the request
// carries a username or a password, or comes from a server entry that has either. hostUnverifiable:
// this host's certificate already failed to verify this session (UnverifiableHosts), so its verify
// would fail again and only cost time.
TlsPlan planTls(TlsPolicy policy, bool skipValidation, bool sendsCredentials, bool hostUnverifiable);

// Why a verified connect failed: in certificate verification, or anywhere else (DNS, TCP, a
// timeout, the handshake itself). Only the first is worth an unverified retry.
enum class VerifiedConnectFailure : uint8_t { Certificate, Other };

// Whether a verified connect that failed is retried once unverified (SecureClient::connect()).
inline bool retryUnverified(const TlsPlan& plan, const VerifiedConnectFailure failure) {
  return plan.fallbackOnVerifyError && failure == VerifiedConnectFailure::Certificate;
}

// The hosts whose certificate could not be verified this session, for VerifiedThenUnverified: a
// later request to one skips the verify it would fail, about 3 s of a handshake on this chip. In RAM
// only, a few bytes: the FNV-1a hashes of up to kCapacity host names, compared without case, the
// oldest replaced first. Never persisted, so a reboot verifies again.
class UnverifiableHosts {
 public:
  static constexpr size_t kCapacity = 4;

  bool contains(std::string_view host) const;
  void add(std::string_view host);
  void clear() { count_ = next_ = 0; }

 private:
  static uint32_t hashOf(std::string_view host);

  uint32_t hashes_[kCapacity] = {};
  uint8_t count_ = 0;
  uint8_t next_ = 0;  // the slot add() fills once all are taken
};

}  // namespace crosspoint
