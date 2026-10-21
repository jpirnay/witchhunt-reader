#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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
// carries a username or a password, or comes from a server entry that has either.
TlsPlan planTls(TlsPolicy policy, bool skipValidation, bool sendsCredentials);

// What one https hop of a request does: a redirect can lead to another server, so SecureHttpClient
// decides this at each connect, for the server that connect reaches. The request's plan, except:
// - a hop that sends credentials never falls back, whatever the plan (planTls() gives such a request
//   no fallback anyway);
// - where the plan allows the fallback, a server whose certificate already failed to verify this
//   session (peerUnverifiable, UnverifiableHosts) goes straight to the unverified connection the
//   fallback would make: its verify would fail again and only cost time.
TlsPlan planHop(const TlsPlan& request, bool hopSendsCredentials, bool peerUnverifiable);

// Why a verified connect failed: in certificate verification, or anywhere else (DNS, TCP, a
// timeout, the handshake itself). Only the first is worth an unverified retry.
enum class VerifiedConnectFailure : uint8_t { Certificate, Other };

// Whether a verified connect that failed is retried once unverified (SecureClient::connect()).
inline bool retryUnverified(const TlsPlan& plan, const VerifiedConnectFailure failure) {
  return plan.fallbackOnVerifyError && failure == VerifiedConnectFailure::Certificate;
}

// How the TLS log lines and UnverifiableHosts name a server: "host:port", the port always given, so
// a request to https://example.org/ and a redirect to https://example.org:443/ name one server.
std::string tlsPeer(std::string_view host, uint16_t port);

// The servers whose certificate could not be verified this session, for VerifiedThenUnverified: a
// later hop to one skips the verify it would fail, about 3 s of a handshake on this chip (planHop()).
// Checked and filled by SecureHttpClient at each https connect, under the server's tlsPeer() name.
// In RAM only, a few bytes: the FNV-1a hashes of up to kCapacity names, compared without case, the
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
