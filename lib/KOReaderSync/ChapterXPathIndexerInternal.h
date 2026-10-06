#pragma once

#include <Epub.h>
#include <SaxParser/SaxParser.h>

#include <memory>
#include <optional>
#include <string>

namespace ChapterXPathIndexerInternal {

std::string toLowerStr(std::string value);

bool isSkippableTag(const std::string& tag);
bool isWhitespaceOnly(const char* text, int len);

size_t countVisibleBytes(const char* text, int len);
size_t countUtf8Codepoints(const char* text, int len);
size_t codepointAtVisibleByte(const char* text, int len, size_t targetVisibleByte);
size_t visibleBytesBeforeCodepoint(const char* text, int len, size_t targetCodepointOffset);

std::string normalizeXPath(const std::string& input);
std::string removeIndices(const std::string& xpath);
// Out-parameter forms reuse the caller's string capacity instead of returning
// a new allocation per call. Use these in hot per-element loops where the same
// scratch string is repopulated thousands of times.
void normalizeXPath(const std::string& input, std::string& out);
void removeIndices(const std::string& xpath, std::string& out);
int pathDepth(const std::string& xpath);
bool isAncestorPath(const std::string& prefix, const std::string& path);

// Inflate the spine item straight into `saxParser`. True when the parse completed or the parser
// stopped itself; false on a missing item or a parse error.
bool streamSpine(const std::shared_ptr<Epub>& epub, int spineIndex, SaxParser& saxParser);
bool isEntityRef(const char* text, int len);
// Visible text bytes in the spine item; nullopt when it could not be read or parsed (distinct from
// a chapter with no text, which is 0), so callers can avoid caching a transient failure.
std::optional<size_t> countTotalTextBytes(const std::shared_ptr<Epub>& epub, int spineIndex);

}  // namespace ChapterXPathIndexerInternal
