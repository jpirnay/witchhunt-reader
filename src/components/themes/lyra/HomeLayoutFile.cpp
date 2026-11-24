#include "HomeLayoutFile.h"

#include <ArduinoJson.h>

#include <cstring>

namespace HomeLayoutFile {

namespace {

using HomeTilesLayout::Cell;
using HomeTilesLayout::Widget;

bool destinationNamed(const char* name, uint8_t& out) {
  static constexpr struct {
    const char* name;
    uint8_t destination;
  } kNames[] = {
      {"library", HomeTilesLayout::destination::Library},     {"stats", HomeTilesLayout::destination::ReadingStats},
      {"bookmarks", HomeTilesLayout::destination::Bookmarks}, {"transfer", HomeTilesLayout::destination::FileTransfer},
      {"weather", HomeTilesLayout::destination::Weather},     {"recent", HomeTilesLayout::destination::Recent}};
  for (const auto& entry : kNames) {
    if (std::strcmp(name, entry.name) == 0) {
      out = entry.destination;
      return true;
    }
  }
  return false;
}

// The cell a JSON object describes, or false for one this firmware does not know. A picture takes
// the option of its path among `pictures`, adding it while there is room.
bool cellFrom(JsonObjectConst object, Cell& cell, std::string* pictures) {
  const char* widget = object["widget"] | "";
  if (std::strcmp(widget, "book") == 0) {
    const char* size = object["size"] | "";
    cell = Cell{Widget::Book, static_cast<uint8_t>(std::strcmp(size, "compact") == 0 ? 1 : 0)};
    return true;
  }
  if (std::strcmp(widget, "also") == 0) {
    cell = Cell{Widget::AlsoReading};
    return true;
  }
  if (std::strcmp(widget, "shortcut") == 0) {
    uint8_t destination = 0;
    if (!destinationNamed(object["to"] | "", destination)) return false;
    cell = Cell{Widget::Shortcut, destination};
    return true;
  }
  if (std::strcmp(widget, "clock") == 0) {
    cell = Cell{Widget::Clock};
    return true;
  }
  if (std::strcmp(widget, "calendar") == 0) {
    cell = Cell{Widget::Calendar};
    return true;
  }
  if (std::strcmp(widget, "week") == 0) {
    cell = Cell{Widget::Summary};
    return true;
  }
  if (std::strcmp(widget, "weather") == 0) {
    cell = Cell{Widget::Weather};
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

Parsed parse(const char* json, const size_t length) {
  Parsed parsed;
  if (json == nullptr || length == 0 || length > kMaxBytes) return parsed;
  JsonDocument doc;
  if (deserializeJson(doc, json, length)) return parsed;
  if ((doc["version"] | 0) != 1) return parsed;
  for (JsonArrayConst row : doc["rows"].as<JsonArrayConst>()) {
    if (parsed.config.rowCount >= HomeTilesLayout::kMaxRows) break;
    Cell cells[2]{};
    int count = 0;
    for (JsonObjectConst object : row) {
      Cell cell;
      if (count < 2 && cellFrom(object, cell, parsed.pictures)) cells[count++] = cell;
    }
    // Two side by side only where both may share a row; otherwise the first takes it alone.
    if (count == 2 && !(HomeTilesLayout::pairs(cells[0].widget) && HomeTilesLayout::pairs(cells[1].widget))) count = 1;
    if (count == 1) parsed.config.add(cells[0]);
    if (count == 2) parsed.config.add(cells[0], cells[1]);
  }
  parsed.ok = parsed.config.rowCount > 0;
  return parsed;
}

}  // namespace HomeLayoutFile
