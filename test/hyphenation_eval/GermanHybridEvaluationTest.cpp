#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "lib/Epub/Epub/hyphenation/HyphenationCommon.h"
#include "lib/Epub/Epub/hyphenation/LanguageHyphenator.h"
#include "lib/Epub/Epub/hyphenation/LanguageRegistry.h"
#include "lib/Epub/Epub/hyphenation/generated/de_hybrid_rules.h"

#ifndef HYPHENATION_RESOURCES_DIR
#error "HYPHENATION_RESOURCES_DIR must be defined by the build system"
#endif

constexpr uint8_t kSymbolOther = 30;
constexpr uint8_t kSymbolPad = 31;

struct DanteCase {
  std::string word;
  std::vector<size_t> legal;
  std::vector<size_t> preferred;
  std::vector<size_t> undesirable;
  std::vector<size_t> ordinary;
  std::vector<size_t> compound;
  std::vector<size_t> prefix;
  std::vector<size_t> suffix;
  std::vector<size_t> uncategorized;
};

struct FalsePositiveDiagnostic {
  std::string source;
  std::string word;
  size_t position;
  std::string markedWord;
  std::string pair;
  std::string context2x2;
  std::string context3x3;
  bool undesirable;
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

std::string markedWord(const std::string& word, const size_t position) {
  const auto cps = collectCodepoints(word);

  if (position >= cps.size()) {
    return word + "|";
  }

  std::string result = word;
  result.insert(cps[position].byteOffset, "|");
  return result;
}

std::string codepointContext(const std::vector<CodepointInfo>& cps, const size_t boundary, const size_t left,
                             const size_t right) {
  std::string result;

  const size_t begin = boundary > left ? boundary - left : 0;
  const size_t end = std::min(cps.size(), boundary + right);

  for (size_t i = begin; i < end; ++i) {
    if (i == boundary) {
      result += "|";
    }

    // CodepointInfo stores the Unicode value and byte offset, not UTF-8 bytes.
    // Re-encode the codepoint only for human-readable diagnostics.
    const uint32_t cp = cps[i].value;
    if (cp <= 0x7F) {
      result.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  if (boundary == end) {
    result += "|";
  }

  return result;
}

uint8_t germanSymbol(uint32_t cp) {
  cp = toLowerLatin(cp);

  if (cp >= 'a' && cp <= 'z') {
    return static_cast<uint8_t>(cp - 'a');
  }

  switch (cp) {
    case 0x00E4:
      return 26;
    case 0x00F6:
      return 27;
    case 0x00FC:
      return 28;
    case 0x00DF:
      return 29;
    default:
      return kSymbolOther;
  }
}

bool contextKey2(const std::vector<CodepointInfo>& cps, const size_t boundary, uint32_t& outKey) {
  if (boundary < 2 || boundary + 1 >= cps.size()) {
    return false;
  }

  const uint8_t symbols[4] = {
      germanSymbol(cps[boundary - 2].value),
      germanSymbol(cps[boundary - 1].value),
      germanSymbol(cps[boundary].value),
      germanSymbol(cps[boundary + 1].value),
  };

  uint32_t key = 0;
  for (const uint8_t symbol : symbols) {
    if (symbol == kSymbolOther) {
      return false;
    }
    key = (key << 5) | symbol;
  }

  outKey = key;
  return true;
}

bool contextKey3(const std::vector<CodepointInfo>& cps, const size_t boundary, uint32_t& outKey) {
  if (boundary == 0 || boundary >= cps.size()) {
    return false;
  }

  uint32_t key = 0;

  for (int index = static_cast<int>(boundary) - 3; index < static_cast<int>(boundary) + 3; ++index) {
    const uint8_t symbol = (index < 0 || index >= static_cast<int>(cps.size()))
                               ? kSymbolPad
                               : germanSymbol(cps[static_cast<size_t>(index)].value);

    if (symbol == kSymbolOther) {
      return false;
    }

    key = (key << 5) | symbol;
  }

  outKey = key;
  return true;
}

uint16_t pairKey(const std::vector<CodepointInfo>& cps, const size_t boundary, bool& valid) {
  valid = false;

  if (boundary == 0 || boundary >= cps.size()) {
    return 0;
  }

  const uint8_t left = germanSymbol(cps[boundary - 1].value);
  const uint8_t right = germanSymbol(cps[boundary].value);

  if (left == kSymbolOther || right == kSymbolOther) {
    return 0;
  }

  valid = true;
  return static_cast<uint16_t>((static_cast<uint16_t>(left) << 5) | right);
}

bool safePair(const uint16_t key) {
  const size_t byteIndex = key >> 3;
  const uint8_t mask = static_cast<uint8_t>(1u << (key & 7u));

  return byteIndex < kGermanSafePairBits.size() && (kGermanSafePairBits[byteIndex] & mask) != 0;
}

uint32_t packed20At(const uint8_t* data, const size_t index) {
  const uint8_t* p = data + index * 3;

  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2] & 0x0F) << 16);
}

template <size_t N>
bool containsPacked20(const std::array<uint8_t, N>& data, const size_t count, const uint32_t key) {
  size_t first = 0;
  size_t last = count;

  while (first < last) {
    const size_t middle = first + (last - first) / 2;
    const uint32_t value = packed20At(data.data(), middle);

    if (value < key) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }

  return first < count && packed20At(data.data(), first) == key;
}

uint32_t packed30At(const uint8_t* data, const size_t index) {
  const uint8_t* p = data + index * 4;

  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3] & 0x3F) << 24);
}

template <size_t N>
bool containsPacked30(const std::array<uint8_t, N>& data, const size_t count, const uint32_t key) {
  size_t first = 0;
  size_t last = count;

  while (first < last) {
    const size_t middle = first + (last - first) / 2;
    const uint32_t value = packed30At(data.data(), middle);

    if (value < key) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }

  return first < count && packed30At(data.data(), first) == key;
}

bool is3x3AddContext(const std::vector<CodepointInfo>& cps, const size_t boundary) {
#if defined(GERMAN_HYBRID_HAS_3X3) && GERMAN_HYBRID_HAS_3X3
  uint32_t key = 0;
  if (!contextKey3(cps, boundary, key)) {
    return false;
  }

  return containsPacked30(kGermanAddContexts3, kGermanAddContext3Count, key);
#else
  (void)cps;
  (void)boundary;
  return false;
#endif
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

TEST(GermanHybridEval, HeldOutDantePrecisionAndRecall) {
  const auto* hyphenator = getLanguageHyphenatorForPrimaryTag("de");

  ASSERT_NE(hyphenator, nullptr);

  const auto cases = loadDanteCases();

  ASSERT_EQ(cases.size(), 5000u) << "Expected the deterministic 5,000-word DANTE fixture. "
                                    "Run scripts/update_hyphenation.sh.";

  size_t truePositives = 0;
  size_t falsePositives = 0;
  size_t falseNegatives = 0;
  size_t preferredFound = 0;
  size_t preferredTotal = 0;
  size_t undesirableUsed = 0;
  size_t exactWords = 0;

  size_t add3FalsePositives = 0;
  size_t safePairFalsePositives = 0;
  size_t safeContextFalsePositives = 0;
  size_t baseRuleFalsePositives = 0;
  size_t undecidableBaselineFalsePositives = 0;

  std::unordered_map<std::string, size_t> baselineContextCounts;

  constexpr size_t kMaxPrintedDiagnostics = 100;
  size_t diagnosticsPrinted = 0;

  for (const auto& testCase : cases) {
    auto cps = collectCodepoints(testCase.word);
    trimSurroundingPunctuationAndFootnote(cps);

    const auto actual = hyphenator->breakIndexes(cps);

    for (const size_t position : actual) {
      if (contains(testCase.legal, position)) {
        ++truePositives;
      } else {
        ++falsePositives;

        const bool isAdd3 = is3x3AddContext(cps, position);

        bool pairValid = false;
        const uint16_t pair = pairKey(cps, position, pairValid);

        uint32_t context2 = 0;
        const bool context2Valid = contextKey2(cps, position, context2);

        const bool safePairHit = pairValid && safePair(pair);

        const bool safeContextHit =
            context2Valid && containsPacked20(kGermanSafeContexts, kGermanSafeContextCount, context2);

        if (isAdd3) {
          ++add3FalsePositives;
        } else if (safePairHit) {
          ++safePairFalsePositives;
        } else if (safeContextHit) {
          ++safeContextFalsePositives;
        } else if (pairValid || context2Valid) {
          // We have a normal German character context, but neither the pair
          // nor safe 2+2 table explains the break. This is useful for spotting
          // future changes in the runtime decision path.
          ++undecidableBaselineFalsePositives;
        } else {
          ++baseRuleFalsePositives;
        }

        const std::string context4 = codepointContext(cps, position, 2, 2);

        ++baselineContextCounts[context4];

        if (diagnosticsPrinted < kMaxPrintedDiagnostics) {
          const std::string context6 = codepointContext(cps, position, 3, 3);

          std::cout << "GERMAN-HYBRID false-positive: " << markedWord(testCase.word, position) << " pos=" << position
                    << " legal={" << formatPositions(testCase.legal) << "}";

          if (contains(testCase.undesirable, position)) {
            std::cout << " DANTE-UNDESIRABLE";
          }

          std::cout << " source="
                    << (isAdd3                       ? "3x3-ADD"
                        : safePairHit                ? "2x2-SAFE-PAIR"
                        : safeContextHit             ? "2x2-SAFE-CONTEXT"
                        : pairValid || context2Valid ? "BASELINE-OTHER"
                                                     : "BASE-RULE")
                    << " pair=" << context4.substr(context4.size() >= 2 ? context4.size() - 2 : 0)
                    << " ctx4=" << context4 << " ctx6=" << context6 << '\n';

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

  const double recall = (truePositives + falseNegatives) == 0
                            ? 1.0
                            : static_cast<double>(truePositives) / static_cast<double>(truePositives + falseNegatives);

  const double preferredRecall =
      preferredTotal == 0 ? 1.0 : static_cast<double>(preferredFound) / static_cast<double>(preferredTotal);

  const double exactRate = static_cast<double>(exactWords) / static_cast<double>(cases.size());

  const size_t legalTotal = truePositives + falseNegatives;
  const size_t actualTotal = truePositives + falsePositives;

  ::testing::Test::RecordProperty("precision_percent", std::to_string(precision * 100.0));
  ::testing::Test::RecordProperty("recall_percent", std::to_string(recall * 100.0));
  ::testing::Test::RecordProperty("preferred_recall_percent", std::to_string(preferredRecall * 100.0));
  ::testing::Test::RecordProperty("exact_word_percent", std::to_string(exactRate * 100.0));
  ::testing::Test::RecordProperty("false_positives", std::to_string(falsePositives));
  ::testing::Test::RecordProperty("undesirable_used", std::to_string(undesirableUsed));
  ::testing::Test::RecordProperty("false_positive_3x3_add", std::to_string(add3FalsePositives));
  ::testing::Test::RecordProperty("false_positive_2x2_safe_pair", std::to_string(safePairFalsePositives));
  ::testing::Test::RecordProperty("false_positive_2x2_safe_context", std::to_string(safeContextFalsePositives));
  ::testing::Test::RecordProperty("false_positive_baseline_other", std::to_string(undecidableBaselineFalsePositives));
  ::testing::Test::RecordProperty("false_positive_base_rule", std::to_string(baseRuleFalsePositives));

  std::cout << "German hybrid DANTE:"
            << " cases=" << cases.size() << " legal-breaks=" << legalTotal << " actual-breaks=" << actualTotal
            << " precision=" << precision * 100.0 << "%"
            << " recall=" << recall * 100.0 << "%"
            << " preferred-recall=" << preferredRecall * 100.0 << "%"
            << " exact=" << exactRate * 100.0 << "%"
            << " false-positive-breaks=" << falsePositives << " undesirable-used=" << undesirableUsed << '\n';

  std::cout << "German hybrid false-positive source breakdown:"
            << " 3x3-ADD=" << add3FalsePositives << " 2x2-SAFE-PAIR=" << safePairFalsePositives
            << " 2x2-SAFE-CONTEXT=" << safeContextFalsePositives
            << " BASELINE-OTHER=" << undecidableBaselineFalsePositives << " BASE-RULE=" << baseRuleFalsePositives
            << '\n';

  // Print repeated contexts. This is intentionally a diagnostic only; the
  // correctness gate remains based on aggregate precision/recall.
  std::vector<std::pair<std::string, size_t>> repeatedContexts;

  for (const auto& [context, count] : baselineContextCounts) {
    if (count >= 2) {
      repeatedContexts.emplace_back(context, count);
    }
  }

  std::sort(repeatedContexts.begin(), repeatedContexts.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.second != rhs.second) {
      return lhs.second > rhs.second;
    }
    return lhs.first < rhs.first;
  });

  std::cout << "German hybrid repeated false-positive "
               "contexts (count >= 2):";

  if (repeatedContexts.empty()) {
    std::cout << " none\n";
  } else {
    std::cout << '\n';
    for (const auto& [context, count] : repeatedContexts) {
      std::cout << "  " << count << "x " << context << '\n';
    }
  }

  EXPECT_GE(precision, 0.990) << "German hybrid generated too many illegal breaks";
  EXPECT_GE(recall, 0.550) << "German hybrid became too conservative to be useful";
}
