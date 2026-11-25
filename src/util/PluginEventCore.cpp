#include "PluginEventCore.h"

#include <JsonText.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "PluginLocations.h"
#include "network/PluginHosts.h"

// Upstream CrossPoint's plugin events (PR #3114, Justin Mitchell; require_meta from PR #4200, Arthur Tazhitdinov):
// the event table, the queue line and the templates follow its src/util/PluginEvents.cpp and PluginHttp.cpp.

namespace plugin_events {

namespace {

constexpr const char* EVENT_NAMES[] = {"reader.open", "reader.exit", "reader.session", "book.downloaded",
                                       "sleep.enter"};
static_assert(sizeof(EVENT_NAMES) / sizeof(EVENT_NAMES[0]) == kEventCount, "event name table out of sync");

// Strings and whole numbers as text; decimals, booleans, null and nested values as "".
std::string variantToString(JsonVariantConst v) {
  if (v.is<const char*>()) return v.as<const char*>();
  if (v.is<long long>()) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", v.as<long long>());
    return buf;
  }
  return "";
}

}  // namespace

const char* eventName(const Event e) {
  const auto i = static_cast<size_t>(e);
  return i < kEventCount ? EVENT_NAMES[i] : "";
}

int eventFromName(const char* name) {
  if (name == nullptr) return -1;
  for (size_t i = 0; i < kEventCount; i++) {
    if (strcmp(EVENT_NAMES[i], name) == 0) return static_cast<int>(i);
  }
  return -1;
}

uint32_t metaKeyHash(const char* key) {
  uint32_t hash = 2166136261u;
  for (; *key; key++) hash = (hash ^ static_cast<uint8_t>(*key)) * 16777619u;
  return hash == 0 ? 1 : hash;
}

bool parseSubscription(const std::string& json, Subscription& out) {
  out = Subscription{};
  // Whole, from a std::string: the parser the firmware already has (a filtered one would be new code).
  JsonDocument doc;
  if (JsonText::parse(doc, json) != DeserializationError::Ok) return false;
  for (JsonPairConst kv : doc["events"].as<JsonObjectConst>()) {
    const int e = eventFromName(kv.key().c_str());
    if (e < 0) continue;
    const JsonVariantConst requireMeta = kv.value()["require_meta"];
    if (!requireMeta.isNull() && !requireMeta.is<const char*>()) continue;
    out.mask |= static_cast<uint8_t>(1u << e);
    // A JSON true, as upstream reads it (`| false`, spelled out for cppcheck's badBitmaskCheck): a number is not one,
    // where as<bool>() alone compared it to zero in floating point.
    const JsonVariantConst connect = kv.value()["connect"];
    if (connect.is<bool>() && connect.as<bool>()) out.connectMask |= static_cast<uint8_t>(1u << e);
    const char* key = requireMeta | "";
    if (key[0] != '\0') out.metaKeyHash[e] = metaKeyHash(key);
  }
  // sleep.enter exists to act before the chip powers down (a sleep image, a last sync), so it always connects.
  if (out.mask & eventBit(Event::SleepEnter)) out.connectMask |= eventBit(Event::SleepEnter);
  return true;
}

std::string eventLine(const Event e, const Var* vars, const size_t count, const char* id, const int64_t ts) {
  JsonDocument doc;
  doc["e"] = eventName(e);
  doc["id"] = id;
  doc["ts"] = static_cast<long long>(ts);
  JsonObject obj = doc["vars"].to<JsonObject>();
  for (size_t i = 0; i < count; i++) obj[vars[i].key] = vars[i].value;
  std::string line;
  JsonText::write(doc, line);
  line += '\n';
  if (doc.overflowed() || line.size() > kMaxEventLine) return "";
  return line;
}

const char* eventBook(JsonVariantConst vars) {
  const char* book = vars["book"] | "";
  return book[0] != '\0' ? book : (vars["path"] | "");
}

bool metaMatches(const std::string& json, const uint32_t wanted) {
  JsonDocument doc;
  if (JsonText::parse(doc, json) != DeserializationError::Ok) return false;
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
    if (metaKeyHash(kv.key().c_str()) == wanted && !variantToString(kv.value()).empty()) return true;
  }
  return false;
}

bool outboxWantsConnect(const std::string& outbox, const uint8_t connectMask) {
  for (size_t i = 0; i < kEventCount; i++) {
    if (!(connectMask & (1u << i))) continue;
    char field[48];
    snprintf(field, sizeof(field), "\"e\":\"%s\"", EVENT_NAMES[i]);
    if (outbox.find(field) != std::string::npos) return true;
  }
  return false;
}

JsonVariantConst handlerFor(JsonVariantConst manifest, const Event e) {
  const JsonVariantConst handler = manifest["events"][eventName(e)];
  if (isDownload(handler)) {
    const char* dest = handler["download"]["dest"] | "";
    return dest[0] != '\0' ? handler : JsonVariantConst();  // a download with nowhere to go
  }
  return handler["request"]["url"].is<const char*>() ? handler : JsonVariantConst();
}

std::string pluginFile(const std::string& pluginDir, const char* name, const PathCheck isProtected) {
  if (name == nullptr) return "";
  std::string path = PluginLocations::namedFilePath(name, pluginDir);
  if (path.empty() || isProtected(path)) return "";
  return path;
}

std::string valueAt(JsonVariantConst node, const char* dottedPath) {
  const char* p = dottedPath;
  while (*p != '\0' && !node.isNull()) {
    const char* dot = strchr(p, '.');
    const size_t len = dot != nullptr ? static_cast<size_t>(dot - p) : strlen(p);
    if (len > 0) {
      const std::string seg(p, len);
      if (seg.find_first_not_of("0123456789") == std::string::npos) {
        node = node[atoi(seg.c_str())];
      } else {
        node = node[seg.c_str()];
      }
    }
    if (dot == nullptr) break;
    p = dot + 1;
  }
  return variantToString(node);
}

std::string expand(const char* tpl, const TemplateContext& ctx) {
  std::string out;
  out.reserve(strlen(tpl));
  // A placeholder is '{', a name of letters, digits, '_', '-' and '.', and '}'. Anything else is text: a JSON body is
  // itself made of braces ({"document":"{event.document}"}).
  const auto nameChar = [](const char c) {
    return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
  };
  for (const char* p = tpl; *p != '\0';) {
    const char* close = p + 1;
    if (*p == '{') {
      while (nameChar(*close)) close++;
    }
    if (*p != '{' || *close != '}' || close == p + 1) {
      out += *p++;
      continue;
    }
    const std::string key(p + 1, close);
    const size_t dot = key.find('.');
    const std::string scope = key.substr(0, dot);
    const std::string name = dot == std::string::npos ? std::string() : key.substr(dot + 1);
    // A key the file or the event does not have stays as written, as upstream's do.
    const JsonVariantConst source = scope == "cfg" ? ctx.cfg : scope == "meta" ? ctx.meta : ctx.vars;
    if (key == "token") {
      out += ctx.token;
    } else if (key == "event.ts") {
      char ts[24];
      snprintf(ts, sizeof(ts), "%lld", static_cast<long long>(ctx.ts));
      out += ts;
    } else if (key == "event.id") {
      out += ctx.id;
    } else if ((scope == "cfg" || scope == "meta" || scope == "event") && !name.empty() &&
               !source[name.c_str()].isNull()) {
      out += variantToString(source[name.c_str()]);
    } else {
      out.append(p, close + 1);
    }
    p = close + 1;
  }
  return out;
}

std::string handlerUrl(JsonVariantConst handler, const TemplateContext& ctx) {
  return escapeUrl(expand(requestOf(handler)["url"] | "", ctx));
}

std::string literalHost(const char* urlTemplate) { return PluginHosts::urlHost(urlTemplate ? urlTemplate : ""); }

namespace {
std::string literalHostOf(JsonVariantConst manifest, const size_t event) {
  return literalHost(requestOf(handlerFor(manifest, static_cast<Event>(event)))["url"] | "");
}
}  // namespace

size_t literalHostBytes(JsonVariantConst manifest) {
  size_t bytes = 0;
  for (size_t e = 0; e < kEventCount; e++) bytes += literalHostOf(manifest, e).size() + 1;
  return bytes;
}

void declareLiteralHosts(JsonVariantConst manifest, PluginHosts::Reach& reach) {
  reach.reserveDeclared(literalHostBytes(manifest));
  for (size_t e = 0; e < kEventCount; e++) reach.declare(literalHostOf(manifest, e));
}

std::string escapeUrl(const std::string& url) {
  std::string out;
  out.reserve(url.size());
  for (const unsigned char c : url) {
    if (c > 0x20 && c < 0x7F) {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

}  // namespace plugin_events
