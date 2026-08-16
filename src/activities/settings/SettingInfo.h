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
// boards multiplies the problem rather than fixing it. See the B0
// capability-predicate section of
// docs/multiboard-bringup-handover-2026-08-15.md.
//
// Each value below names one capability, answered from the HAL or the active
// board profile in getSettingsList(). Add a value when a setting needs
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
};

enum class SettingAction {
  None,
  RemapFrontButtons,
  ButtonActionsOverview,
  CustomiseStatusBar,
  DownloadFonts,
  ClockSettings,
  KOReaderSync,
  OPDSBrowser,
  Network,
  ClearCache,
  CheckForUpdates,
  SdFirmwareUpdate,
  Language,
  SystemInfo,
  DetectTimezone,
  SyncTime,
  Weather,
  ReadingStats,
  SleepTimeoutPicker,
  RefreshFrequencyPicker,
  KOSyncMinPagesPicker,
  SwitchToUsbDrive,
  Submenu,
};

struct SettingInfo {
  StrId nameId;
  SettingType type;
  uint8_t CrossPointSettings::* valuePtr = nullptr;
  // Enum values are used as StrId references for localization via I18N.get().
  // If enumLabels is populated, it is authoritative for display and index bounds.
  // In that case it must have the same length as enumValues, because consumers
  // such as getDisplayValue(), toggleValue(), and CrossPointWebServer::handleGetSettings()
  // prefer enumLabels when present. Code paths which validate posted enum values
  // should also use enumLabels.size() when enumLabels is non-empty.
  std::vector<StrId> enumValues;
  std::vector<std::string> enumLabels;
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
    return static_cast<uint8_t>(enumLabels.empty() ? enumValues.size() : enumLabels.size());
  }

  // Localised label for option `index` (empty if out of range or non-ENUM).
  [[nodiscard]] std::string getEnumOptionLabel(uint8_t index) const {
    if (type != SettingType::ENUM) return {};
    if (!enumLabels.empty()) return index < enumLabels.size() ? enumLabels[index] : std::string{};
    return index < enumValues.size() ? std::string(I18N.get(enumValues[index])) : std::string{};
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
  preparedItems.reserve(items.size());

  for (auto& item : items) {
    if (item.submenu == StrId::STR_NONE_OPT) {
      preparedItems.push_back(std::move(item));
      continue;
    }

    auto it = std::find_if(preparedSubmenus.begin(), preparedSubmenus.end(),
                           [&item](const SubmenuData& d) { return d.id == item.submenu; });
    if (it == preparedSubmenus.end()) {
      auto placeholder = SettingInfo::SubmenuEntry(item.submenu);
      placeholder.subcategory = item.subcategory;  // inherit so addTo inserts the separator
      preparedItems.push_back(std::move(placeholder));
      preparedSubmenus.push_back({item.submenu, {}});
      it = preparedSubmenus.end() - 1;
    }
    it->items.push_back(std::move(item));
  }

  items.swap(preparedItems);

  for (auto& submenu : preparedSubmenus) {
    auto it = std::find_if(submenuData.begin(), submenuData.end(),
                           [&submenu](const SubmenuData& d) { return d.id == submenu.id; });
    if (it == submenuData.end()) {
      submenuData.push_back(std::move(submenu));
    } else {
      it->items.insert(it->items.end(), std::make_move_iterator(submenu.items.begin()),
                       std::make_move_iterator(submenu.items.end()));
    }
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
