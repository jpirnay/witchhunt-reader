#include "OpdsServerStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"
#include "util/UrlUtils.h"

OpdsServerStore OpdsServerStore::instance;

namespace {
constexpr char OPDS_FILE_JSON[] = "/.crosspoint/opds.json";

bool containsWhitespace(const std::string& value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch); });
}
}  // namespace

namespace OpdsServerValidation {

std::optional<std::string> normalizeUrl(const std::string& url) {
  if (url.empty() || containsWhitespace(url)) {
    return std::nullopt;
  }

  // Repair a mistyped scheme before the check below: "https:/host" has no "://",
  // so it would otherwise be treated as scheme-less and become "https://https:/host".
  std::string normalized = UrlUtils::repairSchemeSeparator(url);
  if (normalized.find("://") == std::string::npos) {
    normalized = "https://" + normalized;
  }

  const bool hasHttpScheme = normalized.rfind("http://", 0) == 0 || normalized.rfind("https://", 0) == 0;
  if (!hasHttpScheme || UrlUtils::extractHostname(normalized).empty()) {
    return std::nullopt;
  }

  return normalized;
}

}  // namespace OpdsServerValidation

bool OpdsServerStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveOpds(*this, OPDS_FILE_JSON);
}

bool OpdsServerStore::loadFromFile() {
  if (Storage.exists(OPDS_FILE_JSON)) {
    String json = Storage.readFile(OPDS_FILE_JSON);
    if (json.isEmpty()) {
      LOG_ERR("OPS", "Failed to parse %s", OPDS_FILE_JSON);
      return false;
    }

    // resave flag is set when passwords were stored in plaintext and need re-obfuscation
    bool resave = false;
    bool result = JsonSettingsIO::loadOpds(*this, json.c_str(), &resave);
    if (!result) {
      LOG_ERR("OPS", "Failed to parse %s", OPDS_FILE_JSON);
      return false;
    }
    if (resave) {
      LOG_DBG("OPS", "Resaving JSON with obfuscated passwords");
      if (!saveToFile()) {
        LOG_ERR("OPS", "Failed to resave %s after password migration", OPDS_FILE_JSON);
      }
    }
    return true;
  }

  // No opds.json found — attempt one-time migration from the legacy single-server
  // fields in CrossPointSettings (opdsServerUrl/opdsUsername/opdsPassword).
  if (migrateFromSettings()) {
    LOG_DBG("OPS", "Migrated legacy OPDS settings");
    return true;
  }

  return false;
}

bool OpdsServerStore::migrateFromSettings() {
  if (strlen(SETTINGS.opdsServerUrl) == 0) {
    return false;
  }

  OpdsServer server;
  server.name = "OPDS Server";
  server.url = SETTINGS.opdsServerUrl;
  server.username = SETTINGS.opdsUsername;
  server.password = SETTINGS.opdsPassword;
  servers.push_back(std::move(server));

  if (saveToFile()) {
    // Clear legacy fields so migration won't run again on next boot
    SETTINGS.opdsServerUrl[0] = '\0';
    SETTINGS.opdsUsername[0] = '\0';
    SETTINGS.opdsPassword[0] = '\0';
    SETTINGS.saveToFile();
    LOG_DBG("OPS", "Migrated single-server OPDS config to opds.json");
    return true;
  }

  // Save failed — roll back in-memory state so we don't have a partial migration
  servers.clear();
  return false;
}

std::optional<size_t> OpdsServerStore::addServer(const OpdsServer& server) {
  if (servers.size() >= MAX_SERVERS) {
    LOG_DBG("OPS", "Cannot add more servers, limit of %zu reached", MAX_SERVERS);
    return std::nullopt;
  }

  const auto originalServers = servers;
  servers.push_back(server);
  if (!saveToFile()) {
    servers = originalServers;
    LOG_ERR("OPS", "Failed to persist added server, rolled back in-memory state");
    return std::nullopt;
  }

  const size_t insertedIndex = servers.size() - 1;
  LOG_DBG("OPS", "Added server at index %zu: %s", insertedIndex, server.name.c_str());
  return insertedIndex;
}

bool OpdsServerStore::updateServer(size_t index, const OpdsServer& server) {
  if (index >= servers.size()) {
    return false;
  }

  const auto originalServers = servers;
  servers[index] = server;
  if (!saveToFile()) {
    servers = originalServers;
    LOG_ERR("OPS", "Failed to persist updated server at index %zu, rolled back in-memory state", index);
    return false;
  }

  LOG_DBG("OPS", "Updated server at index %zu: %s", index, server.name.c_str());
  return true;
}

bool OpdsServerStore::removeServer(size_t index) {
  if (index >= servers.size()) {
    return false;
  }

  const auto originalServers = servers;
  const std::string removedName = servers[index].name;
  servers.erase(servers.begin() + static_cast<ptrdiff_t>(index));
  if (!saveToFile()) {
    servers = originalServers;
    LOG_ERR("OPS", "Failed to persist removed server at index %zu, rolled back in-memory state", index);
    return false;
  }

  LOG_DBG("OPS", "Removed server at index %zu: %s", index, removedName.c_str());
  return true;
}

const OpdsServer* OpdsServerStore::getServer(size_t index) const {
  if (index >= servers.size()) {
    return nullptr;
  }
  return &servers[index];
}
