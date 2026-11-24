#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "PluginHosts.h"

// The card-aware half of PluginHosts: what a plugin may reach, read from its manifest's allowedHosts and from the
// hosts the user approved for it. One decision for every request the device makes for a plugin: the relay and
// fetch-to-SD (CrossPointWebServer) and the device events (PluginEvents), so a host a plugin may not reach over the web
// is not reached for it at sleep either.
namespace PluginReach {

// Hosts the user approved for a plugin on a plugin's request (the Plugins page and the plugin host ask), kept apart
// from the plugin's folder so an update keeps them and a removal drops them: {"<plugin>": ["host", ...]}. Exact
// hosts only. Plugins written for upstream declare no allowedHosts, so this is how they reach anything.
inline constexpr char kApprovalsPath[] = "/.crosspoint/plugin-hosts.json";
inline constexpr size_t kApprovalsMaxBytes = 8192;
inline constexpr size_t kMaxApprovalsPerPlugin = 16;

// The approvals, or an empty object when there are none yet or the file is unreadable.
void loadApprovals(JsonDocument& doc);
bool saveApprovals(const JsonDocument& doc);

enum class Load : uint8_t {
  Ok,
  NoSuchPlugin,  // the name is not one, or no folder holds it: the plugin reaches nothing
  NoRoom,        // no block on the heap to read the files: ask again later
};

// Fills `reach` for `plugin`, a single path component the caller has validated, or the scope ":details" (the File
// Manager's Details dialog, whose hosts are built in). Unless Ok, `reach` allows nothing.
Load load(PluginHosts::Reach& reach, const std::string& plugin);
// For a plugin whose folder the caller already knows (PluginLocations::scanPlugins()).
Load load(PluginHosts::Reach& reach, const std::string& plugin, const std::string& dir);

}  // namespace PluginReach
