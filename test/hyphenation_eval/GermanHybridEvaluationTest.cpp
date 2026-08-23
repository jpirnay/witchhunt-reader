#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lib/Epub/Epub/hyphenation/HyphenationCommon.h"
#include "lib/Epub/Epub/hyphenation/LanguageHyphenator.h"
#include "lib/Epub/Epub/hyphenation/LanguageRegistry.h"

#ifndef HYPHENATION_RESOURCES_DIR
#error "HYPHENATION_RESOURCES_DIR must be defined by the build system"
#endif

namespace {

struct DanteCase {
  std::string word;
  std::vector<size_t> legal;
  std::vector<size_t> preferred;
  std::vector<size_t> undesirable;
  std::vector<size_t> ordinary;       // -
  std::vector<size_t> compound;       // =
  std::vector<size_t> prefix;         // <
  std::vector<size_t> suffix;         // >
  std::vector<size_t> uncategorized;  // ·
};

struct RecallStats {
  size_t found = 0;
  size_t total = 0;
};

std::vector<size_t> parsePositions(const std::string& text) {
  std::vector<size_t> result;
  if (text.empty()) {
    return result;
  }

  std::istringstream stream(text);
  std::string token;
  while (std::getline(stream, token, ',')) {
    if (!token.empty()) {
      result.push_back(static_cast<size_t>(std::stoul(token)));
    }
  }
  return result;
}

// Unlike repeated std::getline(), this deliberately preserves a trailing empty
// field.  Most DANTE rows have an empty "undesirable" field.
std::vector<std::string> splitPipeFields(const std::string& line) {
  std::vector<std::string> fields;
  size_t start = 0;

  while (true) {
    const size_t separator = line.find('|', start);
    if (separator == std::string::npos) {
      fields.push_back(line.substr(start));
      break;
    }

    fields.push_back(line.substr(start, separator - start));
    start = separator + 1;
  }

  return fields;
}

std::vector<DanteCase> loadDanteCases() {
  const std::string path = std::string(HYPHENATION_RESOURCES_DIR) + "/german_dante_eval.txt";
  std::ifstream file(path);
  std::vector<DanteCase> result;

  if (!file.is_open()) {
    return result;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    // New format:
    // word|legal|preferred|undesirable|ordinary|compound|prefix|suffix|uncategorized
    const auto fields = splitPipeFields(line);
    if (fields.size() != 9) {
      continue;
    }

    result.push_back({
        fields[0],
        parsePositions(fields[1]),
        parsePositions(fields[2]),
        parsePositions(fields[3]),
        parsePositions(fields[4]),
        parsePositions(fields[5]),
        parsePositions(fields[6]),
        parsePositions(fields[7]),
        parsePositions(fields[8]),
    });
  }

  return result;
}

bool contains(const std::vector<size_t>& values, const size_t value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<size_t> hyphenate(const LanguageHyphenator& hyphenator, const std::string& word) {
  auto cps = collectCodepoints(word);
  trimSurroundingPunctuationAndFootnote(cps);
  return hyphenator.breakIndexes(cps);
}

std::string withBreakMarker(const std::string& word, const size_t position) {
  const auto cps = collectCodepoints(word);
  if (position >= cps.size()) {
    return word + "|";
  }

  std::string result = word;
  result.insert(cps[position].byteOffset, "|");
  return result;
}

std::string formatPositions(const std::vector<size_t>& positions) {
  std::ostringstream out;
  for (size_t i = 0; i < positions.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    out << positions[i];
  }
  return out.str();
}

void addRecall(RecallStats& stats, const std::vector<size_t>& expected, const std::vector<size_t>& actual) {
  stats.total += expected.size();
  for (const size_t position : expected) {
    if (contains(actual, position)) {
      ++stats.found;
    }
  }
}

double recall(const RecallStats& stats) {
  return stats.total == 0 ? 1.0 : static_cast<double>(stats.found) / static_cast<double>(stats.total);
}

void recordRecallProperty(const char* name, const RecallStats& stats) {
  ::testing::Test::RecordProperty(name, std::to_string(recall(stats) * 100.0));
}

}  // namespace

TEST(GermanHybridEval, HeldOutDantePrecisionAndRecall) {
  const auto* hyphenator = getLanguageHyphenatorForPrimaryTag("de");
  ASSERT_NE(hyphenator, nullptr);

  const auto cases = loadDanteCases();
  ASSERT_EQ(cases.size(), 5000u) << "Expected the regenerated 5000-word DANTE fixture. "
                                    "Run scripts/update_hyphenation.sh.";

  size_t truePositives = 0;
  size_t falsePositives = 0;
  size_t falseNegatives = 0;
  size_t preferredFound = 0;
  size_t preferredTotal = 0;
  size_t undesirableUsed = 0;
  size_t exactWords = 0;

  RecallStats ordinaryStats;
  RecallStats compoundStats;
  RecallStats prefixStats;
  RecallStats suffixStats;
  RecallStats uncategorizedStats;

  constexpr size_t kMaxPrintedDiagnostics = 100;
  size_t diagnosticsPrinted = 0;

  for (const auto& testCase : cases) {
    const auto actual = hyphenate(*hyphenator, testCase.word);

    for (const size_t position : actual) {
      if (contains(testCase.legal, position)) {
        ++truePositives;
      } else {
        ++falsePositives;

        if (diagnosticsPrinted < kMaxPrintedDiagnostics) {
          std::cout << "GERMAN-HYBRID false-positive: " << withBreakMarker(testCase.word, position)
                    << " pos=" << position << " legal={" << formatPositions(testCase.legal) << "}";

          if (contains(testCase.undesirable, position)) {
            std::cout << " DANTE-UNDESIRABLE";
          }

          std::cout << '\n';
          ++diagnosticsPrinted;
        }
      }

      if (contains(testCase.undesirable, position)) {
        ++undesirableUsed;
      }
    }

    for (const size_t position : testCase.legal) {
      if (!contains(actual, position)) {
        ++falseNegatives;
      }
    }

    for (const size_t position : testCase.preferred) {
      ++preferredTotal;
      if (contains(actual, position)) {
        ++preferredFound;
      }
    }

    addRecall(ordinaryStats, testCase.ordinary, actual);
    addRecall(compoundStats, testCase.compound, actual);
    addRecall(prefixStats, testCase.prefix, actual);
    addRecall(suffixStats, testCase.suffix, actual);
    addRecall(uncategorizedStats, testCase.uncategorized, actual);

    std::vector<size_t> sortedActual = actual;
    std::vector<size_t> sortedLegal = testCase.legal;
    std::sort(sortedActual.begin(), sortedActual.end());
    std::sort(sortedLegal.begin(), sortedLegal.end());
    if (sortedActual == sortedLegal) {
      ++exactWords;
    }
  }

  const double precision =
      (truePositives + falsePositives) == 0
          ? 1.0
          : static_cast<double>(truePositives) / static_cast<double>(truePositives + falsePositives);

  const double overallRecall =
      (truePositives + falseNegatives) == 0
          ? 1.0
          : static_cast<double>(truePositives) / static_cast<double>(truePositives + falseNegatives);

  const double preferredRecall =
      preferredTotal == 0 ? 1.0 : static_cast<double>(preferredFound) / static_cast<double>(preferredTotal);

  const double exactRate = static_cast<double>(exactWords) / static_cast<double>(cases.size());

  const size_t legalTotal = truePositives + falseNegatives;
  const size_t actualTotal = truePositives + falsePositives;

  ::testing::Test::RecordProperty("precision_percent", std::to_string(precision * 100.0));
  ::testing::Test::RecordProperty("recall_percent", std::to_string(overallRecall * 100.0));
  ::testing::Test::RecordProperty("preferred_recall_percent", std::to_string(preferredRecall * 100.0));
  ::testing::Test::RecordProperty("exact_word_percent", std::to_string(exactRate * 100.0));
  ::testing::Test::RecordProperty("false_positives", std::to_string(falsePositives));
  ::testing::Test::RecordProperty("undesirable_used", std::to_string(undesirableUsed));
  ::testing::Test::RecordProperty("test_cases", std::to_string(cases.size()));

  recordRecallProperty("ordinary_recall_percent", ordinaryStats);
  recordRecallProperty("compound_recall_percent", compoundStats);
  recordRecallProperty("prefix_recall_percent", prefixStats);
  recordRecallProperty("suffix_recall_percent", suffixStats);
  recordRecallProperty("uncategorized_recall_percent", uncategorizedStats);

  std::cout << "German hybrid DANTE:"
            << " cases=" << cases.size() << " legal-breaks=" << legalTotal << " actual-breaks=" << actualTotal
            << " precision=" << precision * 100.0 << "%"
            << " recall=" << overallRecall * 100.0 << "%"
            << " preferred-recall=" << preferredRecall * 100.0 << "%"
            << " exact=" << exactRate * 100.0 << "%"
            << " false-positive-breaks=" << falsePositives << " undesirable-used=" << undesirableUsed << '\n';

  // A boundary may carry more than one DANTE morphology marker (for example
  // mixed markers such as "<=").  Category totals therefore intentionally may
  // overlap; these are diagnostic marker recalls, not a partition of "legal".
  std::cout << "German hybrid DANTE marker recall:"
            << " ordinary(-)=" << ordinaryStats.found << "/" << ordinaryStats.total << " ("
            << recall(ordinaryStats) * 100.0 << "%)"
            << " compound(=)=" << compoundStats.found << "/" << compoundStats.total << " ("
            << recall(compoundStats) * 100.0 << "%)"
            << " prefix(<)=" << prefixStats.found << "/" << prefixStats.total << " (" << recall(prefixStats) * 100.0
            << "%)"
            << " suffix(>)=" << suffixStats.found << "/" << suffixStats.total << " (" << recall(suffixStats) * 100.0
            << "%)"
            << " uncategorized(·)=" << uncategorizedStats.found << "/" << uncategorizedStats.total << " ("
            << recall(uncategorizedStats) * 100.0 << "%)" << '\n';

  // Conservative production gates.  Do not weaken these merely to improve
  // recall: a false hyphen is more damaging than a missed optional break.
  EXPECT_GE(precision, 0.990) << "German hybrid generated too many illegal breaks";
  EXPECT_GE(overallRecall, 0.550) << "German hybrid became too conservative to be useful";
}
