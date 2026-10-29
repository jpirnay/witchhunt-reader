// A seeded random oracle for Dictionary::resolvePrefixes. Each seed builds a dictionary that mixes ASCII
// fillers, CJK near misses that push headwords across the sample windows, 4-byte codepoints, runs of
// duplicate headwords and byte-equal synonyms, then checks 30 queries against tables built from the
// source data: the longest hit is right, every hit is a real prefix, entryCount is min(8, occurrences),
// the location is the run's first entry, and no two hits share a location.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "Dictionary.h"
#include "StringUtils.h"
#include "Utf8.h"

namespace fs = std::filesystem;

namespace {

const fs::path kSdRoot = DICT_TEST_ROOT;
const fs::path kDictRoot = kSdRoot / "dictionaries";

void writeWholeFile(const fs::path& path, const std::string& data) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

bool stardictLess(const std::string& a, const std::string& b) {
  const int c = StringUtils::asciiCaseCmp(a.c_str(), b.c_str());
  return c != 0 ? c < 0 : a < b;
}

void appendBe32(std::string& out, uint32_t v) {
  const char bytes[4] = {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8),
                         static_cast<char>(v)};
  out.append(bytes, sizeof(bytes));
}

struct H {
  std::string word;
  std::string def;
};
struct S {
  std::string word;
  size_t target;  // index into the SORTED headword list
};

std::string cp(uint32_t c) {
  std::string out;
  utf8AppendCodepoint(out, c);
  return out;
}

const std::vector<std::string> kAlpha = {cp(0x4E00), cp(0x5927), cp(0x5B66), cp(0x751F), cp(0x20000), cp(0x30FC)};

std::string randWord(std::mt19937& rng, int minCp, int maxCp) {
  std::uniform_int_distribution<int> len(minCp, maxCp);
  std::uniform_int_distribution<int> ch(0, static_cast<int>(kAlpha.size()) - 1);
  std::string w;
  const int n = len(rng);
  for (int i = 0; i < n; i++) w += kAlpha[ch(rng)];
  return w;
}

bool isPrefix(const std::string& w, const std::string& run) {
  return !w.empty() && w.size() <= run.size() && run.compare(0, w.size(), w) == 0;
}

TEST(DictionaryOracle, RandomDictionaries) {
  constexpr int seeds = 40;
  // Runs whose longest match came from the .idx or only from the .syn: both paths must be exercised.
  int idxRuns = 0;
  int synRuns = 0;

  for (int seed = 0; seed < seeds; seed++) {
    std::mt19937 rng(static_cast<uint32_t>(seed) * 2654435761u + 7u);
    std::vector<H> heads;
    std::uniform_int_distribution<int> fillers(0, 700);
    const int nAscii = fillers(rng);
    for (int i = 0; i < nAscii; i++) {
      char b[16];
      snprintf(b, sizeof(b), "a%04d", i);
      heads.push_back({b, ""});
    }
    // CJK layers: random stem + an ideograph outside the alphabet, so windows fill with near misses.
    const int nLayer = std::uniform_int_distribution<int>(0, 900)(rng);
    for (int i = 0; i < nLayer; i++) heads.push_back({randWord(rng, 0, 3) + cp(0x6000 + (i % 2000)), ""});
    // Real headwords, some repeated.
    const int nReal = std::uniform_int_distribution<int>(3, 120)(rng);
    for (int i = 0; i < nReal; i++) {
      const std::string w = randWord(rng, 1, 4);
      int reps = 1;
      const int roll = std::uniform_int_distribution<int>(0, 9)(rng);
      if (roll >= 7) reps = std::uniform_int_distribution<int>(2, 12)(rng);
      for (int r = 0; r < reps; r++) heads.push_back({w, ""});
    }
    std::stable_sort(heads.begin(), heads.end(), [](const H& a, const H& b) { return stardictLess(a.word, b.word); });
    for (size_t i = 0; i < heads.size(); i++) heads[i].def = "e" + std::to_string(i);

    std::vector<S> syns;
    const bool withSyn = std::uniform_int_distribution<int>(0, 3)(rng) != 0;
    if (withSyn) {
      const int nSyn = std::uniform_int_distribution<int>(1, 500)(rng);
      std::uniform_int_distribution<size_t> pick(0, heads.size() - 1);
      for (int i = 0; i < nSyn; i++) {
        std::string w = std::uniform_int_distribution<int>(0, 2)(rng) == 0 ? randWord(rng, 0, 3) + cp(0x7000 + i)
                                                                           : randWord(rng, 1, 4);
        const int reps = std::uniform_int_distribution<int>(0, 9)(rng) >= 8 ? 6 : 1;
        for (int r = 0; r < reps; r++) syns.push_back({w, pick(rng)});
      }
      std::stable_sort(syns.begin(), syns.end(), [](const S& a, const S& b) { return stardictLess(a.word, b.word); });
    }

    std::string idx, dict, syn;
    for (const auto& h : heads) {
      idx += h.word;
      idx += '\0';
      appendBe32(idx, static_cast<uint32_t>(dict.size()));
      appendBe32(idx, static_cast<uint32_t>(h.def.size()));
      dict += h.def;
    }
    for (const auto& s : syns) {
      syn += s.word;
      syn += '\0';
      appendBe32(syn, static_cast<uint32_t>(s.target));
    }
    const std::string folder = "Oracle" + std::to_string(seed);
    const fs::path dir = kDictRoot / folder;
    fs::remove_all(dir);
    writeWholeFile(dir / "w.idx", idx);
    writeWholeFile(dir / "w.dict", dict);
    writeWholeFile(dir / "w.ifo", "StarDict's dict ifo file\nsametypesequence=m\n");
    if (!syn.empty()) writeWholeFile(dir / "w.syn", syn);

    Dictionary d;
    ASSERT_TRUE(d.open(folder.c_str()));
    Dictionary::IndexResult ir = Dictionary::IndexResult::ReadError;
    ASSERT_TRUE(d.buildIndex(nullptr, nullptr, &ir));

    // Oracle tables.
    std::map<std::string, std::vector<size_t>> occ;  // headword -> sorted positions
    for (size_t i = 0; i < heads.size(); i++) occ[heads[i].word].push_back(i);
    std::map<std::string, std::vector<size_t>> synTargets;  // synonym word -> targets in .syn order
    for (const auto& s : syns) synTargets[s.word].push_back(s.target);

    for (int q = 0; q < 30; q++) {
      const std::string run = randWord(rng, 1, 6);
      DictPrefixMatch m;
      Dictionary::LookupResult r = Dictionary::LookupResult::ReadError;
      const bool found = d.resolvePrefixes(run.c_str(), m, &r);

      // Expected longest.
      size_t idxBest = 0;
      for (const auto& [w, pos] : occ)
        if (isPrefix(w, run)) idxBest = std::max(idxBest, w.size());
      size_t synBest = 0;
      if (idxBest < run.size())
        for (const auto& [w, t] : synTargets)
          if (isPrefix(w, run)) synBest = std::max(synBest, w.size());
      const size_t best = std::max(idxBest, synBest);
      const std::string ctx = folder + " run=" + run;
      if (best == 0) {
        EXPECT_FALSE(found) << ctx;
        EXPECT_EQ(r, Dictionary::LookupResult::NotFound) << ctx;
        EXPECT_EQ(m.count, 0) << ctx;
      } else {
        ASSERT_TRUE(found) << ctx;
        EXPECT_EQ(r, Dictionary::LookupResult::Found) << ctx;
        EXPECT_EQ(m.hits[0].runBytes, best) << ctx;
        if (idxBest == best) {
          idxRuns++;
          EXPECT_EQ(m.hits[0].headword, run.substr(0, best)) << ctx;
        } else {
          synRuns++;
          const auto& targets = synTargets[run.substr(0, best)];
          bool ok = false;
          for (size_t t : targets) ok = ok || heads[t].word == m.hits[0].headword;
          EXPECT_TRUE(ok) << ctx << " syn hit headword " << m.hits[0].headword;
        }
      }
      // Every hit well-formed.
      std::set<uint32_t> locs;
      for (uint8_t i = 0; i < m.count; i++) {
        const DictPrefixHit& h = m.hits[i];
        if (i > 0) {
          EXPECT_LT(h.runBytes, m.hits[i - 1].runBytes) << ctx;
        }
        const auto it = occ.find(h.headword);
        ASSERT_NE(it, occ.end()) << ctx << " unknown headword " << h.headword;
        const size_t first = it->second.front();
        EXPECT_EQ(h.entryCount, std::min<size_t>(8, it->second.size())) << ctx << " hit " << h.headword;
        std::string def;
        ASSERT_TRUE(d.readResolved(h.location, def)) << ctx;
        EXPECT_EQ(def, heads[first].def) << ctx << " hit " << h.headword << " not at its run's first entry";
        EXPECT_TRUE(locs.insert(h.location.offset).second) << ctx << " duplicate location";
        const std::string covered = run.substr(0, h.runBytes);
        bool viaIdx = h.headword == covered;
        bool viaSyn = false;
        const auto st = synTargets.find(covered);
        if (st != synTargets.end())
          for (size_t t : st->second) viaSyn = viaSyn || heads[t].word == h.headword;
        EXPECT_TRUE(viaIdx || viaSyn) << ctx << " hit " << h.headword << "/" << int(h.runBytes);
      }
    }
    fs::remove_all(dir);
  }
  EXPECT_GT(idxRuns, 0);
  EXPECT_GT(synRuns, 0);
}

}  // namespace
