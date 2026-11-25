#pragma once

#include <ProtectedPaths.h>
#include <WString.h>

// The card-aware half of ProtectedPaths: the same rules, with alias-shaped segments
// resolved through Storage. Every path a web or WebDAV request names goes through this
// before the card is touched. `path` must already be normalised (normalizeWebPath).
bool isProtectedWebPath(const String& path, ProtectedPaths::Target target, bool allowHidden);

// For a file a plugin's device.json names as its own (PluginLocations::deviceJsonNames): refused for what no request
// may reach (a credential store, a system folder, a segment spelled by its 8.3 alias) and for the host approvals that
// fence the plugins in (/.crosspoint/plugin-hosts.json), not for being in or named like a dot folder. Upstream's
// isPluginPath() refuses the credential stores alone.
bool isProtectedPluginFile(const String& path);
