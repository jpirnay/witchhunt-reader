#pragma once
#include <cstddef>
#include <string>

// An OpenSearch description: the document an OPDS feed names with
// <link rel="search" type="application/opensearchdescription+xml">. It lists the catalog's search
// as one <Url template="...{searchTerms}..."> per kind of result.
namespace OpenSearch {

// The search template of the description in xml, as it is written there: a relative one is the
// caller's to resolve against the description's URL. Empty when the description has none.
std::string searchTemplate(const char* xml, size_t length);

}  // namespace OpenSearch
