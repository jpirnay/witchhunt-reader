// keyboard_layouts: which layouts the language key cycles through, and which one
// a keyboard opens on, for each UI language.

#include <gtest/gtest.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "activities/util/KeyboardLayoutSet.h"

CrossPointSettings CrossPointSettings::instance;

// The real lib/I18n/I18n.cpp cannot link on the host -- it reaches into the flash
// language partition -- and KeyboardLayoutSet only ever asks for the language.
I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}
void I18n::setLanguage(const Language lang) { _language = lang; }
const char* I18n::getLanguageName(const Language lang) const { return LANGUAGE_NAMES[static_cast<size_t>(lang)]; }

namespace {

using freeink::ui::KeyboardLayoutId;

// Restores English and an unconfigured set after each test, so the order they run in cannot matter.
class KeyboardLayoutSetTest : public ::testing::Test {
 protected:
  void TearDown() override {
    I18N.setLanguage(Language::EN);
    SETTINGS.keyboardLayouts = 0;
  }

  static uint16_t bitOf(const KeyboardLayoutId id) {
    for (uint8_t i = 0; i < keyboard_layouts::COUNT; ++i) {
      if (keyboard_layouts::ALL[i].id == id) return keyboard_layouts::bitAt(i);
    }
    return 0;
  }

  static int enabledCount() { return __builtin_popcount(keyboard_layouts::enabled()); }
};

TEST_F(KeyboardLayoutSetTest, EnglishGetsQwertyAloneAndNoLanguageKey) {
  I18N.setLanguage(Language::EN);
  EXPECT_EQ(keyboard_layouts::startingLayout(), KeyboardLayoutId::QwertyEn);
  EXPECT_EQ(enabledCount(), 1);
  // One layout: the key has nowhere to go, so next() stays put.
  EXPECT_EQ(keyboard_layouts::next(KeyboardLayoutId::QwertyEn), KeyboardLayoutId::QwertyEn);
}

TEST_F(KeyboardLayoutSetTest, EachLanguageWithALayoutOpensOnItAndReachesEnglish) {
  const struct {
    Language language;
    KeyboardLayoutId layout;
  } cases[] = {
      {Language::FR, KeyboardLayoutId::AzertyFr},   {Language::DE, KeyboardLayoutId::QwertzDe},
      {Language::ES, KeyboardLayoutId::SpanishEs},  {Language::RU, KeyboardLayoutId::CyrillicRu},
      {Language::UK, KeyboardLayoutId::CyrillicUk}, {Language::BE, KeyboardLayoutId::CyrillicBe},
      {Language::KK, KeyboardLayoutId::CyrillicKk},
  };
  for (const auto& c : cases) {
    I18N.setLanguage(c.language);
    EXPECT_EQ(keyboard_layouts::startingLayout(), c.layout) << static_cast<int>(c.language);
    EXPECT_EQ(enabledCount(), 2) << static_cast<int>(c.language);
    // The language key alternates between the two, starting from either.
    EXPECT_EQ(keyboard_layouts::next(c.layout), KeyboardLayoutId::QwertyEn) << static_cast<int>(c.language);
    EXPECT_EQ(keyboard_layouts::next(KeyboardLayoutId::QwertyEn), c.layout) << static_cast<int>(c.language);
  }
}

TEST_F(KeyboardLayoutSetTest, LanguageWithoutALayoutFallsBackToQwerty) {
  // Polish, Czech, Italian... have no layout of their own: QWERTY alone.
  for (const Language language : {Language::PL, Language::CS, Language::IT, Language::VI}) {
    I18N.setLanguage(language);
    EXPECT_EQ(keyboard_layouts::startingLayout(), KeyboardLayoutId::QwertyEn) << static_cast<int>(language);
    EXPECT_EQ(enabledCount(), 1) << static_cast<int>(language);
  }
}

TEST_F(KeyboardLayoutSetTest, ALayoutTheTableDoesNotListStillLeadsSomewhere) {
  I18N.setLanguage(Language::RU);
  EXPECT_EQ(keyboard_layouts::next(static_cast<KeyboardLayoutId>(200)), KeyboardLayoutId::CyrillicRu);
}

// ---------------------------------------------------------------- the mask a settings file holds

TEST_F(KeyboardLayoutSetTest, EveryLayoutKeepsItsBit) {
  // The bit is what a settings file stores: a mask an older build wrote means the same layouts here, and the
  // scripts with no UI language come after them.
  const KeyboardLayoutId order[] = {
      KeyboardLayoutId::QwertyEn,   KeyboardLayoutId::AzertyFr,   KeyboardLayoutId::QwertzDe,
      KeyboardLayoutId::SpanishEs,  KeyboardLayoutId::CyrillicRu, KeyboardLayoutId::CyrillicUk,
      KeyboardLayoutId::CyrillicBe, KeyboardLayoutId::CyrillicKk, KeyboardLayoutId::HebrewIl,
      KeyboardLayoutId::ArabicAr,   KeyboardLayoutId::GreekEl,
  };
  ASSERT_EQ(keyboard_layouts::COUNT, sizeof(order) / sizeof(order[0]));
  for (uint8_t i = 0; i < keyboard_layouts::COUNT; ++i) EXPECT_EQ(keyboard_layouts::ALL[i].id, order[i]) << int(i);
  EXPECT_EQ(keyboard_layouts::LATIN_BITS, 0x000F);
}

// ---------------------------------------------------------------- scripts with no UI language

TEST_F(KeyboardLayoutSetTest, HebrewArabicAndGreekAreNamedInTheirOwnLanguage) {
  const struct {
    KeyboardLayoutId id;
    const char* name;
  } scripts[] = {
      {KeyboardLayoutId::HebrewIl, "עברית"},
      {KeyboardLayoutId::ArabicAr, "العربية"},
      {KeyboardLayoutId::GreekEl, "Ελληνικά"},
  };
  for (const auto& script : scripts) {
    uint8_t i = 0;
    while (i < keyboard_layouts::COUNT && keyboard_layouts::ALL[i].id != script.id) ++i;
    ASSERT_LT(i, keyboard_layouts::COUNT);
    EXPECT_EQ(keyboard_layouts::ALL[i].language, keyboard_layouts::NO_UI_LANGUAGE);
    EXPECT_STREQ(keyboard_layouts::name(i), script.name);
  }
  // The others go by their UI language's name, in that language; every row has one name or the other.
  EXPECT_STREQ(keyboard_layouts::name(4), LANGUAGE_NAMES[static_cast<size_t>(Language::RU)]);
  for (uint8_t i = 0; i < keyboard_layouts::COUNT; ++i) {
    const auto& info = keyboard_layouts::ALL[i];
    EXPECT_NE(info.language == keyboard_layouts::NO_UI_LANGUAGE, info.nativeName == nullptr) << int(i);
    EXPECT_GT(std::strlen(keyboard_layouts::name(i)), 0u) << int(i);
  }
}

TEST_F(KeyboardLayoutSetTest, AScriptWithNoUiLanguageIsNeverOnUntilSwitchedOn) {
  // Unconfigured, whatever the UI language: none of the three is enabled, and none is opened on.
  for (int l = 0; l < static_cast<int>(Language::_COUNT); ++l) {
    I18N.setLanguage(static_cast<Language>(l));
    for (const KeyboardLayoutId id :
         {KeyboardLayoutId::HebrewIl, KeyboardLayoutId::ArabicAr, KeyboardLayoutId::GreekEl}) {
      EXPECT_FALSE(keyboard_layouts::enabled() & bitOf(id)) << l;
      EXPECT_NE(keyboard_layouts::startingLayout(), id) << l;
    }
  }
}

TEST_F(KeyboardLayoutSetTest, HebrewSwitchedOnIsReachedFromEnglish) {
  I18N.setLanguage(Language::EN);
  SETTINGS.keyboardLayouts = bitOf(KeyboardLayoutId::QwertyEn) | bitOf(KeyboardLayoutId::HebrewIl);
  EXPECT_EQ(enabledCount(), 2);
  EXPECT_EQ(keyboard_layouts::startingLayout(), KeyboardLayoutId::QwertyEn);
  EXPECT_EQ(keyboard_layouts::next(KeyboardLayoutId::QwertyEn), KeyboardLayoutId::HebrewIl);
  EXPECT_EQ(keyboard_layouts::next(KeyboardLayoutId::HebrewIl), KeyboardLayoutId::QwertyEn);

  // Greek and Arabic alone still keep a Latin layout for URLs and passwords, and the cycle runs in table order.
  SETTINGS.keyboardLayouts = bitOf(KeyboardLayoutId::GreekEl) | bitOf(KeyboardLayoutId::ArabicAr);
  EXPECT_TRUE(keyboard_layouts::enabled() & bitOf(KeyboardLayoutId::QwertyEn));
  EXPECT_EQ(keyboard_layouts::startingLayout(), KeyboardLayoutId::QwertyEn);
  EXPECT_EQ(keyboard_layouts::next(KeyboardLayoutId::QwertyEn), KeyboardLayoutId::ArabicAr);
  EXPECT_EQ(keyboard_layouts::next(KeyboardLayoutId::ArabicAr), KeyboardLayoutId::GreekEl);
  EXPECT_EQ(keyboard_layouts::next(KeyboardLayoutId::GreekEl), KeyboardLayoutId::QwertyEn);
}

// ---------------------------------------------------------------- configured on the settings screen

TEST_F(KeyboardLayoutSetTest, EnglishUiWithCyrillicSwitchedOnGetsTheLanguageKey) {
  // The case the default cannot serve: an English interface, Russian books.
  I18N.setLanguage(Language::EN);
  SETTINGS.keyboardLayouts = bitOf(KeyboardLayoutId::QwertyEn) | bitOf(KeyboardLayoutId::CyrillicRu);
  EXPECT_EQ(enabledCount(), 2);
  EXPECT_EQ(keyboard_layouts::startingLayout(), KeyboardLayoutId::QwertyEn);
  EXPECT_EQ(keyboard_layouts::next(KeyboardLayoutId::QwertyEn), KeyboardLayoutId::CyrillicRu);
  EXPECT_EQ(keyboard_layouts::next(KeyboardLayoutId::CyrillicRu), KeyboardLayoutId::QwertyEn);
}

TEST_F(KeyboardLayoutSetTest, AConfiguredSetAlwaysKeepsALatinLayout) {
  // A hand-edited file can switch every Latin layout off; URL and password fields still need one.
  SETTINGS.keyboardLayouts = bitOf(KeyboardLayoutId::CyrillicUk);
  EXPECT_TRUE(keyboard_layouts::enabled() & bitOf(KeyboardLayoutId::QwertyEn));
  EXPECT_TRUE(keyboard_layouts::enabled() & bitOf(KeyboardLayoutId::CyrillicUk));
  // A Latin layout other than English satisfies it without adding English.
  SETTINGS.keyboardLayouts = bitOf(KeyboardLayoutId::QwertzDe) | bitOf(KeyboardLayoutId::CyrillicRu);
  EXPECT_FALSE(keyboard_layouts::enabled() & bitOf(KeyboardLayoutId::QwertyEn));
}

TEST_F(KeyboardLayoutSetTest, TheUiLanguagesLayoutSwitchedOffIsNotOpenedOn) {
  I18N.setLanguage(Language::RU);
  SETTINGS.keyboardLayouts = bitOf(KeyboardLayoutId::QwertyEn) | bitOf(KeyboardLayoutId::CyrillicUk);
  EXPECT_NE(keyboard_layouts::startingLayout(), KeyboardLayoutId::CyrillicRu);
  EXPECT_TRUE(keyboard_layouts::enabled() & bitOf(keyboard_layouts::startingLayout()));
}

TEST_F(KeyboardLayoutSetTest, BitsNamingNoLayoutAreIgnored) {
  // From a file written by a build with more layouts: only unknown bits reads as unconfigured.
  I18N.setLanguage(Language::FR);
  SETTINGS.keyboardLayouts = 0x8000;
  EXPECT_EQ(keyboard_layouts::startingLayout(), KeyboardLayoutId::AzertyFr);
  EXPECT_EQ(enabledCount(), 2);
}

}  // namespace
