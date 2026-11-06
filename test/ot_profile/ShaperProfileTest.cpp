// Throwaway shaper profile (branch wip/shaper-profile). Shapes Devanagari words with a font read from
// OT_PROFILE_FONT the way CMD:TTF:SHAPE does (one run from the word's first Indic letter, at most 48 code points,
// 112 buffer slots, a CPac for the filters and a plan built from the font), and prints where ot::shape spends its
// time. Built twice: with OT_PROFILE (the stage breakdown) and without (wall clock only, for the overhead).
#include <OtFace.h>
#include <OtFilterWriter.h>
#include <OtPlan.h>
#include <OtShaper.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef OT_PROFILE
#include <OtProfile.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

uint64_t nowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

std::vector<uint8_t> readFile(const char* path) {
  std::vector<uint8_t> bytes;
  if (FILE* f = std::fopen(path, "rb")) {
    uint8_t buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  return bytes;
}

std::vector<uint32_t> decode(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    uint32_t cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    out.push_back(cp);
    i += n;
  }
  return out;
}

// The words of the text outside tags, split at ASCII white space; a few entities decoded.
std::vector<std::string> wordsOf(const std::string& xhtml) {
  std::string text;
  bool inTag = false;
  for (size_t i = 0; i < xhtml.size(); i++) {
    const char c = xhtml[i];
    if (c == '<') {
      inTag = true;
      text += ' ';
    } else if (c == '>') {
      inTag = false;
    } else if (!inTag) {
      if (c == '&') {
        const size_t semi = xhtml.find(';', i);
        if (semi != std::string::npos && semi - i < 8) {
          const std::string e = xhtml.substr(i + 1, semi - i - 1);
          if (e == "amp")
            text += '&';
          else if (e == "lt")
            text += '<';
          else if (e == "gt")
            text += '>';
          else if (e == "quot")
            text += '"';
          else if (e == "nbsp")
            text += ' ';
          i = semi;
          continue;
        }
      }
      text += c;
    }
  }
  std::vector<std::string> words;
  std::string cur;
  for (const char c : text) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      if (!cur.empty()) words.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) words.push_back(cur);
  return words;
}

// The run CMD:TTF:SHAPE shapes: from the first Devanagari letter on, at most 48 code points. Empty: no letter.
std::vector<uint32_t> runOf(const std::string& word) {
  std::vector<uint32_t> cps = decode(word);
  size_t first = 0;
  while (first < cps.size() && !(cps[first] >= 0x0900 && cps[first] <= 0x097F)) first++;
  std::vector<uint32_t> run(cps.begin() + first, cps.end());
  if (run.size() > 48) run.resize(48);
  return run;
}

// Feature tags naming `lookup` in the table (GSUB or GPOS), e.g. "rphf,abvs".
std::string featureTags(const ot::Face& face, const int table, const uint32_t lookup) {
  const ot::Table& t = face.layout(table);
  const ot::Table features = t.offset16(6);
  std::string out;
  for (unsigned i = 0; i < features.u16(0); i++) {
    const uint32_t tag = features.u32(2 + 6 * i);
    const ot::Table feature = features.offset16(2 + 6 * i + 4);
    for (unsigned k = 0; k < feature.u16(2); k++) {
      if (feature.u16(4 + 2 * k) != lookup) continue;
      char buf[8] = {static_cast<char>(tag >> 24), static_cast<char>(tag >> 16), static_cast<char>(tag >> 8),
                     static_cast<char>(tag), 0};
      if (out.find(buf) == std::string::npos) out += (out.empty() ? "" : ",") + std::string(buf);
    }
  }
  return out;
}

struct Fixture {
  std::vector<uint8_t> font;
  std::vector<uint8_t> cpac;
  ot::Face face;
  ot::Plan plan;
  std::vector<uint8_t> storage = std::vector<uint8_t>(ot::Buffer::bytesFor(112));
  ot::Buffer buffer;
  ot::Scale scale;
  uint64_t planNs = 0;

  bool open(const char* path) {
    font = readFile(path);
    ot::FaceTables tables;
    if (font.empty() || !ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables)) return false;
    tables[ot::FaceTables::CPAC] = ot::Table();
    tables[ot::FaceTables::CPPL] = ot::Table();
    ot::Face bare;
    if (!bare.init(tables)) return false;
    ot::FilterTableWriter writer(bare);
    cpac.resize(writer.bytes());
    for (uint32_t done = 0; done < cpac.size();) {
      const uint32_t n = writer.write(cpac.data() + done, static_cast<uint32_t>(cpac.size()) - done);
      if (!n) break;
      done += n;
    }
    tables[ot::FaceTables::CPAC] = ot::Table(cpac.data(), static_cast<uint32_t>(cpac.size()));
    if (!face.init(tables)) return false;
    const uint64_t t0 = nowNs();
    if (!plan.build(face, ot::Script::Devanagari, ot::languageTagsFor("hi"))) return false;
    planNs = nowNs() - t0;
    buffer.setStorage(storage.data(), storage.size());
    const int32_t ppem = 20 * 64;  // 20 px
    scale.set(ppem, (ppem + 32) >> 6, face.upem());
    return true;
  }

  bool shape(const std::vector<uint32_t>& run) {
    return ot::shape(face, scale, plan, run.data(), static_cast<unsigned>(run.size()), buffer);
  }
};

#ifdef OT_PROFILE
void printProfile(const char* what, Fixture& fx, const ot::prof::Data& total) {
  ot::prof::Data mean = total;
  mean.divide(total.calls);
  char line[1200];
  ot::prof::formatFields(line, sizeof line, total);
  std::printf("PROFILE %s TOTAL %s\n", what, line);
  ot::prof::formatFields(line, sizeof line, mean);
  std::printf("PROFILE %s MEAN_PER_SHAPE %s\n", what, line);
  ot::prof::formatCounters(line, sizeof line, total);
  std::printf("PROFILE %s COUNTERS_TOTAL %s\n", what, line);
  ot::prof::formatCounters(line, sizeof line, mean);
  std::printf("PROFILE %s COUNTERS_PER_SHAPE %s\n", what, line);
  for (int table = 0; table < 2; table++) {
    bool taken[ot::prof::MAX_TRACKED] = {};
    for (int k = 0; k < 10; k++) {
      int best = -1;
      for (unsigned i = 0; i < ot::prof::MAX_TRACKED; i++) {
        if (!taken[i] && total.cycles[table][i] && (best < 0 || total.cycles[table][i] > total.cycles[table][best])) {
          best = static_cast<int>(i);
        }
      }
      if (best < 0) break;
      taken[best] = true;
      const uint32_t lookup = fx.plan.lookups[table][best].index;
      std::printf("PROFILE %s TOP_%s #%d pos=%d lookup=%u tags=%s passed=%u ns=%llu ns_per_shape=%llu\n", what,
                  table == ot::GSUB ? "GSUB" : "GPOS", k + 1, best, lookup, featureTags(fx.face, table, lookup).c_str(),
                  total.passed[table][best], static_cast<unsigned long long>(total.cycles[table][best]),
                  static_cast<unsigned long long>(total.cycles[table][best] / (total.calls ? total.calls : 1)));
    }
  }
}
#endif

TEST(ShaperProfile, Words) {
  const char* fontPath = std::getenv("OT_PROFILE_FONT");
  if (!fontPath) GTEST_SKIP() << "OT_PROFILE_FONT not set";
  static Fixture fx;
  ASSERT_TRUE(fx.open(fontPath));
  std::printf("PROFILE plan_ns=%llu gsub_lookups=%zu gpos_lookups=%zu font_lookups=%u/%u\n",
              static_cast<unsigned long long>(fx.planNs), fx.plan.lookups[ot::GSUB].size(),
              fx.plan.lookups[ot::GPOS].size(), fx.face.lookupCount(ot::GSUB), fx.face.lookupCount(ot::GPOS));
  const char* words[] = {
      "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80",
      "\xE0\xA4\xAA\xE0\xA5\x8D\xE0\xA4\xB0\xE0\xA4\xBE\xE0\xA4\xB0\xE0\xA5\x8D\xE0\xA4\xA5\xE0\xA4\xA8\xE0\xA4\xBE",
      "\xE0\xA4\xAE\xE0\xA4\xBE\xE0\xA4\xA8\xE0\xA4\xB5",
      "\xE0\xA4\x85\xE0\xA4\xA7\xE0\xA4\xBF\xE0\xA4\x95\xE0\xA4\xBE\xE0\xA4\xB0\xE0\xA5\x8B\xE0\xA4\x82"};
  constexpr int REPEAT = 400;
  for (const char* w : words) {
    const std::vector<uint32_t> run = runOf(w);
    ASSERT_TRUE(fx.shape(run));  // warm the host caches once
    const unsigned glyphs = fx.buffer.len();
#ifdef OT_PROFILE
    ot::prof::g.reset();
#endif
    const uint64_t t0 = nowNs();
    for (int i = 0; i < REPEAT; i++) ASSERT_TRUE(fx.shape(run));
    const uint64_t wall = nowNs() - t0;
    std::printf("WORD %s cps=%zu glyphs=%u wall_ns_per_shape=%llu\n", w, run.size(), glyphs,
                static_cast<unsigned long long>(wall / REPEAT));
#ifdef OT_PROFILE
    printProfile(w, fx, ot::prof::g);
#endif
  }
}

TEST(ShaperProfile, Chapter) {
  const char* fontPath = std::getenv("OT_PROFILE_FONT");
  const char* chapterPath = std::getenv("OT_PROFILE_CHAPTER");
  if (!fontPath || !chapterPath) GTEST_SKIP() << "OT_PROFILE_FONT / OT_PROFILE_CHAPTER not set";
  static Fixture fx;
  ASSERT_TRUE(fx.open(fontPath));
  const std::vector<uint8_t> raw = readFile(chapterPath);
  ASSERT_FALSE(raw.empty());
  std::vector<std::vector<uint32_t>> runs;
  for (const std::string& w : wordsOf(std::string(raw.begin(), raw.end()))) {
    std::vector<uint32_t> run = runOf(w);
    if (!run.empty()) runs.push_back(std::move(run));
  }
  std::printf("PROFILE chapter devanagari_words=%zu\n", runs.size());
  ASSERT_GT(runs.size(), 100u);
  for (int pass = 1; pass <= 4; pass++) {
#ifdef OT_PROFILE
    ot::prof::g.reset();
#endif
    unsigned failed = 0;
    const uint64_t t0 = nowNs();
    for (const auto& run : runs) failed += !fx.shape(run);
    const uint64_t wall = nowNs() - t0;
    std::printf("CHAPTER pass=%d words=%zu failed=%u wall_ns=%llu wall_ns_per_word=%llu\n", pass, runs.size(), failed,
                static_cast<unsigned long long>(wall), static_cast<unsigned long long>(wall / runs.size()));
#ifdef OT_PROFILE
    if (pass == 4) printProfile("chapter", fx, ot::prof::g);
#endif
  }
}

}  // namespace
