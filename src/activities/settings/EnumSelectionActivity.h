#pragma once

#include <GfxRenderer.h>

#include <string>

#include "../Activity.h"
#include "SettingInfo.h"
#include "util/ButtonNavigator.h"

class MappedInputManager;

/// Full-screen list picker for a single ENUM SettingInfo.
///
/// Generalises the older font-only selector: instead of cycling an enum in place
/// by repeatedly pressing Confirm (tedious and blind for 4-5 option settings), it
/// shows every option as a scrollable list with the current value checked, and
/// writes the chosen value back through the SettingInfo's own accessor path.
///
/// The idea of opening a selection window for a multi-option toggle is adopted
/// from crosspoint-reader PR #2358 (originating in PR #1842 by @pablohc). This
/// implementation reuses the existing MenuListActivity list chrome rather than a
/// separate popup component, so it adds no per-theme render surface and keeps only
/// one activity resident at a time.
///
/// The activity does NOT persist settings itself — the caller's result handler is
/// responsible for SETTINGS.saveToFile() and any dependent-setting normalisation,
/// exactly as the previous font-selector call-sites already did.
class EnumSelectionActivity final : public Activity {
 public:
  // Optional per-index label override, used when the option labels are richer than
  // the SettingInfo's static enum labels (e.g. SD-card font families appended at
  // the call site). Return an empty string to fall back to the SettingInfo label.
  // A plain function pointer + count keeps this free of heap-allocating std::function
  // in the common (no-override) path.
  using LabelOverrideFn = std::string (*)(uint8_t index);

  EnumSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SettingInfo& setting,
                        LabelOverrideFn labelOverride = nullptr, uint8_t overrideCount = 0)
      : Activity("EnumSelect", renderer, mappedInput),
        setting(setting),
        labelOverride(labelOverride),
        overrideCount(overrideCount) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // This screen draws through GUI.drawList, so its rows are published in
  // ListTouchBand and a tap arrives here already resolved to an option index —
  // in the same frame selectedIndex uses, since drawList() is handed
  // selectedIndex directly with no offset.
  //
  // Without this override Activity's default Rejected applies and the list is
  // simply not tappable, which is what it was: the band was recorded and the hit
  // test resolved, and the tap died one call later. Silent, and indistinguishable
  // on hardware from the geometry being wrong.
  ListRowTap::Result selectListRow(int index) override;

 private:
  [[nodiscard]] uint8_t optionCount() const;
  [[nodiscard]] std::string optionLabel(uint8_t index) const;
  void handleSelection();

  const SettingInfo& setting;
  LabelOverrideFn labelOverride;
  uint8_t overrideCount;

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
};
