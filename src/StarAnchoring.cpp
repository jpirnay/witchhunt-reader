#include "StarAnchoring.h"

#include <Arduino.h>
#include <Logging.h>

#include <string>
#include <utility>

#include "ChapterXPathIndexer.h"

namespace StarAnchoring {

namespace {

constexpr uint32_t kNoOffset = StarAnchor::kNoOffset;

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

}  // namespace

bool parseOwed(const BookmarkStore& store, const Section* section, const int spineIndex) {
  const auto& stars = store.getAll();
  const auto& anchors = store.getAnchors();
  const bool settling = section != nullptr && !section->hasActiveBuild() && section->layoutFingerprint() != 0;
  for (size_t i = 0; i < stars.size(); i++) {
    const StarAnchor& anchor = anchors[i];
    if (anchor.layout == 0 || anchor.mappingFailed) continue;
    if (anchor.xpath.empty()) {
      if (anchor.offset != kNoOffset) return true;  // a new star's text to name
      continue;
    }
    if (settling && stars[i].spineIndex == spineIndex && anchor.offset == kNoOffset &&
        (anchor.layout != section->layoutFingerprint() || anchor.pageCount != section->pageCount)) {
      return true;  // counted in another layout, and only its XPath says where its text is
    }
  }
  return false;
}

StarAnchor forPage(const Section& section, const int page) {
  StarAnchor anchor;
  anchor.layout = section.layoutFingerprint();
  anchor.pageCount = section.hasActiveBuild() ? 0 : section.pageCount;
  if (page >= 0) {
    if (const auto offset = section.getVisibleTextOffsetForPage(static_cast<uint16_t>(page))) anchor.offset = *offset;
  }
  return anchor;
}

Work step(BookmarkStore& store, const std::shared_ptr<Epub>& epub, const Section* section, const int spineIndex,
          BuildArena* const scratch) {
  Work done = Work::None;
  const auto& stars = store.getAll();
  const auto& anchors = store.getAnchors();

  if (section != nullptr && !section->hasActiveBuild() && section->layoutFingerprint() != 0) {
    const uint32_t layout = section->layoutFingerprint();
    const uint16_t pageCount = section->pageCount;
    for (size_t i = 0; i < stars.size(); i++) {
      const StarAnchor& anchor = anchors[i];
      if (stars[i].spineIndex != spineIndex || anchor.layout == 0 || anchor.mappingFailed) continue;
      if (anchor.layout == layout && anchor.pageCount == pageCount) continue;  // its page stands

      if (anchor.offset == kNoOffset) {
        if (anchor.layout == layout && anchor.xpath.empty()) {
          // Starred while this layout was still being built, before the page had an offset in the table.
          if (const auto offset = section->getVisibleTextOffsetForPage(stars[i].pageNumber)) {
            store.noteOffset(i, *offset);
          } else {
            store.noteMappingFailed(i);
            continue;
          }
        } else if (!anchor.xpath.empty() && done != Work::Parsed) {
          done = Work::Parsed;
          if (const auto offset = resolveXPath(epub, spineIndex, anchor.xpath, scratch)) {
            store.noteOffset(i, *offset);
          } else {
            store.noteMappingFailed(i);
            continue;
          }
        } else {
          continue;  // nothing to find it by, or this call has parsed once already
        }
      }

      const auto page = pageHolding(*section, stars[i].pageNumber, anchor.offset);
      if (!page) {
        LOG_ERR("STAR", "Spine %d: no page holds offset %u", spineIndex, static_cast<unsigned>(anchor.offset));
        store.noteMappingFailed(i);
        continue;
      }
      if (*page != stars[i].pageNumber) {
        LOG_INF("STAR", "Spine %d: star on page %u moves to page %u", spineIndex,
                static_cast<unsigned>(stars[i].pageNumber), static_cast<unsigned>(*page));
      }
      store.movePage(i, *page, layout, pageCount);
      if (done == Work::None) done = Work::Cheap;
    }
  }
  if (done == Work::Parsed) return done;

  for (size_t i = 0; i < stars.size(); i++) {
    const StarAnchor& anchor = anchors[i];
    if (anchor.layout == 0 || anchor.mappingFailed || !anchor.xpath.empty() || anchor.offset == kNoOffset) continue;
    const unsigned long start = millis();
    std::string xpath =
        ChapterXPathIndexer::findXPathForVisibleOffset(epub, stars[i].spineIndex, anchor.offset, scratch);
    const unsigned long took = millis() - start;
    if (xpath.empty() || xpath.size() > bookmark_file::kMaxXPathBytes) {
      LOG_INF("STAR", "Spine %u: no XPath for offset %u (%lu ms)", static_cast<unsigned>(stars[i].spineIndex),
              static_cast<unsigned>(anchor.offset), took);
      store.noteMappingFailed(i);
    } else {
      LOG_INF("STAR", "Spine %u: offset %u named %s (%lu ms)", static_cast<unsigned>(stars[i].spineIndex),
              static_cast<unsigned>(anchor.offset), xpath.c_str(), took);
      store.setXPath(i, std::move(xpath));
    }
    return Work::Parsed;
  }
  return done;
}

std::optional<uint32_t> textOffset(BookmarkStore& store, const size_t index, const std::shared_ptr<Epub>& epub,
                                   BuildArena* const scratch) {
  if (index >= store.getAll().size()) return std::nullopt;
  const StarAnchor& anchor = store.getAnchors()[index];
  if (anchor.offset != kNoOffset) return anchor.offset;
  if (anchor.xpath.empty() || anchor.mappingFailed) return std::nullopt;
  const auto offset = resolveXPath(epub, store.getAll()[index].spineIndex, anchor.xpath, scratch);
  if (offset) {
    store.noteOffset(index, *offset);
  } else {
    store.noteMappingFailed(index);
  }
  return offset;
}

}  // namespace StarAnchoring
