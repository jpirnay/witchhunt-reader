#pragma once

// Developer screens kept out of shipping builds. Each defaults to off; turn one on for an
// investigation build by adding it to that env's build_flags in platformio.ini, for example
// -DENABLE_FONT_SCALING_TEST=1. No env sets them, so every release and dev build leaves them out.

// Settings > System > Font Scaling Test: the reader's size ladder, every rung marked real or
// scaled (FontScalingTestActivity.h). It settled which sizes ship as real faces; a reader has no
// use for it.
#ifndef ENABLE_FONT_SCALING_TEST
#define ENABLE_FONT_SCALING_TEST 0
#endif

// Settings > System > Boot Diagnostics: the on-device view of BootDiagnostics' boot and sleep
// history (BootDiagnosticsActivity.h). Only the screen is gated. The recorder still runs in every
// build, and the web Diagnostics page and GET /api/diagnostics/boot still show the same history.
#ifndef ENABLE_BOOT_DIAGNOSTICS_SCREEN
#define ENABLE_BOOT_DIAGNOSTICS_SCREEN 0
#endif

// Reader menu > Render Benchmark: the scripted page-turn sweep and its report
// (EpubReaderBenchmark.cpp).
#ifndef ENABLE_BENCHMARKS
#define ENABLE_BENCHMARKS 0
#endif
