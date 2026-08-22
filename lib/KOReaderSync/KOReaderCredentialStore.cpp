#include "KOReaderCredentialStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>

#include "../../src/JsonSettingsIO.h"
#include "../../src/util/UrlUtils.h"

// Initialize the static instance
KOReaderCredentialStore KOReaderCredentialStore::instance;

namespace {
constexpr char KOREADER_FILE_JSON[] = "/.crosspoint/koreader.json";

// Default sync server URL
constexpr char DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443";
}  // namespace

bool KOReaderCredentialStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveKOReader(*this, KOREADER_FILE_JSON);
}

bool KOReaderCredentialStore::loadFromFile() {
  if (Storage.exists(KOREADER_FILE_JSON)) {
    String json = Storage.readFile(KOREADER_FILE_JSON);
    if (!json.isEmpty()) {
      bool resave = false;
      bool result = JsonSettingsIO::loadKOReader(*this, json.c_str(), &resave);
      if (result && resave) {
        saveToFile();
        LOG_DBG("KRS", "Resaved KOReader credentials to update format");
      }
      return result;
    }
  }

  LOG_DBG("KRS", "No credentials file found");
  return false;
}

void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
  LOG_DBG("KRS", "Set credentials for user: %s", user.c_str());
}

std::string KOReaderCredentialStore::getMd5Password() const {
  if (password.empty()) {
    return "";
  }

  // Calculate MD5 hash of password using ESP32's MD5Builder
  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();

  return md5.toString().c_str();
}

bool KOReaderCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }

void KOReaderCredentialStore::clearCredentials() {
  username.clear();
  password.clear();
  saveToFile();
  LOG_DBG("KRS", "Cleared KOReader credentials");
}

void KOReaderCredentialStore::setServerUrl(const std::string& url) {
  // Repair a typo'd scheme here as well as in getBaseUrl(), so the settings
  // screen shows -- and the file keeps -- the URL that will actually be used.
  serverUrl = UrlUtils::repairSchemeSeparator(url);
  LOG_DBG("KRS", "Set server URL: %s", serverUrl.empty() ? "(default)" : serverUrl.c_str());
}

std::string KOReaderCredentialStore::getBaseUrl() const {
  std::string url;
  if (serverUrl.empty()) {
    url = DEFAULT_SERVER_URL;
  } else {
    // A URL entered as "https:/host" (one slash) carries a scheme no "://" test can
    // see; without the repair it becomes "http://https:/host", which resolves to the
    // host "https:" and fails every connect.
    const std::string repaired = UrlUtils::repairSchemeSeparator(serverUrl);
    if (repaired != serverUrl) {
      // Quoted so a missing slash is unmistakable in a log; on screen "https:/host"
      // and "https://host" look almost identical.
      LOG_INF("KRS", "Repaired malformed sync URL: '%s' -> '%s'", serverUrl.c_str(), repaired.c_str());
    }
    // Falls back to http:// when no protocol was given (local servers typically don't have SSL).
    url = UrlUtils::ensureProtocol(repaired);
  }

  // Strip trailing slashes to avoid double-slash in API paths
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  return url;
}

void KOReaderCredentialStore::setMatchMethod(DocumentMatchMethod method) {
  matchMethod = method;
  LOG_DBG("KRS", "Set match method: %s", method == DocumentMatchMethod::FILENAME ? "Filename" : "Binary");
}

void KOReaderCredentialStore::setSendMetadata(bool value) {
  sendMetadata = value;
  LOG_DBG("KRS", "Send metadata: %s", value ? "enabled" : "disabled");
}

void KOReaderCredentialStore::setSyncBehavior(KOReaderSyncBehavior behavior) {
  if (static_cast<uint8_t>(behavior) > static_cast<uint8_t>(KOReaderSyncBehavior::SMART)) {
    behavior = KOReaderSyncBehavior::ASK_EVERY_TIME;
  }
  syncBehavior = behavior;
  LOG_DBG("KRS", "Sync behavior: %s", behavior == KOReaderSyncBehavior::SMART ? "smart" : "ask every time");
}
