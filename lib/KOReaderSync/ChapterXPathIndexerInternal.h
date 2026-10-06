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
// Block-level tags, as crengine renders them (CSS display above inline). One list for all three
// uses: R1's parent, R3's siblings, and which text gets a text point.
bool isBlockTag(const std::string& tag);

size_t countVisibleBytes(const char* text, int len);
// Codepoints of one character-data chunk as crengine stores them (R2): with `collapse`, a run of
// space/CR/LF/TAB is ONE codepoint, and a run continuing from the previous chunk (`lastWasSpace`
// in) adds none; without it every codepoint counts. Entity references count by their expansion.
// `lastWasSpace` leaves holding the chunk's final state.
size_t collapsedCodepoints(const char* text, int len, bool collapse, bool& lastWasSpace);
// The collapsed codepoint index, within the chunk, of the codepoint holding the chunk's
// targetVisibleByte-th (0-based) visible byte. The chunk must hold that byte; one past its last
// visible byte answers the chunk's collapsed count.
size_t collapsedCodepointAtVisibleByte(const char* text, int len, size_t targetVisibleByte, bool collapse,
                                       bool lastWasSpace);
// Visible bytes of the chunk before its k-th collapsed codepoint (k may equal the chunk's count).
size_t visibleBytesBeforeCollapsedCodepoint(const char* text, int len, size_t k, bool collapse, bool lastWasSpace);

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
