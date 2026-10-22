#include "FontInstaller.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_rom_crc.h>

#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"
#include "util/FontDownloadRules.h"

FontInstaller::FontInstaller(SdCardFontRegistry& registry) : registry_(registry) {}

bool FontInstaller::isValidFamilyName(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;

  const size_t nameLen = strlen(name);
  if (nameLen == 0 || nameLen > MAX_FAMILY_NAME_LEN) return false;

  if (strstr(name, "..") != nullptr) return false;
  if (strchr(name, '/') != nullptr) return false;
  if (strchr(name, '\\') != nullptr) return false;

  for (const char* p = name; *p != '\0'; ++p) {
    char c = *p;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
      return false;
    }
  }
  return true;
}

bool FontInstaller::isValidFontFileName(const char* name) {
  if (name == nullptr || name[0] == '\0') return false;
  // Matches the cap used by the web-server manifest parser; the SD-side path buffer is 128
  // bytes including the family prefix, so 60 leaves plenty of headroom.
  static constexpr size_t MAX_FONT_FILE_NAME_LEN = 60;
  const size_t nameLen = strlen(name);
  if (nameLen > MAX_FONT_FILE_NAME_LEN) return false;
  if (name[0] == '/') return false;
  if (strchr(name, '\\') != nullptr) return false;
  if (strstr(name, "..") != nullptr) return false;
  // Reject multiple slashes or trailing slash to limit directory depth
  int slashCount = 0;
  for (const char* p = name; *p; ++p) {
    if (*p == '/') {
      ++slashCount;
      if (slashCount > 1 || *(p + 1) == '\0' || *(p + 1) == '/') return false;
    }
  }
  return true;
}

bool FontInstaller::ensureFamilyDir(const char* familyName) {
  if (!isValidFamilyName(familyName)) {
    LOG_ERR("FONT", "Invalid family name: %s", familyName ? familyName : "<null>");
    return false;
  }

  const size_t baseLen = strlen(SdCardFontRegistry::FONTS_DIR);
  const size_t familyLen = strlen(familyName);
  const size_t neededLen = baseLen + 1 + familyLen + 1;  // "/" + NUL
  if (neededLen > 128) {
    LOG_ERR("FONT", "Family dir path too long: %s", familyName);
    return false;
  }

  if (!Storage.exists(SdCardFontRegistry::FONTS_DIR)) {
    if (!Storage.mkdir(SdCardFontRegistry::FONTS_DIR)) {
      LOG_ERR("FONT", "Failed to create fonts dir: %s", SdCardFontRegistry::FONTS_DIR);
      return false;
    }
  }

  char dirPath[128];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", SdCardFontRegistry::FONTS_DIR, familyName);

  if (!Storage.exists(dirPath)) {
    if (!Storage.mkdir(dirPath)) {
      LOG_ERR("FONT", "Failed to create family dir: %s", dirPath);
      return false;
    }
  }
  return true;
}

bool FontInstaller::validateFontFile(const char* path) {
  const FontDownloadRules::FileKind kind = FontDownloadRules::kindOf(path);
  if (kind == FontDownloadRules::FileKind::Unknown) {
    LOG_ERR("FONT", "Not a font file the reader installs: %s", path);
    return false;
  }
  FsFile file;
  if (!Storage.openFileForRead("FONT", path, file)) {
    LOG_ERR("FONT", "Cannot open for validation: %s", path);
    return false;
  }

  uint8_t head[FontDownloadRules::kHeadBytes];
  // Signed: a failed read (-1) as a size_t passed the size check and left `head` uninitialised.
  const int bytesRead = file.read(head, sizeof head);
  file.close();

  if (bytesRead < static_cast<int>(sizeof head)) {
    LOG_ERR("FONT", "File too small or unreadable: %s (%d bytes)", path, bytesRead);
    return false;
  }

  if (!FontDownloadRules::headOk(kind, head, sizeof head)) {
    const char* kindName = kind == FontDownloadRules::FileKind::Cpfont     ? ".cpfont"
                           : kind == FontDownloadRules::FileKind::TrueType ? "TrueType"
                                                                           : "OpenType CFF";
    LOG_ERR("FONT", "Not a %s file: %s", kindName, path);
    return false;
  }

  return true;
}

bool FontInstaller::hasRoomFor(const uint64_t bytes, uint64_t* freeBytes) {
  const uint64_t available = Storage.sdFreeBytes();
  if (freeBytes) *freeBytes = available;
  const bool fits = FontDownloadRules::fits(available, bytes);
  if (!fits) {
    LOG_ERR("FONT", "No room for %llu B on the card: %llu B free", static_cast<unsigned long long>(bytes),
            static_cast<unsigned long long>(available));
  }
  return fits;
}

void FontInstaller::buildFontPath(const char* family, const char* filename, char* outBuf, size_t outBufSize) {
  snprintf(outBuf, outBufSize, "%s/%s/%s", SdCardFontRegistry::FONTS_DIR, family, filename);
}

void FontInstaller::buildStagingDirPath(const char* family, char* outBuf, size_t outBufSize) {
  snprintf(outBuf, outBufSize, "%s/%s__staging", SdCardFontRegistry::FONTS_DIR, family);
}

void FontInstaller::buildBackupDirPath(const char* family, char* outBuf, size_t outBufSize) {
  snprintf(outBuf, outBufSize, "%s/%s__backup", SdCardFontRegistry::FONTS_DIR, family);
}

bool FontInstaller::computeFileCrc32(const char* path, uint32_t& outCrc) {
  FsFile f;
  if (!Storage.openFileForRead("FONT", path, f)) {
    return false;
  }
  constexpr size_t BUF_SIZE = 128;
  uint8_t buf[BUF_SIZE];
  uint32_t crc = 0;
  while (f.available()) {
    const int n = f.read(buf, BUF_SIZE);
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
  }
  f.close();
  outCrc = crc;
  return true;
}

FontInstaller::Error FontInstaller::deleteFamily(const char* familyName) {
  if (!isValidFamilyName(familyName)) {
    return Error::INVALID_FAMILY_NAME;
  }

  const size_t baseLen = strlen(SdCardFontRegistry::FONTS_DIR);
  const size_t familyLen = strlen(familyName);
  if (baseLen + 1 + familyLen + 1 > 128) {
    LOG_ERR("FONT", "Family dir path too long: %s", familyName);
    return Error::INVALID_FAMILY_NAME;
  }

  char dirPath[128];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", SdCardFontRegistry::FONTS_DIR, familyName);

  if (!Storage.exists(dirPath)) {
    LOG_DBG("FONT", "Family dir does not exist: %s", dirPath);
    return Error::OK;
  }

  if (!Storage.removeDir(dirPath)) {
    LOG_ERR("FONT", "Failed to remove family dir: %s", dirPath);
    return Error::SD_WRITE_ERROR;
  }

  if (strcmp(SETTINGS.sdFontFamilyName, familyName) == 0) {
    SETTINGS.sdFontFamilyName[0] = '\0';
    SETTINGS.saveToFile();
    LOG_DBG("FONT", "Cleared active SD font (deleted family: %s)", familyName);
  }

  return Error::OK;
}

void FontInstaller::refreshRegistry() { registry_.discover(); }

bool FontInstaller::isFamilyInstalled(const char* familyName) const {
  return registry_.findFamily(familyName) != nullptr;
}

bool FontInstaller::isFamilyOnCard(const char* familyName) const {
  if (isFamilyInstalled(familyName)) return true;
  if (!isValidFamilyName(familyName)) return false;
  char dirPath[128];
  snprintf(dirPath, sizeof(dirPath), "%s/%s", SdCardFontRegistry::FONTS_DIR, familyName);
  return Storage.exists(dirPath);
}

bool FontInstaller::hasUpdate(const FontCatalog& families, const size_t family) {
  size_t same = 0;
  size_t changed = 0;
  size_t absent = 0;
  const char* familyName = families.name(family);
  for (size_t j = 0; j < families.fileCount(family); j++) {
    char path[128];
    buildFontPath(familyName, families.fileLocalName(family, j), path, sizeof(path));
    FsFile f;
    if (!Storage.openFileForRead("FONT", path, f)) {
      ++absent;
      continue;
    }
    const size_t actual = f.fileSize();
    f.close();
    if (actual == families.fileSize(family, j)) {
      ++same;
    } else {
      ++changed;
    }
  }
  return FontDownloadRules::isUpdate(same, changed, absent);
}
