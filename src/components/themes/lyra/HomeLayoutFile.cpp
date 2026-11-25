#include "HomeLayoutFile.h"

#include <ArduinoJson.h>
#include <JsonText.h>

#include <cstring>

namespace HomeLayoutFile {

namespace {

using HomeTilesLayout::Cell;
using HomeTilesLayout::Widget;

// A shortcut's "to", by destination (HomeTilesLayout::destination): the file's names and the web API's.
constexpr const char* const kDestinationNames[HomeTilesLayout::destination::Count] = {
    "library", "stats", "bookmarks", "transfer", "weather", "recent"};
static_assert(HomeTilesLayout::destination::Library == 0 && HomeTilesLayout::destination::ReadingStats == 1 &&
                  HomeTilesLayout::destination::Bookmarks == 2 && HomeTilesLayout::destination::FileTransfer == 3 &&
                  HomeTilesLayout::destination::Weather == 4 && HomeTilesLayout::destination::Recent == 5,
              "kDestinationNames is indexed by destination");

bool destinationNamed(const char* name, uint8_t& out) {
  for (uint8_t d = 0; d < HomeTilesLayout::destination::Count; ++d) {
    if (std::strcmp(name, kDestinationNames[d]) == 0) {
      out = d;
      return true;
    }
  }
  return false;
}

// The widgets a cell names by "widget" alone.
constexpr struct {
  const char* name;
  Widget widget;
} kPlainWidgets[] = {{"also", Widget::AlsoReading},
                     {"clock", Widget::Clock},
                     {"calendar", Widget::Calendar},
                     {"week", Widget::Summary},
                     {"weather", Widget::Weather}};

// The cell a JSON object describes, or false for one this firmware does not know. A picture takes
// the option of its path among `pictures`, adding it while there is room.
bool cellFrom(JsonObjectConst object, Cell& cell, std::string* pictures) {
  const char* widget = object["widget"] | "";
  for (const auto& plain : kPlainWidgets) {
    if (std::strcmp(widget, plain.name) == 0) {
      cell = Cell{plain.widget};
      return true;
    }
  }
  if (std::strcmp(widget, "book") == 0) {
    const char* size = object["size"] | "";
    cell = Cell{Widget::Book, static_cast<uint8_t>(std::strcmp(size, "compact") == 0 ? 1 : 0)};
    return true;
  }
  if (std::strcmp(widget, "shortcut") == 0) {
    uint8_t destination = 0;
    if (!destinationNamed(object["to"] | "", destination)) return false;
    cell = Cell{Widget::Shortcut, destination};
    return true;
  }
  if (std::strcmp(widget, "picture") == 0) {
    const char* path = object["path"] | "";
    const std::string source = path[0] != '\0' ? path : "/home";
    for (int i = 0; i < HomeTilesLayout::kMaxPictures; ++i) {
      if (pictures[i].empty()) pictures[i] = source;
      if (pictures[i] == source) {
        cell = Cell{Widget::Picture, static_cast<uint8_t>(i)};
        return true;
      }
    }
    return false;  // more pictures than a layout keeps
  }
  return false;
}

}  // namespace

const char* destinationName(const uint8_t destination) {
  return destination < HomeTilesLayout::destination::Count ? kDestinationNames[destination] : "";
}

Parsed parse(const std::string& json) {
  Parsed parsed;
  if (json.empty() || json.size() > kMaxBytes) return parsed;
  JsonDocument doc;
  if (JsonText::parse(doc, json)) return parsed;
  if (doc["version"].as<int>() != 1) return parsed;
  // Home has one book card and one Also reading row: the selector, the cover pass and the drawing
  // each know one. A second is left out as an unknown widget is, its partner taking the row.
  bool hasBook = false;
  bool hasStrip = false;
  for (JsonArrayConst row : doc["rows"].as<JsonArrayConst>()) {
    if (parsed.config.rowCount >= HomeTilesLayout::kMaxRows) break;
    Cell cells[2]{};
    int count = 0;
    for (JsonObjectConst object : row) {
      Cell cell;
      if (count >= 2 || !cellFrom(object, cell, parsed.pictures)) continue;
      if ((cell.widget == Widget::Book && hasBook) || (cell.widget == Widget::AlsoReading && hasStrip)) continue;
      cells[count++] = cell;
    }
    // Two side by side only where both may share a row; otherwise the first takes it alone.
    if (count == 2 && !(HomeTilesLayout::pairs(cells[0].widget) && HomeTilesLayout::pairs(cells[1].widget))) count = 1;
    if (count == 1) parsed.config.add(cells[0]);
    if (count == 2) parsed.config.add(cells[0], cells[1]);
    for (int c = 0; c < count; ++c) {
      hasBook = hasBook || cells[c].widget == Widget::Book;
      hasStrip = hasStrip || cells[c].widget == Widget::AlsoReading;
    }
  }
  parsed.ok = parsed.config.rowCount > 0;
  return parsed;
}

}  // namespace HomeLayoutFile
