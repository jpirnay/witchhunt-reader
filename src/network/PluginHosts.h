#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Which hosts a plugin may reach through the device: the relay and fetch-to-SD (CrossPointWebServer) and the requests
// and downloads of its device events (src/util/PluginEvents.cpp) ask the same question here. Pure, so the host tests
// check it without a card or a network; PluginReach.h fills a Reach from the card.
namespace PluginHosts {

// The host a URL connects to, in lower case: what an allowlist judges. "" for a URL the HTTP client would not connect
// for (crosspoint::splitUrl(): not http or https, no host, user info, an odd byte in the host, a bad port). The client
// reads the URL with the same splitUrl(), so the host judged here is the one it connects to.
std::string urlHost(std::string_view url);

// Whether `host` (lower case, as urlHost() gives it) matches an allowedHosts entry: exactly, or, for an entry starting
// with a dot, as a subdomain of it (".example.org" matches "covers.example.org" but not "example.org"). The entry
// compares without case.
bool hostMatches(std::string_view host, std::string_view pattern);

// What one plugin may reach: the allowedHosts entries of its manifest, the hosts the user approved for it, and for its
// device events the hosts its event URLs name literally (plugin_events::declareLiteralHosts()); the last two exact
// hosts only. Filled once (PluginReach::load()), then asked per URL without touching the card, so a redirect is judged
// in the middle of a transfer at no cost.
class Reach {
 public:
  // Room for this many bytes of entries (each entry's length plus one), so the adds that follow do not reallocate;
  // the caller asks HeapFit first.
  void reservePatterns(size_t bytes) { patterns_.reserve(bytes); }
  void reserveApproved(size_t bytes) { approved_.reserve(bytes); }
  void reserveDeclared(size_t bytes) { declared_.reserve(bytes); }
  void allow(std::string_view pattern);  // an allowedHosts entry
  void approve(std::string_view host);   // a host the user approved
  void declare(std::string_view host);   // a host the plugin's device.json writes out in an event URL
  bool allowsHost(std::string_view host) const;
  // False also for a URL whose host urlHost() cannot judge.
  bool allows(std::string_view url) const { return allowsHost(urlHost(url)); }

 private:
  // Lower case, each entry followed by '\n' (no host holds one: urlHost() refuses it).
  std::string patterns_;
  std::string approved_;
  std::string declared_;
};

// The hosts a plugin's device events were refused for: what the Plugins page offers to Allow or Dismiss
// (PluginReach keeps the list in the plugin's folder). One entry a line, oldest first: a host waiting for the user's
// answer, or '-' and a host the user dismissed, which is not asked about again. At most kMaxWanted entries, the oldest
// dropped first; a host longer than kMaxWantedHost is not recorded, so the list fits a /api/plugins entry.
inline constexpr size_t kMaxWanted = 4;
inline constexpr size_t kMaxWantedHost = 128;
inline constexpr size_t kWantedMaxBytes = kMaxWanted * (kMaxWantedHost + 2);

// `list` with `host` (lower case, as urlHost() gives it) asked for. `list` itself, unchanged, when the host is in it
// already, asked for or dismissed, or is not a host urlHost() gives.
std::string wantedWith(std::string_view list, std::string_view host);
// `list` without `host`; with it dismissed instead when `dismiss`.
std::string wantedWithout(std::string_view list, std::string_view host, bool dismiss);
// The hosts `list` asks for (not the dismissed ones), oldest first, into `out`; how many.
size_t wantedHosts(std::string_view list, std::string_view (&out)[kMaxWanted]);

}  // namespace PluginHosts
