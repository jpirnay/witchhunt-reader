#pragma once
#include <cstddef>
#include <string>

// An OpenSearch description: the document an OPDS feed names with
// <link rel="search" type="application/opensearchdescription+xml">. It lists the catalog's search
// as one <Url template="...{searchTerms}..."> per kind of result.
namespace OpenSearch {

// The search template of the description in xml, as it is written there: a relative one is the
// caller's to resolve against the description's URL. The browser reads Atom feeds only, so it is the
// first <Url> for an OPDS catalog feed (application/atom+xml;profile=opds-catalog), else the first
// for any other Atom feed, else the first that names no type. Never one for an HTML page, JSON (a
// search box's suggestions), RSS or any other type, nor one whose rel is not "results": every
// search would end in an error. Empty when nothing is left, and the catalog then offers no search.
std::string searchTemplate(const char* xml, size_t length);

}  // namespace OpenSearch
