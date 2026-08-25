#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "lib/Epub/Epub/hyphenation/HyphenationCommon.h"
#include "lib/Epub/Epub/hyphenation/LanguageRegistry.h"

#ifndef HYPHENATION_RESOURCES_DIR
#error "HYPHENATION_RESOURCES_DIR must be defined by the build system"
#endif

namespace {

constexpr size_t kMinPrefix = 2;
constexpr size_t kMinSuffix = 2;

struct TestCase {
  std::string word;
  std::vector<size_t> expected;
  int frequency = 0;
};

struct Metrics {
  size_t tp = 0;
  size_t fp = 0;
  size_t fn = 0;
  size_t undesirable = 0;

  double precision() const {
    const size_t d = tp + fp;
    return d ? static_cast<double>(tp) / d : 1.0;
  }

  double recall() const {
    const size_t d = tp + fn;
    return d ? static_cast<double>(tp) / d : 1.0;
  }

  double f1() const {
    const double p = precision();
    const double r = recall();
    return (p + r) > 0.0 ? 2.0 * p * r / (p + r) : 0.0;
  }
};

struct ContextStat {
  size_t good = 0;
  size_t bad = 0;
};

struct RussianSplit {
  std::vector<TestCase> train;
  std::vector<TestCase> dev;
  std::vector<TestCase> test;
};

std::vector<size_t> expectedPositions(const std::string& annotated) {
  std::vector<size_t> positions;
  size_t codepointIndex = 0;
  size_t i = 0;

  while (i < annotated.size()) {
    const unsigned char byte = static_cast<unsigned char>(annotated[i]);

    if (byte == '=') {
      positions.push_back(codepointIndex);
      ++i;
      continue;
    }

    // Minimal UTF-8 decoder for fixture parsing. We only need to advance by
    // one Unicode code point; validation of the actual Russian alphabet is
    // done by the production hyphenator.
    size_t width = 1;
    if ((byte & 0x80u) == 0) {
      width = 1;
    } else if ((byte & 0xE0u) == 0xC0u) {
      width = 2;
    } else if ((byte & 0xF0u) == 0xE0u) {
      width = 3;
    } else if ((byte & 0xF8u) == 0xF0u) {
      width = 4;
    }

    if (i + width > annotated.size()) {
      width = 1;
    }

    i += width;
    ++codepointIndex;
  }

  return positions;
}

std::vector<TestCase> loadCases() {
  const std::string path = std::string(HYPHENATION_RESOURCES_DIR) + "/russian_hyphenation_tests.txt";

  std::ifstream file(path);
  std::vector<TestCase> result;

  if (!file.is_open()) {
    return result;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::istringstream iss(line);
    std::string word;
    std::string annotated;
    std::string frequency;

    if (!std::getline(iss, word, '|') || !std::getline(iss, annotated, '|') || !std::getline(iss, frequency, '|')) {
      continue;
    }

    result.push_back({
        word,
        expectedPositions(annotated),
        std::stoi(frequency),
    });
  }

  return result;
}

// Deterministic split. This is a SCREENING split over the existing 5,000-word
// fixture, not the final evaluation methodology. It prevents the experiment
// from tuning on the complete fixture and then claiming independent quality.
uint32_t fnv1a(const std::string& s) {
  uint32_t hash = 2166136261u;
  for (const unsigned char c : s) {
    hash ^= c;
    hash *= 16777619u;
  }
  return hash;
}

RussianSplit splitCases(const std::vector<TestCase>& cases) {
  RussianSplit split;

  for (const TestCase& tc : cases) {
    const unsigned bucket = static_cast<unsigned>(fnv1a(tc.word) % 10u);

    if (bucket < 7u) {
      split.train.push_back(tc);
    } else if (bucket == 7u) {
      split.dev.push_back(tc);
    } else {
      split.test.push_back(tc);
    }
  }

  return split;
}

std::vector<size_t> baselineBreaks(const TestCase& tc) {
  const auto* hyphenator = getLanguageHyphenatorForPrimaryTag("ru");
  if (hyphenator == nullptr) {
    return {};
  }

  auto cps = collectCodepoints(tc.word);
  trimSurroundingPunctuationAndFootnote(cps);
  return hyphenator->breakIndexes(cps);
}

uint8_t symbolFor(uint32_t cp) {
  cp = toLowerCyrillic(cp);

  // Russian alphabet ordered as:
  // а..е, ё, ж..я
  if (cp >= 0x0430 && cp <= 0x0435) {
    return static_cast<uint8_t>(cp - 0x0430);
  }

  if (cp == 0x0451) {
    return 6;
  }

  if (cp >= 0x0436 && cp <= 0x044F) {
    return static_cast<uint8_t>(7 + cp - 0x0436);
  }

  return 31;  // OTHER
}

bool encodeContext(const std::vector<CodepointInfo>& cps, size_t boundary, size_t left, size_t right, uint32_t& key) {
  if (boundary < left || boundary + right > cps.size()) {
    return false;
  }

  key = 0;
  for (size_t i = boundary - left; i < boundary + right; ++i) {
    const uint8_t symbol = symbolFor(cps[i].value);

    if (symbol >= 31) {
      return false;
    }

    key = (key << 5) | symbol;
  }

  return true;
}

uint32_t context2x2(const std::vector<CodepointInfo>& cps, size_t boundary, bool& valid) {
  uint32_t key = 0;
  valid = encodeContext(cps, boundary, 2, 2, key);
  return valid ? key : 0;
}

uint32_t context3x3(const std::vector<CodepointInfo>& cps, size_t boundary, bool& valid) {
  uint32_t key = 0;
  valid = encodeContext(cps, boundary, 3, 3, key);
  return valid ? key : 0;
}

template <size_t N>
std::unordered_set<uint32_t> learn3x3Adds(const std::vector<TestCase>& train,
                                          const std::unordered_map<std::string, std::vector<size_t>>& baseline,
                                          size_t minSupport, double minPrecision) {
  (void)N;

  std::unordered_map<uint32_t, ContextStat> stats;

  for (const auto& tc : train) {
    const auto baselineIt = baseline.find(tc.word);
    if (baselineIt == baseline.end()) {
      continue;
    }

    const auto cps = collectCodepoints(tc.word);
    const std::unordered_set<size_t> legal(tc.expected.begin(), tc.expected.end());

    for (size_t boundary = kMinPrefix; boundary + kMinSuffix <= cps.size(); ++boundary) {
      if (std::find(baselineIt->second.begin(), baselineIt->second.end(), boundary) != baselineIt->second.end()) {
        continue;
      }

      bool valid = false;
      const uint32_t key = context3x3(cps, boundary, valid);

      if (!valid) {
        continue;
      }

      auto& stat = stats[key];
      if (legal.count(boundary) != 0) {
        ++stat.good;
      } else {
        ++stat.bad;
      }
    }
  }

  std::vector<std::pair<uint32_t, ContextStat>> ordered;
  ordered.reserve(stats.size());

  for (const auto& item : stats) {
    const auto& stat = item.second;
    const size_t total = stat.good + stat.bad;

    if (total >= minSupport && static_cast<double>(stat.good) / total >= minPrecision) {
      ordered.push_back(item);
    }
  }

  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
    const size_t ta = a.second.good + a.second.bad;
    const size_t tb = b.second.good + b.second.bad;
    const double pa = static_cast<double>(a.second.good) / ta;
    const double pb = static_cast<double>(b.second.good) / tb;

    if (pa != pb) {
      return pa > pb;
    }
    return a.second.good > b.second.good;
  });

  std::unordered_set<uint32_t> result;
  for (const auto& [key, stat] : ordered) {
    result.insert(key);
  }

  return result;
}

std::vector<uint32_t> rankedContextCandidates(const std::vector<TestCase>& words,
                                              const std::unordered_map<std::string, std::vector<size_t>>& baseline,
                                              size_t left, size_t minSupport, double minPrecision) {
  std::unordered_map<uint32_t, ContextStat> stats;

  for (const auto& tc : words) {
    const auto baselineIt = baseline.find(tc.word);
    if (baselineIt == baseline.end()) {
      continue;
    }

    const auto cps = collectCodepoints(tc.word);
    const std::unordered_set<size_t> legal(tc.expected.begin(), tc.expected.end());

    for (size_t boundary = kMinPrefix; boundary + kMinSuffix <= cps.size(); ++boundary) {
      if (std::find(baselineIt->second.begin(), baselineIt->second.end(), boundary) != baselineIt->second.end()) {
        continue;
      }

      bool valid = false;
      uint32_t key = 0;

      if (left == 2) {
        key = context2x2(cps, boundary, valid);
      } else {
        key = context3x3(cps, boundary, valid);
      }

      if (!valid) {
        continue;
      }

      auto& stat = stats[key];
      if (legal.count(boundary) != 0) {
        ++stat.good;
      } else {
        ++stat.bad;
      }
    }
  }

  std::vector<std::pair<uint32_t, ContextStat>> ordered;
  for (const auto& item : stats) {
    const auto& stat = item.second;
    const size_t total = stat.good + stat.bad;
    if (total >= minSupport && static_cast<double>(stat.good) / total >= minPrecision) {
      ordered.push_back(item);
    }
  }

  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
    const size_t ta = a.second.good + a.second.bad;
    const size_t tb = b.second.good + b.second.bad;
    const double pa = static_cast<double>(a.second.good) / ta;
    const double pb = static_cast<double>(b.second.good) / tb;

    if (pa != pb) {
      return pa > pb;
    }
    if (a.second.good != b.second.good) {
      return a.second.good > b.second.good;
    }
    return a.first < b.first;
  });

  std::vector<uint32_t> result;
  result.reserve(ordered.size());
  for (const auto& [key, stat] : ordered) {
    result.push_back(key);
  }

  return result;
}

std::unordered_set<uint32_t> contextSet(const std::vector<TestCase>& words,
                                        const std::unordered_map<std::string, std::vector<size_t>>& baseline,
                                        size_t left, size_t minSupport, double minPrecision) {
  std::unordered_map<uint32_t, ContextStat> stats;

  for (const auto& tc : words) {
    const auto baselineIt = baseline.find(tc.word);
    if (baselineIt == baseline.end()) {
      continue;
    }

    const auto cps = collectCodepoints(tc.word);
    const std::unordered_set<size_t> legal(tc.expected.begin(), tc.expected.end());

    for (size_t boundary = kMinPrefix; boundary + kMinSuffix <= cps.size(); ++boundary) {
      if (std::find(baselineIt->second.begin(), baselineIt->second.end(), boundary) != baselineIt->second.end()) {
        continue;
      }

      bool valid = false;
      uint32_t key = 0;

      if (left == 2) {
        key = context2x2(cps, boundary, valid);
      } else {
        key = context3x3(cps, boundary, valid);
      }

      if (!valid) {
        continue;
      }

      auto& stat = stats[key];
      if (legal.count(boundary) != 0) {
        ++stat.good;
      } else {
        ++stat.bad;
      }
    }
  }

  std::vector<std::pair<uint32_t, ContextStat>> ordered;

  for (const auto& item : stats) {
    const auto& stat = item.second;
    const size_t total = stat.good + stat.bad;

    if (total >= minSupport && static_cast<double>(stat.good) / total >= minPrecision) {
      ordered.push_back(item);
    }
  }

  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
    const size_t ta = a.second.good + a.second.bad;
    const size_t tb = b.second.good + b.second.bad;
    const double pa = static_cast<double>(a.second.good) / ta;
    const double pb = static_cast<double>(b.second.good) / tb;

    if (pa != pb) {
      return pa > pb;
    }
    return a.second.good > b.second.good;
  });

  std::unordered_set<uint32_t> result;
  for (const auto& [key, stat] : ordered) {
    result.insert(key);
  }

  return result;
}

Metrics evaluate(const std::vector<TestCase>& words,
                 const std::unordered_map<std::string, std::vector<size_t>>& baseline,
                 const std::unordered_set<uint32_t>& add2x2, const std::unordered_set<uint32_t>& add3x3) {
  Metrics result;

  for (const auto& tc : words) {
    auto cps = collectCodepoints(tc.word);
    const auto baselineIt = baseline.find(tc.word);
    if (baselineIt == baseline.end()) {
      continue;
    }

    std::unordered_set<size_t> actual(baselineIt->second.begin(), baselineIt->second.end());

    for (size_t boundary = kMinPrefix; boundary + kMinSuffix <= cps.size(); ++boundary) {
      if (actual.count(boundary) != 0) {
        continue;
      }

      bool valid2 = false;
      const uint32_t key2 = context2x2(cps, boundary, valid2);

      if (valid2 && add2x2.count(key2) != 0) {
        actual.insert(boundary);
        continue;
      }

      bool valid3 = false;
      const uint32_t key3 = context3x3(cps, boundary, valid3);

      if (valid3 && add3x3.count(key3) != 0) {
        actual.insert(boundary);
      }
    }

    const std::unordered_set<size_t> expected(tc.expected.begin(), tc.expected.end());

    for (size_t position : actual) {
      if (expected.count(position) != 0) {
        ++result.tp;
      } else {
        ++result.fp;
      }
    }

    for (size_t position : expected) {
      if (actual.count(position) == 0) {
        ++result.fn;
      }
    }
  }

  return result;
}

void printMetrics(const char* name, const Metrics& m) {
  std::cout << name << ": P=" << m.precision() * 100.0 << "% R=" << m.recall() * 100.0 << "% F1=" << m.f1() * 100.0
            << "% TP=" << m.tp << " FP=" << m.fp << " FN=" << m.fn << '\n';
}

}  // namespace

TEST(RussianCompactExperiment, TwoByTwoThreeByThreeScreening) {
  const auto cases = loadCases();

  ASSERT_EQ(cases.size(), 5000u) << "Expected the repository's 5,000-word Russian fixture.";

  const auto split = splitCases(cases);

  std::cout << "RUSSIAN EXPERIMENT SOURCE: V4" << std::endl;
  // The deterministic hash split is intentionally simple and does not
  // guarantee exact 70/15/15 proportions.  Only require that all sets are
  // non-trivial; the reported counts make the actual split explicit.
  ASSERT_GT(split.train.size(), 1000u);
  ASSERT_GT(split.dev.size(), 100u);
  ASSERT_GT(split.test.size(), 100u);

  std::cout << "Russian split: train=" << split.train.size() << " dev=" << split.dev.size()
            << " test=" << split.test.size() << '\n';

  std::unordered_map<std::string, std::vector<size_t>> baseline;

  for (const auto& tc : cases) {
    baseline.emplace(tc.word, baselineBreaks(tc));
  }

  const Metrics baselineDev = evaluate(split.dev, baseline, {}, {});

  const Metrics baselineTest = evaluate(split.test, baseline, {}, {});

  printMetrics("Russian baseline Dev", baselineDev);
  printMetrics("Russian baseline Test", baselineTest);

  const auto safe2x2 = contextSet(split.train, baseline, 2, 3, 0.999);

  const auto add3x3Candidates = rankedContextCandidates(split.train, baseline, 3, 3, 0.995);

  std::cout << "2x2 safe/add contexts: " << safe2x2.size() << '\n';

  std::cout << "3x3 candidate ADD contexts: " << add3x3Candidates.size() << '\n';

  // Screening frontier: candidates are ranked by training precision and then
  // by legal support. This is still NOT the production optimizer, but unlike
  // arbitrary numeric key ordering it gives a meaningful first look at how
  // much compact 3x3 data buys us.
  const size_t frontier[] = {0, 50, 100, 250, 500, 1000};

  for (const size_t maxAdds : frontier) {
    std::unordered_set<uint32_t> selected;

    for (size_t i = 0; i < std::min(maxAdds, add3x3Candidates.size()); ++i) {
      selected.insert(add3x3Candidates[i]);
    }

    const Metrics dev = evaluate(split.dev, baseline, safe2x2, selected);

    const Metrics test = evaluate(split.test, baseline, safe2x2, selected);

    const size_t estimatedBytes = (safe2x2.size() * 3u) + (selected.size() * 4u);

    std::cout << "frontier ADD3=" << maxAdds << " payload~" << estimatedBytes << " bytes\n";

    printMetrics("  Dev", dev);
    printMetrics("  Test", test);
  }
}
