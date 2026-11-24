#pragma once

// Plugin events without the card or the network: the event names, what a plugin's device.json subscribes to, the
// queue's line format, the "require_meta" filter, the drain's view of a manifest, and the request templates. The
// device side (PluginEvents.h) reads and writes the files and sends the requests; everything here is host-tested
// (test/plugin_events).
//
// Upstream CrossPoint's design and file formats, so plugins written for it run here: crosspoint-reader PR #3114
// ("feat: SD card based plugin system", Justin Mitchell / @itsthisjustin) and PR #4200 ("feat: filter plugin events by
// book metadata", Arthur Tazhitdinov). The contract is docs/sd-plugins.md, "Events".

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace plugin_events {

// Names are a compatibility promise with upstream's manifests: semantic, never activity names. Append only.
enum class Event : uint8_t {
  ReaderOpen,      // "reader.open"      book
  ReaderExit,      // "reader.exit"      book, percent, end_percent
  ReaderSession,   // "reader.session"   book, document, start_time, end_time, duration_seconds, ... (ReaderSession.h)
  BookDownloaded,  // "book.downloaded"  path, title, plugin
  SleepEnter,      // "sleep.enter"      book, percent, end_percent when sleeping out of a book, else none
  COUNT
};
inline constexpr size_t kEventCount = static_cast<size_t>(Event::COUNT);

inline constexpr size_t kMaxOutboxBytes = 4 * 1024;    // then the queue is dropped wholesale: its events are stale
inline constexpr size_t kMaxEventLine = 512;           // one queued event
inline constexpr size_t kMaxManifestBytes = 8 * 1024;  // device.json
inline constexpr size_t kMaxSmallFile = 2 * 1024;      // token, config and <book>.meta.json files

const char* eventName(Event e);
// -1 for a name this firmware does not know (a manifest written for newer firmware).
int eventFromName(const char* name);
inline uint8_t eventBit(const Event e) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(e)); }

struct Var {
  const char* key;    // the name inside {event.*}
  const char* value;  // NUL-terminated UTF-8; every event variable is a string
};

// FNV-1a of a "require_meta" key, never 0 (0 marks a handler for every book). A collision only queues an extra
// event; the drain still substitutes the real value.
uint32_t metaKeyHash(const char* key);

// What a device.json subscribes to.
struct Subscription {
  uint8_t mask = 0;  // bit per Event
  // Events whose handler may bring Wi-Fi up at sleep entry: "connect": true, and sleep.enter always.
  uint8_t connectMask = 0;
  uint32_t metaKeyHash[kEventCount] = {};  // per event, 0 = every book
};
// False when the text is not JSON. Unknown event names are skipped; a handler whose require_meta is not a string is
// dropped, so a wrong type cannot widen the filter to every book.
bool parseSubscription(const std::string& json, Subscription& out);

// One queue line, '\n'-terminated: {"e":"reader.exit","id":"...","ts":...,"vars":{...}}. Empty when longer than
// kMaxEventLine.
std::string eventLine(Event e, const Var* vars, size_t count, const char* id, int64_t ts);

// The book an event is about: "book", or "path" for book.downloaded. Empty when none.
const char* eventBook(JsonVariantConst vars);

// Whether a book's flat <book>.meta.json holds a non-empty string or a whole number under a key hashing to `wanted`.
bool metaMatches(const std::string& json, uint32_t wanted);

// Whether a queue holds an event whose bit is set in `connectMask`.
bool outboxWantsConnect(const std::string& outbox, uint8_t connectMask);

// The handler a device.json declares for `e` that this firmware can run, or null: a "request" with a URL, or a
// "download" with a URL and a "dest". Read from the manifest as parsed, so nothing is copied out of it.
JsonVariantConst handlerFor(JsonVariantConst manifest, Event e);
// A handler's request object: its "download" or its "request".
inline JsonVariantConst requestOf(JsonVariantConst handler) {
  return handler["download"]["url"].is<const char*>() ? handler["download"] : handler["request"];
}
inline bool isDownload(JsonVariantConst handler) { return handler["download"]["url"].is<const char*>(); }

// A token or config file a device.json names, by the rules the web server reads the same names with, so the reader
// reads no file the plugin could not read over the web: PluginLocations::namedFilePath() (a relative name is in the
// plugin's folder, an absolute one as written, a name with ".." names nothing), then `isProtected` on the result, which
// the device answers with isProtectedPluginFile() (a credential store, a system folder, an 8.3 alias spelling). "" for
// none and for a refused name.
using PathCheck = bool (*)(const std::string& path);
std::string pluginFile(const std::string& pluginDir, const char* name, PathCheck isProtected);
// The value at a dotted path ("access_token", "data.token", "list.0") of a parsed token file, as text.
std::string valueAt(JsonVariantConst root, const char* dottedPath);

// What a template is filled from: {token}, {cfg.KEY} (the config file), {meta.KEY} (the book's <book>.meta.json),
// {event.ts}, {event.id} and {event.NAME} (the queued line's variables). Strings and whole numbers go in as text,
// anything else as nothing; a placeholder it does not know stays as written. One pass: a value is never read again
// for placeholders.
struct TemplateContext {
  const char* token = "";
  JsonVariantConst cfg;
  JsonVariantConst meta;
  JsonVariantConst vars;
  int64_t ts = 0;
  const char* id = "";
};
std::string expand(const char* tpl, const TemplateContext& ctx);
// The URL a handler's request or download goes to: its "url" filled in (expand()) and escaped (escapeUrl()). What the
// HTTP client is handed, so it is what the drain's host check judges.
std::string handlerUrl(JsonVariantConst handler, const TemplateContext& ctx);
// Spaces, control characters and non-ASCII bytes of a URL percent-encoded; '&', '?', '#', '+' and '/' left alone.
std::string escapeUrl(const std::string& url);

}  // namespace plugin_events
