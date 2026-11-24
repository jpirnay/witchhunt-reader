#pragma once

#include <I18n.h>

class GfxRenderer;

// A short message over the screen after a key or a gesture switched something on or off: "Dark Mode: On". Some of
// these switches change nothing on the screen (Touch Lock, Touch Navigation, the reading light), the others only with
// the next frame, a page laid out again or the whole panel inverted, which says little about what was switched. Drawn
// with GUI.drawPopup() over the frame on the panel, as a plugin's toast is: one partial refresh and no full one, and
// the screen's next frame takes it away. Loop task only.
namespace ToggleNotice {

// What was switched, by its setting's name, and whether it is on now. `afterFrame`: the switch asked for a frame of its
// own as well (Dark Mode, Bionic Reading, Auto Page Turn), which the message waits for, or that frame would draw over
// it. A newer message replaces one not shown yet.
void show(StrId name, bool on, bool afterFrame);

// Paints the message once the screen is idle, or drops it when the screen changed or it waited too long
// (ToggleNoticeGate). The main loop calls it on every pass, after the activity's.
void service(GfxRenderer& renderer);

}  // namespace ToggleNotice
