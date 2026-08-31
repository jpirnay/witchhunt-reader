#pragma once

#include <cstdint>

// Progress reporting for long-running work in the lib layer.
//
// Sibling to CooperativeAbort, and the same dependency direction: the app installs a
// handler at the start of an operation it knows can run long, lib code calls step() at its
// natural milestones, and libs never reach up into src/. Default (no handler) is a null
// check and a return, so host tests and every caller that has not wired it are unaffected.
//
// Why it exists: Epub::load() can spend seconds on a cache-miss CSS rebuild — parsing the
// OPF, scanning the ZIP for stylesheets, compiling them, then deleting every section cache
// so they rebuild against the new CSS. None of that painted anything. On a wake straight
// back into a book there is no splash behind it either (BootResume::ReaderResume), so the
// panel keeps showing the sleep screen throughout and the device reads as dead — the
// failure behind issue #155.
//
// `stage` is a short untranslated token ("fingerprint", "index", "css", ...). It is shown
// verbatim in the progress popup and in diagnostics, so it stays the same word on every
// device regardless of language — the same rule the Boot Diagnostics values follow.
//
// Deliberately a plain function pointer rather than std::function: this is installed on
// paths where the heap is already the reason things are slow, and std::function would add
// a per-signature binary cost plus a heap-allocated closure for no benefit.
namespace LongTaskProgress {

using Handler = void (*)(const char* stage);

// Install (or clear, with nullptr) the handler. Scope it to the operation being watched —
// an installed handler paints, and painting is only safe on a task that owns the
// framebuffer.
void setHandler(Handler handler);

// Report entry into a named stage. Cheap: one null check when nothing is installed.
// May paint, so it belongs only at points the caller knows own the framebuffer.
void step(const char* stage);

// --- Liveness ------------------------------------------------------------------------
// Separate from step() because the two cannot share a call site. The decoders and the page
// renderer already yield frequently — per MCU block in tjpgBmpOutput, per element in
// Page::render — but those points CANNOT paint: they are mid-write into the framebuffer,
// and drawPopup() resyncs the write buffer from the displayed frame, which would erase the
// half-decoded image and leave the decoder filling a buffer holding the previous screen.
// Aborting the work to paint instead is worse: on any decode longer than the paint
// interval it would restart from scratch every time and never finish.
//
// So the yield points feed this instead. It records only — never paints — which is enough
// to separate the two failures that look identical from outside: work that is slow but
// still ticking, and work that has genuinely stopped. That distinction is exactly what a
// "the device looks dead" report could not previously supply.

// Note that long-running work is still making progress. Records a timestamp; no handler is
// invoked and nothing is drawn. Called from CooperativeAbort::shouldAbortLongTask(), so
// every existing and future yield point is covered without touching any of them.
void noteAlive();

// Milliseconds since the last noteAlive(), or 0 if none has ever been recorded.
uint32_t msSinceAlive();

// The stage name most recently passed to step(), or nullptr. Points at the caller's string
// literal — the call sites all pass literals, so it outlives the call.
const char* currentStage();

}  // namespace LongTaskProgress
