# A global action out of a Wi-Fi action in Settings leaves the radio up

Part of [Wi-Fi session end](../wifi-session-end.md).

**Open, pre-existing on every board (found in PR 1's review).** The Wi-Fi Networks picker and Weather's city
search are opened from Settings, and their session is ended by the parent's result handler
(`endRadioLeftOnBySettingAction()`, `src/activities/settings/SettingActionDispatch.cpp`). A bound global action
that leaves the stack, such as Go Home, skips that handler. The radio then stays up until the next session end or
sleep.

The same shape exists for Sync Time and Detect Timezone: a global action taken while their join screen is still
up leaves before `wifiWasUsed_` is set. On a C3 that also skips the restart after the network trim, so the device
goes on with the trimmed heap until its next restart.

**Next step:** end the session where the stack is unwound for a global action, or in the hosting activity's
`onExit()` when the radio is still up. Check it on a C3 and on a PSRAM board: start the picker, take Go Home, and
read the log for a `Session end` line.
