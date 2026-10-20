#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// An OpenSearch description: the document an OPDS feed names with
// <link rel="search" type="application/opensearchdescription+xml">. It lists the catalog's search
// as one <Url template="...{searchTerms}..."> per kind of result.
namespace OpenSearch {

// Which of several search templates the browser searches with. It reads Atom feeds only, so it is
// the first for an OPDS catalog feed (application/atom+xml;profile=opds-catalog), else the first for
// any other Atom feed, else the first that names no type; never one for an HTML page, JSON (a search
// box's suggestions), RSS or any other type: every search would end in an error. One rule for both
// places a catalog names its search: a feed's own <link rel="search"> templates (OpdsParser) and an
// OpenSearch description's <Url> templates (searchTemplate() below).
class TemplatePicker {
 public:
  // A template, ignored unless it holds {searchTerms}, and its media type (nullptr or "" for none).
  void offer(const char* templateUrl, const char* type);
  // The template picked so far; "" when none would serve, and the catalog then offers no search.
  const std::string& best() const { return best_; }
  void clear() {
    best_.clear();
    rank_ = 0;
  }

 private:
  std::string best_;
  uint8_t rank_ = 0;
};

// The search template of the description in xml, as it is written there: a relative one is the
// caller's to resolve against the description's URL. The pick is TemplatePicker's, among the <Url>
// templates whose rel is "results" (the default). Empty when nothing is left.
std::string searchTemplate(const char* xml, size_t length);

}  // namespace OpenSearch
