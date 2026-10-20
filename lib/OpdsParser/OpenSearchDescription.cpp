#include "OpenSearchDescription.h"

#include <Logging.h>
#include <SaxParser/SaxParser.h>
#include <strings.h>

#include <cstdint>
#include <cstring>
#include <string_view>

namespace OpenSearch {
namespace {
bool equalsIgnoreCase(const std::string_view s, const char* word) {
  return s.size() == strlen(word) && strncasecmp(s.data(), word, s.size()) == 0;
}

std::string_view trimmed(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// How well a <Url>'s results suit the browser, which reads Atom feeds only; best last. Never: a rel
// other than "results" (a search box's suggestions, ...) or a type that is not Atom (an HTML page,
// JSON, RSS), since the browser could read no answer to it. Untyped: no type, although OpenSearch
// requires one; such a template served before this rule, so it still does when nothing ranks higher.
enum class Rank : uint8_t { Never, Untyped, Atom, OpdsCatalog };

// The media type is compared without case or spaces, and a profile parameter may be quoted:
// "application/atom+xml; profile=opds-catalog; kind=acquisition" is an OPDS catalog feed.
Rank rankOf(const char* type, const char* rel) {
  if (rel != nullptr && !trimmed(rel).empty() && !equalsIgnoreCase(trimmed(rel), "results")) return Rank::Never;
  if (type == nullptr || trimmed(type).empty()) return Rank::Untyped;
  std::string_view rest{type};
  size_t semicolon = rest.find(';');
  if (!equalsIgnoreCase(trimmed(rest.substr(0, semicolon)), "application/atom+xml")) return Rank::Never;
  while (semicolon != std::string_view::npos) {
    rest.remove_prefix(semicolon + 1);
    semicolon = rest.find(';');
    const std::string_view parameter = trimmed(rest.substr(0, semicolon));
    const size_t equals = parameter.find('=');
    if (equals == std::string_view::npos || !equalsIgnoreCase(trimmed(parameter.substr(0, equals)), "profile")) {
      continue;
    }
    std::string_view value = trimmed(parameter.substr(equals + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
    if (equalsIgnoreCase(value, "opds-catalog")) return Rank::OpdsCatalog;
  }
  return Rank::Atom;
}

struct Description {
  std::string templateUrl;
  Rank rank = Rank::Never;

  static void onStart(void* userData, const char* name, const char** atts) {
    if (strcmp(name, "Url") != 0 && strstr(name, ":Url") == nullptr) return;
    auto* self = static_cast<Description*>(userData);
    const char* templateAttr = nullptr;
    const char* type = nullptr;
    const char* rel = nullptr;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "template") == 0) {
        templateAttr = atts[i + 1];
      } else if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "rel") == 0) {
        rel = atts[i + 1];
      }
    }
    if (templateAttr == nullptr || strstr(templateAttr, "{searchTerms}") == nullptr) return;
    // The first of the best: a later <Url> takes over only by ranking higher.
    const Rank rank = rankOf(type, rel);
    if (rank > self->rank) {
      self->rank = rank;
      self->templateUrl = templateAttr;
    }
  }
};
}  // namespace

std::string searchTemplate(const char* xml, const size_t length) {
  Description description;
  SaxParser parser;
  if (!parser.init(&description, Description::onStart, nullptr)) {
    LOG_ERR("OPDS", "OSD parser alloc failed");
    return {};
  }
  if (!parser.feed(reinterpret_cast<const uint8_t*>(xml), length) || !parser.finalize()) {
    LOG_ERR("OPDS", "OSD parse error at line %d: %s", parser.errorLine(), parser.errorString());
  }
  return description.templateUrl;
}

}  // namespace OpenSearch
