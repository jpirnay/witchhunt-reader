#include "BodySize.h"

#include <BuildArena.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SaxParser/SaxParser.h>

#include <algorithm>
#include <cstdlib>

#include "../Epub.h"
#include "Page.h"
#include "ParsedText.h"
#include "css/CssParser.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace BodySize {
namespace {

// kVersion, the body size in percent (0: the last attempt failed), the failed attempts so far.
constexpr char kFile[] = "/bodysize.bin";
constexpr size_t kFileBytes = 3;
// A read is halved to leave its inflate ring room in the lent region beside the rules, down to this.
constexpr size_t kMinReadBytes = 16 * 1024;

struct Record {
  bool present = false;
  uint8_t pct = 0;
  uint8_t attempts = 0;
};

Record readRecord(const std::string& cachePath) {
  Record record;
  const std::string path = cachePath + kFile;
  std::string bytes;
  // Checked first: a book not measured yet has none, and a failed open logs.
  if (!Storage.exists(path.c_str()) || !Storage.readFileToString("BSZ", path, kFileBytes, bytes) ||
      bytes.size() != kFileBytes || bytes[0] != static_cast<char>(kVersion)) {
    return record;
  }
  record.present = true;
  record.pct = static_cast<uint8_t>(bytes[1]);
  record.attempts = static_cast<uint8_t>(bytes[2]);
  return record;
}

void writeRecord(const std::string& cachePath, const uint8_t pct, const uint8_t attempts) {
  const char bytes[kFileBytes] = {static_cast<char>(kVersion), static_cast<char>(pct), static_cast<char>(attempts)};
  const std::string path = cachePath + kFile;
  HalFile file;
  if (!Storage.openFileForWrite("BSZ", path, file)) return;
  const bool written = file.write(bytes, sizeof bytes) == sizeof bytes;
  if (!file.close() || !written) Storage.remove(path.c_str());
}

// Passes the item on to the parser, counting what was inflated of it.
class CountingSink final : public Print {
 public:
  explicit CountingSink(Print& out) : out_(out) {}
  size_t write(const uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, const size_t n) override {
    bytes_ += n;
    return out_.write(buf, n);
  }
  size_t bytes() const { return bytes_; }

 private:
  Print& out_;
  size_t bytes_ = 0;
};

}  // namespace

uint8_t bodySizeOf(const Census& census) {
  if (census.total < kMinChars) return 100;
  int top = 0;
  for (int pct = 1; pct < Census::kBuckets; ++pct) {
    if (census.chars[pct] > census.chars[top]) top = pct;
  }
  if (top < ParsedText::MIN_WORD_SIZE_PCT || top > ParsedText::MAX_WORD_SIZE_PCT) return 100;
  if (std::abs(top - 100) <= kNearBodyPct) return 100;
  return static_cast<uint8_t>(top);
}

uint8_t read(const std::string& cachePath) {
  const Record record = readRecord(cachePath);
  if (!record.present) return 0;
  if (record.pct != 0) return record.pct;
  return record.attempts >= kMaxAttempts ? 100 : 0;
}

void forget(const std::string& cachePath) {
  const std::string path = cachePath + kFile;
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
}

Detected detect(const Epub& epub, GfxRenderer& renderer, const int fontId, BuildArena* scratch) {
  Detected found;
  const unsigned long start = millis();
  CssParser* css = epub.getCssParser();
  found.spineIndex = epub.getLargestSpineIndex();
  // No item with any text in it: nothing to measure, and nothing to normalize to.
  if (!css || found.spineIndex < 0) return found;
  size_t itemBytes = 0;
  epub.getSpineItemInflatedSize(found.spineIndex, &itemBytes);
  // ~1 KB of counts. Not on the stack, which the open shares with the popup's drawing.
  auto census = makeUniqueNoThrow<Census>();
  if (!census) {
    LOG_ERR("BSZ", "OOM: census");
    found.failed = true;
    return found;
  }

  // The rules as a section build has them: lean (the parser's memo stands in for the hot rule cache), in the lent
  // region in a block of their own below the parser state's and the read's (BuildArena is LIFO), given back last. Only
  // while the parser state and the read still fit beside them: a read that left the region would take its inflate
  // ring, up to 32 KB, from the heap. The read is made shorter to fit, down to kMinReadBytes; past that, or when they
  // do not load, the rules stay on the card and are looked up there.
  const bool lean = css->leanResolve();
  css->setLeanResolve(true);
  css->resetResolveStats();
  const bool lent = scratch != nullptr && scratch->valid();
  size_t cap = kMaxReadBytes;
  // The parser state, and the read of an item of `itemBytes` (0: not known) cut at `cap`.
  const auto parseBytes = [itemBytes](const size_t cap) {
    const size_t readBytes = itemBytes != 0 && itemBytes < cap ? itemBytes : cap;
    return SaxParser::stateBytes() + alignof(std::max_align_t) + Epub::spineReadScratchBytes(readBytes);
  };
  BuildArena::Block rules;
  bool resident = lent;
  if (resident) {
    rules = scratch->reserveBlock();
    css->setIndexArena(scratch);
    const size_t room = css->loadFromCache() ? scratch->capacity() - scratch->used() : 0;
    while (cap > kMinReadBytes && parseBytes(cap) > room) cap = std::max(cap / 2, kMinReadBytes);
    if (parseBytes(cap) > room) {
      css->setIndexArena(nullptr);
      css->clearCaches(/*evictEverything=*/true);  // the views into the region go
      scratch->release(rules);
      resident = false;
    }
  }

  bool itemRead = false;
  {
    // The parser's SAX state is a plain allocation in the region (setBuildArena), given back with this block.
    BuildArena::Block parse;
    if (lent) parse = scratch->reserveBlock();
    {
      // The chapter parser as a build runs it, but for the census: the book's styles on, the wide band of Normalize
      // Font Size (so text within it of the body counts as body text, as it is set), images suppressed (no header is
      // read, an image's block is reset as a placed one's is), no page sizes needed. The metrics serve nothing here.
      auto parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
          std::shared_ptr<Epub>(), renderer, fontId, /*lineCompression=*/1.0f, /*extraParagraphSpacing=*/false,
          /*paragraphAlignment=*/0, /*viewportWidth=*/480, /*viewportHeight=*/800, /*hyphenationEnabled=*/false,
          /*fontSizeNormalization=*/true, /*bionicReadingEnabled=*/false, [](std::unique_ptr<Page>) { return true; },
          /*embeddedStyle=*/true, /*contentBase=*/std::string(),
          /*imageBasePath=*/std::string(), /*imageRendering=*/2, std::vector<std::string>(), nullptr, css, nullptr);
      if (!parser) {
        LOG_ERR("BSZ", "OOM: parser");
      } else {
        parser->setSizeCensus(census.get());
        if (lent) parser->setBuildArena(scratch);
        if (parser->setup(itemBytes)) {
          // A document cut at the cap is not finalized: its words were counted as they came, and what is left open
          // would only be reported as a parse error.
          CountingSink counted(*parser);
          itemRead = epub.readSpineItemToStream(found.spineIndex, counted, nullptr, lent ? scratch : nullptr, cap);
          found.bytesRead = counted.bytes();
        }
      }
    }
    if (parse.valid()) scratch->release(parse);
  }

  // Short of heap, the resolver skips the rules it would read from the card: the sizes counted would be wrong.
  const bool missedRules = css->getResolveStats().lowHeapSkips != 0 && !css->isArenaResident();
  if (resident) css->setIndexArena(nullptr);
  css->clearCaches(/*evictEverything=*/true);  // the resolver's caches go back to the heap, the views into the region
  if (resident) scratch->release(rules);
  css->setLeanResolve(lean);

  found.chars = census->total;
  for (int pct = 0; pct < Census::kBuckets; ++pct) {
    if (census->chars[pct] > found.bodyChars) {
      found.bodyChars = census->chars[pct];
      found.topPct = pct;
    }
  }
  found.pct = bodySizeOf(*census);
  found.failed = missedRules || (!itemRead && census->total == 0);
  found.ms = static_cast<uint32_t>(millis() - start);
  if (missedRules) LOG_ERR("BSZ", "Low heap: the census missed CSS rules");
  return found;
}

uint8_t prepare(const Epub& epub, GfxRenderer& renderer, const int fontId, BuildArena* scratch) {
  const std::string& cachePath = epub.getCachePath();
  // The CSS was not loaded, or did not compile: nothing to measure from yet.
  if (!epub.getCssParser() || !epub.getCssParser()->cacheIsCurrent()) return read(cachePath);
  const Record last = readRecord(cachePath);
  const uint8_t attempts = last.present && last.pct == 0 ? last.attempts : 0;
  const Detected found = detect(epub, renderer, fontId, scratch);
  if (found.failed) {
    LOG_ERR("BSZ", "Body size not measured (attempt %u of %u)", static_cast<unsigned>(attempts + 1),
            static_cast<unsigned>(kMaxAttempts));
    writeRecord(cachePath, 0, static_cast<uint8_t>(attempts + 1));
  } else {
    LOG_INF("BSZ", "Body size %u%%: spine %d, %lu of %lu characters at %d%%, %u bytes read, %lu ms",
            static_cast<unsigned>(found.pct), found.spineIndex, static_cast<unsigned long>(found.bodyChars),
            static_cast<unsigned long>(found.chars), found.topPct, static_cast<unsigned>(found.bytesRead),
            static_cast<unsigned long>(found.ms));
    writeRecord(cachePath, found.pct, 0);
  }
  return read(cachePath);
}

}  // namespace BodySize
