#pragma once

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
void step(const char* stage);

}  // namespace LongTaskProgress
