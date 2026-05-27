#pragma once
#include <Print.h>

#include <functional>
#include <string>
#include <vector>

#include <SaxParser/SaxParser.h>

/**
 * Type of OPDS entry.
 */
enum class OpdsEntryType {
  NAVIGATION,  // Link to another catalog
  BOOK         // Downloadable book
};

struct OpdsAcquisitionLink {
  std::string href;
  std::string mimeType;
  std::string formatKey;
  std::string fileExtension;
};

/**
 * Represents an entry from an OPDS feed (either a navigation link or a book).
 */
struct OpdsEntry {
  OpdsEntryType type = OpdsEntryType::NAVIGATION;
  std::string title;
  std::string author;  // Only for books
  std::string href;    // Navigation URL or epub download URL
  std::string id;
  std::vector<OpdsAcquisitionLink> acquisitionLinks;
  std::string imageHref;  // Cover image URL (rel="http://opds-spec.org/image"), books only
};

// Legacy alias for backward compatibility
using OpdsBook = OpdsEntry;

/**
 * Parser for OPDS (Open Publication Distribution System) Atom feeds.
 * Uses the Expat XML parser to parse OPDS catalog entries.
 *
 * Usage:
 *   OpdsParser parser;
 *   parser.onEntryParsed = [](OpdsEntry entry) {
 *     if (entry.type == OpdsEntryType::BOOK) {
 *       // Process downloadable book
 *     } else {
 *       // Process navigation link
 *     }
 *   };
 *
 *   // Entries are emitted immediately as they are parsed from the stream.
 *   if (parser.parse(xmlData, xmlLength)) {
 *     // Parsing completed successfully
 *   }
 */
class OpdsParser final : public Print {
 public:
  OpdsParser();
  ~OpdsParser();

  // Disable copy
  const std::string& getSearchTemplate() const { return searchTemplate; }
  const std::string& getOsdUrl() const { return osdUrl; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  OpdsParser(const OpdsParser&) = delete;
  OpdsParser& operator=(const OpdsParser&) = delete;

  size_t write(uint8_t) override;
  size_t write(const uint8_t*, size_t) override;

  void flush() override;

  bool error() const;

  operator bool() { return !error(); }

  /**
   * Clear all parsed entries.
   */
  void clear();

  std::function<void(OpdsEntry)> onEntryParsed;

 private:
  static void startElement(void* userData, const char* name, const char** atts);
  static void endElement(void* userData, const char* name);
  static void characterData(void* userData, const char* s, int len);

  std::string searchTemplate;
  std::string osdUrl;
  std::string nextPageUrl;
  std::string prevPageUrl;
  static const char* findAttribute(const char** atts, const char* name);

  SaxParser saxParser_;
  OpdsEntry currentEntry;
  std::string currentText;

  // Parser state
  bool inEntry = false;
  bool inTitle = false;
  bool inAuthor = false;
  bool inAuthorName = false;
  bool inId = false;

  bool errorOccured = false;
};
