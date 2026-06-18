#pragma once

// Cooperative abort hook for long-running, slice-able work in the lib layer
// (image decoders that can otherwise stall the single main loop for >1 s).
//
// The app installs a predicate at startup that reports whether the user is
// waiting on input (e.g. a queued button edge from the background sampler).
// Lower-level libs poll shouldAbortLongTask() at their natural per-row / per-
// block boundary and bail out so the main loop can service that input first.
//
// Dependency direction: the predicate is INJECTED downward from the app; libs
// never reach up into src/.  Default (no installer) is "never abort", so host
// tests and any caller that hasn't wired the hook keep the old behaviour.
namespace CooperativeAbort {

// Returns true when an in-flight long task should stop early and yield control.
// Cheap: a single function-pointer null check plus the installed predicate.
bool shouldAbortLongTask();

// Install (or clear, with nullptr) the predicate. Called once from app setup.
void setLongTaskAbortPredicate(bool (*predicate)());

// ── Abort latch ────────────────────────────────────────────────────────────
// A long task that actually bails mid-work (a decode row loop that broke for
// pending input) calls markAborted(). This is distinct from a task that simply
// FAILED (e.g. an oversize image rejected before any work) — a failure that
// merely coincides with queued input must NOT be mistaken for an abort, or the
// caller retries a permanently-failing task forever.
//
// Protocol: caller clears the latch before the task, runs it, then reads
// consumeAborted() to decide retry (true) vs. record-failure (false).
void markAborted();

// Returns the latch without clearing it. Use at intermediate levels that need to
// branch on abort but must leave the latch for the orchestrator to consume.
bool wasAborted();

// Returns the latch and clears it. The orchestrator calls this once per attempt to
// read abort state; clear it again with clearAborted() before the next attempt.
bool consumeAborted();

// Clears the latch without reading. Call before starting a task.
void clearAborted();

}  // namespace CooperativeAbort
