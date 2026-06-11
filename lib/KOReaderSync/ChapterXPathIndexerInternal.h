#pragma once

#include <Epub.h>
#include <SaxParser/SaxParser.h>

#include <memory>
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

std::string decompressToTempFile(const std::shared_ptr<Epub>& epub, int spineIndex);
bool runParse(SaxParser& saxParser, const std::string& path);
// Like runParse but skips the first seekBytes bytes before feeding data to the parser.
// Valid only when the parser is freshly created and the seek position is known to be on an XML
// boundary (e.g. the byte offset recorded at a page break).
bool runParseFromOffset(SaxParser& saxParser, const std::string& path, uint32_t seekBytes);
bool isEntityRef(const char* text, int len);
size_t countTotalTextBytes(const std::string& tmpPath);

}  // namespace ChapterXPathIndexerInternal
