#include "PluginEvents.h"

#include <ArduinoJson.h>
#include <FileReplace.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <HeapFit.h>
#include <JsonText.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "PluginLocations.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"
#include "network/WebPathGuard.h"

// Upstream CrossPoint's plugin events, PR #3114 (Justin Mitchell) and PR #4200 (Arthur Tazhitdinov), on this
// firmware's storage, HTTP client and popup. See PluginEvents.h.

namespace plugin_events {

namespace {

constexpr const char* OUTBOX_NAME = "/events.jsonl";
// Plugins with an events section that are served; more are logged and left out.
constexpr size_t MAX_SUBSCRIBERS = 8;
// A handler's response is an acknowledgement; this much is read and dropped.
constexpr size_t MAX_RESPONSE_BYTES = 8 * 1024;

struct Subscriber {
  char name[24] = {};
  char dir[64] = {};
  Subscription sub;
};

// Built by refreshSubscriptions() only when a plugin subscribes: a reader without event plugins holds nothing.
std::unique_ptr<Subscriber[]> subscribers;
size_t subscriberCount = 0;

std::string outboxPath(const Subscriber& s) { return std::string(s.dir) + OUTBOX_NAME; }

// A small file (token, config, <book>.meta.json) of at most kMaxSmallFile bytes, or false. Each is optional: most books
// have no meta file, so an absent one is not logged.
bool readSmallFile(const std::string& path, std::string& out) {
  out.clear();
  if (path.empty() || !Storage.exists(path.c_str()) || !heapHasBlockFor(kMaxSmallFile)) return false;
  return Storage.readFileToString("PEVT", path, kMaxSmallFile, out);
}

}  // namespace

int64_t trustedEpoch() { return HalClock::isSynced() && !HalClock::isStaleRestore() ? HalClock::now() : 0; }

void refreshSubscriptions() {
  subscribers.reset();
  subscriberCount = 0;
  Subscriber found[MAX_SUBSCRIBERS];
  size_t count = 0;
  for (const auto& entry : PluginLocations::scanPlugins()) {
    if (!entry.hasDeviceJson || entry.disabled) continue;
    if (count == MAX_SUBSCRIBERS) {
      LOG_ERR("PEVT", "more than %u plugins with events: %s left out", static_cast<unsigned>(MAX_SUBSCRIBERS),
              entry.name.c_str());
      break;
    }
    // A cut name or folder would point the queue at another plugin's outbox.
    if (entry.name.size() >= sizeof(Subscriber::name) || entry.dir.size() >= sizeof(Subscriber::dir)) {
      LOG_ERR("PEVT", "%s: name or folder too long for events", entry.name.c_str());
      continue;
    }
    if (!heapHasBlockFor(kMaxManifestBytes)) {
      LOG_ERR("PEVT", "no room to read %s/device.json", entry.dir.c_str());
      continue;
    }
    std::string raw;
    if (!Storage.readFileToString("PEVT", entry.dir + "/device.json", kMaxManifestBytes, raw)) continue;
    Subscription sub;
    if (!parseSubscription(raw, sub) || sub.mask == 0) continue;
    Subscriber& s = found[count++];
    memcpy(s.name, entry.name.c_str(), entry.name.size() + 1);
    memcpy(s.dir, entry.dir.c_str(), entry.dir.size() + 1);
    s.sub = sub;
    LOG_INF("PEVT", "%s subscribes: mask=0x%02x connect=0x%02x", s.name, sub.mask, sub.connectMask);
  }
  if (count == 0) return;
  subscribers = makeUniqueNoThrow<Subscriber[]>(count);
  if (!subscribers) {
    LOG_ERR("PEVT", "OOM: subscription table");
    return;
  }
  for (size_t i = 0; i < count; i++) subscribers[i] = found[i];
  subscriberCount = count;
}

bool anySubscriber(const Event e) {
  for (size_t i = 0; i < subscriberCount; i++) {
    if (subscribers[i].sub.mask & eventBit(e)) return true;
  }
  return false;
}

bool wantsConnectAny() {
  for (size_t i = 0; i < subscriberCount; i++) {
    const Subscriber& s = subscribers[i];
    if (s.sub.connectMask == 0) continue;
    std::string raw;
    if (!heapHasBlockFor(kMaxOutboxBytes + kMaxEventLine)) return false;
    if (!Storage.readFileToString("PEVT", outboxPath(s), kMaxOutboxBytes + kMaxEventLine, raw)) continue;
    if (outboxWantsConnect(raw, s.sub.connectMask)) return true;
  }
  return false;
}

void emit(const Event e, const Var* vars, const size_t count) {
  if (!anySubscriber(e)) return;

  // A unique id for the receiver to drop a repeated delivery by: a nonce per boot and a counter, so it repeats across
  // neither the queue nor reboots without a card write per event. ts alone repeats (whole seconds, 0 without a clock).
  static const uint32_t bootNonce = esp_random();
  static uint32_t seq = 0;
  char id[24];
  snprintf(id, sizeof(id), "%08lx-%lu", static_cast<unsigned long>(bootNonce), static_cast<unsigned long>(++seq));
  const std::string line = eventLine(e, vars, count, id, static_cast<int64_t>(HalClock::now()));
  if (line.empty()) {
    LOG_ERR("PEVT", "%s: event too large, dropped", eventName(e));
    return;
  }

  // The book's <book>.meta.json, read once for every handler that filters on it.
  const char* book = "";
  for (size_t i = 0; i < count; i++) {
    if (strcmp(vars[i].key, "book") == 0 || (book[0] == '\0' && strcmp(vars[i].key, "path") == 0)) book = vars[i].value;
  }
  std::string meta;
  bool metaRead = false;

  for (size_t i = 0; i < subscriberCount; i++) {
    const Subscriber& s = subscribers[i];
    if (!(s.sub.mask & eventBit(e))) continue;
    const uint32_t wanted = s.sub.metaKeyHash[static_cast<size_t>(e)];
    if (wanted != 0) {
      // Filtered when queued, so another book's event takes no outbox room and never brings Wi-Fi up at sleep.
      if (!metaRead) {
        if (book[0] != '\0') readSmallFile(std::string(book) + ".meta.json", meta);
        metaRead = true;
      }
      if (meta.empty() || !metaMatches(meta, wanted)) continue;
    }
    const std::string path = outboxPath(s);
    HalFile file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
    if (!file) {
      LOG_ERR("PEVT", "%s: cannot open the outbox", s.name);
      continue;
    }
    if (file.fileSize() > kMaxOutboxBytes) {
      // Never drained (no network for a long time): drop the stale queue whole rather than grow it.
      file.close();
      Storage.remove(path.c_str());
      file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
      if (!file) continue;
      LOG_INF("PEVT", "%s: outbox over %u bytes, dropped", s.name, static_cast<unsigned>(kMaxOutboxBytes));
    }
    // A short write leaves a torn line, which the drain drops as corrupt together with the next: no wedged queue.
    if (file.write(reinterpret_cast<const uint8_t*>(line.data()), line.size()) != line.size()) {
      LOG_ERR("PEVT", "%s: short outbox write", s.name);
    }
    file.close();
    LOG_DBG("PEVT", "%s: queued %s", s.name, eventName(e));
  }
}

namespace {

// What a drain needs from one plugin: its manifest and config as parsed, its token.
struct DrainContext {
  JsonDocument manifest;
  JsonDocument config;
  std::string token;
};

// A small JSON file (token, config, <book>.meta.json) parsed into `doc`; false when absent or not JSON.
bool readSmallJson(const std::string& path, JsonDocument& doc) {
  std::string raw;
  return readSmallFile(path, raw) && JsonText::parse(doc, raw) == DeserializationError::Ok;
}

// What became of one queued line.
enum class LineOutcome : uint8_t {
  Sent,        // a request answered 2xx
  Downloaded,  // a download replaced its file
  Dropped,     // it can never go (corrupt, no handler for its event any more, an unsafe destination): removed unsent
  Kept,        // it failed now: it and the lines behind it wait for the next drain
};

// Sends one queued line.
LineOutcome deliverLine(const DrainContext& dc, const std::string& line, GfxRenderer* renderer) {
  JsonDocument doc;
  if (JsonText::parse(doc, line) != DeserializationError::Ok) return LineOutcome::Dropped;
  const int e = eventFromName(doc["e"] | "");
  if (e < 0) return LineOutcome::Dropped;
  const JsonVariantConst handler = handlerFor(dc.manifest.as<JsonVariantConst>(), static_cast<Event>(e));
  if (handler.isNull()) return LineOutcome::Dropped;
  const JsonVariantConst req = requestOf(handler);
  const bool download = isDownload(handler);

  JsonDocument meta;
  const JsonVariantConst vars = doc["vars"];
  const char* book = eventBook(vars);
  if (book[0] != '\0') readSmallJson(std::string(book) + ".meta.json", meta);
  TemplateContext ctx;
  ctx.token = dc.token.c_str();
  ctx.cfg = dc.config.as<JsonVariantConst>();
  ctx.meta = meta.as<JsonVariantConst>();
  ctx.vars = vars;
  ctx.ts = doc["ts"].as<long long>();
  ctx.id = doc["id"] | "";

  HttpDownloader::Headers headers;
  for (JsonPairConst h : req["headers"].as<JsonObjectConst>()) {
    headers.emplace_back(h.key().c_str(), expand(h.value() | "", ctx));
  }
  const std::string url = escapeUrl(expand(req["url"] | "", ctx));
  const char* method = req["method"] | (download ? "GET" : "POST");
  // A request carries the user's data (a progress, a key): its server's certificate must verify. A plain download
  // may fall back like any public read.
  const bool credentialed = !headers.empty() || !download;
  const crosspoint::TlsPolicy tls =
      credentialed ? crosspoint::TlsPolicy::Verified : crosspoint::TlsPolicy::VerifiedThenUnverified;

  int status = 0;
  if (download) {
    const std::string dest = expand(handler["download"]["dest"] | "", ctx);
    if (dest.empty() || dest[0] != '/' || dest.find("..") != std::string::npos ||
        isProtectedWebPath(String(dest.c_str()), ProtectedPaths::Target::Item, false) ||
        PluginLocations::isInsidePluginFolder(dest)) {
      LOG_ERR("PEVT", "unsafe download destination refused: %s", dest.c_str());
      return LineOutcome::Dropped;
    }
    if (strcmp(method, "GET") != 0) {
      LOG_ERR("PEVT", "download by %s is not supported: %s", method, url.c_str());
      return LineOutcome::Dropped;
    }
    // Into a sibling first, and in only on a clean answer, so an error page never replaces the file (a sleep image).
    const std::string part = dest + ".part";
    const auto result = HttpDownloader::downloadToFile(url, part, nullptr, "", "", tls, {}, headers, &status);
    if (result != HttpDownloader::OK || status < 200 || status >= 300 ||
        !FileReplace::replace(part.c_str(), dest.c_str())) {
      Storage.remove(part.c_str());
      LOG_ERR("PEVT", "download %s to %s failed (%d)", url.c_str(), dest.c_str(), status);
      return LineOutcome::Kept;
    }
  } else {
    size_t received = 0;
    status = HttpDownloader::request(
        method, url, headers, expand(req["body"] | "", ctx), nullptr,
        [&received](int, const uint8_t*, const size_t n) {
          received += n;
          return received <= MAX_RESPONSE_BYTES;
        },
        tls);
    if (status < 200 || status >= 300) {
      LOG_ERR("PEVT", "%s %s answered %d", method, url.c_str(), status);
      return LineOutcome::Kept;
    }
  }
  LOG_INF("PEVT", "delivered %s (%d)", eventName(static_cast<Event>(e)), status);
  const char* toast = handler["toast"] | "";
  if (renderer != nullptr && toast[0] != '\0') GUI.drawPopup(*renderer, expand(toast, ctx).c_str());
  return download ? LineOutcome::Downloaded : LineOutcome::Sent;
}

}  // namespace

DrainResult drain(GfxRenderer* renderer, const size_t maxEvents) {
  DrainResult result;
  size_t budget = maxEvents;
  for (size_t i = 0; i < subscriberCount && budget > 0; i++) {
    const Subscriber& s = subscribers[i];
    const std::string path = outboxPath(s);
    if (!Storage.exists(path.c_str())) continue;
    if (!heapHasBlockFor(kMaxOutboxBytes + kMaxEventLine) || !heapHasBlockFor(kMaxManifestBytes)) {
      LOG_ERR("PEVT", "no room to drain %s", s.name);
      return result;
    }
    std::string raw;
    if (!Storage.readFileToString("PEVT", path, kMaxOutboxBytes + kMaxEventLine, raw)) continue;

    DrainContext dc;
    {
      std::string text;
      // Unreadable now (a card fault, a manifest being replaced): the queue waits for the next drain.
      if (!Storage.readFileToString("PEVT", std::string(s.dir) + "/device.json", kMaxManifestBytes, text) ||
          JsonText::parse(dc.manifest, text) != DeserializationError::Ok) {
        continue;
      }
    }
    bool runnable = false;
    for (size_t e = 0; e < kEventCount; e++) {
      runnable = runnable || !handlerFor(dc.manifest.as<JsonVariantConst>(), static_cast<Event>(e)).isNull();
    }
    if (!runnable) {
      // Subscribed, but nothing it can send: the queue could never move, so it goes.
      Storage.remove(path.c_str());
      continue;
    }
    {
      JsonDocument token;
      const char* tokenPath = dc.manifest["token"]["path"] | "token";
      if (readSmallJson(pluginFile(s.dir, dc.manifest["token"]["file"] | ""), token)) {
        dc.token = valueAt(token.as<JsonVariantConst>(), tokenPath);
      }
      readSmallJson(pluginFile(s.dir, dc.manifest["config"]["file"] | ""), dc.config);
    }

    // In order; the first failure stops this plugin's queue, so no later event overtakes it.
    size_t pos = 0;
    bool stalled = false;
    while (pos < raw.size() && budget > 0) {
      size_t nl = raw.find('\n', pos);
      if (nl == std::string::npos) nl = raw.size();
      if (nl > pos) {
        switch (deliverLine(dc, raw.substr(pos, nl - pos), renderer)) {
          case LineOutcome::Kept:
            stalled = true;
            break;
          case LineOutcome::Downloaded:
            result.downloads++;
            break;
          case LineOutcome::Sent:
          case LineOutcome::Dropped:
            break;
        }
        if (stalled) break;
        budget--;
      }
      pos = nl + 1;
    }
    if (pos >= raw.size() && !stalled) {
      Storage.remove(path.c_str());
    } else if (pos > 0) {
      // The rest, so a delivered event is not sent again; on a failed rewrite the old queue stays (sending one again
      // beats losing one).
      const std::string tmp = path + ".tmp";
      if (!Storage.writeFile(tmp.c_str(), String(raw.c_str() + pos)) ||
          !FileReplace::replace(tmp.c_str(), path.c_str())) {
        Storage.remove(tmp.c_str());
        LOG_ERR("PEVT", "%s: outbox rewrite failed", s.name);
      }
    }
    LOG_DBG("PEVT", "%s: drained%s", s.name, stalled ? ", stalled" : "");
  }
  return result;
}

}  // namespace plugin_events
