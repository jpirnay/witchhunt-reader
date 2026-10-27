#include "SdCardFontSystem.h"

#include <BuildArena.h>
#include <Epub/BookFonts.h>
#include <EspNorFlash.h>
#include <GfxRenderer.h>
#include <GlyphProvider.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <ttf/TtfFace.h>
#include <ttf/TtfFamily.h>
#include <ttf/TtfFont.h>

#include <climits>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "CjkUiFonts.h"
#include "CrossPointSettings.h"
#include "SdCardFontGlobals.h"
#include "SerialTestApi.h"
#include "activities/RenderLock.h"

static_assert(CrossPointSettings::FONT_SIZE_RUNGS[CrossPointSettings::FONT_SIZE_RUNG_COUNT - 1].points <=
                  TtfFont::kMaxPointSize,
              "TtfFont plans its scratch block for the largest reader size");

namespace {

// Between glyphs and install chunks: a hostile glyph can take most of a second, and an install
// several seconds, all under RenderLock. A tick's sleep every 200 ms lets the idle task run.
void ttfYield() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 200) return;
  last = now;
  vTaskDelay(1);
}

// What a TrueType miss may have when the heap cannot hold its memory (the built-in fonts' page caches, rebuilt by the
// next prewarm), and the set whose misses' memory goes back with RenderLock. Set in begin().
GfxRenderer* ttfRenderer = nullptr;
SdCardFontSystem* ttfSystem = nullptr;

bool ttfReclaim() { return ttfRenderer && ttfRenderer->releaseFontCaches(); }

uint32_t ttfNowMs() { return millis(); }
// A page's glyph uses count once: in its black-and-white pass, the first of its passes.
bool ttfCountsDraw() { return ttfRenderer && ttfRenderer->getRenderMode() == GfxRenderer::BW; }

// A render pass's glyph misses outside the reader draw in the secondary framebuffer, lent on the first one that needs
// a scratch block (glyph_miss::LendScope, ActivityManager) and given back as the frame is about to be pushed
// (GfxRenderer's before-push hook), or as the pass releases the RenderLock when it pushes nothing. The lend is the
// screens' black-and-white one (HomeActivity, the font selector): RED is seeded with the displayed frame while the
// secondary still holds it. The return seeds the secondary from the finished frame and arms the next FAST to diff
// against RED: the push runs with both buffers (held through it, the X3's HALF was a 2.5 s full refresh), and fast
// refresh survives. Nothing of the heap is taken.
std::optional<BuildArena> glyphLend;

void returnGlyphLend() {
  if (!glyphLend || !ttfRenderer) return;
  glyph_miss::regionReturned(&*glyphLend);
  glyphLend.reset();
  ttfRenderer->returnSecondaryBuffer();  // cannot fail: the region never entered the heap
  ttfRenderer->setSingleBufferFastDiff(false);
}

void ttfPassEnded() {
  if (ttfSystem) ttfSystem->releaseTtfMissMemory();
  returnGlyphLend();
}

// Lists a TTF family only when the glyph store can install it, in flash mode or, larger, in SD mode.
bool ttfFamilyFits(const uint32_t* fileBytes, const uint32_t* sdBytes, const uint8_t count) {
  // While the store is open its mappings hide the budget an install would get: the load decides then.
  if (EspNorFlash::liveMappings() != 0) return true;
  EspNorFlash flash;
  return flash.begin() && ttf::fitsStore(fileBytes, sdBytes, count, flash.size(), flash.freeMapPages());
}

}  // namespace

BuildArena* lendFramebufferForGlyphs() {
  if (!ttfRenderer || glyphLend || !ttfRenderer->hasSecondaryBuffer()) return nullptr;
  ttfRenderer->syncRedRamFromFrameBuffer();  // the displayed frame, while the secondary still holds it
  size_t size = 0;
  uint8_t* const region = ttfRenderer->borrowSecondaryBuffer(&size);
  if (!region) return nullptr;
  glyphLend.emplace(region, size);
  if (!glyphLend->valid()) {
    glyphLend.reset();
    ttfRenderer->returnSecondaryBuffer();
    return nullptr;
  }
  ttfRenderer->setSingleBufferFastDiff(true);
  LOG_DBG("SDFS", "Lent the secondary framebuffer to the pass's glyph misses (%u bytes, free=%lu)",
          static_cast<unsigned>(size), static_cast<unsigned long>(esp_get_free_heap_size()));
  return &*glyphLend;
}

// Free-function resolver used by CrossPointSettings::getReaderFontId() and the reader.
// Resolved by the linker — no callback indirection stored in settings.
// It resolves the family the book's language asks for, the one ensureSdFontLoadedForPath() loaded; a font preview
// asks SdCardFontSystem::resolveFontId() for the family it shows.
int resolveSdCardFontId(const char* familyName, uint8_t pointSize) {
  return sdFontSystem.resolveFontId(sdFontSystem.effectiveReaderFamily(familyName).c_str(), pointSize);
}

// --- Font-family dynamic SettingInfo trampolines ---
//
// The font-family SettingInfo lives in a namespace-static SettingsList that is
// initialized at global-static phase, well before sdFontSystem.begin() runs.
// We therefore cannot bake the SD family list into the SettingInfo at
// construction; instead the SettingInfo holds these stateless trampolines that
// consult sdFontSystem at every call. enumLabels is enriched lazily by the
// consumers (SettingsActivity, CrossPointWebServer) before each iteration.

uint8_t fontFamilyDynamicGetter(const void* /*ctx*/) {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto& families = sdFontSystem.registry().getFamilies();
    for (size_t i = 0; i < families.size(); i++) {
      if (families[i].name == SETTINGS.sdFontFamilyName) {
        return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
      }
    }
    // SD family no longer present (card removed?); fall through to built-in.
  }
  return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily
                                                                      : CrossPointSettings::BOOKERLY;
}

void fontFamilyDynamicSetter(void* /*ctx*/, uint8_t value) {
  if (value < CrossPointSettings::BUILTIN_FONT_COUNT) {
    SETTINGS.fontFamily = value;
    SETTINGS.sdFontFamilyName[0] = '\0';
    return;
  }
  const auto& families = sdFontSystem.registry().getFamilies();
  uint8_t sdIdx = value - CrossPointSettings::BUILTIN_FONT_COUNT;
  if (sdIdx < families.size()) {
    strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
    SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  }
}

uint8_t txtFontFamilyDynamicGetter(const void* /*ctx*/) {
  if (SETTINGS.txtSdFontFamilyName[0] != '\0') {
    const auto& families = sdFontSystem.registry().getFamilies();
    for (size_t i = 0; i < families.size(); i++) {
      if (families[i].name == SETTINGS.txtSdFontFamilyName) {
        return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
      }
    }
  }
  return SETTINGS.txtFontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.txtFontFamily
                                                                         : CrossPointSettings::NOTOSANS;
}

void txtFontFamilyDynamicSetter(void* /*ctx*/, uint8_t value) {
  if (value < CrossPointSettings::BUILTIN_FONT_COUNT) {
    SETTINGS.txtFontFamily = value;
    SETTINGS.txtSdFontFamilyName[0] = '\0';
    return;
  }
  const auto& families = sdFontSystem.registry().getFamilies();
  uint8_t sdIdx = value - CrossPointSettings::BUILTIN_FONT_COUNT;
  if (sdIdx < families.size()) {
    strncpy(SETTINGS.txtSdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.txtSdFontFamilyName) - 1);
    SETTINGS.txtSdFontFamilyName[sizeof(SETTINGS.txtSdFontFamilyName) - 1] = '\0';
  }
}

uint8_t fontFamilyOptionCount() {
  return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + sdFontSystem.registry().getFamilies().size());
}

bool fontFamilyOptionsKnown() { return sdFontSystem.discovered(); }

std::string fontFamilyOptionLabel(uint8_t i) {
  if (i < CrossPointSettings::BUILTIN_FONT_COUNT) {
    static const StrId BUILTIN_LABELS[] = {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS};
    return I18N.get(BUILTIN_LABELS[i]);
  }
  const auto& families = sdFontSystem.registry().getFamilies();
  uint8_t sdIdx = i - CrossPointSettings::BUILTIN_FONT_COUNT;
  return sdIdx < families.size() ? families[sdIdx].name : std::string();
}

// --- Font-size dynamic SettingInfo trampolines ---
//
// The rows store a point size, but the settings UI deals in option indices, and the options are
// the sizes the selected family offers -- so both directions go through that family's list.

namespace {
uint8_t sizeIndex(const char* family, const uint8_t pointSize) {
  return sdFontSystem.sizeListFor(family).indexOf(pointSize);
}

void setSizeFromIndex(const char* family, uint8_t& pointSize, const uint8_t index) {
  const ReaderSizeList sizes = sdFontSystem.sizeListFor(family);
  if (index < sizes.count) pointSize = sizes.points[index];
}

uint8_t sizeOptionCount(const char* family) { return sdFontSystem.sizeListFor(family).count; }

std::string sizeOptionLabel(const char* family, const uint8_t index) {
  const ReaderSizeList sizes = sdFontSystem.sizeListFor(family);
  return index < sizes.count ? fontPointSizeLabel(sizes.points[index]) : std::string();
}
}  // namespace

uint8_t fontSizeDynamicGetter(const void* /*ctx*/) {
  return sizeIndex(SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize);
}

void fontSizeDynamicSetter(void* /*ctx*/, const uint8_t index) {
  setSizeFromIndex(SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize, index);
}

uint8_t txtFontSizeDynamicGetter(const void* /*ctx*/) {
  return sizeIndex(SETTINGS.txtSdFontFamilyName, SETTINGS.txtFontPointSize);
}

void txtFontSizeDynamicSetter(void* /*ctx*/, const uint8_t index) {
  setSizeFromIndex(SETTINGS.txtSdFontFamilyName, SETTINGS.txtFontPointSize, index);
}

uint8_t fontSizeOptionCount(const void* /*ctx*/) { return sizeOptionCount(SETTINGS.sdFontFamilyName); }

std::string fontSizeOptionLabel(const void* /*ctx*/, const uint8_t index) {
  return sizeOptionLabel(SETTINGS.sdFontFamilyName, index);
}

uint8_t txtFontSizeOptionCount(const void* /*ctx*/) { return sizeOptionCount(SETTINGS.txtSdFontFamilyName); }

std::string txtFontSizeOptionLabel(const void* /*ctx*/, const uint8_t index) {
  return sizeOptionLabel(SETTINGS.txtSdFontFamilyName, index);
}

const char* bookSdFontFamily(const int8_t builtinFamilyOverride, const std::string& sdFamilyOverride) {
  if (builtinFamilyOverride >= 0) return "";
  if (!sdFamilyOverride.empty()) return sdFamilyOverride.c_str();
  if (sdFontSystem.bookFamilyName()[0] != '\0') return sdFontSystem.bookFamilyName();  // Publisher Fonts
  return SETTINGS.sdFontFamilyName;
}

const SdCardFontFamilyInfo* SdCardFontSystem::findFamily(const char* name) const {
  if (!name || name[0] == '\0') return nullptr;
  if (bookBody_.name == name) return &bookBody_;
  for (const SdCardFontFamilyInfo& accent : bookAccents_) {
    if (accent.name == name) return &accent;
  }
  // Names compared in place: the registry's findFamily() takes a std::string, and this runs while pages are
  // laid out, where building one for a long family name would allocate.
  for (const auto& family : registry_.getFamilies()) {
    if (family.name == name) return &family;
  }
  return nullptr;
}

void SdCardFontSystem::scanBookFamily(SdCardFontFamilyInfo& family, const std::string& name, const std::string& dir) {
  // The same style probe as a family on the card, and only when the font area can take it.
  if (!name.empty() && registry_.scanFamily(dir.c_str(), family) && family.ttf()) {
    family.name = "book:" + name;
    LOG_INF("SDFS", "Book font %s", family.name.c_str());
    return;
  }
  family.name.clear();
  std::vector<SdCardFontFileInfo>().swap(family.files);
}

void SdCardFontSystem::selectBookFamilies(const std::string& cachePath, const uint8_t mode, const char* readerFamily) {
  const SdCardFontFamilyInfo* reader = findFamily(readerFamily);
  const bool besideCpfont = reader && !reader->ttf();
  // The settings an accent limit holds under: the mode and the reader's family, whose glyph store the accents share.
  const char* family = readerFamily ? readerFamily : "";
  const uint32_t settingsKey = glyphstore::fnv1a(family, strlen(family), glyphstore::fnv1a(&mode, 1));
  // Kept across the book's later opens in the same mode; anything else is read again, since the book may have been
  // examined since.
  if ((!bookBody_.name.empty() || !bookAccents_[0].name.empty()) && cachePath == bookCachePath_ && mode == bookMode_ &&
      besideCpfont == bookBesideCpfont_) {
    if (settingsKey != bookSettingsKey_) {
      bookSettingsKey_ = settingsKey;
      accentLimit_ = BookFonts::accentLimit(cachePath, settingsKey);  // another reader font: forgets the limit
    }
    return;
  }
  bookCachePath_ = cachePath;
  bookMode_ = mode;
  bookBesideCpfont_ = besideCpfont;
  bookSettingsKey_ = settingsKey;
  accentLimit_ = cachePath.empty() ? BookFonts::kMaxAccents : BookFonts::accentLimit(cachePath, settingsKey);
  scanBookFamily(bookBody_, {}, {});
  for (SdCardFontFamilyInfo& accent : bookAccents_) scanBookFamily(accent, {}, {});
  bookFonts_ = {};  // what read() leaves alone stays "none"
  if (cachePath.empty()) return;
  std::string name;
  const bool body = mode == CrossPointSettings::PUBLISHER_FONTS_BODY || mode == CrossPointSettings::PUBLISHER_FONTS_ALL;
  if (BookFonts::read(cachePath, &name, &bookFonts_) == BookFonts::State::Family && body) {
    scanBookFamily(bookBody_, name, BookFonts::familyDir(cachePath));
  }
  if (mode < CrossPointSettings::PUBLISHER_FONTS_ACCENTS) return;
  for (uint8_t n = 0; n < BookFonts::kMaxAccents && bookFonts_.accents[n] != 0; ++n) {
    BookFonts::read(cachePath, &name, nullptr, n);
    if (bookBody_.name.empty() && besideCpfont) {
      LOG_INF("SDFS", "Accent font %s left out: the reader's font is a .cpfont family", name.c_str());
      continue;
    }
    scanBookFamily(bookAccents_[n], name, BookFonts::familyDir(cachePath, n));
  }
}

uint8_t SdCardFontSystem::bookAccentsLoaded() const {
  uint8_t loaded = 0;
  for (uint8_t n = 0; n < BookFonts::kMaxAccents; ++n) {
    const char* name = accentFamilyName(n);
    if (name[0] != '\0' && manager_.getFontId(name, manager_.currentPointSize()) != 0) ++loaded;
  }
  return loaded;
}

void SdCardFontSystem::limitBookAccents(const uint8_t accents) {
  if (bookCachePath_.empty() || accents >= accentLimit_) return;
  accentLimit_ = accents;
  BookFonts::setAccentLimit(bookCachePath_, bookSettingsKey_, accents);
  LOG_INF("SDFS", "Book keeps %u accent famil%s from now on", accents, accents == 1 ? "y" : "ies");
}

const BookFonts::Families* SdCardFontSystem::bookFontFamiliesFor(const std::string& cachePath) const {
  return !cachePath.empty() && cachePath == bookCachePath_ && bookMode_ >= CrossPointSettings::PUBLISHER_FONTS_ACCENTS
             ? &bookFonts_
             : nullptr;
}

bool SdCardFontSystem::resumeTtf(GfxRenderer& renderer) {
  if (!manager_.ttfSuspended()) return manager_.ttfLoaded();
  if (SerialTestApi::takeReloadRefusal()) {
    // A test build's CMD:HEAP:REFUSE: the set does not come back, as when the heap is too short for it.
    LOG_INF("SDFS", "TTF set reload refused (CMD:HEAP:REFUSE)");
    manager_.unloadAll(renderer);
    return false;
  }
  const SdCardFontFamilyInfo* families[SdCardFontManager::kMaxTtfFamilies] = {};
  for (uint8_t f = 0; f < manager_.ttfFamilyCount(); ++f) {
    families[f] = findFamily(manager_.ttfFamilyName(f).c_str());
    if (!families[f]) {
      manager_.unloadAll(renderer);  // gone since: the reader loads what is there
      return false;
    }
  }
  return manager_.resumeTtf(families, renderer);
}

void SdCardFontSystem::unload(GfxRenderer& renderer) {
  manager_.unloadAll(renderer);
  selectBookFamilies({});  // the reader is closing; its next open selects again
}

ReaderSizeList SdCardFontSystem::sizeListFor(const char* familyName) const {
  const SdCardFontFamilyInfo* family = findFamily(familyName);
  if (!family) return ReaderSizeList::builtin();       // none, or not on the card: the reader draws a built-in family
  if (family->ttf()) return ReaderSizeList::native();  // drawn natively at every size
  uint8_t sizes[ReaderSizeList::kCapacity];
  size_t n = 0;
  for (const auto& file : family->files) {
    if (n < ReaderSizeList::kCapacity) sizes[n++] = file.pointSize;
  }
  return ReaderSizeList::forFamily(sizes, n);
}

void SdCardFontSystem::setReaderLanguage(const std::string& language) {
  std::string lang;
  lang.reserve(language.size());
  for (const unsigned char ch : language) {
    if (ch == '_') {
      lang.push_back('-');
    } else if (ch >= 'A' && ch <= 'Z') {
      lang.push_back(static_cast<char>(ch - 'A' + 'a'));
    } else {
      lang.push_back(static_cast<char>(ch));
    }
  }

  const auto tagIs = [&lang](const char* tag) {
    const std::string value(tag);
    return lang == value ||
           (lang.size() > value.size() && lang.compare(0, value.size(), value) == 0 && lang[value.size()] == '-');
  };

  ReaderCjkRegion region = ReaderCjkRegion::None;
  if (tagIs("ja")) {
    region = ReaderCjkRegion::Japanese;
  } else if (tagIs("ko")) {
    region = ReaderCjkRegion::Korean;
  } else if (lang == "cn" || tagIs("zh-hans") || tagIs("zh-cn") || tagIs("zh-sg") || tagIs("zh-chs")) {
    region = ReaderCjkRegion::SimplifiedChinese;
  } else if (tagIs("zh-hant") || tagIs("zh-tw") || tagIs("zh-hk") || tagIs("zh-mo") || tagIs("zh-cht")) {
    region = ReaderCjkRegion::TraditionalChinese;
  } else if (tagIs("zh")) {
    region = ReaderCjkRegion::ChineseAmbiguous;
  }

  readerCjkRegion_ = region;
}

void SdCardFontSystem::clearReaderLanguage() {
  readerCjkRegion_ = ReaderCjkRegion::None;
  languageFamily_.clear();
  languageFamilyKey_ = 0;
}

void SdCardFontSystem::setReaderLanguagePending() { readerCjkRegion_ = ReaderCjkRegion::Pending; }

bool SdCardFontSystem::waitsForReaderLanguage() const {
  if (readerCjkRegion_ != ReaderCjkRegion::Pending) return false;
  // The language may give the book a regional sibling, or a Noto CJK family in place of a font without its script,
  // whenever one is installed.
  std::string stem;
  for (const auto& installed : registry_.getFamilies()) {
    if (familyRegion(installed.name, stem) != ReaderCjkRegion::None) return true;
  }
  return false;
}

bool SdCardFontSystem::familyMapsCodepoint(const SdCardFontFamilyInfo& family, const uint32_t codepoint) {
  if (family.files.empty()) return true;
  const SdCardFontFileInfo* regular = &family.files.front();
  for (const auto& file : family.files) {
    if (file.style == 0) regular = &file;
  }
  if (!family.ttf()) {
    bool readable = false;
    const bool maps = SdCardFont::fileMapsCodepoint(regular->path.c_str(), codepoint, &readable);
    return maps || !readable;
  }
  HalFile handle;
  if (!Storage.openFileForRead("SDFS", regular->path.c_str(), handle)) return true;
  // The table directory and the cmap, read through a page cache (4 KB, for this call only).
  auto pages = makeUniqueNoThrow<ttf::SdPageCache>();
  if (!pages) {
    handle.close();
    return true;
  }
  ttf::SdRange range;
  range.file = ttf::fileSource(handle, static_cast<uint32_t>(handle.fileSize()));
  range.length = range.file.size;
  range.cache = pages.get();
  FontBytes bytes(nullptr, 0, &range);
  const bool maps = ttf::mapsCodepoint(bytes, codepoint);
  handle.close();
  return maps || bytes.ioError();
}

bool SdCardFontSystem::coversCjk(const char* family) {
  const SdCardFontFamilyInfo* info = family && family[0] != '\0' ? findFamily(family) : nullptr;
  if (!info || !info->ttf()) return false;
  const uint32_t files = info->filesKey();
  const uint32_t key = glyphstore::fnv1a(&files, sizeof files, glyphstore::fnv1a(family, strlen(family)));
  if (key != cjkFamilyKey_) {
    cjkFamilyKey_ = key;
    cjkFamily_ = familyMapsCodepoint(*info, 0x4E00) || familyMapsCodepoint(*info, 0xAC00);  // 一, 가
  }
  return cjkFamily_;
}

bool SdCardFontSystem::openReadOnly(GfxRenderer& renderer, const char* const* familyNames, const uint8_t count,
                                    const uint8_t pointSize) {
  const SdCardFontFamilyInfo* families[SdCardFontManager::kMaxTtfFamilies] = {};
  uint8_t found = 0;
  for (uint8_t i = 0; i < count && found < SdCardFontManager::kMaxTtfFamilies; ++i) {
    const SdCardFontFamilyInfo* family = findFamily(familyNames[i]);
    if (family && family->ttf()) families[found++] = family;
  }
  if (found == 0 || families[0] != findFamily(familyNames[0])) return false;
  return manager_.openTtfFamiliesReadOnly(families, found, renderer, sizeListFor(familyNames[0]).snap(pointSize));
}

void SdCardFontSystem::selectLanguageFamily(const char* family) {
  const std::string name = family ? family : "";
  const auto region = static_cast<uint8_t>(readerCjkRegion_);
  const uint8_t own = family ? 0 : 1;
  const uint32_t key =
      glyphstore::fnv1a(name.data(), name.size(), glyphstore::fnv1a(&region, 1, glyphstore::fnv1a(&own, 1)));
  if (key == languageFamilyKey_) return;
  languageFamilyKey_ = key;
  languageFamily_.clear();

  // A character every font of the region's script has.
  uint32_t probe = 0;
  const char* suffix = nullptr;
  switch (readerCjkRegion_) {
    case ReaderCjkRegion::Japanese:
      probe = 0x3042;  // あ
      suffix = "JP";
      break;
    case ReaderCjkRegion::SimplifiedChinese:
    case ReaderCjkRegion::ChineseAmbiguous:  // as effectiveReaderFamily(): Simplified for a bare "zh"
      probe = 0x7684;                        // 的
      suffix = "SC";
      break;
    case ReaderCjkRegion::TraditionalChinese:
      probe = 0x7684;
      suffix = "TC";
      break;
    case ReaderCjkRegion::Korean:
      probe = 0xAC00;  // 가
      suffix = "KR";
      break;
    case ReaderCjkRegion::None:
    case ReaderCjkRegion::Pending:
      return;
  }
  if (!family) return;  // the book's own font
  std::string stem;
  if (familyRegion(name, stem) != ReaderCjkRegion::None) return;  // a Noto CJK family: effectiveReaderFamily()'s
  // A built-in reader face has no CJK: fontconvert.py's default intervals leave it out.
  if (const SdCardFontFamilyInfo* info = name.empty() ? nullptr : findFamily(name.c_str())) {
    if (familyMapsCodepoint(*info, probe)) return;
  }

  const bool sansFirst = name.find("Sans") != std::string::npos;
  for (const char* candidateStem : {sansFirst ? "NotoSans" : "NotoSerif", sansFirst ? "NotoSerif" : "NotoSans"}) {
    const std::string candidate = std::string(candidateStem) + suffix;
    if (findFamily(candidate.c_str())) {
      LOG_INF("SDFS", "Book language draws %s in %s: '%s' has no U+%04lX",
              name.empty() ? "a built-in font" : name.c_str(), candidate.c_str(), name.c_str(),
              static_cast<unsigned long>(probe));
      languageFamily_ = candidate;
      return;
    }
  }
}

SdCardFontSystem::ReaderCjkRegion SdCardFontSystem::familyRegion(const std::string& family, std::string& stem) {
  if (family.rfind("NotoSans", 0) == 0) {
    stem = "NotoSans";
  } else if (family.rfind("NotoSerif", 0) == 0) {
    stem = "NotoSerif";
  } else {
    return ReaderCjkRegion::None;
  }

  const std::string suffix = family.substr(stem.size());
  if (suffix == "JP") return ReaderCjkRegion::Japanese;
  if (suffix == "SC") return ReaderCjkRegion::SimplifiedChinese;
  if (suffix == "TC") return ReaderCjkRegion::TraditionalChinese;
  if (suffix == "KR") return ReaderCjkRegion::Korean;
  return ReaderCjkRegion::None;
}

std::string SdCardFontSystem::effectiveReaderFamily(const char* configuredFamily) const {
  const std::string configured = configuredFamily ? configuredFamily : "";
  if (configured.empty() || readerCjkRegion_ == ReaderCjkRegion::None || readerCjkRegion_ == ReaderCjkRegion::Pending) {
    return configured;
  }

  std::string stem;
  const ReaderCjkRegion configuredRegion = familyRegion(configured, stem);
  if (configuredRegion == ReaderCjkRegion::None) return configured;

  ReaderCjkRegion targetRegion = readerCjkRegion_;
  if (targetRegion == ReaderCjkRegion::ChineseAmbiguous) {
    // Bare "zh" has no script/region. Preserve an explicit Chinese choice;
    // otherwise prefer Simplified Chinese, which is the safest default for the corpus.
    targetRegion = (configuredRegion == ReaderCjkRegion::SimplifiedChinese ||
                    configuredRegion == ReaderCjkRegion::TraditionalChinese)
                       ? configuredRegion
                       : ReaderCjkRegion::SimplifiedChinese;
  }

  if (targetRegion == configuredRegion) return configured;

  const char* targetSuffix = nullptr;
  switch (targetRegion) {
    case ReaderCjkRegion::Japanese:
      targetSuffix = "JP";
      break;
    case ReaderCjkRegion::SimplifiedChinese:
      targetSuffix = "SC";
      break;
    case ReaderCjkRegion::TraditionalChinese:
      targetSuffix = "TC";
      break;
    case ReaderCjkRegion::Korean:
      targetSuffix = "KR";
      break;
    case ReaderCjkRegion::None:
    case ReaderCjkRegion::Pending:
    case ReaderCjkRegion::ChineseAmbiguous:
      return configured;
  }

  const std::string candidate = stem + targetSuffix;
  if (!registry_.findFamily(candidate)) {
    LOG_DBG("SDFS", "Regional CJK sibling %s not installed; keeping %s", candidate.c_str(), configured.c_str());
    return configured;
  }

  return candidate;
}

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  ttfRenderer = &renderer;
  ttfSystem = this;
  manager_.setUiFontHook(&CjkUiFonts::setReaderFont);
  GfxRenderer::setBeforePushHook(&returnGlyphLend);
  TtfFont::setHooks({&RenderLock::heldByCurrentTask, &ttfYield, &ttfReclaim, &ttfNowMs, &ttfCountsDraw});
  // A miss's memory on the heap lasts as long as the pass that drew it: whichever task rendered gives it back with
  // RenderLock, and a page drawn from the glyph store needs none of it (TtfFontSet::releaseMissMemory).
  RenderLock::setReleaseHook(&ttfPassEnded);
  registry_.setTtfFits(&ttfFamilyFits);
  registry_.discover();
  discovered_ = true;

  // Startup is discovery-only. SD font payloads are loaded lazily on reader
  // entry via ensureLoaded/ensureLoadedForPath and unloaded on reader exit.
  LOG_DBG("SDFS", "SD font system ready (discovery-only, %d families)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  const std::string wantedFamilyName = effectiveReaderFamily(SETTINGS.sdFontFamilyName);
  const char* wantedFamily = wantedFamilyName.c_str();
  if (wantedFamilyName != SETTINGS.sdFontFamilyName) {
    LOG_DBG("SDFS", "Book language selects regional CJK family: %s -> %s", SETTINGS.sdFontFamilyName, wantedFamily);
  }
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t targetPt = sizeListFor(wantedFamily).snap(SETTINGS.fontPointSize);

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  // Reload if family changed OR if the user-selected size changed and the
  // family has a closer file than what's currently loaded.
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    if (family->ttf() && manager_.ttfFamiliesLoaded(&family, 1)) {
      manager_.setTtfBodySize(renderer, targetPt);  // drawn natively: a new size, never a reload
      return;
    }
    const auto* best = family->pickClosestSize(targetPt);
    const uint8_t bestPt = best ? best->pointSize : 0;
    if (bestPt == manager_.currentPointSize()) {
      // Right face already loaded; only the target size may have moved (a size change from
      // the reader menu lands here), and that is an alias, not a reload.
      manager_.ensureSizeAlias(renderer, targetPt);
      return;
    }
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (target %u)", wantedFamily, manager_.currentPointSize(), bestPt,
            targetPt);
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, targetPt)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
  }
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, const char* const* wantedFamilies, const uint8_t count,
                                    const uint8_t pointSize, const std::function<void()>& onColdLoad,
                                    const FlashCachePolicy policy, BuildArena* const arena) {
  // The families found, in the order asked; one beside the first only when both are TrueType (a .cpfont family has
  // the font area to itself).
  const SdCardFontFamilyInfo* families[SdCardFontManager::kMaxTtfFamilies] = {};
  uint8_t found = 0;
  for (uint8_t i = 0; i < count && found < SdCardFontManager::kMaxTtfFamilies; ++i) {
    const SdCardFontFamilyInfo* family = findFamily(wantedFamilies[i]);
    if (family && (found == 0 || (family->ttf() && families[0]->ttf()))) families[found++] = family;
  }
  if (found == 0) {
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    return;
  }
  const SdCardFontFamilyInfo* family = families[0];
  const uint8_t targetPt = sizeListFor(family->name.c_str()).snap(pointSize);

  if (family->ttf() && manager_.ttfFamiliesLoaded(families, found)) {
    // The same families from the same files (a book's family of the same name as the last book's is another):
    // book open, so no glyph record or face of theirs is in use, the store can make room here (a pending wipe, a
    // reclaim, a rebuild), and the last book's sizes go.
    manager_.dropTtfNativeSizes(renderer);
    manager_.makeTtfRoom(FlashGlyphStore::Budget::Open, onColdLoad);
    manager_.setTtfBodySize(renderer, targetPt);
    return;
  }
  if (!family->ttf() && family->name == manager_.currentFamilyName()) {
    const auto* best = family->pickClosestSize(targetPt);
    if (best && best->pointSize == manager_.currentPointSize()) {
      manager_.ensureSizeAlias(renderer, targetPt);  // same face, possibly a new target size
      return;
    }
  }

  if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);

  // The loader and the resolver used to disagree, and the loader lost. loadFamily() takes the
  // CLOSEST size the family ships, but resolveFontId() only handed the font out on an EXACT
  // point-size match -- so a family without the requested size was loaded into RAM and then never
  // used, and the reader rendered a built-in face instead. It became reachable when the ladder
  // grew past 18 pt, because no existing .cpfont ships anything larger.
  //
  // Both now agree: the closest face is loaded and the requested size is served from it, scaled,
  // under an alias ID (SdCardFontManager::ensureSizeAlias). The chosen typeface AND the chosen
  // size, which is what the setting promised.
  const bool loaded = found > 1 ? manager_.loadTtfFamilies(families, found, renderer, targetPt, onColdLoad)
                                : manager_.loadFamily(*family, renderer, targetPt, onColdLoad, policy, arena);
  if (!loaded) LOG_ERR("SDFS", "Failed to load SD font family: %s", family->name.c_str());
}

int SdCardFontSystem::resolveFontId(const char* familyName, const uint8_t pointSize) const {
  // The manager loads exactly one face for the active SD family and serves ONE target size from
  // it: the face's own, or a scaled alias made by ensureLoaded(). That target is the stored size
  // snapped to the family's list, exactly as ensureLoaded() snapped it. The settings row shows the
  // same snapped size, so a nearby size is never handed out without the reader seeing which.
  // Anything not prepared returns 0, and the caller falls back to a built-in family.
  if (!familyName || familyName[0] == '\0') return 0;
  return manager_.getFontId(familyName, sizeListFor(familyName).snap(pointSize));
}
