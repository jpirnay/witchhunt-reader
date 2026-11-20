#pragma once

#include <cstddef>
#include <cstdint>

#include "PluginEventCore.h"

class GfxRenderer;

// Events for SD plugins, on the device: a plugin's device.json declares an HTTP request (or a download) per event, the
// firmware queues each event that happens in the plugin's outbox on the card (<plugin dir>/events.jsonl), and replays
// the queue through those requests whenever the reader is online: a Wi-Fi join, File Transfer, and the sleep entry,
// which brings Wi-Fi up for a handler that asks for it. No plugin code runs on the reader. Delivery is at least once,
// in order per plugin. The contract (events, variables, limits) is docs/sd-plugins.md, "Events"; the parts without
// the card or the network are PluginEventCore.h.
//
// Upstream CrossPoint's design, so plugins written for it run here: crosspoint-reader PR #3114 (Justin Mitchell) and
// PR #4200 (Arthur Tazhitdinov). Not carried over yet: book.downloaded (no catalog download on the reader emits it)
// and a password grant's token refresh on 401/403 (no event plugin in the Plugin Hub uses one).
namespace plugin_events {

// Reads every enabled plugin's device.json "events" section into the subscription table, at boot. Plugins are installed
// over File Transfer, whose end restarts the reader (ServerSession::end), so a new plugin subscribes from then on.
// Allocates nothing when no plugin subscribes.
void refreshSubscriptions();

// True when an enabled plugin subscribes to `e`: a table lookup, no I/O.
bool anySubscriber(Event e);

// True when a queued event belongs to a handler that may bring Wi-Fi up at sleep entry ("connect": true, and
// sleep.enter). Reads the subscribed plugins' outboxes.
bool wantsConnectAny();

// Appends the event to each subscribing plugin's outbox: one card append, nothing more. A handler with
// "require_meta" gets only events whose book has that key in its <book>.meta.json.
void emit(Event e, const Var* vars, size_t count);

// What a drain did that its caller may have to act on.
struct DrainResult {
  size_t downloads = 0;  // downloads that replaced their file (a new sleep image wants the sleep screen drawn again)
};

// Replays queued events through their handlers. Only while Wi-Fi is connected: each is one HTTPS request on the
// calling task, at most `maxEvents` per call so a backlog cannot stall the screen it runs under. A delivered event
// leaves the outbox; a failed one stays, and the plugin's later events wait behind it. With `renderer`, a handler's
// "toast" shows as the standard popup after its request succeeds.
DrainResult drain(GfxRenderer* renderer, size_t maxEvents = 4);

// The trusted wall clock for a reading session's times: 0 while the clock is unset or only restored from a stale
// backup.
int64_t trustedEpoch();

}  // namespace plugin_events
