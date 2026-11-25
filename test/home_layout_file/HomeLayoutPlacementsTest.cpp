// The placements the web Home Screen editor has to reproduce. The device's own parser, filter and
// layout engine lay out a set of layouts on five screens; test/home_layout_file/placements.json holds
// the result, and test/web/home_layout_test.mjs runs the page's js/home-layout.js on the same cases and
// wants the same boxes. A change to the rules changes this file: regenerate it with
//
//   HOME_LAYOUT_PLACEMENTS_UPDATE=1 ctest --test-dir build/test -R HomeLayoutPlacements
//
// and the web test then says whether the page's copy of the rules followed.
#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "components/themes/lyra/HomeLayoutFile.h"
#include "components/themes/lyra/HomeTilesLayout.h"

namespace {

using HomeTilesLayout::Box;
using HomeTilesLayout::Widget;

struct Screen {
  const char* name;
  Box content;
  int top;
  bool compact;
};

// Content boxes as Lyra Tiles gets them (the screen above the button hints): the X4, the X3, the T5S3,
// the Murphy M3 and the X4 held sideways, and two made to sit on either side of a threshold.
constexpr Screen kScreens[] = {
    {"x4", {0, 0, 480, 760}, 40, false},
    {"x3", {0, 0, 528, 752}, 40, false},
    {"t5s3", {0, 0, 540, 920}, 40, false},
    {"murphy", {0, 0, 240, 380}, 28, true},
    {"x4-sideways", {44, 0, 756, 480}, 40, false},
    // Where Also reading is decided: the card would keep 350 px beside it (left out) and exactly 360 (kept).
    {"edge-350", {0, 0, 540, 908}, 40, false},
    {"edge-360", {0, 0, 540, 918}, 40, false},
};

struct Layout {
  const char* name;
  const char* json;
};

constexpr Layout kLayouts[] = {
    {"reader",
     R"({"version":1,"rows":[[{"widget":"book"}],[{"widget":"also"}],[{"widget":"shortcut","to":"library"},{"widget":"shortcut","to":"stats"}],[{"widget":"shortcut","to":"transfer"},{"widget":"shortcut","to":"bookmarks"}]]})"},
    {"dashboard",
     R"({"version":1,"rows":[[{"widget":"book"}],[{"widget":"clock"},{"widget":"picture"}],[{"widget":"week"},{"widget":"shortcut","to":"library"}]]})"},
    {"gallery",
     R"({"version":1,"rows":[[{"widget":"picture"}],[{"widget":"book","size":"compact"}],[{"widget":"shortcut","to":"library"},{"widget":"shortcut","to":"transfer"}]]})"},
    {"compact-card",
     R"({"version":1,"rows":[[{"widget":"book","size":"compact"}],[{"widget":"calendar"},{"widget":"weather"}],[{"widget":"shortcut","to":"library"},{"widget":"shortcut","to":"recent"}],[{"widget":"week"}]]})"},
    {"every-widget",
     R"({"version":1,"rows":[[{"widget":"book"}],[{"widget":"also"}],[{"widget":"clock"},{"widget":"picture","path":"/home/a.jpg"}],[{"widget":"calendar"},{"widget":"week"}],[{"widget":"weather"},{"widget":"picture"}],[{"widget":"shortcut","to":"bookmarks"},{"widget":"shortcut","to":"weather"}]]})"},
    {"picture-over-card",
     R"({"version":1,"rows":[[{"widget":"picture","path":"/home/b.png"}],[{"widget":"book"}],[{"widget":"also"}],[{"widget":"clock"}]]})"},
    {"strip-without-card",
     R"({"version":1,"rows":[[{"widget":"also"}],[{"widget":"shortcut","to":"library"},{"widget":"shortcut","to":"stats"}],[{"widget":"weather"}]]})"},
    {"across",
     R"({"version":1,"rows":[[{"widget":"weather"}],[{"widget":"week"}],[{"widget":"clock"}],[{"widget":"calendar"}],[{"widget":"shortcut","to":"recent"}]]})"},
    // What the parser leaves out: the book beside a clock, a third widget, an unknown one, a third
    // picture, a seventh row.
    {"left-out",
     R"({"version":1,"rows":[[{"widget":"book"},{"widget":"clock"}],[{"widget":"clock"},{"widget":"week"},{"widget":"weather"}],[{"widget":"sundial"},{"widget":"shortcut","to":"library"}],[{"widget":"picture","path":"/a.jpg"},{"widget":"picture","path":"/b.jpg"}],[{"widget":"picture","path":"/c.jpg"},{"widget":"shortcut","to":"stats"}],[{"widget":"shortcut","to":"transfer"}],[{"widget":"calendar"}]]})"},
};

constexpr uint32_t kEverything = (1u << HomeTilesLayout::destination::Count) - 1;
// No bookmarks, Weather switched off: their shortcuts are left out.
constexpr uint32_t kBare =
    kEverything & ~(1u << HomeTilesLayout::destination::Bookmarks) & ~(1u << HomeTilesLayout::destination::Weather);

const char* widgetName(const Widget w) {
  switch (w) {
    case Widget::Book:
      return "book";
    case Widget::AlsoReading:
      return "also";
    case Widget::Shortcut:
      return "shortcut";
    case Widget::Clock:
      return "clock";
    case Widget::Calendar:
      return "calendar";
    case Widget::Summary:
      return "week";
    case Widget::Weather:
      return "weather";
    case Widget::Picture:
      return "picture";
  }
  return "";
}

void addBox(JsonArray to, const Box& b) {
  JsonArray box = to.add<JsonArray>();
  box.add(b.x);
  box.add(b.y);
  box.add(b.w);
  box.add(b.h);
}

// Settings, and More when an available destination with a menu entry is not among the shortcuts the
// placement shows: LyraTilesTheme::homeShownDestinations and collectHomeMenuEntries, as the page
// works it out.
int bottomCount(const HomeTilesLayout::Placement& p, const uint32_t available) {
  uint32_t shown = 0;
  for (int i = 0; i < p.count; ++i) {
    if (p.items[i].cell.widget == Widget::Shortcut) shown |= 1u << p.items[i].cell.option;
  }
  const uint32_t menu = kEverything & ~(1u << HomeTilesLayout::destination::Recent);
  return (available & menu & ~shown) != 0 ? 2 : 1;
}

std::string placementsJson() {
  std::string out = "{\"cases\": [\n";
  bool first = true;
  for (const Screen& screen : kScreens) {
    for (const Layout& layout : kLayouts) {
      for (const uint32_t available : {kEverything, kBare}) {
        const HomeLayoutFile::Parsed parsed = HomeLayoutFile::parse(layout.json);
        const HomeTilesLayout::Config rows = HomeTilesLayout::withAvailable(parsed.config, available);
        const HomeTilesLayout::Dimensions& d = screen.compact ? HomeTilesLayout::kCompact : HomeTilesLayout::kRegular;
        const int bottom = bottomCount(HomeTilesLayout::place(screen.content, screen.top, rows, 2, d), available);
        const HomeTilesLayout::Placement p = HomeTilesLayout::place(screen.content, screen.top, rows, bottom, d);

        JsonDocument doc;
        doc["name"] = std::string(screen.name) + "/" + layout.name + (available == kEverything ? "" : "/bare");
        doc["layout"] = layout.json;
        JsonObject device = doc["device"].to<JsonObject>();
        JsonArray content = device["content"].to<JsonArray>();
        content.add(screen.content.x);
        content.add(screen.content.y);
        content.add(screen.content.w);
        content.add(screen.content.h);
        device["top"] = screen.top;
        device["compact"] = screen.compact;
        JsonArray names = device["available"].to<JsonArray>();
        for (uint8_t dest = 0; dest < HomeTilesLayout::destination::Count; ++dest) {
          if ((available & (1u << dest)) != 0) names.add(HomeLayoutFile::destinationName(dest));
        }
        JsonObject placement = doc["placement"].to<JsonObject>();
        JsonArray items = placement["items"].to<JsonArray>();
        for (int i = 0; i < p.count; ++i) {
          JsonObject item = items.add<JsonObject>();
          item["widget"] = widgetName(p.items[i].cell.widget);
          item["option"] = p.items[i].cell.option;
          JsonArray box = item["box"].to<JsonArray>();
          box.add(p.items[i].box.x);
          box.add(p.items[i].box.y);
          box.add(p.items[i].box.w);
          box.add(p.items[i].box.h);
          item["full"] = p.items[i].full;
          item["row"] = p.items[i].row;
        }
        JsonArray bottomBoxes = placement["bottom"].to<JsonArray>();
        for (int i = 0; i < p.bottomCount; ++i) addBox(bottomBoxes, p.bottom[i]);
        placement["dropped"] = p.dropped;
        placement["sideways"] = p.sideways;

        std::string line;
        serializeJson(doc, line);
        out += (first ? "" : ",\n") + line;
        first = false;
      }
    }
  }
  return out + "\n]}\n";
}

}  // namespace

TEST(HomeLayoutPlacements, MatchTheFileThePageIsTestedAgainst) {
  const std::string now = placementsJson();
  if (std::getenv("HOME_LAYOUT_PLACEMENTS_UPDATE") != nullptr) {
    std::ofstream(HOME_LAYOUT_PLACEMENTS) << now;
    GTEST_SKIP() << "wrote " << HOME_LAYOUT_PLACEMENTS;
  }
  std::ifstream in(HOME_LAYOUT_PLACEMENTS);
  std::stringstream saved;
  saved << in.rdbuf();
  EXPECT_EQ(saved.str(), now) << "The layout rules changed: regenerate " << HOME_LAYOUT_PLACEMENTS
                              << " (HOME_LAYOUT_PLACEMENTS_UPDATE=1) and make js/home-layout.js follow.";
}

// The cases cover what the page has to get right: rows dropped from the bottom, Also reading left out
// quietly, the sideways columns, and a shortcut with nothing behind it.
TEST(HomeLayoutPlacements, CoverDropsStripsSidewaysAndFilteredShortcuts) {
  const std::string now = placementsJson();
  EXPECT_NE(now.find("\"dropped\":0"), std::string::npos);
  EXPECT_NE(now.find("\"sideways\":true"), std::string::npos);
  size_t droppedLoudly = 0;
  for (size_t at = now.find("\"dropped\":"); at != std::string::npos; at = now.find("\"dropped\":", at + 1)) {
    if (now.compare(at, 11, "\"dropped\":0") != 0) ++droppedLoudly;
  }
  EXPECT_GT(droppedLoudly, 0u);
}
