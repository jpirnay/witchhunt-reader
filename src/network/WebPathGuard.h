#pragma once

#include <ProtectedPaths.h>
#include <WString.h>

// The card-aware half of ProtectedPaths: the same rules, with alias-shaped segments
// resolved through Storage. Every path a web or WebDAV request names goes through this
// before the card is touched. `path` must already be normalised (normalizeWebPath).
bool isProtectedWebPath(const String& path, ProtectedPaths::Target target, bool allowHidden);
