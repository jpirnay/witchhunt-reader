#pragma once

// Gates the APP-side touch layer -- the tap/row/target recorders, the gesture
// classifier and the gesture settings rows -- out of a build whose board cannot
// have a digitiser. The SDK half is already gated by FREEINK_CAP_TOUCH; this is
// the half our fork owns, and it was never gated at all.
//
// Deliberately NOT derived from FREEINK_CAP_TOUCH here. The recorder headers this
// guards (ListTouchBand, TapTargets, ButtonHintStrip) are dependency-free so the
// host tests can include them, and reaching BoardConfig.h from them would drag in
// Arduino.h. Deriving it from a macro that only exists once BoardConfig.h has been
// included would be worse than that: a TU that included BoardConfig first would
// see a different value from one that did not, and since this changes the SIZE of
// ListTouchBand::Band, that is an ODR violation rather than a missed optimisation.
//
// So the value comes from the command line (platformio.ini) and defaults to "this
// board may have touch". The default is the SAFE direction on purpose: a new board
// keeps the full touch layer until someone explicitly asserts it has no digitiser,
// rather than silently losing touch by inheriting an MCU-family flag.
// MappedInputManager.cpp static_asserts the asserted value against
// FREEINK_CAP_TOUCH where both are in scope, so the two cannot drift apart.
#ifndef CP_TOUCH_UI
#define CP_TOUCH_UI 1
#endif
