#include "PluginReach.h"

#include <FileReplace.h>
#include <HalStorage.h>
#include <HeapFit.h>
#include <JsonFile.h>
#include <JsonText.h>
#include <Logging.h>

#include <cstring>

#include "util/PluginLocations.h"

namespace PluginReach {

namespace {

// A manifest carries a title, a mount point and allowedHosts; the cap keeps a stray large file in a plugin folder
// from being read into the heap. CrossPointWebServer reads manifests with the same cap.
constexpr size_t MANIFEST_MAX_BYTES = 4096;

// The File Manager's Details dialog is part of the web interface, not a plugin, but its cover search reads Goodreads
// and Google Books covers through the relay like the plugin it grew from. Its scope carries a ':', which no FAT folder
// name can, so no plugin can claim this list.
constexpr char DETAILS_SCOPE[] = ":details";
constexpr const char* const DETAILS_HOSTS[] = {"www.goodreads.com", ".gr-assets.com", "books.google.com"};

// A name that stays in the folder it is joined to: no separator, not "." or "..".
bool isFolderName(const std::string& name) {
  return !name.empty() && name.find_first_of("/\\") == std::string::npos && name != "." && name != "..";
}

// The bytes Reach takes for these entries: each one's length and its separator.
size_t entryBytes(const JsonArrayConst entries) {
  size_t bytes = 0;
  for (JsonVariantConst entry : entries) bytes += strlen(entry | "") + 1;
  return bytes;
}

}  // namespace

void loadApprovals(JsonDocument& doc) {
  std::string text;
  if (Storage.exists(kApprovalsPath) && Storage.readFileToString("WEB", kApprovalsPath, kApprovalsMaxBytes, text) &&
      JsonText::parse(doc, text) == DeserializationError::Ok && doc.is<JsonObject>()) {
    return;
  }
  doc.clear();
  doc.to<JsonObject>();
}

bool saveApprovals(const JsonDocument& doc) {
  String text;
  if (!JsonFile::serializeWhole("WEB", kApprovalsPath, doc, text) || text.length() > kApprovalsMaxBytes) {
    return false;
  }
  const std::string temp = std::string(kApprovalsPath) + ".tmp";
  return Storage.writeFile(temp.c_str(), text) && FileReplace::replace(temp.c_str(), kApprovalsPath);
}

Load load(PluginHosts::Reach& reach, const std::string& plugin) {
  reach = PluginHosts::Reach{};
  if (plugin == DETAILS_SCOPE) {
    for (const char* host : DETAILS_HOSTS) reach.allow(host);
    return Load::Ok;
  }
  if (!isFolderName(plugin)) return Load::NoSuchPlugin;
  const std::string dir = PluginLocations::findPluginDir(plugin.c_str());
  return dir.empty() ? Load::NoSuchPlugin : load(reach, plugin, dir);
}

Load load(PluginHosts::Reach& reach, const std::string& plugin, const std::string& dir) {
  reach = PluginHosts::Reach{};
  // Each file is read whole into one block, the manifest and then the approvals, the first freed before the second:
  // the throwing operator new (-fno-exceptions) would abort on a heap without the room.
  if (!heapHasBlockFor(MANIFEST_MAX_BYTES) || !heapHasBlockFor(kApprovalsMaxBytes)) {
    LOG_ERR("PLG", "%s: no room to read the hosts it may reach", plugin.c_str());
    return Load::NoRoom;
  }
  {
    std::string text;
    JsonDocument manifest;
    const std::string path = dir + "/manifest.json";
    if (Storage.exists(path.c_str()) && Storage.readFileToString("PLG", path, MANIFEST_MAX_BYTES, text) &&
        JsonText::parse(manifest, text) == DeserializationError::Ok) {
      const JsonArrayConst hosts = manifest["allowedHosts"].as<JsonArrayConst>();
      const size_t bytes = entryBytes(hosts);  // no more than the manifest's own size
      if (!heapHasBlockFor(bytes)) {
        LOG_ERR("PLG", "%s: no room for the hosts it may reach", plugin.c_str());
        return Load::NoRoom;
      }
      reach.reservePatterns(bytes);
      for (JsonVariantConst entry : hosts) reach.allow(entry | "");
    }
  }
  JsonDocument approvals;
  loadApprovals(approvals);
  const JsonArrayConst approved = approvals[plugin.c_str()].as<JsonArrayConst>();
  const size_t bytes = entryBytes(approved);
  if (!heapHasBlockFor(bytes)) {
    reach = PluginHosts::Reach{};
    LOG_ERR("PLG", "%s: no room for the hosts it may reach", plugin.c_str());
    return Load::NoRoom;
  }
  reach.reserveApproved(bytes);
  for (JsonVariantConst entry : approved) reach.approve(entry | "");
  return Load::Ok;
}

}  // namespace PluginReach
