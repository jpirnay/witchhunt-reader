#include "OpenSearchDescription.h"

#include <Logging.h>
#include <SaxParser/SaxParser.h>

#include <cstdint>
#include <cstring>

namespace OpenSearch {
namespace {
struct Description {
  std::string templateUrl;

  static void onStart(void* userData, const char* name, const char** atts) {
    if (strcmp(name, "Url") != 0 && strstr(name, ":Url") == nullptr) return;
    auto* self = static_cast<Description*>(userData);
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "template") == 0 && strstr(atts[i + 1], "{searchTerms}") != nullptr) {
        self->templateUrl = atts[i + 1];
        break;
      }
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
