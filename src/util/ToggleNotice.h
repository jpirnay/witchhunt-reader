#pragma once

#include <I18n.h>

class GfxRenderer;

// A short message over the screen after a key or a gesture switched something on or off that the screen does not
// show: "Touch Lock: On". Only for those (Touch Lock, Touch Navigation, Auto Page Turn, which turns nothing until its
// interval has run). A switch whose effect is in plain sight (the reading light, Dark Mode, Bionic Reading) has none:
// the message only repeated it (#930). Drawn with GUI.drawPopup() over the frame on the panel, as a plugin's toast is:
// one partial refresh and no full one. The screen's next frame takes it away, or, when none comes, one the message
// asks for after ToggleNoticeGate::kShowMs. Loop task only.
namespace ToggleNotice {

// What was switched, by its setting's name, and whether it is on now. `afterFrame`: the switch asked for a frame of its
// own as well (Auto Page Turn: its status band may lay the page out again), which the message waits for, or that frame
// would draw over it. A newer message replaces one not shown yet.
void show(StrId name, bool on, bool afterFrame);

// Paints the message once the screen is idle, or drops it when the screen changed or it waited too long, and takes it
// down again (ToggleNoticeGate). The main loop calls it on every pass, after the activity's.
void service(GfxRenderer& renderer);

}  // namespace ToggleNotice
