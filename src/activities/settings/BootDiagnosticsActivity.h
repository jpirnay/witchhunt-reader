#pragma once

#include "BootDiagnostics.h"
#include "activities/Activity.h"

/// Settings > System > Boot Diagnostics.
///
/// Renders what BootDiagnostics recorded: how this boot started, where the last sleep
/// stopped, and the persisted history pairing the two across power cycles. It exists so
/// a "stuck on the sleep screen" report can be answered without a serial monitor —
/// which most reporters do not have, and which a battery-powered device never opens
/// anyway (the CDC guard means light sleep is off for as long as one is attached).
///
/// The page is deliberately one screenful with no scrolling: its job is to be
/// photographed and pasted into an issue. Row labels are translated; the values stay as
/// English technical tokens so reports from different locales stay comparable. Once the
/// persisted history loads, the same evidence is emitted to serial without screen clipping.
class BootDiagnosticsActivity final : public Activity {
 public:
  explicit BootDiagnosticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BootDiagnostics", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void logToSerial() const;

  // Fixed storage rather than a vector: 256 bytes as part of the activity object, which
  // is already a heap allocation, versus a second one that would have to be sized from
  // the file anyway. loadRecords() fills it newest-first.
  BootDiag::Record records_[BootDiag::kCapacity] = {};
  uint8_t recordCount_ = 0;
  bool loaded_ = false;
};
