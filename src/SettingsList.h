#pragma once

#include <BoardConfig.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "SdCardFontGlobals.h"
#include "activities/settings/SettingInfo.h"

// Shared settings list used by both the device settings UI and the web settings API.
//
// Fields that drive UI behaviour:
//   category    — which tab the setting appears under (STR_CAT_DISPLAY, STR_CAT_READER, …).
//                 Entries with STR_NONE_OPT or web-only categories are skipped by the device UI.
//   subcategory — optional section heading within a tab.  Items remain in their defined order;
//                 no reordering or grouping occurs.  When an item's subcategory differs from the
//                 previous item's, SettingsActivity::onEnter() automatically inserts a separator
//                 row before it.  Add with .withSubcategory(StrId::STR_MY_SECTION).
//                 Items without a subcategory (STR_NONE_OPT) never trigger a separator.
//   submenu     — optional submenu grouping.  Items with the same submenu StrId are hidden from
//                 the main list and collected behind a single placeholder entry.  Selecting that
//                 entry launches SettingsSubmenuActivity with those items.  withSubcategory()
//                 works inside a submenu exactly as it does in the parent tab.
//                 Add with .withSubmenu(StrId::STR_MY_SUBMENU).
//   key         — JSON property name used by the web settings API (nullptr = device-only).
//   requiredCapability — hardware visibility. Default is "always visible";
//     override with .requiring(SettingRequires::X) to hide the setting on boards
//     that physically cannot do the thing it controls. Ask for a CAPABILITY,
//     never a board name — see SettingInfo.h.
//
// ACTION-type entries and entries without a key are device-only and are added directly
// in SettingsActivity::onEnter(), not here.
//
// Implementation note: the list is a namespace-level static (not a function-local static)
// so it is initialised during the global-static phase before setup() runs. A
// function-local static would trigger __cxa_guard_acquire on first call, which creates a
// FreeRTOS mutex deep inside the heap allocator chain — enough stack to overflow the 8 KB
// loop task stack when called from inside SETTINGS.loadFromFile() at boot time.
// Therefore the list is built at runtime in a local function and returned by value.
namespace SettingsListDetail {
inline uint8_t getKoReaderMatchMethod(const void*) { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); }

inline std::string getKoReaderServerUrl(void*) { return KOREADER_STORE.getServerUrl(); }

inline std::string getKoReaderUsername(void*) { return KOREADER_STORE.getUsername(); }

inline std::string getKoReaderPassword(void*) { return KOREADER_STORE.getPassword(); }

inline std::string getSleepTimeoutDisplay(void*) {
  const uint8_t v = SETTINGS.sleepTimeoutMinutes;
  if (v == 0) return std::string(tr(STR_NEVER));
  return std::to_string(v) + tr(STR_MIN_SUFFIX);
}

inline std::string getDictionaryDisplay(void*) {
  if (SETTINGS.dictionaryName[0] == '\0') return std::string(tr(STR_NONE_OPT));
  return SETTINGS.dictionaryName;
}

inline std::string getRefreshFrequencyDisplay(void*) {
  const uint8_t v = SETTINGS.refreshFrequencyPages;
  if (v == 0) return std::string(tr(STR_NEVER));
  return std::to_string(v) + tr(STR_PAGES_SUFFIX);
}

inline std::string getKoSyncMinPagesDisplay(void*) {
  const uint8_t v = SETTINGS.koSyncMinSessionPages;
  // Zero is "Always", not "Never": it means no minimum, so every close pushes. Turning the
  // feature off is the separate Auto-Push toggle.
  if (v == 0) return std::string(tr(STR_ALWAYS));
  return std::to_string(v) + tr(STR_PAGES_SUFFIX);
}

inline std::string getFrontlightBrightnessDisplay(void*) { return std::to_string(SETTINGS.frontlightBrightness) + "%"; }

inline std::string getFrontlightWarmthDisplay(void*) { return std::to_string(SETTINGS.frontlightWarmth) + "%"; }

inline std::vector<SettingInfo> buildSettingsList() {
  // Shared button action options used by all button enum entries.
  const std::vector<StrId> btnActionOptions = {StrId::STR_BTN_ACT_PAGE_FORWARD,
                                               StrId::STR_BTN_ACT_PAGE_BACK,
                                               StrId::STR_BTN_ACT_PAGE_FORWARD_10,
                                               StrId::STR_BTN_ACT_PAGE_BACK_10,
                                               StrId::STR_BTN_ACT_GO_HOME,
                                               StrId::STR_BTN_ACT_SLEEP,
                                               StrId::STR_BTN_ACT_FORCE_REFRESH,
                                               StrId::STR_BTN_ACT_FORCE_FAST_REFRESH,
                                               StrId::STR_BTN_ACT_OPEN_TOC,
                                               StrId::STR_BTN_ACT_OPEN_BOOKMARKS,
                                               StrId::STR_BTN_ACT_STAR_PAGE,
                                               StrId::STR_BTN_ACT_FOOTNOTES,
                                               StrId::STR_BTN_ACT_NEXT_SECTION,
                                               StrId::STR_BTN_ACT_PREV_SECTION,
                                               StrId::STR_BTN_ACT_EXIT_READER,
                                               StrId::STR_BTN_ACT_READER_MENU,
                                               StrId::STR_BTN_ACT_TOGGLE_BIONIC_READING,
                                               StrId::STR_BTN_ACT_KOREADER_SYNC,
                                               StrId::STR_BTN_ACT_CYCLE_FONT_SIZE,
                                               StrId::STR_BTN_ACT_CYCLE_ORIENTATION,
                                               StrId::STR_BTN_ACT_QUICK_OVERRIDES,
                                               StrId::STR_BTN_ACT_IGNORE,
                                               StrId::STR_DICTIONARY};

  // The frontlight actions sit at the END of BUTTON_ACTION, so dropping them on
  // a board with no light leaves every other option's numeric value untouched —
  // a settings file written on a lit board still reads correctly here.
  const std::vector<StrId> lightActionOptions = {StrId::STR_BTN_ACT_LIGHT_TOGGLE, StrId::STR_BTN_ACT_LIGHT_BRIGHTER,
                                                 StrId::STR_BTN_ACT_LIGHT_DIMMER};

  // Prepend the per-button default action to the shared options list.
  const bool hasLight = Frontlight.present();
  auto makeBtnActionOptions = [&](StrId defaultAction) {
    std::vector<StrId> result;
    result.reserve(1 + btnActionOptions.size() + (hasLight ? lightActionOptions.size() : 0));
    result.push_back(defaultAction);
    result.insert(result.end(), btnActionOptions.begin(), btnActionOptions.end());
    if (hasLight) {
      result.insert(result.end(), lightActionOptions.begin(), lightActionOptions.end());
    }
    return result;
  };

  std::vector<SettingInfo> settings;
  settings.reserve(77);

  // --- Display ---
  settings.push_back(SettingInfo::Action(StrId::STR_TIME_TO_SLEEP, SettingAction::SleepTimeoutPicker)
                         .withDisplayGetter(getSleepTimeoutDisplay)
                         .withCategory(StrId::STR_CAT_DISPLAY));
  settings.push_back(
      SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                        {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER, StrId::STR_NONE_OPT,
                         StrId::STR_COVER_CUSTOM, StrId::STR_PAGE_OVERLAY, StrId::STR_QUICK_RESUME},
                        "sleepScreen", StrId::STR_CAT_DISPLAY)
          .withSubcategory(StrId::STR_MENU_DISP_SLEEP)
          .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                                       {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode",
                                       StrId::STR_CAT_DISPLAY));
  settings.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                                       {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED,
                                        StrId::STR_FILTER_ADAPTIVE, StrId::STR_FILTER_EQUALIZE},
                                       "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY));
  settings.push_back(SettingInfo::Enum(StrId::STR_SLEEP_COVER_OVERLAY, &CrossPointSettings::sleepCoverOverlay,
                                       {StrId::STR_OVERLAY_OFF, StrId::STR_OVERLAY_WHITE, StrId::STR_OVERLAY_GRAY,
                                        StrId::STR_OVERLAY_BLACK},
                                       "sleepCoverOverlay", StrId::STR_CAT_DISPLAY)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_SLEEP_IMAGE_PICK_MODE, &CrossPointSettings::sleepImagePickMode,
                                       {StrId::STR_RANDOM, StrId::STR_SEQUENTIAL}, "sleepImagePickMode",
                                       StrId::STR_CAT_DISPLAY));
  settings.push_back(SettingInfo::Enum(StrId::STR_QUICK_RESUME_TIMEOUT, &CrossPointSettings::quickResumeSleepScreen,
                                       {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, "quickResumeSleepScreen",
                                       StrId::STR_CAT_DISPLAY));
  settings.push_back(SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                                       {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS},
                                       "hideBatteryPercentage", StrId::STR_CAT_DISPLAY)
                         .withSubcategory(StrId::STR_MENU_DISP_BATTERY));
  settings.push_back(SettingInfo::Action(StrId::STR_REFRESH_FREQ, SettingAction::RefreshFrequencyPicker)
                         .withDisplayGetter(getRefreshFrequencyDisplay)
                         .withCategory(StrId::STR_CAT_DISPLAY)
                         .withSubmenu(StrId::STR_MENU_DISP_REFRESH)
                         .withSubcategory(StrId::STR_MENU_DISP_REFRESH));
  settings.push_back(SettingInfo::Toggle(StrId::STR_REFRESH_AFTER_IMAGE_PAGES,
                                         &CrossPointSettings::halfRefreshAfterImagePage, "halfRefreshAfterImagePage",
                                         StrId::STR_CAT_DISPLAY)
                         .withSubmenu(StrId::STR_MENU_DISP_REFRESH)
                         .withSubcategory(StrId::STR_MENU_DISP_REFRESH));
  settings.push_back(SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                                         StrId::STR_CAT_DISPLAY));

  // --- Reading light (boards with a PWM frontlight/backlight) ---
  // Grouped in a submenu so the Display tab stays one screen on boards that
  // have one, and disappears entirely on boards that do not.
  settings.push_back(SettingInfo::DynamicToggle(
                         StrId::STR_READING_LIGHT, [](const void*) -> uint8_t { return Frontlight.isOn() ? 1 : 0; },
                         [](void*, const uint8_t v) {
                           // The hardware, not SETTINGS, is the authority for
                           // "is the light on" (a wake with Restore off leaves
                           // them legitimately apart), so read it back rather
                           // than mirroring the requested value.
                           Frontlight.setOn(v != 0);
                           SETTINGS.frontlightOn = Frontlight.isOn() ? 1 : 0;
                         },
                         "frontlightOn", StrId::STR_CAT_DISPLAY)
                         .withSubmenu(StrId::STR_MENU_DISP_LIGHT)
                         .withSubcategory(StrId::STR_MENU_DISP_LIGHT)
                         .requiring(SettingRequires::ReadingLight));
  settings.push_back(SettingInfo::Action(StrId::STR_LIGHT_BRIGHTNESS, SettingAction::FrontlightBrightnessPicker)
                         .withDisplayGetter(getFrontlightBrightnessDisplay)
                         .withCategory(StrId::STR_CAT_DISPLAY)
                         .withSubmenu(StrId::STR_MENU_DISP_LIGHT)
                         .requiring(SettingRequires::ReadingLight));
  settings.push_back(SettingInfo::Action(StrId::STR_LIGHT_WARMTH, SettingAction::FrontlightWarmthPicker)
                         .withDisplayGetter(getFrontlightWarmthDisplay)
                         .withCategory(StrId::STR_CAT_DISPLAY)
                         .withSubmenu(StrId::STR_MENU_DISP_LIGHT)
                         .requiring(SettingRequires::WarmLight));
  settings.push_back(SettingInfo::Toggle(StrId::STR_RESTORE_LIGHT_ON_WAKE, &CrossPointSettings::frontlightRestoreOnWake,
                                         "frontlightRestoreOnWake", StrId::STR_CAT_DISPLAY)
                         .withSubmenu(StrId::STR_MENU_DISP_LIGHT)
                         .requiring(SettingRequires::ReadingLight));
  settings.push_back(SettingInfo::Enum(StrId::STR_UI_THEME, &CrossPointSettings::uiTheme,
                                       {StrId::STR_THEME_CLASSIC, StrId::STR_THEME_LYRA, StrId::STR_THEME_LYRA_EXTENDED,
                                        StrId::STR_THEME_LYRA_CAROUSEL},
                                       "uiTheme", StrId::STR_CAT_DISPLAY)
                         .withSelectorActivity());

  // --- Reader ---
  // General reader settings
  settings.push_back(
      SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                        {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                        "orientation", StrId::STR_CAT_READER)
          .withSelectorActivity());
  // EPUB font submenu — family first, then size/AA/darkness.
  // DynamicEnum so SD card font families can be appended at the consumer
  // side (SettingsActivity / CrossPointWebServer enrich enumLabels before
  // iterating). The built-in StrIds are kept as a fallback for code paths that
  // don't enrich enumLabels.
  settings.push_back(SettingInfo::DynamicEnum(StrId::STR_FONT_FAMILY, {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS},
                                              fontFamilyDynamicGetter, fontFamilyDynamicSetter, "fontFamily",
                                              StrId::STR_CAT_READER)
                         .withSubcategory(StrId::STR_MENU_READER_FONT)
                         .withSubmenu(StrId::STR_MENU_READER_FONT)
                         .withSelectorActivity());
  settings.push_back(
      SettingInfo::Enum(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
                        {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE, StrId::STR_TINY},
                        "fontSize", StrId::STR_CAT_READER)
          .withSubmenu(StrId::STR_MENU_READER_FONT)
          .withSelectorActivity());
  settings.push_back(SettingInfo::Toggle(StrId::STR_TEXT_AA, &CrossPointSettings::textAntiAliasing, "textAntiAliasing",
                                         StrId::STR_CAT_READER)
                         .withSubmenu(StrId::STR_MENU_READER_FONT));
  // X3-only fast AA LUT toggle. Swaps the 53-frame OEM grayscale waveform
  // (~2.4 s panel time, X4-accurate grays) for the 7-frame community LUT
  // (~130 ms, mid-tones slightly darker). See freeink-sdk
  // FreeInkDisplay::setFastGrayscaleLut for trade-offs.
  settings.push_back(SettingInfo::Toggle(StrId::STR_FAST_AA, &CrossPointSettings::fastAntiAliasing,
                                         "fastAntiAliasingV2", StrId::STR_CAT_READER)
                         .withSubmenu(StrId::STR_MENU_READER_FONT)
                         .requiring(SettingRequires::SelectableGrayscaleLut));
  settings.push_back(SettingInfo::Enum(StrId::STR_TEXT_DARKNESS, &CrossPointSettings::textDarkness,
                                       {StrId::STR_NORMAL, StrId::STR_DARK, StrId::STR_EXTRA_DARK, StrId::STR_MAX_DARK},
                                       "textDarkness", StrId::STR_CAT_READER)
                         .withSubmenu(StrId::STR_MENU_READER_FONT)
                         .withSelectorActivity());
  // TXT/MD font submenu — same dynamic structure as EPUB, includes SD card fonts.
  settings.push_back(SettingInfo::DynamicEnum(StrId::STR_TXT_FONT_FAMILY, {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS},
                                              txtFontFamilyDynamicGetter, txtFontFamilyDynamicSetter, "txtFontFamily",
                                              StrId::STR_CAT_READER)
                         .withSubmenu(StrId::STR_MENU_TXT_FONT)
                         .withSelectorActivity());
  settings.push_back(
      SettingInfo::Enum(StrId::STR_TXT_FONT_SIZE, &CrossPointSettings::txtFontSize,
                        {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE, StrId::STR_TINY},
                        "txtFontSize", StrId::STR_CAT_READER)
          .withSubmenu(StrId::STR_MENU_TXT_FONT)
          .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                                       {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER,
                                        StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE},
                                       "paragraphAlignment", StrId::STR_CAT_READER)
                         .withSubcategory(StrId::STR_MENU_READER_LAYOUT)
                         .withSelectorActivity());
  // Formatting settings
  settings.push_back(SettingInfo::Toggle(StrId::STR_EMBEDDED_STYLE, &CrossPointSettings::embeddedStyle, "embeddedStyle",
                                         StrId::STR_CAT_READER));
  settings.push_back(SettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled,
                                         "hyphenationEnabled", StrId::STR_CAT_READER));
  settings.push_back(SettingInfo::Toggle(StrId::STR_FONT_SIZE_NORMALIZATION, &CrossPointSettings::fontSizeNormalization,
                                         "fontSizeNormalization", StrId::STR_CAT_READER));
  // Which StarDict dictionary word lookup uses. An Action rather than an Enum:
  // the options are folders discovered on the SD card at open time, not a fixed
  // list, so the picker has to scan.
  settings.push_back(SettingInfo::Action(StrId::STR_DICTIONARY, SettingAction::DictionarySelect)
                         .withDisplayGetter(getDictionaryDisplay)
                         .withCategory(StrId::STR_CAT_READER));
  settings.push_back(
      SettingInfo::Enum(StrId::STR_IMAGES, &CrossPointSettings::imageRendering,
                        {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
                        "imageRendering", StrId::STR_CAT_READER)
          .withSubmenu(StrId::STR_IMAGES));
  settings.push_back(SettingInfo::Toggle(StrId::STR_LARGE_IMAGE_PLACEHOLDER, &CrossPointSettings::largeImagePlaceholder,
                                         "largeImagePlaceholder", StrId::STR_CAT_READER)
                         .withSubmenu(StrId::STR_IMAGES));
  settings.push_back(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMargin, {5, 40, 5},
                                        "screenMargin", StrId::STR_CAT_READER)
                         .withSubmenu(StrId::STR_MENU_READER_SPACING));
  settings.push_back(SettingInfo::Enum(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacing,
                                       {StrId::STR_TIGHT, StrId::STR_NORMAL, StrId::STR_WIDE}, "lineSpacing",
                                       StrId::STR_CAT_READER)
                         .withSubmenu(StrId::STR_MENU_READER_SPACING));
  settings.push_back(SettingInfo::Toggle(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacing,
                                         "extraParagraphSpacing", StrId::STR_CAT_READER)
                         .withSubmenu(StrId::STR_MENU_READER_SPACING));

  // Generic reader settings
  settings.push_back(SettingInfo::Toggle(StrId::STR_CREATE_FALLBACK_FOR_INVALID_TOC,
                                         &CrossPointSettings::syntheticTocFallback, "syntheticTocFallback",
                                         StrId::STR_CAT_READER)
                         .withSubcategory(StrId::STR_MENU_READER_TWEAKS));
  settings.push_back(SettingInfo::Toggle(StrId::STR_BIONIC_READING, &CrossPointSettings::bionicReading, "bionicReading",
                                         StrId::STR_CAT_READER)
                         .withSubmenu(StrId::STR_READING_AIDS));
  settings.push_back(
      SettingInfo::Toggle(StrId::STR_GUIDE_DOTS, &CrossPointSettings::guideDots, "guideDots", StrId::STR_CAT_READER)
          .withSubmenu(StrId::STR_READING_AIDS));
  settings.push_back(SettingInfo::Toggle(StrId::STR_INLINE_FOOTNOTE_PREVIEWS,
                                         &CrossPointSettings::inlineFootnotePreviews, "inlineFootnotePreviews",
                                         StrId::STR_CAT_READER));

  // --- Controls ---
  // --- Button Actions (short / double / long press per logical button) ---
  // All entries share the same ordered action-label list; the submenu groups them
  // behind a single placeholder row in the device UI.
  // Shared action options (everything except the first "default" entry).
  // Back button: short=exit reader, double=ignore, long=go home
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortBack,
                                       {StrId::STR_BTN_DEF_EXIT_READER}, "btnShortBack", StrId::STR_CAT_CONTROLS)
                         .withSubcategory(StrId::STR_MENU_BTN_ACTIONS)
                         .withSubmenu(StrId::STR_BTN_BACK));
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoubleBack,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoubleBack",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_BACK)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongBack,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_GO_HOME), "btnLongBack",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_BACK)
                         .withSelectorActivity());
  // Confirm button: short=reader menu, double=ignore, long=KOReader sync
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortConfirm,
                                       {StrId::STR_BTN_DEF_READER_MENU}, "btnShortConfirm", StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_CONFIRM));
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoubleConfirm,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoubleConfirm",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_CONFIRM)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongConfirm,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_KOREADER_SYNC), "btnLongConfirm",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_CONFIRM)
                         .withSelectorActivity());
  // Left button: short=previous page, double=ignore, long=chapter back
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortLeft,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_PREV_PAGE), "btnShortLeft",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_LEFT)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoubleLeft,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoubleLeft",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_LEFT)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongLeft,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_CHAPTER_BACK), "btnLongLeft",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_LEFT)
                         .withSelectorActivity());
  // Right button: short=next page, double=ignore, long=chapter forward
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortRight,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_NEXT_PAGE), "btnShortRight",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_RIGHT)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoubleRight,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoubleRight",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_RIGHT)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongRight,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_CHAPTER_FORWARD), "btnLongRight",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_RIGHT)
                         .withSelectorActivity());
  // Page Back button: short=previous page, double=ignore, long=chapter back
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortPageBack,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_PREV_PAGE), "btnShortPageBack",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_UP)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoublePageBack,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoublePageBack",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_UP)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongPageBack,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_CHAPTER_BACK), "btnLongPageBack",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_UP)
                         .withSelectorActivity());
  // Page Forward button: short=next page, double=ignore, long=chapter forward
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortPageForward,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_NEXT_PAGE), "btnShortPageForward",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_DOWN)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoublePageForward,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoublePageForward",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_DOWN)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongPageForward,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_CHAPTER_FORWARD), "btnLongPageForward",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_DOWN)
                         .withSelectorActivity());
  // Power button: short=ignore, double=ignore, long=sleep (via hold timer, not event system)
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_SHORT_PRESS, &CrossPointSettings::btnShortPower,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnShortPower",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_POWER)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_DOUBLE_PRESS, &CrossPointSettings::btnDoublePower,
                                       makeBtnActionOptions(StrId::STR_BTN_DEF_IGNORE), "btnDoublePower",
                                       StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_POWER)
                         .withSelectorActivity());
  settings.push_back(SettingInfo::Enum(StrId::STR_BTN_LONG_PRESS, &CrossPointSettings::btnLongPower,
                                       {StrId::STR_BTN_DEF_SLEEP}, "btnLongPower", StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_BTN_POWER));
  // Touch reading controls (touch boards only — gated on the capability, not on
  // a board name, so X4 Pro and T5S3 both get them).
  settings.push_back(SettingInfo::Enum(StrId::STR_TOUCH_READER_CONTROLS, &CrossPointSettings::touchReaderControls,
                                       {StrId::STR_TOUCH_READER_OFF, StrId::STR_TOUCH_READER_TAP,
                                        StrId::STR_TOUCH_READER_SWIPE, StrId::STR_TOUCH_READER_INVERTED},
                                       "touchReaderCtl", StrId::STR_CAT_CONTROLS)
                         .requiring(SettingRequires::TouchPanel));
  settings.push_back(SettingInfo::Toggle(StrId::STR_TAP_FOR_READER_MENU, &CrossPointSettings::tapForReaderMenu,
                                         "tapReaderMenu", StrId::STR_CAT_CONTROLS)
                         .requiring(SettingRequires::TouchPanel));
  // Tilt page turn (X3-only)
  settings.push_back(SettingInfo::Toggle(StrId::STR_TILT_PAGE_TURN, &CrossPointSettings::tiltPageTurn, "tiltPageTurn",
                                         StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_TILT_PAGE_TURN)
                         .requiring(SettingRequires::TiltSensor));
  settings.push_back(SettingInfo::Enum(StrId::STR_DIR_RIGHT, &CrossPointSettings::tiltPositiveAction,
                                       {StrId::STR_NONE_OPT, StrId::STR_NEXT_PAGE, StrId::STR_PREV_PAGE},
                                       "tiltPositiveAction", StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_TILT_PAGE_TURN)
                         .requiring(SettingRequires::TiltSensor));
  settings.push_back(SettingInfo::Enum(StrId::STR_DIR_LEFT, &CrossPointSettings::tiltNegativeAction,
                                       {StrId::STR_NONE_OPT, StrId::STR_NEXT_PAGE, StrId::STR_PREV_PAGE},
                                       "tiltNegativeAction", StrId::STR_CAT_CONTROLS)
                         .withSubmenu(StrId::STR_TILT_PAGE_TURN)
                         .requiring(SettingRequires::TiltSensor));
  // --- System ---
  settings.push_back(SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles,
                                         "showHiddenFiles", StrId::STR_CAT_SYSTEM)
                         .withSubmenu(StrId::STR_SHOW_FILES));
  settings.push_back(SettingInfo::Toggle(StrId::STR_SHOW_FILE_EXTENSIONS, &CrossPointSettings::showFileExtensions,
                                         "showFileExtensions", StrId::STR_CAT_SYSTEM)
                         .withSubmenu(StrId::STR_SHOW_FILES));
  settings.push_back(SettingInfo::Toggle(StrId::STR_INCLUDE_BETA_UPDATES, &CrossPointSettings::includeBetaUpdates,
                                         "includeRcUpdates", StrId::STR_CAT_SYSTEM));
  settings.push_back(SettingInfo::Toggle(StrId::STR_SKIP_HTTPS_VALIDATION, &CrossPointSettings::skipHttpsValidation,
                                         "skipHttpsValidation", StrId::STR_CAT_SYSTEM));
  // OPDS download settings are edited from the OPDS server list. Keep them
  // category-less so they persist and remain available to the web API without
  // cluttering the device settings screen.
  settings.push_back(SettingInfo::String(StrId::STR_OPDS_DOWNLOAD_FOLDER, SETTINGS.opdsDownloadFolder,
                                         sizeof(SETTINGS.opdsDownloadFolder), "opdsDownloadFolder"));
  settings.push_back(SettingInfo::Enum(StrId::STR_OPDS_FILENAME_FORMAT, &CrossPointSettings::opdsFilenameFormat,
                                       {StrId::STR_FMT_AUTHOR_TITLE, StrId::STR_FMT_TITLE_AUTHOR, StrId::STR_FMT_TITLE},
                                       "opdsFilenameFormat"));
  // Will be dealt with separately, so these receive none of the main categories and
  // are visible in the web UI but not the device UI.
  settings.push_back(
      SettingInfo::Toggle(StrId::STR_USE_CLOCK, &CrossPointSettings::useClock, "useClock", StrId::STR_CLOCK));
  settings.push_back(SettingInfo::Enum(StrId::STR_CLOCK_FORMAT, &CrossPointSettings::clockFormat12h,
                                       {StrId::STR_24H, StrId::STR_12H}, "clockFormat12h", StrId::STR_CLOCK));
  settings.push_back(
      SettingInfo::Enum(StrId::STR_TIMEZONE, &CrossPointSettings::timeZone,
                        {StrId::STR_TZ_UTC, StrId::STR_TZ_CET, StrId::STR_TZ_EET, StrId::STR_TZ_MSK,
                         StrId::STR_TZ_UTC_PLUS4, StrId::STR_TZ_IST, StrId::STR_TZ_UTC_PLUS7, StrId::STR_TZ_UTC_PLUS8,
                         StrId::STR_TZ_UTC_PLUS9, StrId::STR_TZ_AEST, StrId::STR_TZ_NZST, StrId::STR_TZ_UTC_MINUS3,
                         StrId::STR_TZ_EST, StrId::STR_TZ_CST, StrId::STR_TZ_MST, StrId::STR_TZ_PST},
                        "timeZone", StrId::STR_CLOCK)
          .withSelectorActivity());
  settings.push_back(SettingInfo::String(StrId::STR_NTP_SERVER, SETTINGS.ntpServer, sizeof(SETTINGS.ntpServer),
                                         "ntpServer", StrId::STR_CLOCK));
  // Weather
  settings.push_back(
      SettingInfo::Toggle(StrId::STR_USE_WEATHER, &CrossPointSettings::useWeather, "useWeather", StrId::STR_WEATHER));

  // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
  settings.push_back(SettingInfo::DynamicString(
      StrId::STR_SYNC_SERVER_URL, static_cast<SettingInfo::StringGetterFn>(getKoReaderServerUrl),
      [](void*, const std::string& v) {
        KOREADER_STORE.setServerUrl(v);
        KOREADER_STORE.saveToFile();
      },
      "koServerUrl", StrId::STR_KOREADER_SYNC));
  settings.push_back(SettingInfo::DynamicString(
      StrId::STR_KOREADER_USERNAME, static_cast<SettingInfo::StringGetterFn>(getKoReaderUsername),
      [](void*, const std::string& v) {
        KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
        KOREADER_STORE.saveToFile();
      },
      "koUsername", StrId::STR_KOREADER_SYNC));
  settings.push_back(SettingInfo::DynamicString(
                         StrId::STR_KOREADER_PASSWORD, static_cast<SettingInfo::StringGetterFn>(getKoReaderPassword),
                         [](void*, const std::string& v) {
                           KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
                           KOREADER_STORE.saveToFile();
                         },
                         "koPassword", StrId::STR_KOREADER_SYNC)
                         .withObfuscated());
  settings.push_back(SettingInfo::DynamicEnum(
      StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY}, getKoReaderMatchMethod,
      [](void*, uint8_t v) {
        KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
        KOREADER_STORE.saveToFile();
      },
      "koMatchMethod", StrId::STR_KOREADER_SYNC));
  settings.push_back(SettingInfo::DynamicEnum(
      StrId::STR_KO_SYNC_CONFLICT, {StrId::STR_KO_ASK_EVERY_TIME, StrId::STR_KO_SMART_SYNC},
      [](const void*) -> uint8_t { return static_cast<uint8_t>(KOREADER_STORE.getSyncBehavior()); },
      [](void*, uint8_t v) {
        KOREADER_STORE.setSyncBehavior(static_cast<KOReaderSyncBehavior>(v));
        KOREADER_STORE.saveToFile();
      },
      "koSyncBehavior", StrId::STR_KOREADER_SYNC));
  settings.push_back(SettingInfo::Toggle(StrId::STR_KO_SYNC_ON_BOOK_CLOSE, &CrossPointSettings::koSyncOnBookClose,
                                         "koSyncOnBookClose", StrId::STR_KOREADER_SYNC));
  settings.push_back(SettingInfo::Action(StrId::STR_KO_MIN_SESSION_PAGES, SettingAction::KOSyncMinPagesPicker)
                         .withDisplayGetter(getKoSyncMinPagesDisplay)
                         .withCategory(StrId::STR_KOREADER_SYNC));
  settings.push_back([]() {
    SettingInfo s;
    s.nameId = StrId::STR_SEND_METADATA;
    s.type = SettingType::TOGGLE;
    s.key = "koSendMetadata";
    s.category = StrId::STR_KOREADER_SYNC;
    s.valueGetter = [](const void*) -> uint8_t { return KOREADER_STORE.getSendMetadata() ? 1u : 0u; };
    s.valueSetter = [](void*, uint8_t v) {
      KOREADER_STORE.setSendMetadata(v != 0);
      KOREADER_STORE.saveToFile();
    };
    return s;
  }());

  // --- Status Bar Settings (web-only, uses StatusBarSettingsActivity) ---
  settings.push_back(SettingInfo::Enum(StrId::STR_UPPER_PROGRESS_BAR, &CrossPointSettings::statusBarUpperProgressBar,
                                       {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE},
                                       "statusBarUpperProgressBar", StrId::STR_CUSTOMISE_STATUS_BAR));
  settings.push_back(SettingInfo::Enum(
      StrId::STR_UPPER_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarUpperProgressBarThickness,
      {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
      "statusBarUpperProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));
  settings.push_back(SettingInfo::Enum(StrId::STR_STATUS_ITEMS_POSITION, &CrossPointSettings::statusBarItemsPosition,
                                       {StrId::STR_TOP, StrId::STR_BOTTOM}, "statusBarItemsPosition",
                                       StrId::STR_CUSTOMISE_STATUS_BAR));
  settings.push_back(SettingInfo::Toggle(StrId::STR_CHAPTER_PAGE_COUNT, &CrossPointSettings::statusBarChapterPageCount,
                                         "statusBarChapterPageCount", StrId::STR_CUSTOMISE_STATUS_BAR));
  settings.push_back(SettingInfo::Toggle(StrId::STR_PRINTED_PAGE_NUMBER, &CrossPointSettings::statusBarPrintedPage,
                                         "statusBarPrintedPage", StrId::STR_CUSTOMISE_STATUS_BAR));
  settings.push_back(SettingInfo::Toggle(StrId::STR_BOOK_PROGRESS_PERCENTAGE,
                                         &CrossPointSettings::statusBarBookProgressPercentage,
                                         "statusBarBookProgressPercentage", StrId::STR_CUSTOMISE_STATUS_BAR));
  settings.push_back(SettingInfo::Enum(StrId::STR_TITLE, &CrossPointSettings::statusBarTitle,
                                       {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE}, "statusBarTitle",
                                       StrId::STR_CUSTOMISE_STATUS_BAR));
  settings.push_back(SettingInfo::Toggle(StrId::STR_BATTERY, &CrossPointSettings::statusBarBattery, "statusBarBattery",
                                         StrId::STR_CUSTOMISE_STATUS_BAR));
  settings.push_back(SettingInfo::Toggle(StrId::STR_CLOCK, &CrossPointSettings::statusBarClock, "statusBarClock",
                                         StrId::STR_CUSTOMISE_STATUS_BAR));
  settings.push_back(SettingInfo::Enum(StrId::STR_LOWER_PROGRESS_BAR, &CrossPointSettings::statusBarLowerProgressBar,
                                       {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE},
                                       "statusBarLowerProgressBar", StrId::STR_CUSTOMISE_STATUS_BAR));
  settings.push_back(SettingInfo::Enum(
      StrId::STR_LOWER_PROGRESS_BAR_THICKNESS, &CrossPointSettings::statusBarLowerProgressBarThickness,
      {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK},
      "statusBarLowerProgressBarThickness", StrId::STR_CUSTOMISE_STATUS_BAR));

  return settings;
}

}  // namespace SettingsListDetail

inline std::vector<SettingInfo> getSettingsList() {
  std::vector<SettingInfo> settings = SettingsListDetail::buildSettingsList();
  // Answer each capability ONCE here, from the HAL or the active board profile,
  // so the rest of the codebase never has to ask "which board is this" to decide
  // whether a setting is meaningful.
  const auto boardHas = [](const SettingRequires capability) {
    switch (capability) {
      case SettingRequires::Nothing:
        return true;
      case SettingRequires::TouchPanel:
        return gpio.hasTouch();
      case SettingRequires::TiltSensor:
        return BoardConfig::ACTIVE.sensors.imuType != BoardConfig::ImuType::None;
      case SettingRequires::SelectableGrayscaleLut:
        // SSD1677 develops grayscale from a factory waveform, so there is no
        // LUT to swap and the fast/OEM trade-off does not exist there.
        return BoardConfig::ACTIVE.displayController != BoardConfig::DisplayController::SSD1677;
      case SettingRequires::ReadingLight:
        // Asked of the HAL rather than the board profile: the EEGO A4's I2C
        // light is only present after begin() gets an ACK, so the profile alone
        // would over-report.
        return Frontlight.present();
      case SettingRequires::WarmLight:
        return Frontlight.present() && Frontlight.hasColorTemperature();
    }
    return true;
  };
  settings.erase(
      std::remove_if(settings.begin(), settings.end(),
                     [&boardHas](const SettingInfo& setting) { return !boardHas(setting.requiredCapability); }),
      settings.end());
  return settings;
}
