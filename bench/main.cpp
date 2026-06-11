// ESP32-C3 SaxParser benchmark
//
// Measures wall-clock time (esp_timer_get_time, 1µs resolution) and heap
// consumption for the yxml-backed SaxParser on the real target hardware.
//
// Fixture data is embedded in flash — no SD card required.
// Results are printed over USB-CDC serial, then the chip halts.
//
// Build & flash:  pio run -e bench -t upload
// Monitor:        pio device monitor -e bench

#include <Arduino.h>
#include <SaxParser/SaxParser.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "fixtures.h"

// ---------------------------------------------------------------------------
// Heap measurement
//
// esp_get_minimum_free_heap_size() is a global low-watermark since boot, so
// we use the free-heap delta across a create→use→destroy cycle instead.
// Measured before warm-up so the first allocation is cold.
// ---------------------------------------------------------------------------

static size_t snapHeap() { return esp_get_free_heap_size(); }

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

static int64_t t0;
static void timerStart() { t0 = esp_timer_get_time(); }
static int64_t timerElapsedUs() { return esp_timer_get_time() - t0; }

// ---------------------------------------------------------------------------
// Minimal SAX event collectors (count events, accumulate nothing to heap)
// ---------------------------------------------------------------------------

struct Counter {
  int starts = 0, ends = 0, chars = 0;
  static void onStart(void* ud, const char*, const char**) { ((Counter*)ud)->starts++; }
  static void onEnd(void* ud, const char*) { ((Counter*)ud)->ends++; }
  static void onChar(void* ud, const char*, int) { ((Counter*)ud)->chars++; }
};

// ---------------------------------------------------------------------------
// Benchmark: raw SaxParser lifecycle (create + init + destroy, no input)
// ---------------------------------------------------------------------------

static void benchRawLifecycle() {
  static void (*noop)(void*, const char*, const char**) = [](void*, const char*, const char**) {};
  static void (*noopEnd)(void*, const char*) = [](void*, const char*) {};

  constexpr int REPS = 500;

  // Cold heap measurement: one create/destroy before any warm-up.
  size_t before = snapHeap();
  {
    SaxParser p;
    p.init(nullptr, noop, noopEnd);
  }
  size_t after = snapHeap();
  int heapDelta = (int)before - (int)after;  // positive = consumed

  // Warm-up then timed loop.
  {
    SaxParser w;
    w.init(nullptr, noop, noopEnd);
  }
  timerStart();
  for (int i = 0; i < REPS; ++i) {
    SaxParser p;
    p.init(nullptr, noop, noopEnd);
  }
  int64_t us = timerElapsedUs();

  Serial.printf("BENCH raw_lifecycle     heap_delta=%+dB  total=%lldus  avg=%lldus  reps=%d\n", heapDelta, us,
                us / REPS, REPS);
}

// ---------------------------------------------------------------------------
// Benchmark: parse container.xml (258 B, structural only, no charCb)
// ---------------------------------------------------------------------------

static void benchContainerXml() {
  constexpr int REPS = 500;
  Counter c;

  size_t before = snapHeap();
  {
    SaxParser p;
    p.init(&c, Counter::onStart, Counter::onEnd);
    p.feed(container_xml_data, container_xml_size);
    p.finalize();
  }
  size_t after = snapHeap();
  int heapDelta = (int)before - (int)after;

  {
    SaxParser w;
    w.init(&c, Counter::onStart, Counter::onEnd);
    w.feed(container_xml_data, container_xml_size);
    w.finalize();
  }

  timerStart();
  for (int i = 0; i < REPS; ++i) {
    SaxParser p;
    p.init(&c, Counter::onStart, Counter::onEnd);
    p.feed(container_xml_data, container_xml_size);
    p.finalize();
  }
  int64_t us = timerElapsedUs();

  Serial.printf("BENCH container_xml     heap_delta=%+dB  total=%lldus  avg=%lldus  reps=%d  input=%uB\n", heapDelta,
                us, us / REPS, REPS, (unsigned)container_xml_size);
}

// ---------------------------------------------------------------------------
// Benchmark: parse toc.ncx (33 KB, 138 chapters, no charCb)
// ---------------------------------------------------------------------------

static void benchTocNcx() {
  constexpr int REPS = 50;
  Counter c;

  size_t before = snapHeap();
  {
    SaxParser p;
    p.init(&c, Counter::onStart, Counter::onEnd);
    p.feed(toc_ncx_data, toc_ncx_size);
    p.finalize();
  }
  size_t after = snapHeap();
  int heapDelta = (int)before - (int)after;

  {
    SaxParser w;
    w.init(&c, Counter::onStart, Counter::onEnd);
    w.feed(toc_ncx_data, toc_ncx_size);
    w.finalize();
  }

  timerStart();
  for (int i = 0; i < REPS; ++i) {
    SaxParser p;
    p.init(&c, Counter::onStart, Counter::onEnd);
    p.feed(toc_ncx_data, toc_ncx_size);
    p.finalize();
  }
  int64_t us = timerElapsedUs();

  Serial.printf("BENCH toc_ncx           heap_delta=%+dB  total=%lldus  avg=%lldus  reps=%d  input=%uKB\n", heapDelta,
                us, us / REPS, REPS, (unsigned)(toc_ncx_size / 1024));
}

// ---------------------------------------------------------------------------
// Benchmark: parse chapter-large.xhtml (76 KB, with charCb)
// ---------------------------------------------------------------------------

static void benchLargeXhtml() {
  constexpr int REPS = 20;
  Counter c;

  size_t before = snapHeap();
  {
    SaxParser p;
    p.init(&c, Counter::onStart, Counter::onEnd, Counter::onChar);
    p.feed(chapter_large_xhtml_data, chapter_large_xhtml_size);
    p.finalize();
  }
  size_t after = snapHeap();
  int heapDelta = (int)before - (int)after;

  {
    SaxParser w;
    w.init(&c, Counter::onStart, Counter::onEnd, Counter::onChar);
    w.feed(chapter_large_xhtml_data, chapter_large_xhtml_size);
    w.finalize();
  }

  timerStart();
  for (int i = 0; i < REPS; ++i) {
    SaxParser p;
    p.init(&c, Counter::onStart, Counter::onEnd, Counter::onChar);
    p.feed(chapter_large_xhtml_data, chapter_large_xhtml_size);
    p.finalize();
  }
  int64_t us = timerElapsedUs();

  Serial.printf("BENCH large_xhtml       heap_delta=%+dB  total=%lldus  avg=%lldus  reps=%d  input=%uKB\n", heapDelta,
                us, us / REPS, REPS, (unsigned)(chapter_large_xhtml_size / 1024));
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n=== SaxParser ESP32-C3 benchmark ===");
  Serial.printf("CPU: %u MHz   free heap: %u B\n\n", (unsigned)getCpuFrequencyMhz(),
                (unsigned)esp_get_free_heap_size());

  benchRawLifecycle();
  benchContainerXml();
  benchTocNcx();
  benchLargeXhtml();

  Serial.printf("\nfree heap after: %u B   minimum ever: %u B\n", (unsigned)esp_get_free_heap_size(),
                (unsigned)esp_get_minimum_free_heap_size());
  Serial.println("=== done ===");
}

void loop() {}
