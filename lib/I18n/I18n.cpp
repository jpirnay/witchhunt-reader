#include "I18n.h"

#include <FlashLangPartition.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "I18nStrings.h"
#include "LangPack.h"

using namespace i18n_strings;

// Settings file path
static constexpr const char* SETTINGS_FILE = "/.crosspoint/language.bin";
static constexpr uint8_t SETTINGS_VERSION = 2;

static constexpr auto KEY_COUNT = static_cast<uint16_t>(StrId::_COUNT);
static_assert(langpack::WINDOW_BYTES == FlashLangPartition::INFLATE_RING_BYTES, "the packs' window is the slot's ring");
static_assert(langpack::WINDOW_BYTES + langpack::IO_BYTES <= FlashLangPartition::SCRATCH_BYTES,
              "a pack inflates in the scratch the slot builder lends");

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  // FALLBACK: the language pack has no string for this key, so it reads as English. English itself
  // never carries the flag.
  const uint16_t off = _active.offsets[index];
  if (off & FlashLangPartition::FALLBACK) return STRINGS_EN_DATA + OFFSETS_EN[index];
  return _active.data + off;
}

namespace {

// The pack file on the card: its header read, the file left open for the build.
struct CardPack {
  HalFile file;
  langpack::Header header;
};

bool openCardPack(const char* path, const char* code, CardPack& pack) {
  if (!Storage.exists(path) || !Storage.openFileForRead("I18N", path, pack.file)) return false;
  uint8_t bytes[langpack::HEADER_BYTES];
  if (pack.file.read(bytes, sizeof(bytes)) != static_cast<int>(sizeof(bytes)) ||
      !langpack::parseHeader(bytes, sizeof(bytes), pack.header)) {
    LOG_ERR("I18N", "%s is not a language pack this firmware reads", path);
    return false;
  }
  // The file's name says which language it is for; its header must say the same.
  if (strcmp(pack.header.code, code) != 0) {
    LOG_ERR("I18N", "%s holds language %s", path, pack.header.code);
    return false;
  }
  if (pack.file.fileSize() != pack.header.extOffset() + pack.header.extLen) {
    LOG_ERR("I18N", "%s is %u B, its header says %u B", path, static_cast<unsigned>(pack.file.fileSize()),
            static_cast<unsigned>(pack.header.extOffset() + pack.header.extLen));
    return false;
  }
  return true;
}

// The entries of a pack, into the slot: those of keys this firmware has, by StrId.
class SlotStrings final : public langpack::EntrySink {
 public:
  explicit SlotStrings(FlashLangPartition::StringsWriter& out) : out_(out) {}
  bool begin(const uint32_t hash) override {
    const uint16_t id = langpack::findKey(hash, KEY_HASHES, KEYS_BY_HASH, KEY_COUNT);
    skip_ = id == langpack::NO_KEY;
    if (skip_) {
      ++unknown_;
      return true;
    }
    ++known_;
    return out_.beginKey(id);
  }
  bool bytes(const char* data, const size_t len) override { return skip_ || out_.write(data, len); }
  bool end() override { return skip_ || out_.write("", 1); }
  uint32_t known() const { return known_; }
  uint32_t unknown() const { return unknown_; }

 private:
  FlashLangPartition::StringsWriter& out_;
  bool skip_ = false;
  uint32_t known_ = 0;
  uint32_t unknown_ = 0;
};

int readCard(void* ctx, uint8_t* buf, const size_t len) { return static_cast<HalFile*>(ctx)->read(buf, len); }

bool feedParser(void* ctx, const uint8_t* data, const size_t len) {
  return static_cast<langpack::EntryParser*>(ctx)->feed(data, len);
}

// FlashLangPartition::Strings::produce: the pack's payload, inflated through the slot builder's scratch.
bool producePackStrings(void* ctx, FlashLangPartition::StringsWriter& out) {
  auto& pack = *static_cast<CardPack*>(ctx);
  const langpack::Header& h = pack.header;
  if (!pack.file.seekSet(h.payloadOffset())) return false;
  SlotStrings strings(out);
  langpack::EntryParser parser(strings);
  uint8_t* scratch = out.scratch();
  if (!langpack::inflateSection({&readCard, &pack.file}, h.payloadLen, h.rawLen, h.payloadCrc, scratch,
                                scratch + langpack::WINDOW_BYTES, {&feedParser, &parser}) ||
      !parser.atBoundary()) {
    LOG_ERR("I18N", "language pack %s does not inflate to what its header says", h.code);
    return false;
  }
  LOG_INF("I18N", "language pack %s: %u strings, %u of keys this firmware does not have", h.code,
          static_cast<unsigned>(strings.known()), static_cast<unsigned>(strings.unknown()));
  return true;
}

}  // namespace

bool I18n::packPath(const Language lang, char* out, const size_t outSize) {
  const auto index = static_cast<size_t>(lang);
  if (index == 0 || index >= static_cast<size_t>(Language::_COUNT)) return false;
  char code[8];
  size_t n = 0;
  for (const char* c = LANGUAGE_CODES[index]; *c && n + 1 < sizeof(code); ++c) {
    code[n++] = (*c >= 'A' && *c <= 'Z') ? static_cast<char>(*c - 'A' + 'a') : *c;
  }
  code[n] = '\0';
  return snprintf(out, outSize, "%s/%s.cplang", PACK_DIR, code) < static_cast<int>(outSize);
}

bool I18n::packOnCard(const Language lang) {
  char path[48];
  return !packPath(lang, path, sizeof(path)) || Storage.exists(path);
}

void I18n::refreshActiveStrings() {
  // Drop to English FIRST: _active may point into the mapping that build()
  // below is about to tear down, and a stale pointer here is a crash in every
  // drawn row. The extension's reader lets go of it for the same reason.
  _active = englishStrings();
  if (_ext) _ext->attach(_language, nullptr, 0);

  char path[48];
  if (!packPath(_language, path, sizeof(path))) return;  // English
  const auto langIndex = static_cast<uint8_t>(_language);
  // The code as the pack's file name and header spell it: "de".
  const char* code = path + strlen(PACK_DIR) + 1;
  char codeBuf[langpack::CODE_BYTES + 1] = {};
  for (size_t i = 0; i < langpack::CODE_BYTES && code[i] != '.'; ++i) codeBuf[i] = code[i];

  CardPack pack;
  if (!openCardPack(path, codeBuf, pack)) {
    LOG_INF("I18N", "language %s: no pack on the card (%s) - English until one is installed", codeBuf, path);
    return;
  }

  // What the slot must hold for this language: the pack's strings for this firmware's keys, and
  // whatever the extension adds. The extension's stamp is folded in so new font data rebuilds the
  // slot as new strings do.
  const uint32_t extBytes = _ext ? _ext->bytes(_language) : 0;
  uint32_t stamp = langpack::slotStamp(pack.header, KEY_LAYOUT_STAMP);
  if (extBytes) stamp ^= _ext->stamp(_language);

  // Built when the slot does not hold this pack -- a different language, another pack of the same
  // one, a firmware with other keys (the stamp covers all three), or bytes that changed behind its
  // header since, which acquire() finds by the slot's CRCs and rebuilds at once, in this boot.
  // Otherwise the pack's header is all that was read of the card. Rebuilding costs an erase plus
  // ~30 KB of inflate; a CJK language adds its UI faces, ~45-73 KB of inflate more.
  struct Producer {
    LanguagePackExtension* ext;
    Language lang;
  } producer{_ext, _language};
  const FlashLangPartition::Extension extension{extBytes,
                                                [](void* ctx, FlashLangPartition::SlotWriter& out) {
                                                  const auto* p = static_cast<const Producer*>(ctx);
                                                  return p->ext->produce(p->lang, out);
                                                },
                                                &producer};
  // The strings never outgrow the payload they come from: its hashes are dropped on the way.
  const FlashLangPartition::Strings strings{pack.header.rawLen, &producePackStrings, &pack};
  FlashLangPartition::Mapped mapped;
  const bool ok =
      FlashLangPartition::acquire(langIndex, stamp, KEY_COUNT, strings, extBytes ? &extension : nullptr, &mapped);
  pack.file.close();
  if (!ok) {
    LOG_ERR("I18N", "language pack %s could not be built or mapped - staying on English", codeBuf);
    return;
  }

  _active = {mapped.data, mapped.offsets};
  LOG_INF("I18N", "language pack %s active", codeBuf);

  // An extension that failed to build, to map or to verify is simply not attached; its reader works
  // without it. One that failed to build or to verify is built again by the next refresh; one the
  // MMU had no page for is not, since a rebuild would not change that.
  if (_ext && mapped.ext && mapped.extLen == extBytes) _ext->attach(_language, mapped.ext, mapped.extLen);
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  _language = lang;
  refreshActiveStrings();
  saveSettings();
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

const char* I18n::getLanguageCode(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return LANGUAGE_CODES[0];
  }
  return LANGUAGE_CODES[index];
}

void I18n::saveSettings() {
  Storage.mkdir("/.crosspoint");

  FsFile file;
  if (!Storage.openFileForWrite("I18N", SETTINGS_FILE, file)) {
    LOG_ERR("I18N", "Failed to save settings");
    return;
  }

  serialization::writePod(file, SETTINGS_VERSION);
  serialization::writeString(file, getLanguageCode(_language));

  file.close();
  LOG_INF("I18N", "Settings saved: language=%d code=%s", static_cast<int>(_language), getLanguageCode(_language));
}

void I18n::loadSettings() {
  loadLanguageSetting();
  // Separate step because loadLanguageSetting() has several early exits and the
  // pack has to be resolved on every one of them, including the "no settings
  // file" path that leaves the language at English.
  refreshActiveStrings();
}

void I18n::loadLanguageSetting() {
  FsFile file;
  if (!Storage.openFileForRead("I18N", SETTINGS_FILE, file)) {
    LOG_INF("I18N", "No settings file, using default (English)");
    return;
  }

  uint8_t version;
  serialization::readPod(file, version);

  if (version == SETTINGS_VERSION) {
    std::string code;
    serialization::readString(file, code);
    bool found = false;

    for (uint8_t i = 0; i < getLanguageCount(); i++) {
      if (code == LANGUAGE_CODES[i]) {
        _language = static_cast<Language>(i);
        found = true;
        break;
      }
    }

    if (found) {
      LOG_INF("I18N", "Loaded language code: %s (%d)", code.c_str(), static_cast<int>(_language));
    } else {
      LOG_ERR("I18N", "Unknown language code in settings: %s", code.c_str());
    }
    file.close();
    return;
  }

  // Legacy migration path: version 1 stored language enum index directly.
  if (version == 1) {
    uint8_t lang;
    serialization::readPod(file, lang);
    if (lang < static_cast<size_t>(Language::_COUNT)) {
      _language = static_cast<Language>(lang);
      LOG_INF("I18N", "Migrating v1 language index: %d -> %s", static_cast<int>(_language), getLanguageCode(_language));
      file.close();
      saveSettings();
      return;
    }
    file.close();
    LOG_ERR("I18N", "Invalid v1 language index: %d", static_cast<int>(lang));
    return;
  }

  LOG_ERR("I18N", "Settings version mismatch: %d\n", static_cast<int>(version));

  file.close();
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
