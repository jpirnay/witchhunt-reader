#pragma once
#include <I18n.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

enum class SettingType { TOGGLE, ENUM, ACTION, VALUE, STRING };

// What a setting NEEDS from the hardware, so the list can ask "can this board
// do it" instead of "which board is this".
//
// The old form was SettingDeviceTarget{BOTH, X3, X4} and resolved through
// gpio.deviceIsX3(). That conflates unrelated questions and does not survive a
// third board: on X4 Pro deviceIsX3() is false, so every X4-targeted setting
// would silently appear there whether or not the hardware supports it, and
// every X3-targeted one would silently vanish. Widening the enum to name four
// boards multiplies the problem rather than fixing it. See
// docs/contributing/board-support.md, "Ask what the board can do".
//
// Each value below names one capability, answered from the HAL or the active
// board profile in SettingsListDetail::boardHas(). Add a value when a setting needs
// something no existing one covers — never a board name.
enum class SettingRequires : uint8_t {
  Nothing,     // always visible
  TouchPanel,  // a touch controller (BoardConfig touch.controller != None)
  TiltSensor,  // an IMU for tilt page turning (sensors.imuType != None)
  // The panel builds grayscale from a swappable LUT, so the fast/OEM waveform
  // trade-off is a real choice. Controllers with a factory grayscale waveform
  // (SSD1677) have nothing to swap. This is a genuine silicon difference, which
  // B0 allows keying on the display controller — with a comment saying why,
  // which is this one.
  SelectableGrayscaleLut,
  // A PWM frontlight/backlight the firmware can drive (BoardConfig frontlight
  // config, or the PM1 PWM path). Answered through HalFrontlight so a board
  // whose light is probed at runtime reports honestly. Named ReadingLight, not
  // Frontlight: HalFrontlight.h defines `Frontlight` as a singleton macro, and
  // a macro does not respect the enum's scope.
  ReadingLight,
  // A second (warm) light channel, so colour temperature is a real control.
  // Sub-capability of Frontlight — the T5S3's single backlight channel has none.
  WarmLight,
  // A touch controller that reports more than one contact (GT911). Pinch and
  // rotation can never fire without it. Sub-capability of TouchPanel.
  MultiTouchPanel,
  // The panel fades in direct sunlight unless powered down between refreshes,
  // so the compensation is worth its cost here. Recorded per board
  // (BoardProfile::panelFadesInSunlight) because it is a property of the glass
  // and the enclosure: boards around the same controllers differ, so there is
  // nothing to derive it from and nothing to probe.
  SunlightFadingPanel,
};

enum class SettingAction {
  None,
  RemapFrontButtons,
  ButtonActionsOverview,
  GestureActionsOverview,
  CustomiseStatusBar,
  DownloadFonts,
  ClockSettings,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  ScreenRepair,
  CheckForUpdates,
  SdFirmwareUpdate,
  AllFiles,
  Language,
  KeyboardLayouts,
  SystemInfo,
  BootDiagnostics,
  FontScalingTest,
  DetectTimezone,
  SyncTime,
  Weather,
  ReadingStats,
  DictionarySelect,
  SleepTimeoutPicker,
  RefreshFrequencyPicker,
  KOSyncOnClosePicker,
  KOSyncIntervalPicker,
  FrontlightBrightnessPicker,
  FrontlightWarmthPicker,
  SwitchToUsbDrive,
  Submenu,
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  // Where a row's options come from:
  //
  //   enumValues        -- StrId references, localized through I18N.get(). The normal case.
  //   enumLabels        -- ready-made std::strings, for options whose text is not a translatable
  //                        phrase and is not known at compile time: a font family read off the
  //                        SD card, or a point size ("14pt").
  //   enumLiteralLabelFn -- the same idea for options that ARE known at compile time and live in
  //                         flash as literals. See its own note below.
  //   enumCountFn/enumLabelFn -- options read when asked, for lists that follow live state. See
  //                              withDynamicOptions().
  //
  // enumLabels is exclusive of enumValues: it wins whenever it is non-empty, so a row that sets it leaves
  // enumValues EMPTY rather than padding it to the same length. enumLiteralLabelFn is not -- it
  // EXTENDS enumValues, so a row can carry translated options followed by literal ones.
  //
  // Everything that displays, cycles or bounds options goes through getEnumOptionCount() /
  // getEnumOptionLabel(). Read a vector directly and a row using one of the other forms renders
  // blank and clamps to zero, which is how this has now failed twice.
  std::vector<StrId> enumValues;
  std::vector<std::string> enumLabels;

  // A third form, for options that are not translatable text: the first enumValues.size()
  // options come from StrIds as usual, and the options at and after that index come from this
  // callback, which returns a pointer into flash.
  //
  // enumLabels would seem to fit, but it is a vector<std::string> -- and forEachSetting()
  // rebuilds every row on every settings save AND load, so the timezone row's 86 city names
  // would mean 86 heap strings per call, for labels that are string literals and never change.
  //
  // A FUNCTION rather than a const char* array for the same reason the settings rows are built
  // at runtime (see the note at the top of SettingsList.h): the array would
  // want a function-local static to assemble it, and the guard variable on first use pulls
  // __cxa_guard_acquire and a FreeRTOS mutex onto a stack that forEachSetting() is already deep
  // into, called as it is from inside SETTINGS.loadFromFile() at boot. A captureless lambda
  // decays to a plain function pointer, needs no storage at all, and cannot allocate.
  //
  // Only meaningful alongside enumValues; ignored when enumLabels is non-empty.
  using LiteralLabelFn = const char* (*)(uint8_t literalIndex);
  LiteralLabelFn enumLiteralLabelFn = nullptr;
  uint8_t enumLiteralCount = 0;

  // Appends flash-resident option labels after the translated ones. See enumLiteralLabelFn.
  SettingInfo& withLiteralOptions(const LiteralLabelFn labelFn, const uint8_t count) {
    enumLiteralLabelFn = labelFn;
    enumLiteralCount = count;
    return *this;
  }

  // A fourth form, for options that follow live state. A font-size row lists the sizes of the
  // family selected right now, and that changes while the screen holding the row is open (choose a
  // family, then a size, in the same submenu). The other forms are filled in when the row is
  // built, so they would go on listing the previous family's sizes.
  //
  // Function pointers taking accessorCtx, for the same reason as the dynamic getter and setter
  // (no std::function heap). Wins over the other three forms when set.
  using OptionCountFn = uint8_t (*)(const void* ctx);
  using OptionLabelFn = std::string (*)(const void* ctx, uint8_t index);
  OptionCountFn enumCountFn = nullptr;
  OptionLabelFn enumLabelFn = nullptr;

  SettingInfo& withDynamicOptions(const OptionCountFn countFn, const OptionLabelFn labelFn) {
    enumCountFn = countFn;
    enumLabelFn = labelFn;
    return *this;
  }
  SettingAction action = SettingAction::None;

  struct ValueRange {
    uint8_t min;
    uint8_t max;
    uint8_t step;
  };
  ValueRange valueRange = {};

  const char* key = nullptr;             // JSON API key (nullptr for ACTION types)
  StrId category = StrId::STR_NONE_OPT;  // Category for web UI grouping
  bool obfuscated = false;               // Save/load via base64 obfuscation (passwords)
  SettingRequires requiredCapability = SettingRequires::Nothing;

  // Direct char[] string fields (for settings stored in CrossPointSettings)
  size_t stringOffset = 0;
  size_t stringMaxLen = 0;

  // Dynamic accessors (for settings stored outside CrossPointSettings, e.g. KOReaderCredentialStore).
  // Function pointers + opaque context avoid the heap allocation of std::function. Stateless
  // lambdas pass ctx=nullptr; captures must be hand-written as trampoline functions. See
  // DynamicEnumCtx / DynamicStringCtx factories below.
  using ValueGetterFn = uint8_t (*)(const void*);
  using ValueSetterFn = void (*)(void*, uint8_t);
  using StringGetterFn = std::string (*)(void*);
  using StringSetterFn = void (*)(void*, const std::string&);

  void* accessorCtx = nullptr;
  ValueGetterFn valueGetter = nullptr;
  ValueSetterFn valueSetter = nullptr;
  StringGetterFn stringGetter = nullptr;
  StringSetterFn stringSetter = nullptr;

  uint8_t callValueGetter() const {
    assert(valueGetter && "SettingInfo::callValueGetter requires a non-null valueGetter");
    return valueGetter(accessorCtx);
  }
  void callValueSetter(uint8_t v) const {
    assert(valueSetter && "SettingInfo::callValueSetter requires a non-null valueSetter");
    valueSetter(accessorCtx, v);
  }
  std::string callStringGetter() const {
    assert(stringGetter && "SettingInfo::callStringGetter requires a non-null stringGetter");
    return stringGetter(accessorCtx);
  }
  void callStringSetter(const std::string& v) const {
    assert(stringSetter && "SettingInfo::callStringSetter requires a non-null stringSetter");
    stringSetter(accessorCtx, v);
  }

  struct SubmenuData {
    StrId id = StrId::STR_NONE_OPT;
    std::vector<SettingInfo> items;
  };

  static void prepareSubmenus(std::vector<SettingInfo>& items, std::vector<SubmenuData>& submenuData);

  // Walks `items` and inserts a SettingInfo::Separator before each item whose subcategory
  // differs from the previous one — same rule used by SettingsActivity for the main tabs.
  // Existing separator rows preserve their own nameId as the running subcategory so a
  // manually placed Separator suppresses an immediate auto-insert for the same group.
  static void insertSubcategorySeparators(std::vector<SettingInfo>& items);

  SettingInfo& withObfuscated() {
    obfuscated = true;
    return *this;
  }

  static SettingInfo Toggle(StrId nameId, uint8_t CrossPointSettings::* ptr, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valuePtr = ptr;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Enum(StrId nameId, uint8_t CrossPointSettings::* ptr, std::vector<StrId> values,
                          const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.valuePtr = ptr;
    s.enumValues = std::move(values);
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo Action(StrId nameId, SettingAction action) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.action = action;
    return s;
  }

  // Attach a stateless display-value getter to an ACTION entry so it shows the current
  // value instead of ">>". ctx is passed through; nullptr for stateless lambdas.
  SettingInfo& withDisplayGetter(StringGetterFn fn, void* ctx = nullptr) {
    stringGetter = fn;
    accessorCtx = ctx;
    return *this;
  }

  static SettingInfo Value(StrId nameId, uint8_t CrossPointSettings::* ptr, const ValueRange valueRange,
                           const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valuePtr = ptr;
    s.valueRange = valueRange;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo String(StrId nameId, char* ptr, size_t maxLen, const char* key = nullptr,
                            StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringOffset = (size_t)ptr - (size_t)&SETTINGS;
    s.stringMaxLen = maxLen;
    s.key = key;
    s.category = category;
    return s;
  }

  // Stateless variant of Toggle — for a flag whose authority is not a
  // CrossPointSettings field alone. The frontlight's on/off is one: the setter
  // must reach the hardware as well as the setting, or the light would only
  // follow the switch after a reboot.
  static SettingInfo DynamicToggle(StrId nameId, ValueGetterFn getter, ValueSetterFn setter, const char* key = nullptr,
                                   StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::TOGGLE;
    s.valueGetter = getter;
    s.valueSetter = setter;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicValue(StrId nameId, const ValueRange valueRange, ValueGetterFn getter, ValueSetterFn setter,
                                  const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::VALUE;
    s.valueRange = valueRange;
    s.valueGetter = getter;
    s.valueSetter = setter;
    s.key = key;
    s.category = category;
    return s;
  }

  // Stateless variant — getter/setter are free/static functions with no captured state.
  static SettingInfo DynamicEnum(StrId nameId, std::vector<StrId> values, ValueGetterFn getter, ValueSetterFn setter,
                                 const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ENUM;
    s.enumValues = std::move(values);
    s.valueGetter = getter;
    s.valueSetter = setter;
    s.key = key;
    s.category = category;
    return s;
  }

  // Context-carrying variant — trampolines receive `ctx` as first argument and cast it back to
  // their concrete owner type.
  static SettingInfo DynamicEnumCtx(StrId nameId, std::vector<StrId> values, void* ctx, ValueGetterFn getter,
                                    ValueSetterFn setter, const char* key = nullptr,
                                    StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s = DynamicEnum(nameId, std::move(values), getter, setter, key, category);
    s.accessorCtx = ctx;
    return s;
  }

  static SettingInfo DynamicString(StrId nameId, StringGetterFn getter, StringSetterFn setter,
                                   const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::STRING;
    s.stringGetter = getter;
    s.stringSetter = setter;
    s.key = key;
    s.category = category;
    return s;
  }

  static SettingInfo DynamicStringCtx(StrId nameId, void* ctx, StringGetterFn getter, StringSetterFn setter,
                                      const char* key = nullptr, StrId category = StrId::STR_NONE_OPT) {
    SettingInfo s = DynamicString(nameId, getter, setter, key, category);
    s.accessorCtx = ctx;
    return s;
  }

  static SettingInfo Separator(StrId nameId) {
    SettingInfo s;
    s.nameId = nameId;
    s.type = SettingType::ACTION;
    s.isSeparator = true;
    return s;
  }

  // Where this row's value LIVES, for rows whose UI type does not carry a field pointer of its
  // own: a slider ACTION, chiefly, which is edited through SliderSetting rather than by toggling.
  //
  // Deliberately not `valuePtr`. That member means "the UI reads and writes this directly", and
  // getDisplayValue()/toggleValue() prefer it over a row's getter — so putting a field there
  // would shadow the live value of a dynamic row. This one is about storage only.
  //
  // Rows that declare it are saved and loaded by the generic loop in JsonSettingsIO like any
  // other field. Rows that do not, are not — which is how five slider rows came to need
  // hand-written serialisation lines, and how two of them (the frontlight levels) ended up with
  // none at all: they worked for a session and reset at the next boot.
  uint8_t CrossPointSettings::* persistPtr = nullptr;
  // Inclusive upper bound for a loaded value. Above it the compiled default is used instead,
  // which is the same protection the generic loop gives an ENUM via its option count.
  uint8_t persistMax = 100;

  // Declares where this row's value is stored and under what key. See persistPtr.
  SettingInfo& persisting(uint8_t CrossPointSettings::* field, const char* storageKey, const uint8_t maxValue = 100) {
    persistPtr = field;
    key = storageKey;
    persistMax = maxValue;
    return *this;
  }

  bool isSeparator = false;
  bool usesSelectorActivity = false;        // Confirm opens a full-screen selector instead of inline cycling
  StrId subcategory = StrId::STR_NONE_OPT;  // Triggers a separator row on first use and on change
  StrId submenu = StrId::STR_NONE_OPT;      // Routes item into a submenu; hidden from main list

  // Marks this entry as requiring a full-screen selector activity on Confirm
  // (instead of inline value cycling). The SettingsActivity / SettingsSubmenuActivity
  // intercept entries with this flag before toggleValue() is called.
  SettingInfo& withSelectorActivity() {
    usesSelectorActivity = true;
    return *this;
  }

  SettingInfo& withCategory(StrId cat) {
    category = cat;
    return *this;
  }

  // Inserts a separator row in the parent tab when this item's subcategory first appears or changes.
  SettingInfo& withSubcategory(StrId sub) {
    subcategory = sub;
    return *this;
  }

  // Hides this item from the parent tab and places it inside a SettingsSubmenuActivity instead.
  // All items sharing the same submenu StrId are grouped under one placeholder entry.
  SettingInfo& withSubmenu(StrId sub) {
    submenu = sub;
    return *this;
  }

  // Hide this setting on hardware that cannot do the thing it controls.
  // Default is SettingRequires::Nothing (always visible).
  SettingInfo& requiring(SettingRequires capability) {
    requiredCapability = capability;
    return *this;
  }

  // For internal use by SettingsActivity: placeholder entry that launches the submenu.
  static SettingInfo SubmenuEntry(StrId titleId) {
    SettingInfo s;
    s.nameId = titleId;
    s.type = SettingType::ACTION;
    s.action = SettingAction::Submenu;
    return s;
  }

  // Returns the localised title; separators are decorated automatically.
  [[nodiscard]] std::string getTitle() const;

  // Returns the current display value string (ON/OFF for toggles, enum label, numeric value, >>).
  [[nodiscard]] std::string getDisplayValue() const;

  // The boolean behind a TOGGLE row, read from wherever this row keeps it (a CrossPointSettings
  // field, or a dynamic getter for state the settings struct does not own -- the reading light
  // asks the hardware). Split out so the on-screen SWITCH and the ON/OFF text cannot disagree
  // about which is lit: getDisplayValue() reads it through here too. False for any other type.
  [[nodiscard]] bool getToggleState() const;

  // Toggles/cycles the underlying value for TOGGLE, ENUM, and VALUE types.
  // Does nothing for ACTION and STRING types (callers handle those separately).
  // Marked const because it mutates the external SETTINGS global (via valuePtr),
  // not the SettingInfo itself.
  void toggleValue() const;

  // --- ENUM option accessors, used by the full-screen EnumSelectionActivity ---
  // These mirror the read/write logic in getDisplayValue()/toggleValue() but expose
  // the option set directly so a list selector can render every choice and jump to
  // one, instead of cycling. enumLabels takes precedence over enumValues (see the
  // enumLabels contract above). All are no-ops / empty for non-ENUM types.

  // Number of selectable options (0 for non-ENUM).
  [[nodiscard]] uint8_t getEnumOptionCount() const {
    if (type != SettingType::ENUM) return 0;
    if (enumCountFn) return enumCountFn(accessorCtx);
    if (!enumLabels.empty()) return static_cast<uint8_t>(enumLabels.size());
    return static_cast<uint8_t>(enumValues.size() + enumLiteralCount);
  }

  // The option's label as a BORROWED pointer, or nullptr when this row keeps its labels as
  // std::strings (enumLabels) and there is no such pointer to hand out.
  //
  // For the one caller that can use a borrowed pointer and would otherwise pay to copy: the web
  // settings API passes these straight to ArduinoJson, which stores a const char* by reference
  // but copies a std::string. Going through getEnumOptionLabel() there would copy every option
  // of every enum row into the document -- around 1.7 KB for the timezone row's 86 names alone,
  // where before it stored 86 pointers into flash. Everything else should use
  // getEnumOptionLabel() and not think about lifetimes.
  [[nodiscard]] const char* getEnumOptionFlashLabel(uint8_t index) const {
    if (type != SettingType::ENUM || !enumLabels.empty() || enumLabelFn) return nullptr;
    if (index < enumValues.size()) return I18N.get(enumValues[index]);
    const size_t literal = index - enumValues.size();
    if (enumLiteralLabelFn && literal < enumLiteralCount) return enumLiteralLabelFn(static_cast<uint8_t>(literal));
    return nullptr;
  }

  // Localised label for option `index` (empty if out of range or non-ENUM).
  [[nodiscard]] std::string getEnumOptionLabel(uint8_t index) const {
    if (type != SettingType::ENUM) return {};
    if (enumLabelFn) return index < getEnumOptionCount() ? enumLabelFn(accessorCtx, index) : std::string{};
    if (!enumLabels.empty()) return index < enumLabels.size() ? enumLabels[index] : std::string{};
    const char* flash = getEnumOptionFlashLabel(index);
    return flash ? std::string(flash) : std::string{};
  }

  // Currently selected option index (0 if unreadable).
  [[nodiscard]] uint8_t getEnumSelectedIndex() const {
    if (type != SettingType::ENUM) return 0;
    if (valuePtr) return SETTINGS.*(valuePtr);
    if (valueGetter) return callValueGetter();
    return 0;
  }

  // Writes the selected option index through the same path getDisplayValue() reads.
  void setEnumSelectedIndex(uint8_t index) const {
    if (type != SettingType::ENUM) return;
    if (index >= getEnumOptionCount()) return;
    if (valuePtr)
      SETTINGS.*(valuePtr) = index;
    else if (valueSetter)
      callValueSetter(index);
  }
};

inline void SettingInfo::prepareSubmenus(std::vector<SettingInfo>& items,
                                         std::vector<SettingInfo::SubmenuData>& submenuData) {
  if (items.empty()) return;

  std::vector<SettingInfo> preparedItems;
  std::vector<SubmenuData> preparedSubmenus;
  std::vector<size_t> placeholderAt;  // parallel to preparedSubmenus: where its row landed
  preparedItems.reserve(items.size());

  for (auto& item : items) {
    if (item.submenu == StrId::STR_NONE_OPT) {
      preparedItems.push_back(std::move(item));
      continue;
    }

    const StrId submenuId = item.submenu;
    auto it = std::find_if(preparedSubmenus.begin(), preparedSubmenus.end(),
                           [submenuId](const SubmenuData& d) { return d.id == submenuId; });
    if (it == preparedSubmenus.end()) {
      preparedItems.push_back(SettingInfo::SubmenuEntry(submenuId));
      placeholderAt.push_back(preparedItems.size() - 1);
      preparedSubmenus.push_back({submenuId, {}});
      it = preparedSubmenus.end() - 1;
    }
    item.submenu = StrId::STR_NONE_OPT;
    it->items.push_back(std::move(item));
  }

  items.swap(preparedItems);

  for (size_t s = 0; s < preparedSubmenus.size(); ++s) {
    auto& submenu = preparedSubmenus[s];

    // The placeholder takes a subcategory only when EVERY row behind it agrees on one -- the
    // shape of "Refresh" or "Front light", where the submenu name and the heading are the same
    // word and the heading is really about the row itself.
    //
    // The gesture rows are the other shape: one submenu holding four groups (swipes, taps, long
    // taps, multi-touch). Inheriting from the first of them put a "Swipes" heading over a row
    // that leads to all twenty gestures, and left the other three headings behind in the parent
    // tab with nothing under them once their rows had moved into the submenu. The grouping
    // describes the children, so it belongs to the submenu, not to the row that opens it.
    const StrId first = submenu.items.empty() ? StrId::STR_NONE_OPT : submenu.items.front().subcategory;
    const bool allAgree = std::all_of(submenu.items.begin(), submenu.items.end(),
                                      [first](const SettingInfo& child) { return child.subcategory == first; });
    items[placeholderAt[s]].subcategory = allAgree ? first : StrId::STR_NONE_OPT;

    auto it = std::find_if(submenuData.begin(), submenuData.end(),
                           [&submenu](const SubmenuData& d) { return d.id == submenu.id; });
    if (it == submenuData.end()) {
      submenuData.push_back(std::move(submenu));
      it = submenuData.end() - 1;
    } else {
      it->items.insert(it->items.end(), std::make_move_iterator(submenu.items.begin()),
                       std::make_move_iterator(submenu.items.end()));
    }
    // Carry the grouping INTO the submenu, which is the list the rows actually appear in.
    // SettingsSubmenuActivity has always known how to draw separator rows; nothing ever put any
    // in front of it. Re-running over a list that already has them is harmless: the pass tracks
    // an existing separator as the running heading rather than adding a second one.
    insertSubcategorySeparators(it->items);
  }
}

inline void SettingInfo::insertSubcategorySeparators(std::vector<SettingInfo>& items) {
  if (items.empty()) return;
  std::vector<SettingInfo> out;
  out.reserve(items.size() + 4);
  StrId lastSub = StrId::STR_NONE_OPT;
  for (auto& item : items) {
    if (item.isSeparator) {
      lastSub = item.nameId;
      out.push_back(std::move(item));
      continue;
    }
    if (item.subcategory != StrId::STR_NONE_OPT && item.subcategory != lastSub) {
      out.push_back(SettingInfo::Separator(item.subcategory));
      lastSub = item.subcategory;
    }
    out.push_back(std::move(item));
  }
  items.swap(out);
}
