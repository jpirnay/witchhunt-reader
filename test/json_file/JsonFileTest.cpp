// A JSON file written to the card whole, or not at all (lib/Serialization/JsonFile).
//
// Only settings.json checked that its JSON had serialised in full. koreader.json, wifi.json,
// opds.json, the auto-sync state, both weather files and the OPDS discovery cache wrote whatever the
// String held: on a heap with no block big enough, ArduinoJson's String writer stops appending
// without an error, and the short JSON replaced a complete file. wifi.json is rewritten right after a
// connect, the C3's tightest moment, and a cut one loses every saved network.
// After matcha-reader PR #315 and PR #508 by Eszter Schuffert.
#include <ArduinoJson.h>
#include <JsonFile.h>
#include <WeatherSettingsStore.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "HalStorage.h"

namespace {

constexpr char kPath[] = "/.crosspoint/test.json";
constexpr char kWeatherPath[] = "/.crosspoint/weather_settings.json";
constexpr size_t kNoLimit = static_cast<size_t>(-1);

// Grants the first `allowed` allocations, then refuses every one after: a heap that runs out while
// a document is filled.
class RefusingAllocator : public ArduinoJson::Allocator {
 public:
  explicit RefusingAllocator(const int allowed) : allowed_(allowed) {}
  void* allocate(const size_t size) override { return allowed_-- > 0 ? std::malloc(size) : nullptr; }
  void deallocate(void* ptr) override { std::free(ptr); }
  void* reallocate(void* ptr, const size_t size) override { return allowed_-- > 0 ? std::realloc(ptr, size) : nullptr; }

 private:
  int allowed_;
};

void fill(JsonDocument& doc) {
  doc["lastConnectedSsid"] = std::string("Home network");
  JsonArray networks = doc["credentials"].to<JsonArray>();
  for (int i = 0; i < 4; i++) {
    JsonObject network = networks.add<JsonObject>();
    network["ssid"] = std::string("network-") + std::to_string(i);
    network["password_obf"] = std::string(40, static_cast<char>('a' + i));
  }
}

std::string serialised(const JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

class JsonFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    json_file_fixture::state = {};
    String::growLimit() = kNoLimit;
  }
  void TearDown() override { String::growLimit() = kNoLimit; }
};

// The premise: a String that cannot grow ends short, and nothing says so.
TEST_F(JsonFileTest, AStringThatCannotGrowEndsShortWithoutAnError) {
  JsonDocument doc;
  fill(doc);
  String::growLimit() = 100;
  String json;
  serializeJson(doc, json);
  EXPECT_LT(json.length(), measureJson(doc));
  EXPECT_FALSE(doc.overflowed());
}

TEST_F(JsonFileTest, AWholeDocumentIsWritten) {
  JsonDocument doc;
  fill(doc);
  ASSERT_TRUE(JsonFile::save("TEST", kPath, doc));
  EXPECT_EQ(json_file_fixture::state.writes, 1);
  EXPECT_EQ(json_file_fixture::state.files[kPath], serialised(doc));
}

TEST_F(JsonFileTest, AStringThatFellShortLeavesTheFileAsItWas) {
  json_file_fixture::state.files[kPath] = "{\"previous\":true}";
  JsonDocument doc;
  fill(doc);
  String::growLimit() = 100;
  EXPECT_FALSE(JsonFile::save("TEST", kPath, doc));
  EXPECT_EQ(json_file_fixture::state.writes, 0);
  EXPECT_EQ(json_file_fixture::state.files[kPath], "{\"previous\":true}");
}

// Short by a single byte is short.
TEST_F(JsonFileTest, OneByteShortIsRefused) {
  JsonDocument doc;
  fill(doc);
  String::growLimit() = measureJson(doc) - 1;
  String json;
  EXPECT_FALSE(JsonFile::serializeWhole("TEST", kPath, doc, json));
  String::growLimit() = measureJson(doc);
  String whole;
  EXPECT_TRUE(JsonFile::serializeWhole("TEST", kPath, doc, whole));
  EXPECT_EQ(std::string(whole.c_str()), serialised(doc));
}

// The document itself ran out of memory: a key it could not store would be missing from the file.
TEST_F(JsonFileTest, AnOverflowedDocumentLeavesTheFileAsItWas) {
  json_file_fixture::state.files[kPath] = "{\"previous\":true}";
  RefusingAllocator allocator(1);
  JsonDocument doc(&allocator);
  fill(doc);
  ASSERT_TRUE(doc.overflowed());
  EXPECT_FALSE(JsonFile::save("TEST", kPath, doc));
  EXPECT_EQ(json_file_fixture::state.writes, 0);
  EXPECT_EQ(json_file_fixture::state.files[kPath], "{\"previous\":true}");
}

// One of the stores that wrote the String unchecked: on a heap where the String cannot grow, the
// weather settings on the card stay as they were.
TEST_F(JsonFileTest, WeatherSettingsAreNotCutShort) {
  auto& store = WeatherSettingsStore::getInstance();
  store.setLocation(52520000, 13405000, "Berlin");
  store.setForecastDays(5);
  ASSERT_TRUE(store.saveToFile());
  const std::string saved = json_file_fixture::state.files[kWeatherPath];
  ASSERT_NE(saved.find("\"Berlin\""), std::string::npos) << saved;

  store.setLocation(48137000, 11575000, "Munich");
  String::growLimit() = 40;
  EXPECT_FALSE(store.saveToFile());
  EXPECT_EQ(json_file_fixture::state.files[kWeatherPath], saved);
}

}  // namespace
