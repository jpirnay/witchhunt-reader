#include <fstream>
#include <string>

#include "lib/Epub/Epub/hyphenation/HyphenationCommon.h"
#include "lib/Epub/Epub/hyphenation/LanguageRegistry.h"

#ifndef HYPHENATION_RESOURCES_DIR
#error "HYPHENATION_RESOURCES_DIR must be defined"
#endif

int main() {
  const std::string fixture = std::string(HYPHENATION_RESOURCES_DIR) + "/russian_hyphenation_tests.txt";
  const std::string output = "build/russian_hyphenation_baseline.tsv";

  std::ifstream in(fixture);
  std::ofstream out(output);

  if (!in.is_open() || !out.is_open()) {
    return 2;
  }

  const auto* hyphenator = getLanguageHyphenatorForPrimaryTag("ru");

  if (hyphenator == nullptr) {
    return 3;
  }

  std::string line;

  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    const size_t first = line.find('|');
    const size_t second = line.find('|', first + 1);

    if (first == std::string::npos || second == std::string::npos) {
      continue;
    }

    const std::string word = line.substr(0, first);

    auto cps = collectCodepoints(word);
    trimSurroundingPunctuationAndFootnote(cps);

    const auto breaks = hyphenator->breakIndexes(cps);

    out << word << '\t';

    for (size_t i = 0; i < breaks.size(); ++i) {
      if (i != 0) {
        out << ',';
      }
      out << breaks[i];
    }

    out << '\n';
  }

  return 0;
}
