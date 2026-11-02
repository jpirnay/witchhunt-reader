// Measures lib/OtShaper on the host: struct sizes, Face::init, Plan::build
// (heap peak, allocations, time) and ot::shape over the oracle corpus.
//
//   ot_shaper_probe <corpus dir> <script> <font> [<script> <font>]...
//
// <script> is a corpus file stem of test/ot_shaper/generate_corpus.py
// (devanagari ... sinhala); its position in that script's SCRIPTS list is its
// ot::Script. <font> is a TrueType/OpenType file or a .cpfont layout font.
// Prints four Markdown tables to stdout: Sizes, Faces, Plans, Shaping.
//
// Every global operator new / new[] (throwing, nothrow, aligned) and every
// operator delete is replaced by a counting pair over malloc. Each block
// carries a 16-byte header holding its size.
//
// It builds, unchanged, against upstream's engine (crosspoint-reader #3787)
// and against ours: with OtFixedVector.h present (deviation 1) the buffer is
// bound to caller memory before first use, as upstream's reserves for itself.

#include <OtShaper.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#if __has_include("OtFixedVector.h")
#define PROBE_BIND_BUFFER 1
#else
#define PROBE_BIND_BUFFER 0
#endif

// ---------------------------------------------------------------------------
// Counting allocator.

namespace {

struct Counters {
  unsigned long long allocations = 0;
  unsigned long long throwing = 0;
  long long live = 0;
  long long peak = 0;
};
Counters gCount;

constexpr size_t HEADER = 16;

struct Header {
  size_t size;
  void* base;
};
static_assert(sizeof(Header) <= HEADER, "header");

void* countedAlloc(size_t size, size_t align, bool throwing) {
  if (align < HEADER) align = HEADER;
  // malloc returns at least 16-byte-aligned memory on the hosts this runs on;
  // a larger alignment is made by over-allocating.
  const size_t extra = align > HEADER ? align : 0;
  void* base = std::malloc(size + HEADER + extra);
  if (!base) {
    if (throwing) std::abort();  // -fno-exceptions: nothing to throw
    return nullptr;
  }
  uintptr_t user = reinterpret_cast<uintptr_t>(base) + HEADER;
  if (extra) user = (user + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
  Header* h = reinterpret_cast<Header*>(user - HEADER);
  h->size = size;
  h->base = base;
  gCount.allocations++;
  if (throwing) gCount.throwing++;
  gCount.live += static_cast<long long>(size);
  if (gCount.live > gCount.peak) gCount.peak = gCount.live;
  return reinterpret_cast<void*>(user);
}

void countedFree(void* p) {
  if (!p) return;
  const Header* h = reinterpret_cast<const Header*>(static_cast<uint8_t*>(p) - HEADER);
  gCount.live -= static_cast<long long>(h->size);
  std::free(h->base);
}

}  // namespace

void* operator new(size_t n) { return countedAlloc(n, 0, true); }
void* operator new[](size_t n) { return countedAlloc(n, 0, true); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return countedAlloc(n, 0, false); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return countedAlloc(n, 0, false); }
void* operator new(size_t n, std::align_val_t a) { return countedAlloc(n, static_cast<size_t>(a), true); }
void* operator new[](size_t n, std::align_val_t a) { return countedAlloc(n, static_cast<size_t>(a), true); }
void* operator new(size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return countedAlloc(n, static_cast<size_t>(a), false);
}
void* operator new[](size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return countedAlloc(n, static_cast<size_t>(a), false);
}
void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, size_t) noexcept { countedFree(p); }
void operator delete[](void* p, size_t) noexcept { countedFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { countedFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { countedFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { countedFree(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { countedFree(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { countedFree(p); }

// ---------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

double micros(const Clock::time_point a, const Clock::time_point b) {
  return std::chrono::duration<double, std::micro>(b - a).count();
}

// A window over the counters: allocations made and the peak above the live
// bytes at its start.
struct Window {
  unsigned long long allocations0;
  unsigned long long throwing0;
  long long live0;
  long long peak0;

  Window() {
    allocations0 = gCount.allocations;
    throwing0 = gCount.throwing;
    live0 = gCount.live;
    peak0 = gCount.peak;
    gCount.peak = gCount.live;
  }
  unsigned long long allocations() const { return gCount.allocations - allocations0; }
  unsigned long long throwing() const { return gCount.throwing - throwing0; }
  long long peakAbove() const { return gCount.peak - live0; }
  // Leaves the global peak as the higher of both.
  ~Window() {
    if (peak0 > gCount.peak) gCount.peak = peak0;
  }
};

const char* const SCRIPTS[] = {"devanagari", "bengali", "gurmukhi", "gujarati", "oriya",
                               "tamil",      "telugu",  "kannada",  "malayalam", "sinhala"};
constexpr int SCRIPT_COUNT = sizeof(SCRIPTS) / sizeof(SCRIPTS[0]);

int scriptIndex(const char* name) {
  for (int i = 0; i < SCRIPT_COUNT; i++) {
    if (std::strcmp(SCRIPTS[i], name) == 0) return i;
  }
  return -1;
}

bool readFile(const std::string& path, std::vector<uint8_t>* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out->resize(n > 0 ? static_cast<size_t>(n) : 0);
  const size_t got = n > 0 ? std::fread(out->data(), 1, out->size(), f) : 0;
  std::fclose(f);
  return got == out->size();
}

std::string baseName(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void decodeUtf8(const std::string& s, std::vector<uint32_t>* out) {
  out->clear();
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<uint8_t>(s[i]);
    uint32_t cp;
    int n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c >> 5) == 6) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c >> 4) == 14) {
      cp = c & 0x0F;
      n = 3;
    } else {
      cp = c & 0x07;
      n = 4;
    }
    for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
    out->push_back(cp);
    i += n;
  }
}

// The corpus lines as compare_harfbuzz.py reads them: stripped, without
// empty lines and '#' comments, first occurrence kept.
bool readCorpus(const std::string& path, std::vector<std::vector<uint32_t>>* out) {
  std::vector<uint8_t> bytes;
  if (!readFile(path, &bytes)) return false;
  std::vector<std::string> seen;
  size_t start = 0;
  const auto isSpace = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; };
  while (start <= bytes.size()) {
    size_t end = start;
    while (end < bytes.size() && bytes[end] != '\n') end++;
    if (!(end > start && bytes[start] == '#')) {
      size_t a = start;
      size_t b = end;
      while (a < b && isSpace(static_cast<char>(bytes[a]))) a++;
      while (b > a && isSpace(static_cast<char>(bytes[b - 1]))) b--;
      if (b > a) seen.emplace_back(reinterpret_cast<const char*>(bytes.data()) + a, b - a);
    }
    if (end >= bytes.size()) break;
    start = end + 1;
  }
  // The generator writes a sorted set, so duplicates do not occur; keep the
  // first anyway, as dict.fromkeys does.
  std::vector<std::string> unique;
  unique.reserve(seen.size());
  {
    std::vector<std::pair<std::string, size_t>> sorted;
    sorted.reserve(seen.size());
    for (size_t i = 0; i < seen.size(); i++) sorted.emplace_back(seen[i], i);
    std::sort(sorted.begin(), sorted.end());
    std::vector<bool> keep(seen.size(), true);
    for (size_t i = 1; i < sorted.size(); i++) {
      if (sorted[i].first == sorted[i - 1].first) keep[sorted[i].second] = false;
    }
    for (size_t i = 0; i < seen.size(); i++) {
      if (keep[i]) unique.push_back(seen[i]);
    }
  }
  out->clear();
  out->resize(unique.size());
  for (size_t i = 0; i < unique.size(); i++) decodeUtf8(unique[i], &(*out)[i]);
  return true;
}

struct Entry {
  int script;
  std::string fontPath;
  std::string name;
  std::vector<uint8_t> font;
  ot::FaceTables tables;  // as in the file
  bool hasCpac = false;
  bool hasCppl = false;
};

ot::FaceTables computedRoute(const ot::FaceTables& t) {
  ot::FaceTables out = t;
  out[ot::FaceTables::CPAC] = ot::Table();
  out[ot::FaceTables::CPPL] = ot::Table();
  return out;
}

ot::FaceTables storedRoute(const ot::FaceTables& t) {
  ot::FaceTables out = t;
  out[ot::FaceTables::CPPL] = ot::Table();
  return out;
}

volatile uint32_t gSink = 0;

#if PROBE_BIND_BUFFER
alignas(alignof(ot::GlyphInfo)) uint8_t gBufferBlock[ot::Buffer::bytesFor(4096)];
#endif

double bestFaceInit(const ot::FaceTables& tables, int runs) {
  double best = 1e30;
  for (int i = 0; i < runs; i++) {
    ot::Face f;
    const auto t0 = Clock::now();
    const bool ok = f.init(tables);
    const auto t1 = Clock::now();
    gSink = gSink + (ok ? f.glyphCount() : 0u);
    const double us = micros(t0, t1);
    if (us < best) best = us;
  }
  return best;
}

void printSizes() {
  std::printf("## Sizes\n\n");
  std::printf("Engine: %s. Host: %zu-bit pointers.\n\n",
              PROBE_BIND_BUFFER ? "ours (OtFixedVector.h present)" : "upstream (#3787)", sizeof(void*) * 8);
  std::printf("| Type | sizeof (B) |\n|---|---:|\n");
  std::printf("| ot::GlyphInfo | %zu |\n", sizeof(ot::GlyphInfo));
  std::printf("| ot::GlyphPosition | %zu |\n", sizeof(ot::GlyphPosition));
  std::printf("| ot::Buffer | %zu |\n", sizeof(ot::Buffer));
  std::printf("| ot::Face | %zu |\n", sizeof(ot::Face));
  std::printf("| ot::Plan | %zu |\n", sizeof(ot::Plan));
  std::printf("| ot::PlannedLookup | %zu |\n", sizeof(ot::PlannedLookup));
  std::printf("| ot::Stage | %zu |\n", sizeof(ot::Stage));
  std::printf("\n");
}

void printFaces(const std::vector<Entry>& entries) {
  std::printf("## Faces\n\n");
  std::printf(
      "Face::init best of 100, a Face copy best of 1,000 (and its mean over the 1,000), steady_clock microseconds.\n\n");
  std::printf("| Font | CPac (B) | init with CPac (us) | init without CPac (us) | copy best (us) | copy mean (us) |\n");
  std::printf("|---|---:|---:|---:|---:|---:|\n");
  for (const Entry& e : entries) {
    const ot::FaceTables without = computedRoute(e.tables);
    char with[32];
    if (e.hasCpac) {
      std::snprintf(with, sizeof(with), "%.2f", bestFaceInit(e.tables, 100));
    } else {
      std::snprintf(with, sizeof(with), "n/a");
    }
    const double withoutUs = bestFaceInit(without, 100);
    ot::Face face;
    face.init(e.hasCpac ? e.tables : without);
    double best = 1e30;
    double total = 0;
    for (int i = 0; i < 1000; i++) {
      const auto t0 = Clock::now();
      ot::Face copy = face;
      const auto t1 = Clock::now();
      gSink = gSink + copy.glyphCount();
      const double us = micros(t0, t1);
      total += us;
      if (us < best) best = us;
    }
    std::printf("| %s | %u | %s | %.2f | %.3f | %.4f |\n", e.name.c_str(),
                e.tables[ot::FaceTables::CPAC].length(), with, withoutUs, best, total / 1000);
  }
  std::printf("\n");
}

struct PlanRow {
  bool ok;
  size_t gsub, gpos, gsubStages, gposStages, memory;
  long long peak;
  unsigned long long allocations, throwing;
  double best;
};

PlanRow measurePlan(const ot::Face& face, const ot::Script script, const uint32_t* languages) {
  PlanRow r{};
  {
    ot::Plan plan;
    {
      const Window w;
      r.ok = plan.build(face, script, languages);
      r.peak = w.peakAbove();
      r.allocations = w.allocations();
      r.throwing = w.throwing();
    }
    r.gsub = plan.lookups[ot::GSUB].size();
    r.gpos = plan.lookups[ot::GPOS].size();
    r.gsubStages = plan.stages[ot::GSUB].size();
    r.gposStages = plan.stages[ot::GPOS].size();
    r.memory = plan.memoryBytes();
  }
  r.best = 1e30;
  for (int i = 0; i < 20; i++) {
    ot::Plan plan;
    const auto t0 = Clock::now();
    const bool ok = plan.build(face, script, languages);
    const auto t1 = Clock::now();
    gSink = gSink + (ok ? 1u : 0u);
    const double us = micros(t0, t1);
    if (us < r.best) r.best = us;
  }
  return r;
}

void printPlans(const std::vector<Entry>& entries) {
  const uint32_t* languages = ot::languageTagsFor("");
  std::printf("## Plans\n\n");
  std::printf(
      "Plan::build, no language. `computed`: CPac and CPpl dropped (the TrueType route); `stored`: CPpl dropped, "
      "CPac kept. Heap peak above the live bytes before the call; allocations of the first build (throwing ones in "
      "brackets); time best of 20, microseconds.\n\n");
  std::printf(
      "| Font | Script | Route | build | GSUB lookups | GPOS lookups | GSUB stages | GPOS stages | memoryBytes | heap "
      "peak (B) | allocations | time (us) |\n");
  std::printf("|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");
  for (const Entry& e : entries) {
    for (int route = 0; route < 2; route++) {
      if (route == 1 && !e.hasCpac) continue;
      const ot::FaceTables tables = route == 0 ? computedRoute(e.tables) : storedRoute(e.tables);
      ot::Face face;
      if (!face.init(tables)) {
        std::printf("| %s | %s | %s | face init failed | | | | | | | | |\n", e.name.c_str(), SCRIPTS[e.script],
                    route == 0 ? "computed" : "stored");
        continue;
      }
      const PlanRow r = measurePlan(face, static_cast<ot::Script>(e.script), languages);
      std::printf("| %s | %s | %s | %s | %zu | %zu | %zu | %zu | %zu | %lld | %llu (%llu) | %.1f |\n", e.name.c_str(),
                  SCRIPTS[e.script], route == 0 ? "computed" : "stored", r.ok ? "ok" : "FAIL", r.gsub, r.gpos,
                  r.gsubStages, r.gposStages, r.memory, r.peak, r.allocations, r.throwing, r.best);
    }
  }
  std::printf("\n");
}

void printShaping(const std::vector<Entry>& entries, const std::string& corpusDir) {
  const uint32_t* languages = ot::languageTagsFor("");
  std::printf("## Shaping\n\n");
  std::printf(
      "Route `computed`, no language, Scale::set(2133, 33, upem). Every corpus line shaped once, all with one "
      "buffer%s. Heap peak above the live bytes before the loop; times per line, microseconds.\n\n",
      PROBE_BIND_BUFFER ? " (bound to Buffer::bytesFor(4096) bytes)" : "");
  std::printf(
      "| Font | Script | lines | max code points | max glyphs | max glyphs - code points | failed, successful "
      "false | failed, successful true | allocations | heap peak (B) | mean (us) | max (us) |\n");
  std::printf("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
  for (const Entry& e : entries) {
    std::vector<std::vector<uint32_t>> lines;
    const std::string path = corpusDir + "/" + SCRIPTS[e.script] + ".txt";
    if (!readCorpus(path, &lines)) {
      std::printf("| %s | %s | corpus %s unreadable | | | | | | | | | |\n", e.name.c_str(), SCRIPTS[e.script],
                  path.c_str());
      continue;
    }
    const ot::FaceTables tables = computedRoute(e.tables);
    ot::Face face;
    if (!face.init(tables)) {
      std::printf("| %s | %s | face init failed | | | | | | | | | |\n", e.name.c_str(), SCRIPTS[e.script]);
      continue;
    }
    ot::Scale scale;
    scale.set(2133, 33, face.upem());
    ot::Plan plan;
    plan.build(face, static_cast<ot::Script>(e.script), languages);

    size_t maxCps = 0;
    unsigned maxGlyphs = 0;
    long long maxGrowth = -1000000;
    unsigned failedFalse = 0;
    unsigned failedTrue = 0;
    double total = 0;
    double worst = 0;
    unsigned long long allocations = 0;
    long long peak = 0;
    {
      ot::Buffer buffer;
#if PROBE_BIND_BUFFER
      buffer.setStorage(gBufferBlock, sizeof(gBufferBlock));
#endif
      const Window w;
      for (const std::vector<uint32_t>& cps : lines) {
        const auto t0 = Clock::now();
        const bool ok = ot::shape(face, scale, plan, cps.data(), static_cast<unsigned>(cps.size()), buffer);
        const auto t1 = Clock::now();
        const double us = micros(t0, t1);
        total += us;
        if (us > worst) worst = us;
        if (cps.size() > maxCps) maxCps = cps.size();
        if (!ok) {
          if (buffer.successful) {
            failedTrue++;
          } else {
            failedFalse++;
          }
          continue;
        }
        const unsigned glyphs = buffer.len();
        if (glyphs > maxGlyphs) maxGlyphs = glyphs;
        const long long growth = static_cast<long long>(glyphs) - static_cast<long long>(cps.size());
        if (growth > maxGrowth) maxGrowth = growth;
        gSink = gSink + (glyphs ? buffer.info[0].codepoint : 0u);
      }
      allocations = w.allocations();
      peak = w.peakAbove();
    }
    std::printf("| %s | %s | %zu | %zu | %u | %lld | %u | %u | %llu | %lld | %.2f | %.1f |\n", e.name.c_str(),
                SCRIPTS[e.script], lines.size(), maxCps, maxGlyphs, maxGrowth, failedFalse, failedTrue, allocations,
                peak, lines.empty() ? 0.0 : total / static_cast<double>(lines.size()), worst);
  }
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4 || (argc - 2) % 2 != 0) {
    std::fprintf(stderr, "usage: %s <corpus dir> <script> <font> [<script> <font>]...\n", argv[0]);
    return 2;
  }
  const std::string corpusDir = argv[1];
  std::vector<Entry> entries;
  entries.reserve(static_cast<size_t>(argc - 2) / 2);
  for (int i = 2; i + 1 < argc; i += 2) {
    Entry e;
    e.script = scriptIndex(argv[i]);
    if (e.script < 0) {
      std::fprintf(stderr, "unknown script %s\n", argv[i]);
      return 2;
    }
    e.fontPath = argv[i + 1];
    e.name = baseName(e.fontPath);
    if (!readFile(e.fontPath, &e.font)) {
      std::fprintf(stderr, "cannot read %s\n", e.fontPath.c_str());
      return 1;
    }
    if (!ot::tablesFromSfnt(e.font.data(), static_cast<uint32_t>(e.font.size()), &e.tables)) {
      std::fprintf(stderr, "bad font %s\n", e.fontPath.c_str());
      return 1;
    }
    e.hasCpac = !e.tables[ot::FaceTables::CPAC].empty();
    e.hasCppl = !e.tables[ot::FaceTables::CPPL].empty();
    entries.push_back(std::move(e));
  }
  printSizes();
  printFaces(entries);
  printPlans(entries);
  printShaping(entries, corpusDir);
  return 0;
}
