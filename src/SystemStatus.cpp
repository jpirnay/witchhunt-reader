#include "SystemStatus.h"

// The only reads of the two Git-derived build defines on behalf of SystemStatus.h.
// scripts/git_branch.py gives CROSSPOINT_VERSION (env:default and the *_gh_release_rc
// envs) and CROSSPOINT_DISPLAY_SDK (every env) only to the translation units whose own
// text names them, so that a branch switch or an SDK pin bump recompiles these few
// objects instead of the whole tree. Naming them here makes this file one of those units.

// Fallback keeps builds compiling if the pre-script could not resolve the SDK.
#ifndef CROSSPOINT_DISPLAY_SDK
#define CROSSPOINT_DISPLAY_SDK "unknown"
#endif

const char* SystemStatus::firmwareVersion() { return CROSSPOINT_VERSION; }

const char* SystemStatus::displaySdkVersion() { return CROSSPOINT_DISPLAY_SDK; }
