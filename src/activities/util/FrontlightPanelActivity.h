#pragma once

#include "TouchUi.h"

#if CP_TOUCH_UI

#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

// Top-anchored frontlight drawer opened by a top-edge down-swipe: a sheet pulled over the page,
// only as tall as its contents, with the rest of the screen left standing.
//
// Ported from crosspoint-reader PR #2983 ("feat: Add support for x4pro & papermono devices",
// Justin Mitchell / @itsthisjustin), commit bbca4886.
//
// THEIRS: the screen's whole behaviour -- the controls it carries and the ones it deliberately
// leaves out, driving brightness and warmth live, seeding from the hardware rather than from
// SETTINGS, the lightOnChanged rule for what may be persisted on exit, and the arithmetic in
// percentFromPermille().
//
// OURS: the layout. Upstream hand-builds the band, the step buttons, the sun hit rect and the
// tap-outside test, because it predates the FreeInkUI control-panel kit (SDK 7133b45,
// 2026-08-22) that exists for exactly this screen. So it is built from that kit instead:
//
//   fui::sheet      the drawer chrome -- body, rule, grabber pill, rounded free edge, and the
//                   dismissAction over the page below that makes a tap outside close it.
//   fui::sliderRow  caption + value + [-] [capsule] [+], with the optional trailing icon button
//                   its own documentation names as "a lamp on/off toggle beside a brightness
//                   slider". That is the sun.
//
// The kit also removes upstream's draggingSlider guard: FUI's router has grab semantics (a drag
// stays bound to the element the finger landed on, and a contact that dragged commits as a drag
// rather than falling through to the tap path), so a slider drag ending below the sheet cannot
// be mistaken for a dismissing tap.
//
// Two divergences that are this fork's, not upstream's:
//
//  - Getting the displayed frame into the write buffer is work, and it is NOT all done here.
//    Upstream builds with EINK_DISPLAY_SINGLE_BUFFER_MODE, where the write buffer IS the
//    displayed frame and painting only part of it is correct for free. This fork is dual-buffer,
//    and two separate things can leave that buffer wrong, so dispatchLightPanelGesture() prepares
//    it before this activity is pushed -- see the comment there. render() only re-syncs on LATER
//    passes, where the stale frame is its own. Device-confirmed 2026-09-17 on a T5S3, after two
//    wrong diagnoses: the reader parks Background-A's pre-rendered NEXT page in the secondary
//    buffer, so a sync on the opening pass copies the wrong page over a correctly prepared one.
//  - Brightness floors at HalFrontlight::MIN_BRIGHTNESS (this fork's constant; upstream has no
//    floor). A 0% level with the light still "on" would leave the sun icon lit over a dark
//    panel, which is a second and worse way of saying off when the switch is one tap away.
//
// Also this fork's: a Dark Mode row under the sliders (DarkModeNow::flip), the switch a reader reaches for in the same
// moment as the light. A tap closes the drawer, so the screen under it is drawn again in the new polarity.
//
// Gated on CP_TOUCH_UI: nothing opens this without a digitiser, and this fork compiles the whole
// app-side touch layer out of a board that cannot have one.
class FrontlightPanelActivity final : public Activity, private UiAppHost {
  ButtonNavigator buttonNavigator;

  uint8_t brightness = 60;
  uint8_t warmth = 50;
  bool lightOn = false;
  // lightOn is seeded from the live hardware state (Frontlight.isOn()), which legitimately
  // diverges from the saved SETTINGS.frontlightOn preference -- e.g. after a wake with
  // frontlightRestoreOnWake off, the light stays off live while the saved "was on" preference is
  // deliberately kept. brightness/warmth have no such divergence (always restored
  // unconditionally on boot), so only lightOn needs a touched-by-the-user flag: onExit() must
  // not persist a mirror that never reflected user intent in the first place.
  bool lightOnChanged = false;
  // Sheet height for the pass being built. Set in render() before renderUi(), because the sheet
  // must be sized before anything lays out inside it.
  int panelHeight = 0;
  // The opening pass composites over a frame another activity drew; see render() for why that
  // one cannot be a FAST differential.
  bool firstRender = true;

  static void panelScreen(UiScreen& screen, void* user);
  static void onBrightnessEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onToggleEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBrightnessStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onWarmthStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onDismissEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onDarkModeEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildPanelScreen(UiScreen& screen);
  // Height of one sliderRow as Screen::sliderRow() will actually lay it out -- the same theme
  // substitutions, so the sheet is sized to what goes in it rather than to an estimate.
  int16_t sliderRowBandHeight() const;
  int computePanelHeight() const;
  void adjustBrightness(int delta);
  void adjustWarmth(int delta);
  void toggleLight();
  void close();

 public:
  explicit FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  // The drawer paints only its own sheet band and leaves the rest of the frame standing, which is
  // why dispatchLightPanelGesture() prepares the framebuffer for it. Nothing may repaint between
  // that preparation and this activity's first render.
  bool suppressesBusyIndicator() const override { return true; }
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};

#endif  // CP_TOUCH_UI
