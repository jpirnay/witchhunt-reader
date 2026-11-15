#include "StarAnchoring.h"

#include <Arduino.h>
#include <Logging.h>

#include <string>
#include <utility>

#include "ChapterXPathIndexer.h"

namespace StarAnchoring {

namespace {

// The pull's reverse mapping, as far as a star needs it: the offset of the text the XPath names. Our own push names
// text to the codepoint (/text()[N].M); only stray text in a table row is named by its element, which an exact match
// anchors at that text. Anything else is a stand-in (an ancestor, a sibling) and lands nowhere a page starts.
std::optional<uint32_t> resolveXPath(const std::shared_ptr<Epub>& epub, const int spineIndex, const std::string& xpath,
                                     BuildArena* const scratch) {
  float intra = 0.0f;
  bool exact = false;
  uint32_t offset = 0;
  bool textPoint = false;
  const unsigned long start = millis();
  const bool found = ChapterXPathIndexer::findProgressForXPath(epub, spineIndex, xpath, intra, exact, nullptr, &offset,
                                                               &textPoint, scratch);
  const unsigned long took = millis() - start;
  if (!found || (!textPoint && !exact)) {
    LOG_INF("STAR", "Spine %d: %s did not resolve (%lu ms)", spineIndex, xpath.c_str(), took);
    return std::nullopt;
  }
  LOG_INF("STAR", "Spine %d: %s -> offset %u (%lu ms)", spineIndex, xpath.c_str(), static_cast<unsigned>(offset), took);
  return offset;
}

// The page `offset` is on: the star's own page while that still starts there (a page that shares its start with the
// next, an image before the text, stays itself), else the one the table finds.
std::optional<uint16_t> pageHolding(const Section& section, const uint16_t page, const uint32_t offset) {
  if (section.getVisibleTextOffsetForPage(page) == offset) return page;
  return section.getPageForVisibleTextOffset(offset);
}

// A section whose page numbers are final: loaded, or built to the end.
bool completeLayout(const Section& section) { return !section.hasActiveBuild() && section.layoutFingerprint() != 0; }

// The star's page number is one of `section`'s: counted in its layout, which lays the chapter out in as many pages.
bool standsIn(const StarAnchor& anchor, const Section& section) {
  return anchor.layout == section.layoutFingerprint() && anchor.pageCount == section.pageCount;
}

// Moves star `index`, whose offset is known, onto the page of `section` holding it, and counts it in that layout.
void settle(BookmarkStore& store, const size_t index, const Section& section, const int spineIndex) {
  const Bookmark& star = store.getAll()[index];
  const uint32_t offset = *knownOffset(store.getAnchors()[index]);
  const auto page = pageHolding(section, star.pageNumber, offset);
  if (!page) {
    LOG_ERR("STAR", "Spine %d: no page holds offset %u", spineIndex, static_cast<unsigned>(offset));
    store.noteMappingFailed(index);
    return;
  }
  if (*page != star.pageNumber) {
    LOG_INF("STAR", "Spine %d: star on page %u moves to page %u", spineIndex, static_cast<unsigned>(star.pageNumber),
            static_cast<unsigned>(*page));
  }
  store.movePage(index, *page, section.layoutFingerprint(), section.pageCount);
}

// A bookmark from older firmware (no anchor: layout 0, no XPath) whose chapter is shown complete for the first time:
// its page is taken as counted in this layout, and its text is named from there as a new bookmark's is (parseNext()).
// A page past the chapter's end is left as it is: this layout has no such page, and one that has it takes it later.
// True when anything changed.
bool anchorOlder(BookmarkStore& store, const size_t index, const Section& section, const int spineIndex) {
  const uint16_t page = store.getAll()[index].pageNumber;
  if (page >= section.pageCount) return false;
  const auto offset = section.getVisibleTextOffsetForPage(page);
  if (!offset) {
    store.noteMappingFailed(index);
    return true;
  }
  store.setOffset(index, *offset, Section::formatVersion());
  store.movePage(index, page, section.layoutFingerprint(), section.pageCount);
  store.noteNameOwed(index);
  LOG_INF("STAR", "Spine %d: older bookmark on page %u anchored at offset %u", spineIndex, static_cast<unsigned>(page),
          static_cast<unsigned>(*offset));
  return true;
}

}  // namespace

std::optional<uint32_t> knownOffset(const StarAnchor& anchor) {
  if (anchor.offset == StarAnchor::kNoOffset || anchor.offsetFormat != Section::formatVersion()) return std::nullopt;
  return anchor.offset;
}

bool parseOwed(const BookmarkStore& store, const Section* section, const int spineIndex) {
  const auto& stars = store.getAll();
  const auto& anchors = store.getAnchors();
  const bool settling = section != nullptr && completeLayout(*section);
  for (size_t i = 0; i < stars.size(); i++) {
    const StarAnchor& anchor = anchors[i];
    if (anchor.layout == 0 || anchor.mappingFailed) continue;
    if (anchor.xpath.empty()) {
      if (anchor.nameOwed && knownOffset(anchor)) return true;  // a new star's text to name
      continue;
    }
    if (settling && stars[i].spineIndex == spineIndex && !knownOffset(anchor) && !standsIn(anchor, *section)) {
      return true;  // counted in another layout, and only its XPath says where its text is
    }
  }
  return false;
}

StarAnchor forPage(const Section& section, const int page) {
  StarAnchor anchor;
  anchor.layout = section.layoutFingerprint();
  anchor.nameOwed = true;
  // While the build runs, its file has no offset table yet: settleKnown() takes it from there once the build is done.
  if (section.hasActiveBuild() || page < 0) return anchor;
  anchor.pageCount = section.pageCount;
  if (const auto offset = section.getVisibleTextOffsetForPage(static_cast<uint16_t>(page))) {
    anchor.offset = *offset;
    anchor.offsetFormat = Section::formatVersion();
  }
  return anchor;
}

bool settleKnown(BookmarkStore& store, const Section& section, const int spineIndex) {
  if (!completeLayout(section)) return false;
  const auto& stars = store.getAll();
  const auto& anchors = store.getAnchors();
  bool changed = false;
  for (size_t i = 0; i < stars.size(); i++) {
    const StarAnchor& anchor = anchors[i];
    if (stars[i].spineIndex != spineIndex || anchor.mappingFailed) continue;
    if (anchor.layout == 0) {
      if (anchor.xpath.empty() && anchorOlder(store, i, section, spineIndex)) changed = true;
      continue;
    }
    if (standsIn(anchor, section)) continue;
    if (!knownOffset(anchor)) {
      if (anchor.layout != section.layoutFingerprint() || !anchor.xpath.empty()) continue;  // for parseNext()
      // Starred while this layout was still being built, before the page had an offset in the table.
      if (const auto offset = section.getVisibleTextOffsetForPage(stars[i].pageNumber)) {
        store.setOffset(i, *offset, Section::formatVersion());
      } else {
        store.noteMappingFailed(i);
        changed = true;
        continue;
      }
    }
    settle(store, i, section, spineIndex);
    changed = true;
  }
  return changed;
}

Work parseNext(BookmarkStore& store, const std::shared_ptr<Epub>& epub, const Section* section, const int spineIndex,
               BuildArena* const scratch) {
  const auto& stars = store.getAll();
  const auto& anchors = store.getAnchors();

  if (section != nullptr && completeLayout(*section)) {
    for (size_t i = 0; i < stars.size(); i++) {
      const StarAnchor& anchor = anchors[i];
      if (stars[i].spineIndex != spineIndex || anchor.layout == 0 || anchor.mappingFailed) continue;
      if (standsIn(anchor, *section) || knownOffset(anchor) || anchor.xpath.empty()) continue;
      if (const auto offset = resolveXPath(epub, spineIndex, anchor.xpath, scratch)) {
        store.setOffset(i, *offset, Section::formatVersion());
        settle(store, i, *section, spineIndex);
      } else {
        store.noteMappingFailed(i);
      }
      return Work::Parsed;
    }
  }

  for (size_t i = 0; i < stars.size(); i++) {
    const StarAnchor& anchor = anchors[i];
    if (!anchor.nameOwed || anchor.mappingFailed || !knownOffset(anchor)) continue;
    store.noteNameTried(i);
    const unsigned long start = millis();
    std::string xpath =
        ChapterXPathIndexer::findXPathForVisibleOffset(epub, stars[i].spineIndex, anchor.offset, scratch);
    const unsigned long took = millis() - start;
    if (xpath.empty() || xpath.size() > bookmark_file::kMaxXPathBytes) {
      // Not a failed mapping: the offset still finds the text, so the star still settles by it.
      LOG_INF("STAR", "Spine %u: no XPath for offset %u (%lu ms)", static_cast<unsigned>(stars[i].spineIndex),
              static_cast<unsigned>(anchor.offset), took);
    } else {
      LOG_INF("STAR", "Spine %u: offset %u named %s (%lu ms)", static_cast<unsigned>(stars[i].spineIndex),
              static_cast<unsigned>(anchor.offset), xpath.c_str(), took);
      store.setXPath(i, std::move(xpath));
    }
    return Work::Parsed;
  }
  return Work::None;
}

Work step(BookmarkStore& store, const std::shared_ptr<Epub>& epub, const Section* section, const int spineIndex,
          BuildArena* const scratch) {
  if (section != nullptr && settleKnown(store, *section, spineIndex)) return Work::Cheap;
  return parseNext(store, epub, section, spineIndex, scratch);
}

std::optional<uint32_t> textOffset(BookmarkStore& store, const size_t index, const std::shared_ptr<Epub>& epub,
                                   BuildArena* const scratch) {
  if (index >= store.getAll().size()) return std::nullopt;
  const StarAnchor& anchor = store.getAnchors()[index];
  if (const auto known = knownOffset(anchor)) return known;
  if (anchor.xpath.empty() || anchor.mappingFailed) return std::nullopt;
  const auto offset = resolveXPath(epub, store.getAll()[index].spineIndex, anchor.xpath, scratch);
  if (offset) {
    store.setOffset(index, *offset, Section::formatVersion());
  } else {
    store.noteMappingFailed(index);
  }
  return offset;
}

}  // namespace StarAnchoring
