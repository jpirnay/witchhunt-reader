// Plugin events without the card or the network (src/util/PluginEventCore.h): subscriptions read from the manifests
// of real Plugin Hub plugins, the queue line, the require_meta filter, the drain's manifest and the templates.
#include <PluginEventCore.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

using namespace plugin_events;

namespace {

// The events sections of three Plugin Hub plugins as published (kosync-autopush 0.0.1, monthwallpaper 2.0.2, readeck).
const char* const kKosyncAutopush = R"({"title":"KOSync Auto-push","config":{"file":"config.json"},"events":{
  "reader.session":{"connect":true,"request":{"method":"PUT","url":"{cfg.server}/syncs/progress",
    "headers":{"x-auth-user":"{cfg.user}","x-auth-key":"{cfg.key}","Accept":"application/vnd.koreader.v1+json",
      "Content-Type":"application/json"},
    "body":"{\"document\":\"{event.document}\",\"progress\":\"crosspoint-autopush\",\"percentage\":{event.end_progress_bp}e-4,\"device\":\"{cfg.device}\",\"device_id\":\"{cfg.device_id}\"}"},
    "toast":"KOSync: progress pushed"}}})";
const char* const kMonthWallpaper = R"({"title":"Month Wallpaper","config":{"file":"config.json"},"events":{
  "sleep.enter":{"download":{"url":"https://crosspoint-calendar.marczyk.ovh/current.bmp?w={cfg.width}&h={cfg.height}",
    "dest":"/sleep.bmp"},"toast":"Month Wallpaper updated"}}})";
const char* const kReadeck = R"({"token":{"file":"token.json","path":"access_token"},"config":{"file":"config.json"},
  "browse":{"url":"{cfg.server}/api/bookmarks"},"events":{
  "reader.exit":{"require_meta":"readeck_id","connect":true,"request":{"method":"PATCH",
    "url":"{cfg.server}/api/bookmarks/{meta.readeck_id}","headers":{"Authorization":"Bearer {token}"},
    "body":"{\"read_progress\":{event.end_percent}}"}},
  "sleep.enter":{"require_meta":"readeck_id","request":{"method":"PATCH","url":"{cfg.server}/api/bookmarks/{meta.readeck_id}"}}}})";

Subscription subscribe(const char* json) {
  Subscription s;
  EXPECT_TRUE(parseSubscription(json, s));
  return s;
}

}  // namespace

TEST(PluginEventCore, EventNamesRoundTrip) {
  for (size_t i = 0; i < kEventCount; i++) {
    const auto e = static_cast<Event>(i);
    EXPECT_EQ(eventFromName(eventName(e)), static_cast<int>(i));
  }
  EXPECT_EQ(eventFromName("reader.page"), -1);
  EXPECT_STREQ(eventName(Event::SleepEnter), "sleep.enter");
}

TEST(PluginEventCore, SubscriptionsOfRealPlugins) {
  const Subscription kosync = subscribe(kKosyncAutopush);
  EXPECT_EQ(kosync.mask, eventBit(Event::ReaderSession));
  EXPECT_EQ(kosync.connectMask, eventBit(Event::ReaderSession));
  EXPECT_EQ(kosync.metaKeyHash[static_cast<size_t>(Event::ReaderSession)], 0u);

  // sleep.enter always connects, with or without "connect".
  const Subscription wallpaper = subscribe(kMonthWallpaper);
  EXPECT_EQ(wallpaper.mask, eventBit(Event::SleepEnter));
  EXPECT_EQ(wallpaper.connectMask, eventBit(Event::SleepEnter));

  const Subscription readeck = subscribe(kReadeck);
  EXPECT_EQ(readeck.mask, eventBit(Event::ReaderExit) | eventBit(Event::SleepEnter));
  EXPECT_EQ(readeck.connectMask, eventBit(Event::ReaderExit) | eventBit(Event::SleepEnter));
  EXPECT_EQ(readeck.metaKeyHash[static_cast<size_t>(Event::ReaderExit)], metaKeyHash("readeck_id"));
}

TEST(PluginEventCore, SubscriptionIgnoresWhatItCannotRun) {
  // An event this firmware does not know, and a require_meta that is not a string: neither may subscribe, and the
  // second must not turn into "every book".
  const char* json = R"({"events":{"reader.page":{"request":{"url":"u"}},
    "reader.exit":{"require_meta":7,"request":{"url":"u"}},"reader.open":{"request":{"url":"u"}}}})";
  const Subscription s = subscribe(json);
  EXPECT_EQ(s.mask, eventBit(Event::ReaderOpen));
  Subscription none;
  EXPECT_TRUE(parseSubscription("{\"title\":\"x\"}", none));
  EXPECT_EQ(none.mask, 0);
  EXPECT_FALSE(parseSubscription("not json", none));
}

TEST(PluginEventCore, ConnectIsAJsonTrue) {
  // As upstream reads "connect": a number is not a yes (as<bool>() compared it to zero, in floating point).
  const char* json = R"({"events":{"reader.open":{"connect":true,"request":{"url":"u"}},
    "reader.exit":{"connect":1,"request":{"url":"u"}},"reader.session":{"connect":false,"request":{"url":"u"}}}})";
  const Subscription s = subscribe(json);
  EXPECT_EQ(s.mask, eventBit(Event::ReaderOpen) | eventBit(Event::ReaderExit) | eventBit(Event::ReaderSession));
  EXPECT_EQ(s.connectMask, eventBit(Event::ReaderOpen));
}

TEST(PluginEventCore, QueueLine) {
  const Var vars[] = {{"book", "/Books/Loan \"1\".epub"}, {"percent", "42"}};
  const std::string line = eventLine(Event::ReaderExit, vars, 2, "1a2b3c4d-7", 1795000000);
  EXPECT_EQ(line,
            "{\"e\":\"reader.exit\",\"id\":\"1a2b3c4d-7\",\"ts\":1795000000,"
            "\"vars\":{\"book\":\"/Books/Loan \\\"1\\\".epub\",\"percent\":\"42\"}}\n");
  const std::string longBook(600, 'x');
  const Var big[] = {{"book", longBook.c_str()}};
  EXPECT_EQ(eventLine(Event::ReaderOpen, big, 1, "id", 0), "");
}

TEST(PluginEventCore, RequireMetaMatchesFilledValues) {
  const uint32_t wanted = metaKeyHash("readeck_id");
  const char* id = R"({"readeck_id":"abc123","source":"readeck"})";
  EXPECT_TRUE(metaMatches(id, wanted));
  const char* number = R"({"readeck_id":36835})";
  EXPECT_TRUE(metaMatches(number, wanted));
  for (const char* no :
       {R"({"readeck_id":""})", R"({"readeck_id":true})", R"({"readeck_id":1.5})", R"({"other":"x"})", "garbage"}) {
    EXPECT_FALSE(metaMatches(no, wanted)) << no;
  }
}

TEST(PluginEventCore, OutboxWantsConnectOnlyForFlaggedEvents) {
  const std::string outbox =
      "{\"e\":\"reader.exit\",\"id\":\"a-1\",\"ts\":0,\"vars\":{}}\n"
      "{\"e\":\"reader.open\",\"id\":\"a-2\",\"ts\":0,\"vars\":{}}\n";
  EXPECT_TRUE(outboxWantsConnect(outbox, eventBit(Event::ReaderExit)));
  EXPECT_FALSE(outboxWantsConnect(outbox, eventBit(Event::ReaderSession) | eventBit(Event::SleepEnter)));
  EXPECT_FALSE(outboxWantsConnect("", 0xFF));
}

namespace {
JsonDocument parsed(const char* json) {
  JsonDocument doc;
  EXPECT_EQ(deserializeJson(doc, json), DeserializationError::Ok);
  return doc;
}
}  // namespace

TEST(PluginEventCore, HandlersAreReadFromTheManifest) {
  const JsonDocument kosync = parsed(kKosyncAutopush);
  const JsonVariantConst session = handlerFor(kosync.as<JsonVariantConst>(), Event::ReaderSession);
  ASSERT_FALSE(session.isNull());
  EXPECT_FALSE(isDownload(session));
  EXPECT_STREQ(requestOf(session)["method"] | "", "PUT");
  EXPECT_TRUE(handlerFor(kosync.as<JsonVariantConst>(), Event::ReaderExit).isNull());

  const JsonDocument wallpaper = parsed(kMonthWallpaper);
  const JsonVariantConst sleep = handlerFor(wallpaper.as<JsonVariantConst>(), Event::SleepEnter);
  ASSERT_FALSE(sleep.isNull());
  EXPECT_TRUE(isDownload(sleep));
  EXPECT_STREQ(sleep["download"]["dest"] | "", "/sleep.bmp");

  // A download with nowhere to go, and a request without a URL, cannot run.
  const JsonDocument broken = parsed(R"({"events":{"sleep.enter":{"download":{"url":"https://x/y.bmp"}},
    "reader.exit":{"request":{"method":"POST"}}}})");
  EXPECT_TRUE(handlerFor(broken.as<JsonVariantConst>(), Event::SleepEnter).isNull());
  EXPECT_TRUE(handlerFor(broken.as<JsonVariantConst>(), Event::ReaderExit).isNull());
}

TEST(PluginEventCore, PluginFilesAndTokenPaths) {
  EXPECT_EQ(pluginFile("/.crosspoint/plugins/readeck", "token.json"), "/.crosspoint/plugins/readeck/token.json");
  EXPECT_EQ(pluginFile("/.crosspoint/plugins/x", "/.crosspoint/x.json"), "/.crosspoint/x.json");
  EXPECT_EQ(pluginFile("/p", ""), "");
  const JsonDocument token = parsed(R"({"data":{"access_token":"tok-1"},"list":["a","b"],"n":7})");
  EXPECT_EQ(valueAt(token.as<JsonVariantConst>(), "data.access_token"), "tok-1");
  EXPECT_EQ(valueAt(token.as<JsonVariantConst>(), "list.1"), "b");
  EXPECT_EQ(valueAt(token.as<JsonVariantConst>(), "n"), "7");
  EXPECT_EQ(valueAt(token.as<JsonVariantConst>(), "missing.x"), "");
}

TEST(PluginEventCore, TemplatesFillTheKosyncRequest) {
  const JsonDocument manifest = parsed(kKosyncAutopush);
  const JsonDocument config = parsed(R"({"server":"https://sync.example","user":"reader","key":"5f4dcc3b",
    "device":"Witch Reader","device_id":"w1","port":8080,"debug":true})");
  const JsonDocument line =
      parsed(R"({"vars":{"document":"0123456789abcdef0123456789abcdef","end_progress_bp":"4321"}})");
  TemplateContext ctx;
  ctx.cfg = config.as<JsonVariantConst>();
  ctx.vars = line["vars"];
  const JsonVariantConst req = requestOf(handlerFor(manifest.as<JsonVariantConst>(), Event::ReaderSession));
  EXPECT_EQ(expand(req["url"] | "", ctx), "https://sync.example/syncs/progress");
  EXPECT_EQ(expand(req["body"] | "", ctx),
            "{\"document\":\"0123456789abcdef0123456789abcdef\",\"progress\":\"crosspoint-autopush\","
            "\"percentage\":4321e-4,\"device\":\"Witch Reader\",\"device_id\":\"w1\"}");
  EXPECT_EQ(expand(req["headers"]["x-auth-key"] | "", ctx), "5f4dcc3b");
  EXPECT_EQ(expand("{cfg.port}|{cfg.debug}|{cfg.missing}", ctx), "8080||{cfg.missing}");
}

TEST(PluginEventCore, TemplatesTokenMetaAndEventFields) {
  const JsonDocument meta = parsed(R"({"readeck_id":"r9"})");
  const JsonDocument cfg = parsed(R"({"server":"https://r","odd":"{token}"})");
  const JsonDocument line = parsed(R"({"vars":{"end_percent":"57"}})");
  TemplateContext ctx;
  ctx.token = "tok-1";
  ctx.cfg = cfg.as<JsonVariantConst>();
  ctx.meta = meta.as<JsonVariantConst>();
  ctx.vars = line["vars"];
  ctx.ts = 1795000000;
  ctx.id = "n-3";
  EXPECT_EQ(expand("{cfg.server}/b/{meta.readeck_id}?t={token}&p={event.end_percent}&ts={event.ts}&id={event.id}"
                   "&x={event.unknown}&y={nope}&open={",
                   ctx),
            "https://r/b/r9?t=tok-1&p=57&ts=1795000000&id=n-3&x={event.unknown}&y={nope}&open={");
  // One pass: a value that itself looks like a placeholder is not filled in again.
  EXPECT_EQ(expand("{cfg.odd}", ctx), "{token}");
}

TEST(PluginEventCore, EscapeUrl) { EXPECT_EQ(escapeUrl("https://x/a b?q=1&r=ä#f"), "https://x/a%20b?q=1&r=%C3%A4#f"); }
