#include "ChapterHtmlSlimParser.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SaxParser/SaxParser.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cctype>

#include "../../Epub.h"
#include "../Page.h"
#include "../converters/ImageDecoderFactory.h"
#include "../converters/ImageToFramebufferDecoder.h"
#include "../htmlEntities.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Size thresholds (bytes of XHTML) controlling indexing popup behavior.
// Each progress callback costs ~640ms of e-ink refresh, so we trade granularity off
// against indexing time based on expected duration.
//   < 15KB:  no popup at all - indexing finishes faster than the popup would draw
//   < 30KB:  popup only (one refresh up-front, no mid-parse updates)
//   < 80KB:  popup + one heartbeat at 50%
//   >= 80KB: popup + ticks at 25/50/75%
constexpr size_t MIN_SIZE_FOR_POPUP = 15 * 1024;
constexpr size_t SIZE_FOR_PROGRESS_HEARTBEAT = 30 * 1024;
constexpr size_t SIZE_FOR_PROGRESS_FINE = 80 * 1024;
constexpr size_t MIN_FREE_HEAP_FOR_INDEXING_POPUP = 32 * 1024;
constexpr size_t MIN_CONTIG_HEAP_FOR_INDEXING_POPUP = 12 * 1024;

constexpr size_t PARSE_BUFFER_SIZE = 1024;
// Image extraction is now deferred to render time (ImageBlock::ensureExtracted).
// No heap guard needed at parse time — only a ZIP header read (~4 KB buffer on stack in
// getDimensionsFromZipEntry) happens during createSectionFile.

#ifndef EHP_TEXT_LAYOUT_SOFT_MIN_FREE_HEAP
#define EHP_TEXT_LAYOUT_SOFT_MIN_FREE_HEAP (18 * 1024)
#endif

#ifndef EHP_TEXT_LAYOUT_SOFT_MIN_MAX_ALLOC
#define EHP_TEXT_LAYOUT_SOFT_MIN_MAX_ALLOC (12 * 1024)
#endif

#ifndef EHP_TEXT_LAYOUT_HARD_MIN_FREE_HEAP
#define EHP_TEXT_LAYOUT_HARD_MIN_FREE_HEAP (9 * 1024)
#endif

#ifndef EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC
#define EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC (6 * 1024)
#endif

constexpr size_t MIN_FREE_HEAP_FOR_TEXT_LAYOUT = EHP_TEXT_LAYOUT_SOFT_MIN_FREE_HEAP;
constexpr size_t MIN_MAX_ALLOC_FOR_TEXT_LAYOUT = EHP_TEXT_LAYOUT_SOFT_MIN_MAX_ALLOC;
constexpr size_t MIN_FREE_HEAP_FOR_TEXT_LAYOUT_HARD = EHP_TEXT_LAYOUT_HARD_MIN_FREE_HEAP;
constexpr size_t MIN_MAX_ALLOC_FOR_TEXT_LAYOUT_HARD = EHP_TEXT_LAYOUT_HARD_MIN_MAX_ALLOC;

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote", "pre"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr int NUM_UNDERLINE_TAGS = sizeof(UNDERLINE_TAGS) / sizeof(UNDERLINE_TAGS[0]);

const char* STRIKETHROUGH_TAGS[] = {"s", "del", "strike"};
constexpr int NUM_STRIKETHROUGH_TAGS = sizeof(STRIKETHROUGH_TAGS) / sizeof(STRIKETHROUGH_TAGS[0]);

const char* IMAGE_TAGS[] = {"img", "image"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// Returns true if the trailing UTF-8 codepoint in [buf, buf+len) is a dash that allows
// a line break opportunity after it. Inline-tag boundaries like "gone—<i>Umbriel</i>"
// would otherwise glue the dash to the following word via nextWordContinues, making the
// dash unbreakable; callers use this to skip setting that flag when the buffered text
// already ends at a natural break point.
//
// Soft hyphen (U+00AD) and non-breaking hyphen (U+2011) are intentionally excluded:
// soft hyphen is invisible (a hyphenation hint) and non-breaking hyphen forbids breaks
// by definition. Minus sign (U+2212) is excluded because it's mathematical, not a word
// separator.
bool bufferEndsWithBreakableDash(const char* buf, const int len) {
  if (len <= 0) return false;
  int start = len - 1;
  while (start > 0 && (static_cast<uint8_t>(buf[start]) & 0xC0) == 0x80) {
    --start;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(buf + start);
  const uint32_t cp = utf8NextCodepoint(&ptr);
  switch (cp) {
    case '-':
    case 0x2010:  // HYPHEN
    case 0x2012:  // FIGURE DASH
    case 0x2013:  // EN DASH
    case 0x2014:  // EM DASH
    case 0x2015:  // HORIZONTAL BAR
    case 0x2E3A:  // TWO-EM DASH
    case 0x2E3B:  // THREE-EM DASH
      return true;
    default:
      return false;
  }
}

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, NUM_HEADER_TAGS) || matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS);
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

std::string buildTextBlockPreview(const std::shared_ptr<TextBlock>& line, const size_t maxLen = 120) {
  if (!line) {
    return {};
  }

  std::string preview;
  const auto& words = line->getWords();
  for (size_t i = 0; i < words.size(); ++i) {
    if (i > 0) {
      preview.push_back(' ');
    }
    preview += words[i];
    if (preview.size() >= maxLen) {
      preview.resize(maxLen);
      preview += "...";
      break;
    }
  }
  return preview;
}

// Calibre sometimes injects empty <p style="margin:0; border:0; height:0">...</p>
// spacers inside running prose. Keep them as paragraph boundaries, but ignore
// their inner text payload (usually NBSP) to avoid no-break-space glue artifacts.
bool isZeroHeightSpacerParagraph(const char* name, const std::string& styleAttr) {
  if (strcmp(name, "p") != 0 || styleAttr.empty()) {
    return false;
  }

  std::string normalized;
  normalized.reserve(styleAttr.size());
  for (const char ch : styleAttr) {
    if (!isWhitespace(ch)) {
      normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
  }

  const bool hasZeroHeight = normalized.find("height:0") != std::string::npos;
  const bool hasZeroMargin = normalized.find("margin:0") != std::string::npos;
  const bool hasZeroBorder = normalized.find("border:0") != std::string::npos;
  return hasZeroHeight && hasZeroMargin && hasZeroBorder;
}

// Update effective bold/italic/underline based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveUnderline = currentCssStyle.hasTextDecoration() && (static_cast<uint8_t>(currentCssStyle.textDecoration) &
                                                               static_cast<uint8_t>(CssTextDecoration::Underline)) != 0;
  effectiveStrikethrough =
      currentCssStyle.hasTextDecoration() && (static_cast<uint8_t>(currentCssStyle.textDecoration) &
                                              static_cast<uint8_t>(CssTextDecoration::LineThrough)) != 0;
  effectiveSup = false;
  effectiveSub = false;
  effectiveInlineMarginLeft = 0;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    if (entry.hasUnderline) {
      effectiveUnderline = entry.underline;
    }
    if (entry.hasStrikethrough) {
      effectiveStrikethrough = entry.strikethrough;
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
    if (entry.hasMarginLeft) {
      effectiveInlineMarginLeft = entry.marginLeftPx;
    }
  }
}

bool ChapterHtmlSlimParser::ensureHeapForTextLayout(const char* phase) {
  if (streamFailed) {
    return false;
  }

  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap >= MIN_FREE_HEAP_FOR_TEXT_LAYOUT && maxAllocHeap >= MIN_MAX_ALLOC_FOR_TEXT_LAYOUT) {
    return true;
  }

  // Soft low-memory zone: keep parsing in degraded mode and only hard-abort when
  // both free and contiguous heap fall to critical levels.
  if (freeHeap >= MIN_FREE_HEAP_FOR_TEXT_LAYOUT_HARD && maxAllocHeap >= MIN_MAX_ALLOC_FOR_TEXT_LAYOUT_HARD) {
    lowMemoryImageFallback = true;
    LOG_DBG("EHP", "Low heap (%u free, %u max alloc) before %s; continuing in degraded mode", freeHeap, maxAllocHeap,
            phase);
    return true;
  }

  LOG_ERR("EHP", "Low heap (%u free, %u max alloc), aborting parse before %s", freeHeap, maxAllocHeap, phase);
  streamFailed = true;
  layoutFailed = true;
  saxParser_.stop();
  return false;
}

// flush the contents of partWordBuffer to currentTextBlock
bool ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (streamFailed) {
    partWordBufferIndex = 0;
    nextWordContinues = false;
    return false;
  }

  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;
  const bool isUnderline = underlineUntilDepth < depth || effectiveUnderline;
  const bool isStrikethrough = strikethroughUntilDepth < depth || effectiveStrikethrough;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  if (isUnderline) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::UNDERLINE);
  }
  if (isStrikethrough) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::STRIKETHROUGH);
  }
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }

  // flush the buffer — route to table cell text when inside a <td>/<th>
  partWordBuffer[partWordBufferIndex] = '\0';
  if (currentTableCell) {
    currentTableCell->text->addWord(partWordBuffer, fontStyle, false, nextWordContinues);
  } else if (currentTextBlock) {
    // If a float image is pending and the block is still empty, attach it now so the
    // first word (and all subsequent words) are laid out beside the image.
    // This handles <p><img style="float:left"/>text...</p>: pendingInlineImage_ is set
    // while the block is empty, but no startNewTextBlock() fires before the first word.
    if (pendingInlineImage_.active && currentTextBlock->isEmpty()) {
      attachPendingFloatImage(currentTextBlock->getBlockStyle());
    }
    currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues);

    if (currentTextBlock->size() > 96) {
      if (!ensureHeapForTextLayout("long-block split")) {
        partWordBufferIndex = 0;
        nextWordContinues = false;
        return false;
      }
      LOG_DBG("EHP", "Text block too long, splitting into multiple pages");
      auto& splitBlockStyle = currentTextBlock->getBlockStyle();
      const int horizontalInset = splitBlockStyle.totalHorizontalInset();
      const uint16_t effectiveWidth =
          (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
      const int splitLineHeight =
          (splitBlockStyle.floatZoneCount > 0)
              ? static_cast<int>(renderer.getLineHeight(fontId) * lineCompression * splitBlockStyle.fontSizeMultiplier +
                                 0.5f)
              : 0;
      if (splitBlockStyle.floatZoneCount > 0) {
        for (int zi = 0; zi < splitBlockStyle.floatZoneCount; ++zi) {
          const int imgH = splitBlockStyle.floatZones[zi].bottom - splitBlockStyle.floatZones[zi].top;
          splitBlockStyle.floatZones[zi].top = static_cast<int16_t>(currentPageNextY);
          splitBlockStyle.floatZones[zi].bottom = static_cast<int16_t>(currentPageNextY + imgH);
        }
      }
      currentTextBlock->layoutAndExtractLines(
          renderer, fontId, effectiveWidth,
          [this](const std::shared_ptr<TextBlock>& textBlock, const bool lineEndsWithHyphenatedWord,
                 const bool suppressHyphenationRetry) {
            return addLineToPage(textBlock, lineEndsWithHyphenatedWord, suppressHyphenationRetry);
          },
          false, static_cast<int16_t>(currentPageNextY), splitLineHeight);
      // emitPage() clears floatZoneCount mid-layout when the page overflows — that's
      // intentional: lines on the continuation page should not be narrowed for an
      // image that lives on the previous page.
    }
  }
  partWordBufferIndex = 0;
  nextWordContinues = false;
  return true;
}

// Emit the current page, keeping paragraphLutPerPage and completedPageCount in lockstep.
// Callers must ensure currentPage is non-null and carries content; the helper resets
// currentPage to a fresh Page and zeroes currentPageNextY so the caller can keep building.
void ChapterHtmlSlimParser::emitPage(uint32_t xhtmlByteOffset) {
  paragraphLutPerPage.push_back({xhtmlByteOffset, xpathParagraphIndex, xpathListItemIndex});
  completePageFn(std::move(currentPage));
  completedPageCount++;
  currentPage.reset(new (std::nothrow) Page());
  currentPageNextY = 0;
  lastBlockMarginBottom = 0;
  deferredPageImage_.reset();  // tile A's deferred yPos update is no longer needed

  if (continuationImage_.active) {
    // Capture dimensions before moving the imageBlock out.
    const int16_t contH = continuationImage_.renderedHeight;
    const int16_t contW = continuationImage_.width;
    const bool contIsRight = continuationImage_.isRight;
    const int16_t contX = contIsRight ? static_cast<int16_t>(viewportWidth - contW) : 0;

    // Place tile B (bottom crop of split image) at the top of the new page.
    auto pageImg = std::make_shared<PageImage>(std::move(continuationImage_.imageBlock), contX, 0);
    currentPage->elements.push_back(pageImg);
    continuationImage_.active = false;

    // Set a float zone on the continuing text block so lines on the new page
    // are indented for the duration of tile B's height.
    if (currentTextBlock) {
      auto& bs = currentTextBlock->getBlockStyle();
      bs.floatZoneCount = 0;  // clear stale zones from old page first
      auto& z = bs.floatZones[bs.floatZoneCount++];
      z.top = 0;
      z.bottom = contH;
      z.width = static_cast<int16_t>(contW + 4);
      z.isRight = contIsRight;
    }
  } else {
    // No continuation image: clear float zones so text on the new page is not
    // indented for an image that lives on the previous page.
    if (currentTextBlock) {
      currentTextBlock->getBlockStyle().floatZoneCount = 0;
    }
  }
}

void ChapterHtmlSlimParser::recordPageBreakLabel(const std::string& label) {
  if (label.empty()) {
    return;
  }

  // Record the printed page label for the current rendered section page.
  // Do not alter pagination; the reader keeps its own page breaks.
  pageBreakLabels.emplace_back(static_cast<uint16_t>(completedPageCount), label);
}

void ChapterHtmlSlimParser::setExternalPageBreakAnchors(std::vector<std::pair<std::string, std::string>> anchors) {
  externalPageBreakAnchors.clear();
  topOfFilePageLabel.clear();
  topOfFilePageLabelEmitted = false;
  for (auto& [id, label] : anchors) {
    if (id.empty()) {
      // NCX pageTarget with no fragment (e.g. "OEBPS/c9_split_000.xhtml") — applies to the
      // first rendered page of this chapter. Keep only the first such entry if multiple.
      if (topOfFilePageLabel.empty()) {
        topOfFilePageLabel = std::move(label);
      }
    } else {
      externalPageBreakAnchors.emplace_back(std::move(id), std::move(label));
    }
  }
}

void ChapterHtmlSlimParser::attachPendingFloatImage(BlockStyle& bs) {
  if (!pendingInlineImage_.active) return;
  if (!currentPage) currentPage.reset(new (std::nothrow) Page());

  const int16_t imgH = pendingInlineImage_.height;
  const int16_t imgW = pendingInlineImage_.width;
  const bool imgIsRight = pendingInlineImage_.isRight;
  const int16_t remainingOnPage = static_cast<int16_t>(viewportHeight - currentPageNextY);
  const int16_t imgX = imgIsRight ? static_cast<int16_t>(viewportWidth - imgW) : 0;

  auto fullImageBlock =
      std::make_shared<ImageBlock>(pendingInlineImage_.cachedPath, imgW, imgH, pendingInlineImage_.alt, epub->getPath(),
                                   pendingInlineImage_.epubEntryPath);

  if (remainingOnPage >= imgH) {
    if (bs.floatZoneCount < BlockStyle::kMaxFloatZones) {
      auto& z = bs.floatZones[bs.floatZoneCount++];
      z.top = static_cast<int16_t>(currentPageNextY);
      z.bottom = static_cast<int16_t>(currentPageNextY + imgH);
      z.width = static_cast<int16_t>(imgW + 4);
      z.isRight = imgIsRight;
    }
    deferredPageImage_ = std::make_shared<PageImage>(fullImageBlock, imgX, currentPageNextY);
    currentPage->elements.push_back(deferredPageImage_);
    continuationImage_.active = false;
  } else {
    const int16_t tileAHeight = remainingOnPage;
    const int16_t tileBHeight = static_cast<int16_t>(imgH - tileAHeight);
    if (bs.floatZoneCount < BlockStyle::kMaxFloatZones) {
      auto& z = bs.floatZones[bs.floatZoneCount++];
      z.top = static_cast<int16_t>(currentPageNextY);
      z.bottom = static_cast<int16_t>(viewportHeight);
      z.width = static_cast<int16_t>(imgW + 4);
      z.isRight = imgIsRight;
    }
    auto tileA = fullImageBlock->makeCrop(0, tileAHeight);
    deferredPageImage_ = std::make_shared<PageImage>(std::move(tileA), imgX, currentPageNextY);
    currentPage->elements.push_back(deferredPageImage_);
    continuationImage_.imageBlock = fullImageBlock->makeCrop(tileAHeight, tileBHeight);
    continuationImage_.width = imgW;
    continuationImage_.renderedHeight = tileBHeight;
    continuationImage_.isRight = imgIsRight;
    continuationImage_.active = true;
  }

  pendingInlineImage_.active = false;
  pendingInlineImage_.cachedPath.clear();
  pendingInlineImage_.epubEntryPath.clear();
  pendingInlineImage_.alt.clear();
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  // Base style for the new block — normally the incoming blockStyle, but when falling
  // through from the empty-block merge path (see below) we use the merged style so that
  // accumulated parent-element margins are preserved for the inline-image paragraph.
  const BlockStyle* effectiveBase = &blockStyle;
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // Merge with existing block style to accumulate CSS styling from parent block elements.
      // This handles cases like <div style="margin-bottom:2em"><h1>text</h1></div> where the
      // div's margin should be preserved, even though it has no direct text content.
      BlockStyle incoming = blockStyle;
      const bool brGapPending = currentTextBlock->getBlockStyle().fromBrElement;
      if (brGapPending) {
        // The empty block was created by a <br> section separator. Inject a full line of
        // blank space before the following paragraph so the scene/section break is visible.
        // This only fires when the <br> block stayed empty (i.e. no inline text was added).
        const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      BlockStyle merged = currentTextBlock->getBlockStyle().getCombinedBlockStyle(incoming);
      // Preserve only whether the current empty block still represents <br> separators.
      // This lets consecutive <br> accumulate one line each without leaking the flag to real content blocks.
      merged.fromBrElement = blockStyle.fromBrElement;
      currentTextBlock->setBlockStyle(merged);

      if (!pendingAnchorId.empty()) {
        if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
          if (currentPage && !currentPage->elements.empty()) {
            emitPage(lastBodyChildByteOffset);
          }
        }
        anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
        pendingAnchorId.clear();
      }
      wordsExtractedInBlock = 0;
      // If an inline image is waiting, fall through to place it now rather than
      // returning early — otherwise the image skips empty wrapper blocks and
      // attaches to the *second* paragraph instead of the first.
      if (!pendingInlineImage_.active) return;
      // Fall through: use the merged style as the base so parent-element margins
      // (accumulated into this empty block) are carried into the new paragraph.
      effectiveBase = &currentTextBlock->getBlockStyle();
    }

    if (!currentTextBlock->isEmpty()) makePages();
  }
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (!pendingAnchorId.empty() &&
      std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      emitPage(lastBodyChildByteOffset);
    }
  }
  // Record deferred anchor after previous block is flushed (and any TOC page break)
  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
  // Apply pending inline image: attach float zone and place image on current page.
  // The image's actual yPos will be fixed in addLineToPage once the baseline is known.
  BlockStyle blockStyleWithIndent = *effectiveBase;
  attachPendingFloatImage(blockStyleWithIndent);
  currentTextBlock.reset(new (std::nothrow) ParsedText(extraParagraphSpacing, hyphenationEnabled, blockStyleWithIndent,
                                                       bionicReadingEnabled));
  wordsExtractedInBlock = 0;
}

void ChapterHtmlSlimParser::startElement(void* userData, const char* name, const char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  if (self->streamFailed) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  // Track SVG nesting depth. Must be checked before the svgDepth>0 guard below so that
  // nested <svg> elements increment the counter rather than being swallowed as unknowns.
  if (strcmp(name, "svg") == 0) {
    self->svgDepth += 1;
    self->depth += 1;
    return;
  }

  // Inside SVG: only process <image> elements (raster images); skip everything else.
  // SVG child elements like <path>, <rect>, <circle>, <text> must not reach the layout
  // engine — they would accumulate path data and exhaust heap on large inline SVG.
  if (self->svgDepth > 0 && !matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    self->depth += 1;
    return;
  }

  // Extract class, style, id, and pagebreak metadata attributes
  std::string classAttr;
  std::string styleAttr;
  std::string idAttr;
  std::string ariaLabel;
  std::string titleAttr;
  bool isPageBreakMarker = false;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        idAttr = atts[i + 1];
      } else if (strcmp(atts[i], "aria-label") == 0) {
        ariaLabel = atts[i + 1];
      } else if (strcmp(atts[i], "title") == 0) {
        titleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0) {
        isPageBreakMarker = true;
      } else if (strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        isPageBreakMarker = true;
      }
    }
  }

  // Emit any "top-of-file" printed-page label as soon as we see real markup. NCX entries
  // without a fragment refer to the start of this XHTML; record now so the label lands on
  // page 0 (completedPageCount is still 0 until the first emitPage()).
  if (!self->topOfFilePageLabelEmitted && !self->topOfFilePageLabel.empty()) {
    self->recordPageBreakLabel(self->topOfFilePageLabel);
    self->topOfFilePageLabelEmitted = true;
  }

  // Match id against NCX-supplied pagebreak anchors (printed page list). If matched,
  // treat this element as if it carried an inline doc-pagebreak marker.
  std::string externalLabel;
  if (!isPageBreakMarker && !idAttr.empty() && !self->externalPageBreakAnchors.empty()) {
    for (const auto& [extId, extLabel] : self->externalPageBreakAnchors) {
      if (extId == idAttr) {
        externalLabel = extLabel;
        isPageBreakMarker = true;
        break;
      }
    }
  }

  if (isPageBreakMarker) {
    std::string label = !ariaLabel.empty() ? ariaLabel : titleAttr;
    if (label.empty()) {
      label = std::move(externalLabel);
    }
    self->recordPageBreakLabel(label);
    if (!idAttr.empty()) {
      self->anchorData.emplace_back(idAttr, static_cast<uint16_t>(self->completedPageCount));
      self->pendingAnchorId = idAttr;
    }
  }

  // Defer generic anchor recording until startNewTextBlock, after the previous block
  // is flushed to pages via makePages(). Skip pagebreak anchors since they were already recorded.
  if (!isPageBreakMarker && !idAttr.empty()) {
    self->pendingAnchorId = idAttr;
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    {
      std::string cacheKey(name);
      cacheKey += '|';
      cacheKey += classAttr;
      auto it = self->cssStyleCache_.find(cacheKey);
      if (it != self->cssStyleCache_.end()) {
        cssStyle = it->second;
      } else {
        CssStyle resolved = self->cssParser->resolveStyle(name, classAttr);
        if (resolved.defined.anySet())
          cssStyle = self->cssStyleCache_.emplace(cacheKey, resolved).first->second;
        else
          cssStyle = resolved;  // transient fallback: skip cache so future calls can re-resolve
      }
    }
    if (!styleAttr.empty()) {
      auto it = self->inlineStyleCache_.find(styleAttr);
      if (it == self->inlineStyleCache_.end())
        it = self->inlineStyleCache_.emplace(styleAttr, CssParser::parseInlineStyle(styleAttr)).first;
      cssStyle.applyOver(it->second);
    }
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Buffered table rendering: accumulate cells in memory, emit as PageTableFragment on </table>.
  if (strcmp(name, "table") == 0) {
    if (self->currentTable) {
      // Nested table — mark unsupported and track depth
      self->currentTable->depth += 1;
      self->currentTable->unsupported = true;
      self->depth += 1;
      return;
    }
    // Flush any pending text before starting the table
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->currentTable = std::unique_ptr<BufferedTable>(new BufferedTable());
    self->currentTable->depth = 1;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "border") == 0 && strcmp(atts[i + 1], "0") == 0) {
          self->currentTable->hasBorder = false;
        }
      }
    }
    self->depth += 1;
    return;
  }

  if (self->currentTable && self->currentTable->depth == 1 && strcmp(name, "tr") == 0) {
    self->currentTable->rows.emplace_back();
    if (self->currentTable->rows.size() > MAX_TABLE_ROWS) {
      self->currentTable->unsupported = true;
    }
    self->depth += 1;
    return;
  }

  if (self->currentTable && self->currentTable->depth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
    }
    if (self->currentTable->rows.empty()) {
      self->currentTable->rows.emplace_back();
    }
    BufferedTableRow& row = self->currentTable->rows.back();

    // Parse colspan attribute (inspired by uxjulia/CrossInk; rewritten for our codebase).
    // Any rowspan != 1 is unsupported; we ignore it and let the fallback handle those tables.
    uint8_t colSpan = 1;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "colspan") == 0) {
          char* end;
          const long v = std::strtol(atts[i + 1], &end, 10);
          if (end != atts[i + 1] && v >= 1 && v <= MAX_TABLE_COLS) {
            colSpan = static_cast<uint8_t>(v);
          }
        } else if (strcmp(atts[i], "rowspan") == 0) {
          char* end;
          const long v = std::strtol(atts[i + 1], &end, 10);
          if (end != atts[i + 1] && v != 1) {
            self->currentTable->unsupported = true;
          }
        }
      }
    }

    const bool isHeader = (strcmp(name, "th") == 0);
    row.cells.emplace_back();
    row.cells.back().isHeader = isHeader;
    row.cells.back().colSpan = colSpan;
    row.cells.back().text =
        std::unique_ptr<ParsedText>(new ParsedText(false, false));  // no paragraph spacing, no hyphenation in cells
    row.effectiveCols = static_cast<uint8_t>(row.effectiveCols + colSpan);
    if (row.effectiveCols > self->currentTable->maxCols) {
      self->currentTable->maxCols = row.effectiveCols;
    }
    if (row.cells.size() > MAX_TABLE_COLS || row.effectiveCols > MAX_TABLE_COLS) {
      self->currentTable->unsupported = true;
    }
    self->currentTableCell = &row.cells.back();
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    std::string src;
    std::string alt;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0 || strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0) {
          if (src.empty()) {
            src = atts[i + 1];
            // Strip fragment anchors (e.g. "cover.jpg#xywh=0,0,100,100")
            auto hash = src.find('#');
            if (hash != std::string::npos) src.erase(hash);
          }
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }

      // Images inside table cells cannot be placed inline in the cell (no layout space).
      // Defer them so they are emitted as block images immediately after the table.
      if (self->currentTableCell && self->currentTable && !src.empty() && self->imageRendering != 2) {
        self->currentTable->deferredImages.push_back({src, alt});
        self->depth += 1;
        return;
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        // Suppressing an image should not leak accumulated wrapper block spacing
        // (e.g. figure/h1 margins) into the next text paragraph.
        if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
          BlockStyle resetStyle;
          resetStyle.textAlignDefined = true;
          const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(self->paragraphAlignment);
          resetStyle.alignment = align;
          self->currentTextBlock->setBlockStyle(resetStyle);
          LOG_DBG("EHP", "Image suppressed: pending empty block style reset");
        }
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      // Skip image if CSS display:none
      if (self->cssParser) {
        std::string imgCacheKey("img|");
        imgCacheKey += classAttr;
        auto imgIt = self->cssStyleCache_.find(imgCacheKey);
        if (imgIt == self->cssStyleCache_.end())
          imgIt = self->cssStyleCache_.emplace(imgCacheKey, self->cssParser->resolveStyle("img", classAttr)).first;
        CssStyle imgDisplayStyle = imgIt->second;
        if (!styleAttr.empty()) {
          auto it = self->inlineStyleCache_.find(styleAttr);
          if (it == self->inlineStyleCache_.end())
            it = self->inlineStyleCache_.emplace(styleAttr, CssParser::parseInlineStyle(styleAttr)).first;
          imgDisplayStyle.applyOver(it->second);
        }
        if (imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::None) {
          // CSS-hidden images should behave like suppressed images for spacing.
          if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
            BlockStyle resetStyle;
            resetStyle.textAlignDefined = true;
            const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                   ? CssTextAlign::Justify
                                   : static_cast<CssTextAlign>(self->paragraphAlignment);
            resetStyle.alignment = align;
            self->currentTextBlock->setBlockStyle(resetStyle);
            LOG_DBG("EHP", "Image hidden via CSS display:none: pending empty block style reset");
          }
          self->skipUntilDepth = self->depth;
          self->depth += 1;
          return;
        }
      }

      const auto handleImageFallback = [&]() {
        // Fallback to alt text if image processing fails.
        if (!alt.empty()) {
          alt = "[Image: " + alt + "]";
          self->startNewTextBlock(centeredBlockStyle);
          self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
          self->depth += 1;
          self->characterData(userData, alt.c_str(), alt.length());
          // Skip any child content (skip until parent as we pre-advanced depth above)
          self->skipUntilDepth = self->depth - 1;
          return;
        }

        // No alt text, skip.
        self->skipUntilDepth = self->depth;
        self->depth += 1;
      };

      if (!src.empty() && self->imageRendering != 1) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());

        if (self->lowMemoryImageFallback) {
          handleImageFallback();
          return;
        }

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));

          if (ImageDecoderFactory::isFormatSupported(resolvedPath)) {
            // Determine SD cache path (image will be extracted here lazily at first render).
            std::string ext;
            size_t extPos = resolvedPath.rfind('.');
            if (extPos != std::string::npos) ext = resolvedPath.substr(extPos);
            std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

            // Get dimensions from the pre-built manifest (fast, heap-safe) or fall back to
            // reading the ZIP entry header directly (safe outside a streaming inflate context).
            ImageDimensions dims = {0, 0};
            bool dimsOk = false;
            if (self->imageManifest) {
              const ImageManifestEntry* entry = self->imageManifest->find(resolvedPath);
              if (entry) {
                dims.width = entry->width;
                dims.height = entry->height;
                dimsOk = true;
              }
            }
            if (!dimsOk) {
              dimsOk = ImageDecoderFactory::getDimensionsFromZipEntry(self->epub->getPath(), resolvedPath, dims);
            }
            if (dimsOk) {
              LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);
              {
                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                std::string imgCacheKey("img|");
                imgCacheKey += classAttr;
                auto imgStyleIt = self->cssParser ? self->cssStyleCache_.find(imgCacheKey) : self->cssStyleCache_.end();
                if (self->cssParser && imgStyleIt == self->cssStyleCache_.end())
                  imgStyleIt =
                      self->cssStyleCache_.emplace(imgCacheKey, self->cssParser->resolveStyle("img", classAttr)).first;
                CssStyle imgStyle = self->cssParser ? imgStyleIt->second : CssStyle{};
                // Merge inline style (e.g. style="height: 2em") so it overrides stylesheet rules
                if (!styleAttr.empty()) {
                  auto it = self->inlineStyleCache_.find(styleAttr);
                  if (it == self->inlineStyleCache_.end())
                    it = self->inlineStyleCache_.emplace(styleAttr, CssParser::parseInlineStyle(styleAttr)).first;
                  imgStyle.applyOver(it->second);
                }
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth();
                int containerWidth = self->viewportWidth;
                if (self->currentTextBlock) {
                  const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                  if (inset > 0 && inset < self->viewportWidth) {
                    containerWidth = self->viewportWidth - inset;
                  }
                }

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: resolve both, then clamp to
                  // current container preserving requested ratio.
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  if (displayWidth < 1) displayWidth = 1;
                  if (displayWidth > containerWidth || displayHeight > self->viewportHeight) {
                    float scaleX =
                        (displayWidth > containerWidth) ? static_cast<float>(containerWidth) / displayWidth : 1.0f;
                    float scaleY = (displayHeight > self->viewportHeight)
                                       ? static_cast<float>(self->viewportHeight) / displayHeight
                                       : 1.0f;
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    displayWidth = static_cast<int>(displayWidth * scale + 0.5f);
                    displayHeight = static_cast<int>(displayHeight * scale + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  LOG_DBG("EHP", "Display size from CSS height+width: %dx%d", displayWidth, displayHeight);
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > containerWidth) {
                    displayWidth = containerWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                  LOG_DBG("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against container width) and derive
                  // height from aspect ratio.
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayWidth > containerWidth) displayWidth = containerWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_DBG("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
                } else {
                  // Scale to fit current container while maintaining aspect ratio.
                  int maxWidth = containerWidth;
                  int maxHeight = self->viewportHeight;
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                  LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
                }

                // Inline image path: if inside a CSS float context and image is small enough,
                // defer placement beside the next paragraph rather than emitting as a block.
                // Concept inspired by CidVonHighwind/microreader and KOReader/CREngine research.
                const bool isInlineCandidate =
                    self->floatDepth_ > 0 && displayWidth <= self->viewportWidth / 3 && displayHeight <= 120;
                if (isInlineCandidate) {
                  self->pendingInlineImage_.cachedPath = std::move(cachedImagePath);
                  self->pendingInlineImage_.epubEntryPath = resolvedPath;
                  self->pendingInlineImage_.width = static_cast<int16_t>(displayWidth);
                  self->pendingInlineImage_.height = static_cast<int16_t>(displayHeight);
                  self->pendingInlineImage_.alt = alt;
                  self->pendingInlineImage_.isRight =
                      (self->floatDepth_ > 0) && self->floatOpenSides_[self->floatDepth_ - 1];
                  self->pendingInlineImage_.active = true;
                  LOG_DBG("EHP", "Inline image deferred: w=%d h=%d", displayWidth, displayHeight);
                  // Don't flush the current text block — let it continue into the next paragraph.
                  self->depth += 1;
                  return;
                }

                // Block image path (existing behaviour) — flush text before placing image
                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  if (!self->flushPartWordBuffer()) return;
                }
                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                }

                // If the current text block is still empty, it may carry accumulated parent
                // block spacing (e.g. div/figure/h1 wrappers). Apply that spacing around the
                // image itself so it doesn't leak into the next text paragraph.
                BlockStyle pendingImageBlockStyle;
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  pendingImageBlockStyle = self->currentTextBlock->getBlockStyle();
                }

                const int imageSpacingTop = std::max(0, static_cast<int>(pendingImageBlockStyle.marginTop)) +
                                            std::max(0, static_cast<int>(pendingImageBlockStyle.paddingTop));
                const int imageSpacingBottom = std::max(0, static_cast<int>(pendingImageBlockStyle.marginBottom)) +
                                               std::max(0, static_cast<int>(pendingImageBlockStyle.paddingBottom));
                const int totalImageHeightWithSpacing = imageSpacingTop + displayHeight + imageSpacingBottom;

                LOG_DBG("EHP",
                        "Image layout prep: src=%s dims=%dx%d display=%dx%d y=%d spacing(top=%d,bottom=%d,total=%d)",
                        src.c_str(), dims.width, dims.height, displayWidth, displayHeight, self->currentPageNextY,
                        imageSpacingTop, imageSpacingBottom, totalImageHeightWithSpacing);

                // Create page for image - only break if image won't fit remaining space
                if (self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + totalImageHeightWithSpacing > self->viewportHeight)) {
                  LOG_DBG("EHP", "Image page break: currentY=%d needed=%d viewportH=%d", self->currentPageNextY,
                          totalImageHeightWithSpacing, self->viewportHeight);
                  self->emitPage(self->lastBodyChildByteOffset);
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create new page");
                    return;
                  }
                } else if (!self->currentPage) {
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create initial page");
                    return;
                  }
                  self->currentPageNextY = 0;
                }

                self->currentPageNextY += imageSpacingTop;

                // Create ImageBlock with lazy-extraction source info.
                // The SD file at cachedImagePath does not exist yet — it will be extracted
                // from the EPUB at first render time by ImageBlock::ensureExtracted().
                auto imageBlock = std::make_shared<ImageBlock>(cachedImagePath, displayWidth, displayHeight, alt,
                                                               self->epub->getPath(), resolvedPath);
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  return;
                }
                int xPos = (self->viewportWidth - displayWidth) / 2;
                auto pageImage = std::make_shared<PageImage>(imageBlock, xPos, self->currentPageNextY);
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  return;
                }
                self->currentPage->elements.push_back(pageImage);
                self->currentPageNextY += displayHeight;
                self->currentPageNextY += imageSpacingBottom;

                LOG_DBG("EHP", "Image placed: x=%d y=%d w=%d h=%d nextY=%d", xPos, pageImage->yPos, displayWidth,
                        displayHeight, self->currentPageNextY);

                // Reset empty pending block style after consuming spacing around the image.
                // This prevents figure/header wrapper margins from being applied again to the
                // next paragraph block.
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  BlockStyle resetStyle;
                  resetStyle.textAlignDefined = true;
                  const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                         ? CssTextAlign::Justify
                                         : static_cast<CssTextAlign>(self->paragraphAlignment);
                  resetStyle.alignment = align;
                  self->currentTextBlock->setBlockStyle(resetStyle);
                  LOG_DBG("EHP", "Image spacing consumed; pending empty block style reset for following text");
                }

                self->depth += 1;
                return;
              }  // layout geometry block
            } else {
              LOG_ERR("EHP", "Failed to read image dimensions from ZIP: %s", resolvedPath.c_str());
            }
          }  // isFormatSupported
        }
      }

      handleImageFallback();
      return;
    }
  }

  // Track body element depth for paragraph index counting
  if (strcmp(name, "body") == 0 && self->xpathBodyDepth < 0) {
    self->xpathBodyDepth = self->depth;
  }

  // Count <p> sibling indices at body-child level. Must happen BEFORE the display:none
  // check so that hidden <p> elements are still counted, matching ChapterXPathIndexer's
  // counting (pure XML, no CSS). This ensures paragraph indices in the section cache LUT
  // align with KOReader's crengine XPath indices.
  // At the same time, record the byte offset of every direct-body-child element start:
  // the forward mapper's partial-parse heuristic requires the seek hint to land on a
  // body-child boundary, otherwise partialBaseDepth can misidentify wrapped paragraphs.
  if (self->xpathBodyDepth >= 0 && self->depth == self->xpathBodyDepth + 1) {
    self->lastBodyChildByteOffset = self->saxParser_.byteOffset();
    if (strcmp(name, "p") == 0) {
      self->xpathParagraphIndex++;
    }
  }

  // <li> can appear nested inside <ul>/<ol> at any depth, so count it globally —
  // not at body-child level. The running count must match what the runtime reverse
  // mapper sees so getPageForListItemIndex can snap a KOReader li XPath to a page.
  if (self->xpathBodyDepth >= 0 && strcmp(name, "li") == 0) {
    self->xpathListItemIndex++;
  }

  if (matches(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // removed skipping of doc-pagebreak and epub:type="pagebreak"
  // as publishers sometimes wrap actual content in these tags
  /*
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }
  */

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
        if (!self->flushPartWordBuffer()) return;
        if (!endsAtDashBreak) {
          self->nextWordContinues = true;
        }
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      strncpy(self->currentFootnote.href, href, sizeof(self->currentFootnote.href) - 1);
      self->currentFootnote.href[sizeof(self->currentFootnote.href) - 1] = '\0';
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link
      self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasUnderline = true;
      entry.underline = true;
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  // Track CSS float depth — used to detect inline images beside paragraph text.
  // Fixed-size array, cap at kMaxFloatDepth — deeper nesting is pathological.
  if (cssStyle.hasCssFloat() && cssStyle.cssFloat != CssFloat::None &&
      self->floatDepth_ < ChapterHtmlSlimParser::kMaxFloatDepth) {
    self->floatOpenDepths_[self->floatDepth_] = self->depth;
    self->floatOpenSides_[self->floatDepth_] = (cssStyle.cssFloat == CssFloat::Right);
    self->floatDepth_++;
  }

  if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
    int startCounter = 0;
    if (name[0] == 'o') {
      const char* startAttr = getAttribute(atts, "start");
      if (startAttr) {
        int v = atoi(startAttr);
        if (v > 0) startCounter = v - 1;  // counter is pre-incremented on each <li>
      }
    }
    self->listStack.push_back({self->depth, name[0] == 'o', startCounter, cssStyle.listStyleNone});
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  const auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), self->viewportWidth);

  // Block/header boundaries must flush any buffered trailing word first.
  // Otherwise tags like ..."item?"<p ...> can carry the final word into the next paragraph.
  if (self->partWordBufferIndex > 0 && ((matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) ||
                                        (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS) && strcmp(name, "br") != 0))) {
    if (!self->flushPartWordBuffer()) return;
  }

  // CSS page-break-before: always — emit the current page before this block starts.
  if (cssStyle.pageBreakBefore &&
      (matches(name, HEADER_TAGS, NUM_HEADER_TAGS) || matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) && self->currentPage &&
      !self->currentPage->elements.empty()) {
    self->emitPage(self->lastBodyChildByteOffset);
  }

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign() &&
        self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None)) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    // Apply default heading font-size multipliers when no explicit CSS font-size is set.
    // Concept inspired by CidVonHighwind/microreader.
    if (!cssStyle.hasFontSizeMultiplier()) {
      const int level = name[1] - '0';  // 'h1'->1, 'h2'->2, …
      if (level == 1)
        headerBlockStyle.fontSizeMultiplier = 1.6f;
      else if (level == 2)
        headerBlockStyle.fontSizeMultiplier = 1.4f;
      else if (level == 3)
        headerBlockStyle.fontSizeMultiplier = 1.2f;
      // h4-h6 stay at 1.0f
    }
    self->startNewTextBlock(headerBlockStyle);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (isZeroHeightSpacerParagraph(name, styleAttr)) {
      // Preserve paragraph break semantics for this <p>, but skip its inner text payload.
      self->currentCssStyle = cssStyle;
      auto blockStyle = userAlignmentBlockStyle;
      if (self->embeddedStyle && cssStyle.hasTextAlign() &&
          self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None)) {
        blockStyle.alignment = cssStyle.textAlign;
        blockStyle.textAlignDefined = true;
      }
      self->startNewTextBlock(blockStyle);
      self->updateEffectiveInlineStyle();

      self->skipTextUntilDepth = self->depth;
      self->depth += 1;
      return;
    }

    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        if (!self->flushPartWordBuffer()) return;
      }
      // Tag the new block so startNewTextBlock can inject a full line-height gap if
      // the block remains empty (i.e. <br> is a section separator between paragraphs).
      // If the block gets text added before the next block opens it becomes non-empty,
      // goes through makePages() normally, and the flag has no effect (inline <br> case).
      // Build a neutral <br> style that keeps inline alignment/indent context but avoids
      // carrying cumulative margins from previous empty blocks (which can force spurious page breaks).
      const BlockStyle& currentStyle = self->currentTextBlock->getBlockStyle();
      BlockStyle brStyle;
      brStyle.alignment = currentStyle.alignment;
      brStyle.textAlignDefined = currentStyle.textAlignDefined;
      // text-indent is not inherited across <br>: it applies to the first line of a block only.
      // Span-based indents (poem stanza pattern) are applied directly to each block at span-open time.
      brStyle.fromBrElement = true;
      self->startNewTextBlock(brStyle);
    } else {
      self->currentCssStyle = cssStyle;
      auto blockStyle = userAlignmentBlockStyle;
      if (self->embeddedStyle && cssStyle.hasTextAlign() &&
          self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None)) {
        blockStyle.alignment = cssStyle.textAlign;
        blockStyle.textAlignDefined = true;
      }
      // For <li> with no CSS margin, apply depth-based indent so nested lists are visually
      // distinguishable. listStack.size() == 1 for top-level, 2 for first nested, etc.
      if (strcmp(name, "li") == 0 && !cssStyle.hasMarginLeft() && !self->listStack.empty()) {
        const int depth = static_cast<int>(std::min(self->listStack.size(), size_t(3)));
        blockStyle.marginLeft = static_cast<int16_t>(emSize * 1.5f * depth);
      }
      self->startNewTextBlock(blockStyle);
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0) {
        if (!self->listStack.empty()) {
          if (self->listStack.back().isOrdered) {
            const char* valueAttr = getAttribute(atts, "value");
            if (valueAttr) {
              int v = atoi(valueAttr);
              if (v > 0) self->listStack.back().counter = v - 1;
            }
            self->listStack.back().counter += 1;
          }
          if (!self->listStack.back().suppressMarker) {
            char marker[12];
            if (self->listStack.back().isOrdered) {
              snprintf(marker, sizeof(marker), "%d.", self->listStack.back().counter);
            } else {
              strcpy(marker, "\xe2\x80\xa2");
            }
            self->currentTextBlock->addWord(marker, EpdFontFamily::REGULAR);
          }
        }
      } else if (strcmp(name, "pre") == 0) {
        // Record depth so characterData can treat \n as a hard line break inside <pre>.
        // depth has not been incremented yet here; it will be after startElement returns.
        self->preUntilDepth = std::min(self->preUntilDepth, self->depth);
      }
    }
  } else if (strcmp(name, "hr") == 0) {
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
    }
    self->makePages();
    if (!self->currentPage) {
      self->currentPage.reset(new Page());
      self->currentPageNextY = 0;
    }
    const int lineHeight = static_cast<int>(self->renderer.getLineHeight(self->fontId) * self->lineCompression + 0.5f);
    const int16_t marginV = static_cast<int16_t>(lineHeight / 2);
    self->currentPageNextY += marginV;
    if (self->currentPageNextY + 1 + marginV > self->viewportHeight) {
      self->emitPage(self->lastBodyChildByteOffset);
      self->currentPage.reset(new Page());
      self->currentPageNextY = 0;
    }
    self->currentPage->elements.push_back(
        std::make_shared<PageHR>(0, self->currentPageNextY, static_cast<int16_t>(self->viewportWidth)));
    self->currentPageNextY += 1 + marginV;
    BlockStyle emptyStyle;
    self->startNewTextBlock(emptyStyle);
  } else if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS) ||
             matches(name, STRIKETHROUGH_TAGS, NUM_STRIKETHROUGH_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
      if (!self->flushPartWordBuffer()) return;
      if (!endsAtDashBreak) {
        self->nextWordContinues = true;
      }
    }
    if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS)) {
      self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
    }
    if (matches(name, STRIKETHROUGH_TAGS, NUM_STRIKETHROUGH_TAGS)) {
      self->strikethroughUntilDepth = std::min(self->strikethroughUntilDepth, self->depth);
    }
    // Push inline style entry for underline/strikethrough tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS)) {
      entry.hasUnderline = true;
      entry.underline = true;
    }
    if (matches(name, STRIKETHROUGH_TAGS, NUM_STRIKETHROUGH_TAGS)) {
      entry.hasStrikethrough = true;
      entry.strikethrough = true;
    }
    if (cssStyle.hasTextDecoration()) {
      const uint8_t dec = static_cast<uint8_t>(cssStyle.textDecoration);
      if (dec & static_cast<uint8_t>(CssTextDecoration::Underline)) {
        entry.hasUnderline = true;
        entry.underline = true;
        self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
      }
      if (dec & static_cast<uint8_t>(CssTextDecoration::LineThrough)) {
        entry.hasStrikethrough = true;
        entry.strikethrough = true;
        self->strikethroughUntilDepth = std::min(self->strikethroughUntilDepth, self->depth);
      }
    }
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
      if (!self->flushPartWordBuffer()) return;
      if (!endsAtDashBreak) {
        self->nextWordContinues = true;
      }
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasTextDecoration()) {
      const uint8_t dec = static_cast<uint8_t>(cssStyle.textDecoration);
      if (dec & static_cast<uint8_t>(CssTextDecoration::Underline)) {
        entry.hasUnderline = true;
        entry.underline = true;
      }
      if (dec & static_cast<uint8_t>(CssTextDecoration::LineThrough)) {
        entry.hasStrikethrough = true;
        entry.strikethrough = true;
      }
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
      if (!self->flushPartWordBuffer()) return;
      if (!endsAtDashBreak) {
        self->nextWordContinues = true;
      }
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasTextDecoration()) {
      const uint8_t dec = static_cast<uint8_t>(cssStyle.textDecoration);
      if (dec & static_cast<uint8_t>(CssTextDecoration::Underline)) {
        entry.hasUnderline = true;
        entry.underline = true;
      }
      if (dec & static_cast<uint8_t>(CssTextDecoration::LineThrough)) {
        entry.hasStrikethrough = true;
        entry.strikethrough = true;
      }
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasVerticalAlign() || cssStyle.hasMarginLeft()) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
        if (!self->flushPartWordBuffer()) return;
        if (!endsAtDashBreak) {
          self->nextWordContinues = true;
        }
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      if (cssStyle.hasTextDecoration()) {
        const uint8_t dec = static_cast<uint8_t>(cssStyle.textDecoration);
        if (dec == static_cast<uint8_t>(CssTextDecoration::None)) {
          entry.hasUnderline = true;
          entry.underline = false;
          entry.hasStrikethrough = true;
          entry.strikethrough = false;
        } else {
          if (dec & static_cast<uint8_t>(CssTextDecoration::Underline)) {
            entry.hasUnderline = true;
            entry.underline = true;
          }
          if (dec & static_cast<uint8_t>(CssTextDecoration::LineThrough)) {
            entry.hasStrikethrough = true;
            entry.strikethrough = true;
          }
        }
      }
      if (cssStyle.hasVerticalAlign()) {
        if (cssStyle.verticalAlign == CssVerticalAlign::Super) {
          entry.hasSup = true;
          entry.sup = true;
        } else if (cssStyle.verticalAlign == CssVerticalAlign::Sub) {
          entry.hasSub = true;
          entry.sub = true;
        } else {
          // baseline: explicitly cancel any inherited sup/sub
          entry.hasSup = true;
          entry.sup = false;
          entry.hasSub = true;
          entry.sub = false;
        }
      }
      if (cssStyle.hasMarginLeft()) {
        // margin-left on an inline span acts as a per-line indent (poem stanza pattern).
        // Applied immediately to the current block because the span closes before the
        // trailing <br>, so the indent must be on the block that receives the text.
        const int16_t marginPx = cssStyle.marginLeft.toPixelsInt16(emSize, static_cast<float>(self->viewportWidth));
        entry.hasMarginLeft = true;
        entry.marginLeftPx = marginPx;
        if (marginPx > 0 && self->currentTextBlock) {
          BlockStyle updatedStyle = self->currentTextBlock->getBlockStyle();
          updatedStyle.textIndent = marginPx;
          updatedStyle.textIndentDefined = true;
          self->currentTextBlock->setBlockStyle(updatedStyle);
        }
      }
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void ChapterHtmlSlimParser::characterData(void* userData, const char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  if (self->streamFailed) {
    return;
  }

  // Skip content of nested tables (depth > 1 means we're inside a nested table)
  if (self->currentTable && self->currentTable->depth > 1) {
    return;
  }

  // Route character data into the active table cell's ParsedText
  if (self->currentTableCell) {
    // Use the existing partWordBuffer + word-level accumulation logic below,
    // but the flush target will be currentTableCell->text (handled in flushPartWordBuffer).
    // Fall through to the normal character accumulation path.
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Ignore character data inside synthetic zero-height spacer <p> tags.
  if (self->skipTextUntilDepth < self->depth) {
    return;
  }

  // Skip SVG text content (path data, coordinates, etc.) — it would be treated as words
  // and exhaust heap on EPUBs with large inline SVG elements.
  if (self->svgDepth > 0) {
    return;
  }

  // Collect footnote link display text (for the number label)
  // Remove leading/trailing whitespace and square brackets from the
  // footnote link text to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  for (int i = 0; i < len; i++) {
    const unsigned char c = static_cast<unsigned char>(s[i]);

    // Fast path for plain ASCII word characters (> 0x20 and < 0x80).
    // This covers the vast majority of characters in Latin-script text.
    // All multi-byte UTF-8 sequences start with a byte >= 0x80, so this
    // path is safe to take without any further multi-byte checks.
    if (c > 0x20 && c < 0x80) {
      if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
        // Buffer is full — flush before appending. Pure ASCII means no
        // partial multi-byte sequence can be at the boundary.
        if (!self->flushPartWordBuffer()) return;
      }
      self->partWordBuffer[self->partWordBufferIndex++] = s[i];
      continue;
    }

    if (isWhitespace(s[i])) {
      // Inside <pre>: treat \n as a hard line break.
      if (s[i] == '\n' && self->preUntilDepth < self->depth) {
        if (self->partWordBufferIndex > 0) {
          if (!self->flushPartWordBuffer()) return;
        }
        // Blank line: the current block is empty, but we still need to emit a visible
        // empty line.  Add a single space so the block is non-empty and makePages()
        // will produce a line of the correct height instead of reusing the empty block.
        if (self->currentTextBlock->isEmpty()) {
          self->currentTextBlock->addWord(" ", EpdFontFamily::REGULAR);
        }
        self->startNewTextBlock(self->currentTextBlock->getBlockStyle());
        self->nextWordContinues = false;
        continue;
      }
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        if (!self->flushPartWordBuffer()) return;
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        if (!self->flushPartWordBuffer()) return;
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      if (!self->flushPartWordBuffer()) return;

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        if (!self->flushPartWordBuffer()) return;
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;
      if (!self->flushPartWordBuffer()) return;

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const char FEFF_BYTE_1 = static_cast<char>(0xEF);
    const char FEFF_BYTE_2 = static_cast<char>(0xBB);
    const char FEFF_BYTE_3 = static_cast<char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        if (!self->flushPartWordBuffer()) return;
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
      } else {
        if (!self->flushPartWordBuffer()) return;
      }
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }
}

void ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void ChapterHtmlSlimParser::endElement(void* userData, const char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  if (self->streamFailed) {
    return;
  }

  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;
  const bool willClearUnderline = self->underlineUntilDepth == self->depth - 1;
  const bool willClearStrikethrough = self->strikethroughUntilDepth == self->depth - 1;

  const bool styleWillChange =
      willPopStyleStack || willClearBold || willClearItalic || willClearUnderline || willClearStrikethrough;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);

  if (self->currentTable && self->currentTable->depth > 1 && strcmp(name, "table") == 0) {
    self->partWordBufferIndex = 0;
    self->currentTable->depth -= 1;
    self->depth -= 1;
    LOG_DBG("EHP", "nested table end, depth now %d", self->currentTable->depth);
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag =
        !headerOrBlockTag && !tableStructuralTag && !matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, NUM_BOLD_TAGS) ||
                             matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) ||
                             matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS) ||
                             matches(name, STRIKETHROUGH_TAGS, NUM_STRIKETHROUGH_TAGS) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) || self->depth == 1;

    if (shouldFlush) {
      const bool endsAtDashBreak = bufferEndsWithBreakableDash(self->partWordBuffer, self->partWordBufferIndex);
      if (!self->flushPartWordBuffer()) return;
      // If closing an inline element, the next word fragment continues the same visual word —
      // unless the buffered text ended at a dash that should allow a line break (em/en dash, etc.).
      if (isInlineTag && !endsAtDashBreak) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Decrement float depth when the floated element's scope closes.
  while (self->floatDepth_ > 0 && self->floatOpenDepths_[self->floatDepth_ - 1] >= self->depth) {
    self->floatDepth_--;
  }

  if (strcmp(name, "svg") == 0 && self->svgDepth > 0) {
    self->svgDepth -= 1;
  }

  // Pop list entries whose ul/ol is now out of scope
  while (!self->listStack.empty() && self->listStack.back().depth >= self->depth) {
    self->listStack.pop_back();
  }

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && self->currentFootnote.href[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnote.href, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    self->insideFootnoteLink = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  // Leaving zero-height spacer paragraph text-skip scope
  if (self->skipTextUntilDepth == self->depth) {
    self->skipTextUntilDepth = INT_MAX;
  }

  if (self->currentTable && self->currentTable->depth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
    }
    // Determine if the whole row consists of header cells
    if (!self->currentTable->rows.empty()) {
      auto& row = self->currentTable->rows.back();
      bool allHeaders = !row.cells.empty();
      for (const auto& c : row.cells) {
        if (!c.isHeader) {
          allHeaders = false;
          break;
        }
      }
      row.isHeaderRow = allHeaders;
    }
    self->currentTableCell = nullptr;
    self->nextWordContinues = false;
  }

  if (self->currentTable && self->currentTable->depth == 1 && strcmp(name, "tr") == 0) {
    self->nextWordContinues = false;
  }

  if (self->currentTable && self->currentTable->depth == 1 && strcmp(name, "table") == 0) {
    if (self->partWordBufferIndex > 0) {
      if (!self->flushPartWordBuffer()) return;
    }
    self->currentTableCell = nullptr;
    self->emitBufferedTable();
    self->currentTable.reset();
    self->nextWordContinues = false;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Leaving underline tag
  if (self->underlineUntilDepth == self->depth) {
    self->underlineUntilDepth = INT_MAX;
  }

  // Leaving strikethrough tag
  if (self->strikethroughUntilDepth == self->depth) {
    self->strikethroughUntilDepth = INT_MAX;
  }

  // Leaving pre tag
  if (self->preUntilDepth == self->depth) {
    self->preUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // Reset alignment on empty text blocks to prevent stale alignment from bleeding
    // into the next sibling element. This fixes issue #1026 where an empty <h1> (default
    // Center) followed by an image-only <p> causes Center to persist through the chain
    // of empty block reuse into subsequent text paragraphs.
    // Margins/padding are preserved so parent element spacing still accumulates correctly.
    if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
      auto style = self->currentTextBlock->getBlockStyle();
      // Keep alignment only when closing the <br> separator itself so subsequent text
      // within the same block container stays aligned. Reset alignment when closing
      // other block tags (e.g. div/p) to avoid leaking centered/right alignment globally.
      const bool preserveForBrClose = style.fromBrElement && strcmp(name, "br") == 0;
      if (!preserveForBrClose) {
        style.textAlignDefined = false;
        style.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                              ? CssTextAlign::Justify
                              : static_cast<CssTextAlign>(self->paragraphAlignment);
        self->currentTextBlock->setBlockStyle(style);
      }
    }
  }
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() = default;

bool ChapterHtmlSlimParser::setup(const size_t totalInflatedSize) {
  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  // Resolve None sentinel to Justify for initial block (no CSS context yet)
  const auto align = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                         ? CssTextAlign::Justify
                         : static_cast<CssTextAlign>(this->paragraphAlignment);
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD.
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE.
  if (!saxParser_.init(this, startElement, endElement, characterData, defaultHandlerExpand)) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  totalStreamSize = totalInflatedSize;
  bytesStreamed = 0;
  lastReportedProgress = -1;
  streamFailed = false;
  layoutFailed = false;
  streamStartTimeMs = millis();

  // Choose progress granularity by chapter size. Each callback drives a full-screen
  // e-ink refresh (~640ms), so smaller chapters skip mid-parse ticks entirely.
  // progressStepPercent == 0 means "popup only, no mid-parse updates".
  progressStepPercent = 0;
  if (totalStreamSize >= SIZE_FOR_PROGRESS_FINE) {
    progressStepPercent = 25;
  } else if (totalStreamSize >= SIZE_FOR_PROGRESS_HEARTBEAT) {
    progressStepPercent = 50;
  }

  const uint32_t popupFreeHeap = ESP.getFreeHeap();
  const uint32_t popupContigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  progressUiEnabled =
      popupFreeHeap >= MIN_FREE_HEAP_FOR_INDEXING_POPUP && popupContigHeap >= MIN_CONTIG_HEAP_FOR_INDEXING_POPUP;
  if (!progressUiEnabled) {
    LOG_DBG("EHP", "Skipping indexing popup due to low heap (free=%u contig=%u)", popupFreeHeap, popupContigHeap);
    // When popup is disabled, also disable mid-parse ticks.
    progressStepPercent = 0;
  }

  // Show initial progress popup for files above threshold.
  if (progressFn && progressUiEnabled && totalStreamSize >= MIN_SIZE_FOR_POPUP) {
    progressFn(0);
  }
  return true;
}

size_t ChapterHtmlSlimParser::write(const uint8_t data) { return write(&data, 1); }

size_t ChapterHtmlSlimParser::write(const uint8_t* buffer, const size_t size) {
  if (size == 0) return 0;
  if (!saxParser_.isActive() || streamFailed) return 0;

  bytesStreamed += size;
  // The streaming source doesn't know "this was the last chunk" — pass isFinal=false
  // here and let finalize() emit the terminating empty parse with isFinal=true.
  if (!saxParser_.feed(buffer, size)) {
    LOG_ERR("EHP", "Parse error at line %d:\n%s", saxParser_.errorLine(), saxParser_.errorString());
    streamFailed = true;
    return 0;
  }

  // Report progress at the granularity chosen up-front (see progressStepPercent).
  // Skip the 100% callback — the page render that follows immediately replaces the popup,
  // so the final tick is wasted work.
  if (progressFn && progressUiEnabled && progressStepPercent > 0 && totalStreamSize > 0) {
    const int progress = static_cast<int>(bytesStreamed * 100 / totalStreamSize);
    if (progress < 100 && progress / progressStepPercent > lastReportedProgress / progressStepPercent) {
      lastReportedProgress = progress;
      progressFn(progress);
    }
  }

  return size;
}

bool ChapterHtmlSlimParser::finalize() {
  bool success = !streamFailed;
  if (saxParser_.isActive()) {
    // Emit terminating empty parse so the parser finalizes any pending tokens.
    if (success && !saxParser_.finalize()) {
      LOG_ERR("EHP", "Parse error at line %d (finalize):\n%s", saxParser_.errorLine(), saxParser_.errorString());
      success = false;
      streamFailed = true;
    }
  }

  const uint32_t totalTimeMs = millis() - streamStartTimeMs;
  LOG_DBG("EHP", "Time to parse and build pages: %lu ms", totalTimeMs);

  // Process last page if there is still text. Done unconditionally so that a partial
  // success scenario still flushes whatever pages were produced.
  if (currentTextBlock) {
    makePages();
    if (!layoutFailed) {
      const bool hasFinalPageContent = currentPage && !currentPage->elements.empty();
      if (!pendingAnchorId.empty()) {
        uint16_t anchorPage = static_cast<uint16_t>(completedPageCount);
        // Avoid mapping trailing anchors to a non-existent blank page when the
        // chapter ended exactly on a page boundary.
        if (!hasFinalPageContent && completedPageCount > 0) {
          anchorPage = static_cast<uint16_t>(completedPageCount - 1);
        }
        anchorData.push_back({std::move(pendingAnchorId), anchorPage});
        pendingAnchorId.clear();
      }
      if (hasFinalPageContent) {
        emitPage(0u);  // post-parse: no byte offset available
      }
    }
    currentPage.reset();
    currentTextBlock.reset();
  }

  return success;
}

ParsedText::LineProcessResult ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line,
                                                                   const bool lineEndsWithHyphenatedWord,
                                                                   const bool suppressHyphenationRetry) {
  const float scale = line->getBlockStyle().fontSizeMultiplier;
  const int lineHeight = static_cast<int>(renderer.getLineHeight(fontId) * lineCompression * scale + 0.5f);

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    emitPage(lastBodyChildByteOffset);
  }

  const bool noRoomForAnotherLine =
      currentPageNextY + lineHeight <= viewportHeight && currentPageNextY + (lineHeight * 2) > viewportHeight;
  if (lineEndsWithHyphenatedWord && !suppressHyphenationRetry && noRoomForAnotherLine) {
    const std::string linePreview = buildTextBlockPreview(line);
    LOG_DBG("EHP", "Requesting line rerender without hyphenation to avoid page-break split word: %s",
            linePreview.c_str());
    return ParsedText::LineProcessResult::RetryWithoutHyphenation;
  }

  // Capture first-line flag before incrementing wordsExtractedInBlock.
  const bool isFirstLineOfBlock = (wordsExtractedInBlock == 0);

  // Track cumulative words to assign footnotes to the page containing their anchor
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

  // Apply horizontal left inset (margin + padding) as x position offset.
  // For lines that overlap an active left float zone, also shift right by the zone
  // width so text starts after the image rather than overlapping it.
  // Right-floated zones narrow the line width (handled in widthForLine) but don't shift text left.
  int16_t xOffset = line->getBlockStyle().leftInset();
  {
    const auto& bs = line->getBlockStyle();
    for (int zi = 0; zi < bs.floatZoneCount; ++zi) {
      const auto& z = bs.floatZones[zi];
      if (!z.isRight && currentPageNextY < z.bottom && currentPageNextY + lineHeight > z.top) {
        xOffset = static_cast<int16_t>(xOffset + z.width);
      }
    }
  }
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));

  // On the first line of a block with a deferred inline image, fix the image's
  // yPos so its top aligns with the glyph top of the first text line.
  // PageLine y and image y both use the same coordinate: the line's top edge.
  // Float zones were already pre-corrected in makePages() to the same value.
  if (isFirstLineOfBlock && deferredPageImage_) {
    deferredPageImage_->yPos = static_cast<int16_t>(currentPageNextY);
    deferredPageImage_.reset();
  }

  currentPageNextY += lineHeight;
  return ParsedText::LineProcessResult::Accepted;
}

void ChapterHtmlSlimParser::makePages() {
  if (layoutFailed) {
    currentTextBlock.reset();
    return;
  }

  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  const int lineHeight =
      static_cast<int>(renderer.getLineHeight(fontId) * lineCompression * blockStyle.fontSizeMultiplier + 0.5f);

  // Apply top spacing before the paragraph — skip for continuation fragments
  // (words left over after an intermediate flush): the top margin was already
  // applied before the first set of lines from this logical paragraph.
  if (!currentTextBlock->isContinuation()) {
    if (blockStyle.marginTop > 0) {
      // CSS margin collapsing: gap between adjacent blocks = max(prevMarginBottom, thisMarginTop).
      // lastBlockMarginBottom was already added after the previous block; subtract the overlap.
      const int16_t collapse = std::min(lastBlockMarginBottom, blockStyle.marginTop);
      currentPageNextY += static_cast<int16_t>(blockStyle.marginTop - collapse);
    }
    if (blockStyle.paddingTop > 0) {
      currentPageNextY += blockStyle.paddingTop;
    }
  }
  lastBlockMarginBottom = 0;

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  if (!ensureHeapForTextLayout("paragraph layout")) {
    layoutFailed = true;
    currentTextBlock.reset();
    return;
  }

  // Pre-correct float zone coordinates before line-breaking so widthForLine
  // and the xOffset check in addLineToPage use the same y values.
  // Image top aligns with the line top (currentPageNextY after margin-top).
  const int lineHeightForFloat =
      (blockStyle.floatZoneCount > 0)
          ? static_cast<int>(renderer.getLineHeight(fontId) * lineCompression * blockStyle.fontSizeMultiplier + 0.5f)
          : 0;
  LOG_DBG("EHP", "makePages: floatZoneCount=%d lineHeightForFloat=%d currentPageNextY=%d",
          (int)blockStyle.floatZoneCount, lineHeightForFloat, (int)currentPageNextY);
  if (blockStyle.floatZoneCount > 0) {
    auto& mbs = currentTextBlock->getBlockStyle();
    for (int zi = 0; zi < mbs.floatZoneCount; ++zi) {
      const int imgH = mbs.floatZones[zi].bottom - mbs.floatZones[zi].top;
      mbs.floatZones[zi].top = static_cast<int16_t>(currentPageNextY);
      mbs.floatZones[zi].bottom = static_cast<int16_t>(currentPageNextY + imgH);
    }
  }
  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock, const bool lineEndsWithHyphenatedWord,
             const bool suppressHyphenationRetry) {
        return addLineToPage(textBlock, lineEndsWithHyphenatedWord, suppressHyphenationRetry);
      },
      /*includeLastLine=*/true, static_cast<int16_t>(currentPageNextY), lineHeightForFloat);

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes.clear();
  }

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
    lastBlockMarginBottom = blockStyle.marginBottom;
  } else {
    lastBlockMarginBottom = 0;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior).
  // Suppressed between lines within a <pre> block so code/preformatted text is not
  // double-spaced; the last line of the block is flushed after </pre> is closed and
  // preUntilDepth has already been reset, so it still receives normal paragraph spacing.
  if (extraParagraphSpacing && preUntilDepth == INT_MAX) {
    currentPageNextY += lineHeight / 2;
  }
}

// Guard: minimum free heap before attempting table layout (cell wrapping allocates TextBlock vectors)
static constexpr size_t MIN_FREE_HEAP_FOR_TABLE = 20 * 1024;

void ChapterHtmlSlimParser::emitBufferedTable() {
  if (!currentTable) return;

  if (currentTable->unsupported || currentTable->rows.empty()) {
    LOG_DBG("EHP", "Table unsupported or empty — falling back to paragraph mode");
    emitTableAsParagraphs(*currentTable);
  } else if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_TABLE) {
    LOG_ERR("EHP", "Low heap (%u), falling back to paragraph mode for table", ESP.getFreeHeap());
    emitTableAsParagraphs(*currentTable);
  } else {
    emitTableAsFragments(*currentTable);
  }

  emitDeferredTableImages(*currentTable);
}

void ChapterHtmlSlimParser::emitDeferredTableImages(BufferedTable& table) {
  if (table.deferredImages.empty()) return;

  // Images found inside table cells are emitted sequentially below the table, one per line,
  // scaled to fit the full viewport width (same as any other block image).
  for (auto& img : table.deferredImages) {
    if (img.src.empty()) continue;

    const std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(contentBase + img.src));
    if (!ImageDecoderFactory::isFormatSupported(resolvedPath)) continue;

    ImageDimensions dims = {0, 0};
    bool dimsOk = false;
    if (imageManifest) {
      const ImageManifestEntry* entry = imageManifest->find(resolvedPath);
      if (entry) {
        dims.width = entry->width;
        dims.height = entry->height;
        dimsOk = true;
      }
    }
    if (!dimsOk) {
      dimsOk = ImageDecoderFactory::getDimensionsFromZipEntry(epub->getPath(), resolvedPath, dims);
    }
    if (!dimsOk || dims.width == 0 || dims.height == 0) {
      LOG_DBG("EHP", "Deferred table image: no dims for %s", resolvedPath.c_str());
      continue;
    }

    // Scale to fit viewport, preserving aspect ratio.
    float scale = 1.0f;
    if (static_cast<int>(dims.width) > static_cast<int>(viewportWidth))
      scale = static_cast<float>(viewportWidth) / dims.width;
    if (static_cast<int>(dims.height) * scale > static_cast<int>(viewportHeight))
      scale = static_cast<float>(viewportHeight) / dims.height;
    const int displayWidth = std::max(1, static_cast<int>(dims.width * scale));
    const int displayHeight = std::max(1, static_cast<int>(dims.height * scale));

    std::string ext;
    const size_t extPos = resolvedPath.rfind('.');
    if (extPos != std::string::npos) ext = resolvedPath.substr(extPos);
    const std::string cachedPath = imageBasePath + std::to_string(imageCounter++) + ext;

    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }

    if (!currentPage->elements.empty() && currentPageNextY + displayHeight > viewportHeight) {
      emitPage(lastBodyChildByteOffset);
      if (!currentPage) {
        currentPage.reset(new Page());
        currentPageNextY = 0;
      }
    }

    const int xPos = (viewportWidth - displayWidth) / 2;
    auto imageBlock =
        std::make_shared<ImageBlock>(cachedPath, displayWidth, displayHeight, img.alt, epub->getPath(), resolvedPath);
    currentPage->elements.push_back(std::make_shared<PageImage>(imageBlock, xPos, currentPageNextY));
    currentPageNextY += displayHeight;
    LOG_DBG("EHP", "Deferred table image placed: %s %dx%d", resolvedPath.c_str(), displayWidth, displayHeight);
  }
}

void ChapterHtmlSlimParser::emitTableAsFragments(BufferedTable& table) {
  // Use the pre-computed maxCols which accounts for colspan values (inspired by uxjulia/CrossInk).
  const uint8_t columnCount = table.maxCols > 0 ? table.maxCols : [&]() {
    uint8_t n = 0;
    for (const auto& row : table.rows) n = std::max(n, static_cast<uint8_t>(row.cells.size()));
    return n;
  }();

  if (columnCount == 0 || columnCount > MAX_TABLE_COLS) {
    emitTableAsParagraphs(table);
    return;
  }

  const uint16_t totalWidth = viewportWidth;
  const uint16_t colWidth = totalWidth / columnCount;
  const uint16_t innerColWidth =
      (colWidth > 2 * TABLE_CELL_PADDING) ? static_cast<uint16_t>(colWidth - 2 * TABLE_CELL_PADDING) : 0;
  if (innerColWidth < MIN_COL_INNER_WIDTH) {
    LOG_DBG("EHP", "Table columns too narrow (%u px inner) — falling back to paragraphs", innerColWidth);
    emitTableAsParagraphs(table);
    return;
  }

  const int lineHeight = static_cast<int>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
  const bool hasBorder = table.hasBorder;

  // Pre-wrap all cells and compute row heights.
  // A row whose single cell spans the full table width (colspan == maxCols) is treated as a
  // 1-column fragment so it renders full-width inline with the surrounding grid rather than
  // falling back to paragraph mode. Idea from uxjulia/CrossInk; rewritten for our layout model.
  struct LayoutRow {
    std::vector<TableCell> cells;
    uint16_t height = 0;
    bool isHeaderRow = false;
    uint8_t renderCols = 0;  // 1 for full-width single-cell rows, else columnCount
  };
  std::vector<LayoutRow> layoutRows;
  layoutRows.reserve(table.rows.size());

  for (auto& bufRow : table.rows) {
    // Detect a full-width spanning row: exactly one cell whose colSpan equals the table's column count.
    const bool isFullWidthSpan = bufRow.cells.size() == 1 && bufRow.cells[0].colSpan == columnCount;

    const uint8_t renderCols = isFullWidthSpan ? 1 : columnCount;
    const uint16_t renderColWidth = totalWidth / renderCols;
    const uint16_t renderInnerWidth =
        (renderColWidth > 2 * TABLE_CELL_PADDING) ? static_cast<uint16_t>(renderColWidth - 2 * TABLE_CELL_PADDING) : 0;

    // Any other colspan structure falls back; we only handle full-span or plain cells.
    const bool hasMergedCell = std::any_of(bufRow.cells.begin(), bufRow.cells.end(),
                                           [](const BufferedTableCell& c) { return c.colSpan != 1; });
    if (hasMergedCell && !isFullWidthSpan) {
      LOG_DBG("EHP", "Table row has unsupported colspan — falling back to paragraphs");
      emitTableAsParagraphs(table);
      return;
    }

    LayoutRow lr;
    lr.isHeaderRow = bufRow.isHeaderRow;
    lr.renderCols = renderCols;
    lr.cells.reserve(renderCols);
    uint16_t maxLines = 0;

    for (auto& bufCell : bufRow.cells) {
      TableCell cell;
      cell.isHeader = bufCell.isHeader;

      if (bufCell.text && !bufCell.text->isEmpty()) {
        bufCell.text->layoutAndExtractLines(renderer, fontId, renderInnerWidth,
                                            [&cell](const std::shared_ptr<TextBlock>& tb, bool, bool) {
                                              if (cell.lines.size() < MAX_CELL_LINES) {
                                                cell.lines.push_back(tb);
                                              }
                                              return ParsedText::LineProcessResult::Accepted;
                                            });
      }

      if (cell.lines.size() > maxLines) {
        maxLines = static_cast<uint16_t>(cell.lines.size());
      }
      lr.cells.push_back(std::move(cell));
    }

    // Pad non-spanning rows that have fewer cells than columnCount with empty cells.
    if (!isFullWidthSpan) {
      while (lr.cells.size() < columnCount) {
        lr.cells.emplace_back();
      }
    }

    lr.height = static_cast<uint16_t>(maxLines * lineHeight + 2 * TABLE_CELL_PADDING);
    if (lr.height == 0) lr.height = static_cast<uint16_t>(lineHeight + 2 * TABLE_CELL_PADDING);
    layoutRows.push_back(std::move(lr));
  }

  // Ensure page is initialised.
  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Greedily pack rows into fragments, page-breaking between fragments.
  // Rows with a different renderCols than the pending fragment flush it first, since each
  // PageTableFragment has a single fixed column count.
  std::vector<TableRow> fragmentRows;
  uint16_t fragmentHeight = 0;
  uint8_t fragmentCols = 0;

  auto emitFragment = [&]() {
    if (fragmentRows.empty()) return;

    // When bordered: outer drawRect covers top+bottom; inter-row separators (+1 per row) are already
    // in fragmentHeight; add 1 for the bottom border. When borderless: fragmentHeight is exact.
    const uint16_t fragTotalHeight =
        hasBorder ? static_cast<uint16_t>(fragmentHeight + 1) : static_cast<uint16_t>(fragmentHeight);

    if (currentPageNextY + fragTotalHeight > viewportHeight && currentPageNextY > 0) {
      emitPage(lastBodyChildByteOffset);
    }

    currentPage->elements.push_back(
        std::make_shared<PageTableFragment>(fragmentCols, totalWidth, fragTotalHeight, std::move(fragmentRows),
                                            /*xPos=*/0, /*yPos=*/static_cast<int16_t>(currentPageNextY), hasBorder));
    currentPageNextY += fragTotalHeight;
    fragmentRows.clear();
    fragmentHeight = 0;
    fragmentCols = 0;
  };

  for (auto& lr : layoutRows) {
    if (lr.height > viewportHeight) {
      emitFragment();
      BufferedTable singleRowFallback;
      BufferedTableRow fbRow;
      fbRow.isHeaderRow = lr.isHeaderRow;
      for (auto& cell : lr.cells) {
        BufferedTableCell fbc;
        fbc.isHeader = cell.isHeader;
        fbc.text = std::unique_ptr<ParsedText>(new ParsedText(false, false));
        for (const auto& line : cell.lines) {
          for (const auto& word : line->getWords()) {
            fbc.text->addWord(word, EpdFontFamily::REGULAR, false, false);
          }
        }
        fbRow.cells.push_back(std::move(fbc));
      }
      singleRowFallback.rows.push_back(std::move(fbRow));
      emitTableAsParagraphs(singleRowFallback);
      continue;
    }

    // A change in column count requires a new fragment.
    if (!fragmentRows.empty() && lr.renderCols != fragmentCols) {
      emitFragment();
    }

    if (fragmentCols == 0) fragmentCols = lr.renderCols;

    const uint16_t rowContrib = hasBorder ? static_cast<uint16_t>(lr.height + 1) : lr.height;

    if (!fragmentRows.empty() && currentPageNextY + fragmentHeight + rowContrib > viewportHeight) {
      emitFragment();
      fragmentCols = lr.renderCols;
    }

    TableRow tr;
    tr.isHeaderRow = lr.isHeaderRow;
    tr.height = lr.height;
    tr.cells = std::move(lr.cells);
    fragmentRows.push_back(std::move(tr));
    fragmentHeight += rowContrib;
  }

  emitFragment();
}

void ChapterHtmlSlimParser::emitTableAsParagraphs(BufferedTable& table) {
  // Emit each cell as a sequential paragraph (content-preserving fallback)
  for (auto& row : table.rows) {
    for (auto& cell : row.cells) {
      if (!cell.text || cell.text->isEmpty()) continue;
      auto cellBlockStyle = BlockStyle();
      cellBlockStyle.textAlignDefined = true;
      cellBlockStyle.alignment = (paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                     ? CssTextAlign::Justify
                                     : static_cast<CssTextAlign>(paragraphAlignment);
      // Re-use the existing paragraph pipeline by moving the cell text into currentTextBlock
      startNewTextBlock(cellBlockStyle);
      // Transfer words from the buffered cell text into the new currentTextBlock
      // by re-running layout directly
      cell.text->layoutAndExtractLines(
          renderer, fontId, viewportWidth,
          [this](const std::shared_ptr<TextBlock>& tb, bool lineEndsWithHyphen, bool suppressRetry) {
            return addLineToPage(tb, lineEndsWithHyphen, suppressRetry);
          });
    }
  }
}
