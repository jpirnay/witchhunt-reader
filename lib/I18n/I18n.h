#pragma once

#include <cstddef>
#include <cstdint>

#include "I18nKeys.h"

namespace FlashLangPartition {
class SlotWriter;
struct Strings;
}  // namespace FlashLangPartition

/// Bytes a language's flash pack carries after its strings, built, erased and mapped with them
/// (FlashLangPartition). CjkUiFonts keeps the active CJK language's UI faces there. One may be
/// registered; without it a pack holds its strings only.
///
/// I18n calls it from refreshActiveStrings(), at boot and on every language change, and only there.
class LanguagePackExtension {
 public:
  /// Bytes the pack of `lang` carries after its strings; 0 for none.
  virtual uint32_t bytes(Language lang) = 0;
  /// Identifies those bytes. Mixed into the pack's stamp, so a firmware whose bytes differ rebuilds
  /// the slot even when the strings did not change.
  virtual uint32_t stamp(Language lang) = 0;
  /// Writes exactly bytes(lang) bytes. False abandons the extension; the pack keeps its strings.
  virtual bool produce(Language lang, FlashLangPartition::SlotWriter& out) = 0;
  /// The extension of `lang` is readable at `data`, `len` bytes, until the next call. nullptr
  /// (len 0): there is none, from now on -- always sent before anything that could unmap it.
  virtual void attach(Language lang, const uint8_t* data, uint32_t len) = 0;

 protected:
  ~LanguagePackExtension() = default;
};

/**
 * Internationalization (i18n) system for CrossPoint Reader
 */

class I18n {
 public:
  static I18n& getInstance();

  // Disable copy
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Get localized string by ID
  const char* get(StrId id) const;

  const char* operator[](StrId id) const { return get(id); }

  Language getLanguage() const { return _language; }
  void setLanguage(Language lang);
  const char* getLanguageCode(Language lang) const;
  const char* getLanguageName(Language lang) const;

  /// Where the language packs live on the card (LangPack.h); the Font Manager installs them there.
  static constexpr const char* PACK_DIR = "/.crosspoint/lang";
  /// The pack file of `lang`: "/.crosspoint/lang/de.cplang". False for English, which has none.
  static bool packPath(Language lang, char* out, size_t outSize);
  /// Whether the card holds the pack of `lang`; English always counts as there.
  static bool packOnCard(Language lang);

  void saveSettings();
  void loadSettings();

  /// Register what the language packs carry after their strings. Before loadSettings(), so the
  /// first pack resolved already includes it.
  void setPackExtension(LanguagePackExtension* ext) { _ext = ext; }

  /// Where the strings of the chosen language come from, as the last refresh left it: what System
  /// Information shows, so a report of an English interface says why.
  enum class PackState : uint8_t {
    English,     // English is chosen: the image's strings
    Active,      // the language's pack, from the flash slot
    NoPack,      // no pack on the card: English
    BadPack,     // a pack that does not read as one: English
    SlotFailed,  // the slot could not be had (FlashLangPartition::lastFailure() says why): English
    Card,        // the language's pack, read from the card a string at a time: the flash area has no room for the slot
  };
  PackState packState() const { return _packState; }

  // Get all unique characters used in a specific language
  // Returns a sorted string of unique characters
  static const char* getCharacterSet(Language lang);

 private:
  I18n() : _language(Language::EN), _active(englishStrings()) {}

  // The language read from the card (langcard::CardStrings and its file), while the flash area has no slot for it.
  struct CardMode;

  // Read the stored language code. Does not resolve the strings for it.
  void loadLanguageSetting();

  // Point _active at the strings for _language: English straight from rodata,
  // anything else from the flash language slot — built first from the language's
  // pack on the card when the slot does not already hold that pack for this
  // firmware's keys. When the flash area has no room for the slot, from the card a
  // string at a time instead (card mode). Falls back to English on any failure, a pack
  // missing from the card included, so the reader degrades to readable English rather
  // than to blank rows. Callers hold RenderLock (UiLanguage::apply), as for the slot:
  // the strings handed out before go.
  void refreshActiveStrings();

  // After an update from a firmware that kept its languages in the image (2.40, 2.41RC): when the
  // flash slot that firmware left holds _language in the layout of a release the generator knows
  // (LEGACY_LAYOUTS), writes its strings to the card as the pack `path` (LangPackConvert.h). False when
  // there is no such slot, it holds another language or a CJK language's faces, or the card refused
  // the file.
  bool convertLegacySlot(const char* code, const char* path);

  // Card mode for the pack of `langIndex`: its strings unpacked to the card (when the file there holds another pack)
  // and read from it as they are asked for. False, leaving _card empty, when the card or the heap refuses.
  bool openCardStrings(uint8_t langIndex, uint32_t stamp, const FlashLangPartition::Strings& strings, const char* code);

  Language _language;

  // The resolved (data, offsets) pair for _language.
  //
  // Cached rather than resolved per lookup: for a pack language that means
  // reading a flash header and comparing a stamp, and tr() is called several
  // times for every row drawn. For English it points at rodata exactly as
  // before. Either way get() is two loads and an add.
  LangStrings _active;

  LanguagePackExtension* _ext = nullptr;
  PackState _packState = PackState::English;
  // Set only in card mode (PackState::Card), owned, and deleted by refreshActiveStrings(): get() asks it first, and
  // reads English where it has no string yet. A plain pointer, so the constructor stays inline for the host tests that
  // stand in for I18n.cpp.
  CardMode* _card = nullptr;
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
