#include "BookFonts.h"

#include <BuildArena.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SaxParser/SaxFeedSink.h>
#include <SaxParser/SaxParser.h>
#include <strings.h>
#include <wolfssl/wolfcrypt/hash.h>  // wc_ShaHash: wolfCrypt is linked for TLS already

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../Epub.h"
#include "VisibleText.h"
#include "css/CssParser.h"
#include "parsers/ContentOpfParser.h"

namespace BookFonts {
namespace {

constexpr char kFontsDir[] = "/fonts";
constexpr char kAccentDir[] = "/fonts/accent0";  // the digit is the accent family's
constexpr char kStateFile[] = "/fonts.bin";      // beside fonts/, which prepare() empties
// The accent limit (accentLimit()): kVersion, the settings key as stored, the accent families kept.
constexpr char kLimitFile[] = "/fontlimit.bin";
constexpr size_t kLimitBytes = 2 + sizeof(uint32_t);
// The state file: kVersion, then a code -- None, Family (then the name, to the end of the file), or kFailed + n
// for n failed attempts so far. At kMaxAttempts the book is given up on, as None.
constexpr uint8_t kCodeNone = 0;
constexpr uint8_t kCodeFamily = 1;
constexpr uint8_t kCodeFailed = 2;
constexpr uint8_t kMaxAttempts = 3;

// The IDPF algorithm obfuscates a font's first 1040 bytes, Adobe's the first 1024 (EPUB OCF 3.0, section 4;
// Adobe's "Font Mangling").
constexpr size_t kIdpfBytes = 1040;
constexpr size_t kAdobeBytes = 1024;

// ---- Which family the body text is set in ----------------------------------------------------------------
//
// Based on crosspoint-reader PR #3934's BodyFontProbe (Julia Nguyen, "fix: preserve reader fonts in
// accents-only mode"), reworked for one answer per book over several chapters: the family most of the text is
// set in, an inline accent's text counting for the paragraph (p, div, li, dd) around it. Measured on 921 real
// books with @font-face rules, against the family each draws most of its text in with every chapter styled by
// the sheets it links: right for 839. #3934's further rules made it worse here, at 807: html's or body's
// family first (this parser merges a book's sheets, and a marketing page's `body` rule then names a family no
// chapter uses) and centred or right-aligned text passed over. Headings, tables and hidden text left out
// changed one book.
//
// The same pass counts the text each family is set in, by style, for the accent families: what #3934 (Justin
// Mitchell) draws in its "Ignore Body Font" mode, the book's families other than the body's, here ranked by their
// text. Inline text counts for its own family, as the reader gives a word in another family its face (a letter filled
// in by hand inside a typed word); a block whose text is in one family counts as kBlockMinBytes at least, as a heading
// is drawn in its family at every chapter's start. Counting blocks instead ranked an ad page's four lines above a
// typed manuscript, and inline families not at all.
class BodyFontProbe {
 public:
  explicit BodyFontProbe(CssParser& css) : css_(css) {}

  // A spine item, read up to `cap` bytes, its counts weighted by `weight256` (probeWeight256): the element frames
  // start over; the counts carry on.
  void beginParser(SaxParser& parser, const uint32_t weight256, const size_t cap) {
    parser_ = &parser;
    depth_ = 0;
    weight256_ = weight256;
    cap_ = cap;
  }
  // The family with the most text (by identity: a family named with and without a generic fallback is one), with
  // the generic fallback most of that text names; 0 (the reader's font) without any.
  uint32_t family() const {
    const Usage* best = &usage_[0];
    uint32_t bestBytes = 0;
    for (const Usage& u : usage_) {
      const uint32_t identity = CssFontFamily::identity(u.family);
      uint32_t bytes = 0;
      for (const Usage& v : usage_) {
        if (CssFontFamily::identity(v.family) == identity) bytes += v.bytes;
      }
      if (bytes > bestBytes ||
          (bytes == bestBytes && identity == CssFontFamily::identity(best->family) && u.bytes > best->bytes)) {
        best = &u;
        bestBytes = bytes;
      }
    }
    return best->family;
  }
  // The accent families (Publisher Fonts' Accents) into `found`: of the families text is set in, other than the body's
  // and the reader's, those `usable` takes and whose text is not negligible, the one with the most text first (the
  // first seen on a tie); their identities, 0 past the last. With the styles of the body's text and theirs. Each
  // usable family left out as negligible goes to `leftOut` with its text.
  template <typename Usable, typename LeftOut>
  void accents(Usable usable, LeftOut leftOut, Found& found) const {
    const uint32_t body = CssFontFamily::identity(found.body);
    uint64_t total = 0;
    for (size_t i = 0; i < styledCount_; ++i) total += score(styled_[i]);
    const auto negligible = [total](const uint32_t bytes) {
      return bytes < kMinFamilyBytes && uint64_t{bytes} * 1000 < total * kMinFamilyPerMille;
    };
    for (size_t i = 0; i < styledCount_; ++i) {
      const Styled& s = styled_[i];
      if (s.family == body) std::copy_n(s.bytes, 4, found.bodyStyles);
      if (s.family != 0 && s.family != body && negligible(score(s)) && usable(s.family)) leftOut(s.family, score(s));
    }
    for (int n = 0; n < kMaxAccents; ++n) {
      const Styled* best = nullptr;
      for (size_t i = 0; i < styledCount_; ++i) {
        const Styled& s = styled_[i];
        const uint32_t bytes = score(s);
        if ((best && bytes <= score(*best)) || s.family == 0 || s.family == body ||
            std::find(found.accents, found.accents + n, s.family) != found.accents + n || negligible(bytes) ||
            !usable(s.family)) {
          continue;
        }
        best = &s;
      }
      if (!best) break;
      found.accents[n] = best->family;
      std::copy_n(best->bytes, 4, found.accentStyles[n]);
    }
  }

  static void onStart(void* ctx, const char* name, const char** atts) {
    auto& self = *static_cast<BodyFontProbe*>(ctx);
    if (self.stopAtLimit()) return;
    const size_t index = self.depth_++;
    if (index >= kMaxDepth) return;
    const char* classes = "";
    const char* id = "";
    const char* inlineStyle = nullptr;
    for (size_t i = 0; atts && atts[i] && atts[i + 1]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classes = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        id = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        inlineStyle = atts[i + 1];
      }
    }
    Frame& frame = self.frames_[index];
    frame = index == 0 ? Frame{} : self.frames_[index - 1];
    // Bold and italic as the chapter parser reads them; a CSS weight or style the element defines has the last word.
    const bool heading = (name[0] | 0x20) == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0';
    if (heading || strcasecmp(name, "b") == 0 || strcasecmp(name, "strong") == 0) frame.style |= kBold;
    if (strcasecmp(name, "i") == 0 || strcasecmp(name, "em") == 0) frame.style |= kItalic;
    self.tag_.assign(name);
    self.class_.assign(classes);
    self.id_.assign(self.css_.hasIdSelectors() ? id : "");
    frame.take(self.css_.resolveStyle(self.tag_, self.class_, self.id_));
    if (inlineStyle) {  // the style attribute has the last word
      self.inline_.assign(inlineStyle);
      frame.take(CssParser::parseInlineStyle(self.inline_));
    }
    frame.inBody = frame.inBody || strcasecmp(name, "body") == 0;
    const bool paragraph = strcasecmp(name, "p") == 0 || strcasecmp(name, "div") == 0 || strcasecmp(name, "li") == 0 ||
                           strcasecmp(name, "dd") == 0;
    if (paragraph) {
      frame.paragraphFamily = frame.family;
      frame.inParagraph = true;
    }
    // A block, which the reader lays out in one family: its text's when all of it is in one, else its own.
    if (paragraph || heading || strcasecmp(name, "blockquote") == 0 || strcasecmp(name, "pre") == 0) {
      frame.block = static_cast<int8_t>(index);
      frame.textState = 0;
      frame.blockBytes = 0;
    }
  }

  static void onEnd(void* ctx, const char*) {
    auto& self = *static_cast<BodyFontProbe*>(ctx);
    if (self.depth_ == 0) return;
    const size_t index = --self.depth_;
    if (index >= kMaxDepth) return;
    const Frame& frame = self.frames_[index];
    // A short block in one family: its shortfall to kBlockMinBytes, in the style its text starts in.
    if (frame.block != static_cast<int8_t>(index) || frame.textState != 1 || frame.blockBytes >= kBlockMinBytes) return;
    self.addText(frame.textFamily, frame.firstStyle, kBlockMinBytes - frame.blockBytes);
  }

  static void onText(void* ctx, const char* text, const int len) {
    auto& self = *static_cast<BodyFontProbe*>(ctx);
    if (self.stopAtLimit() || self.depth_ == 0 || self.depth_ > kMaxDepth) return;
    const Frame& frame = self.frames_[self.depth_ - 1];
    const auto bytes = static_cast<uint32_t>(VisibleText::visibleBytes(text, len));
    if (!frame.inBody || bytes == 0) return;
    self.count(self.usage_, frame.inParagraph ? frame.paragraphFamily : frame.family, bytes);
    const uint32_t family = CssFontFamily::identity(frame.family);
    self.addText(family, frame.style, bytes);
    if (frame.block < 0) return;
    Frame& block = self.frames_[frame.block];
    if (block.textState == 0) {
      block.textFamily = family;
      block.firstStyle = frame.style;
      block.textState = 1;
    } else if (block.textFamily != family) {
      block.textState = 2;
    }
    block.blockBytes = static_cast<uint16_t>(std::min<uint32_t>(block.blockBytes + bytes, kBlockMinBytes));
  }

  // An entity the parser passes through (&nbsp; and the like) is one character of text. Without this handler
  // such an entity, undeclared in an XHTML file, would end the parse.
  static void onDefault(void* ctx, const char* text, const int len) {
    if (len >= 3 && text[0] == '&' && text[len - 1] == ';') onText(ctx, "x", 1);
  }

 private:
  static constexpr size_t kMaxDepth = 24;
  // Every face's family, and none; a family named with and without a generic fallback counts twice.
  static constexpr size_t kMaxFamilies = CssParser::MAX_FONT_FACES + 4;
  // EpdFontFamily's style bits.
  static constexpr uint8_t kBold = 1;
  static constexpr uint8_t kItalic = 2;
  struct Frame {
    uint32_t family = 0;           // font-family in effect, inherited
    uint32_t paragraphFamily = 0;  // the family of the paragraph the element is in
    uint32_t textFamily = 0;       // a block's: the family (identity) of its text so far
    bool inBody = false;
    bool inParagraph = false;
    int8_t block = -1;        // the frame of the block the element is in
    uint8_t textState = 0;    // a block's: 0 no text yet, 1 all of it in textFamily, 2 in several families
    uint8_t style = 0;        // bold and italic in effect, inherited (kBold, kItalic)
    uint8_t firstStyle = 0;   // a block's: the style its text starts in
    uint16_t blockBytes = 0;  // a block's: its text so far, up to kBlockMinBytes
    void take(const CssStyle& css) {
      if (css.defined.fontFamily) family = css.fontFamily;
      if (css.hasFontWeight()) style = (style & ~kBold) | (css.fontWeight == CssFontWeight::Bold ? kBold : 0);
      if (css.hasFontStyle()) style = (style & ~kItalic) | (css.fontStyle == CssFontStyle::Italic ? kItalic : 0);
    }
  };
  struct Usage {
    uint32_t family = 0;
    uint32_t bytes = 0;
  };
  using Usages = std::array<Usage, kMaxFamilies>;
  // A family's text (by identity), by style.
  struct Styled {
    uint32_t family = 0;
    uint32_t bytes[4] = {};
  };

  static uint32_t score(const Styled& s) {
    uint64_t total = 0;
    for (const uint32_t bytes : s.bytes) total += bytes;
    return static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX));
  }

  // Adds `n`, weighted, to `family`'s text in `style`; a family past the table's room is not counted.
  void addText(const uint32_t family, const uint8_t style, const uint32_t n) {
    size_t i = 0;
    while (i < styledCount_ && styled_[i].family != family) ++i;
    if (i == styled_.size()) return;
    if (i == styledCount_) styled_[styledCount_++].family = family;
    styled_[i].bytes[style] = addWeighted(styled_[i].bytes[style], n, weight256_);
  }

  // Adds `n`, weighted, to `family`'s count; a family past the table's room is not counted.
  void count(Usages& usages, const uint32_t family, const uint32_t n) {
    for (Usage& u : usages) {
      if (u.bytes == 0 || u.family == family) {
        u.family = family;
        u.bytes = addWeighted(u.bytes, n, weight256_);
        return;
      }
    }
  }

  // Past the item's cap: stops the parser.
  bool stopAtLimit() const {
    if (parser_->byteOffset() < cap_) return false;
    parser_->stop();
    return true;
  }

  CssParser& css_;
  SaxParser* parser_ = nullptr;
  uint32_t weight256_ = 256;
  size_t cap_ = 0;
  std::array<Frame, kMaxDepth> frames_{};
  Usages usage_{};                             // text bytes by family, an inline accent's counting for its paragraph's
  std::array<Styled, kMaxFamilies> styled_{};  // text by family identity and style, in the order first seen
  size_t styledCount_ = 0;
  size_t depth_ = 0;
  // resolveStyle()'s arguments, reused so an element costs no allocation once they have grown.
  std::string tag_, class_, id_, inline_;
};

// A block of `scratch` holding one parser state for the passes, released when it goes.
class ParserState {
 public:
  explicit ParserState(BuildArena* scratch) : scratch_(scratch) {
    if (!scratch_ || !scratch_->valid()) return;
    block_ = scratch_->reserveBlock();
    state_ = scratch_->alloc(SaxParser::stateBytes());
  }
  ~ParserState() {
    if (block_.valid()) scratch_->release(block_);
  }
  ParserState(const ParserState&) = delete;
  ParserState& operator=(const ParserState&) = delete;
  void* get() const { return state_; }

 private:
  BuildArena* scratch_;
  BuildArena::Block block_;
  void* state_ = nullptr;
};

// ---- The font files --------------------------------------------------------------------------------------

// Whether the font's first four bytes, XORed with `key`'s first four when it is set, are an outline font's
// signature: TrueType (0x00010000, or 'true' from Apple) or CFF ('OTTO'). Anything else is not drawn: WOFF, WOFF2,
// a collection, or a font encrypted rather than obfuscated.
bool isOutlineFont(const uint8_t (&sig)[4], const uint8_t* key) {
  uint32_t tag = 0;
  for (int i = 0; i < 4; ++i) tag = tag << 8 | static_cast<uint8_t>(sig[i] ^ (key ? key[i] : 0));
  return tag == 0x00010000u || tag == 0x74727565u || tag == 0x4F54544Fu;
}

// The book's two obfuscation keys, read from the OPF the first time an obfuscated face needs them.
struct Keys {
  bool loaded = false;
  bool hasIdpf = false;
  bool hasAdobe = false;
  uint8_t idpf[20] = {};
  uint8_t adobe[16] = {};
};

bool loadKeys(const Epub& epub, BuildArena* scratch, Keys& keys) {
  keys.loaded = true;
  std::string opfPath;
  size_t opfSize = 0;
  if (!epub.contentOpfPath(&opfPath) || !epub.getItemSize(opfPath, &opfSize)) return false;
  // Released before the face is extracted, whose inflate ring needs the room; after the parser, which uses it.
  const ParserState state(scratch);
  // The package parser with no cache, as for a metadata sidecar: the metadata alone. About 1 KB, so on the heap
  // rather than in prepare()'s frame, under the probe and the copies. It holds the paths by reference.
  const std::string none;
  auto opf = makeUniqueNoThrow<ContentOpfParser>(none, none, opfSize, nullptr);
  if (opf && state.get()) opf->setParserState(state.get());
  if (!opf || !opf->setup() || !epub.readItemContentsToStream(opfPath, *opf, 1024, opf->metadataRead(), scratch)) {
    LOG_ERR("BKF", "Cannot read %s", opfPath.c_str());  // or no heap for the parser
    return false;
  }
  keys.hasIdpf = idpfKey(std::move(opf->uniqueIdentifier), keys.idpf);
  keys.hasAdobe = adobeKey(opf->uuidIdentifier, keys.adobe);
  return true;
}

// XORs the first `count` bytes of the file at `path` with `key` (`keyLen` bytes, repeated), in place.
bool xorPrefix(const std::string& path, const uint8_t* key, const size_t keyLen, const size_t count) {
  HalFile file;
  if (!Storage.openFileForUpdate("BKF", path, file)) return false;
  uint8_t buf[64];
  bool ok = true;
  for (size_t at = 0; ok && at < count;) {
    const int got = file.read(buf, std::min(sizeof(buf), count - at));
    if (got <= 0) break;  // a font shorter than the obfuscated prefix
    for (int i = 0; i < got; ++i) buf[i] ^= key[(at + static_cast<size_t>(i)) % keyLen];
    ok = file.seekSet(at) && file.write(buf, static_cast<size_t>(got)) == static_cast<size_t>(got);
    at += static_cast<size_t>(got);
    ok = ok && file.seekSet(at);  // a read after a write needs a seek between, to stdio on Windows
  }
  return file.close() && ok;
}

enum class Copy : uint8_t { Done, Refused, Failed };

// Copies one face to `dest` as a plain font file (TrueType or CFF), its obfuscation undone. Refused: not a font the TTF
// engine draws (logged). Failed: a read or a write went wrong, which may pass.
Copy copyFace(const Epub& epub, const std::string& href, const std::string& dest, Keys& keys, BuildArena* scratch) {
  size_t size = 0;
  if (!epub.getItemSize(href, &size) || size < 12 || size > kMaxFaceBytes) {
    LOG_INF("BKF", "%s: missing or %u bytes", href.c_str(), static_cast<unsigned>(size));
    return Copy::Refused;
  }
  uint8_t sig[4] = {};
  if (epub.readItemHeaderBytes(href, sig, sizeof(sig)) != sizeof(sig)) return Copy::Failed;
  const uint8_t* key = nullptr;
  size_t keyLen = 0;
  size_t obfuscated = 0;
  if (!isOutlineFont(sig, nullptr)) {
    // An obfuscated font starts with its first bytes XORed; a key that gives the signature back is the one.
    // Testing the signature replaces reading META-INF/encryption.xml: an algorithm it names that is real
    // encryption gives no signature back, and the face is refused.
    if (!keys.loaded && !loadKeys(epub, scratch, keys)) return Copy::Failed;
    if (keys.hasIdpf && isOutlineFont(sig, keys.idpf)) {
      key = keys.idpf;
      keyLen = sizeof(keys.idpf);
      obfuscated = kIdpfBytes;
    } else if (keys.hasAdobe && isOutlineFont(sig, keys.adobe)) {
      key = keys.adobe;
      keyLen = sizeof(keys.adobe);
      obfuscated = kAdobeBytes;
    } else {
      // The signature tells a support log what it is: 774f4646 WOFF, 774f4632 WOFF2.
      LOG_INF("BKF", "%s: not TrueType or CFF (%02x%02x%02x%02x)", href.c_str(), sig[0], sig[1], sig[2], sig[3]);
      return Copy::Refused;
    }
  }
  if (!epub.extractItemToFile(href, dest, scratch)) return Copy::Failed;
  if (key && !xorPrefix(dest, key, keyLen, obfuscated)) {
    Storage.remove(dest.c_str());
    return Copy::Failed;
  }
  return Copy::Done;
}

// For each style (EpdFontFamily numbering: bit 0 bold, bit 1 italic), the face of `family` nearest weight 400,
// or 700 for the bold styles; one with an empty name where the family has none.
void pickFaces(std::string_view records, const uint32_t family, CssParser::FontFace (&out)[4]) {
  for (CssParser::FontFace face; CssParser::nextFontFace(records, face);) {
    if (face.family != family) continue;
    const uint8_t style = (face.weight >= 600 ? 1 : 0) | (face.italic ? 2 : 0);
    const int target = (style & 1) != 0 ? 700 : 400;
    CssParser::FontFace& best = out[style];
    if (best.name.empty() || std::abs(face.weight - target) < std::abs(best.weight - target)) best = face;
  }
}

// The name the book declares `family` under, its first face's, for the log.
std::string_view familyName(std::string_view records, const uint32_t family) {
  for (CssParser::FontFace face; CssParser::nextFontFace(records, face);) {
    if (face.family == family) return face.name;
  }
  return "?";
}

// A family without a regular face to copy (a script declared italic only, a display face bold only, an italic that
// carries the family's text) draws in its nearest: that face becomes the regular, and the styles it leaves fall back
// to it. Moved, not copied, so the store holds it once.
void moveNearestToRegular(CssParser::FontFace (&out)[4]) {
  if (out[0].name.empty()) {
    for (const uint8_t style : {2, 1, 3}) {
      if (out[style].name.empty()) continue;
      out[0] = out[style];
      out[style] = CssParser::FontFace{};
      break;
    }
  }
}

// The state file's code, and a family's name (see read()); kNoCode when there is no file of this kVersion.
constexpr uint8_t kNoCode = 0xFF;
constexpr size_t kHeaderBytes = 2 + sizeof(Families);  // version, code, the body and accent families
static_assert(sizeof(Families) == (1 + kMaxAccents) * sizeof(uint32_t), "the families are stored as they are");
uint8_t readCode(const std::string& cachePath, std::string* name, Families* families, const int accent) {
  const std::string path = cachePath + kStateFile;
  std::string bytes;
  // Checked first: an unexamined book has none, and a failed open logs.
  if (!Storage.exists(path.c_str()) ||
      !Storage.readFileToString("BKF", path, kHeaderBytes + (1 + kMaxAccents) * 256, bytes) ||
      bytes.size() < kHeaderBytes || bytes[0] != static_cast<char>(kVersion)) {
    return kNoCode;
  }
  const auto code = static_cast<uint8_t>(bytes[1]);
  if (families) std::memcpy(families, &bytes[2], sizeof(Families));
  // The names: the body family's (empty unless a Family), then '\0' and each accent family's (c_str() ends each).
  bytes.erase(0, kHeaderBytes);
  if (code == kCodeFamily && bytes.c_str()[0] == '\0') return kNoCode;
  if (name) {
    size_t at = 0;
    for (int skip = accent + 1; skip > 0 && at < bytes.size(); --skip) at += strlen(bytes.c_str() + at) + 1;
    *name = at < bytes.size() ? bytes.c_str() + at : "";
  }
  return code;
}

State stateOf(const uint8_t code) {
  if (code == kCodeFamily) return State::Family;
  if (code == kCodeNone || (code != kNoCode && code >= kCodeFailed + kMaxAttempts)) return State::None;
  return State::Pending;
}

State writeState(const std::string& cachePath, const uint8_t code, const Families& families,
                 const std::string_view names) {
  std::string bytes(kHeaderBytes, '\0');
  bytes[0] = static_cast<char>(kVersion);
  bytes[1] = static_cast<char>(code);
  std::memcpy(&bytes[2], &families, sizeof(families));
  bytes.append(names);
  const std::string path = cachePath + kStateFile;
  HalFile file;
  if (!Storage.openFileForWrite("BKF", path, file)) return State::Pending;
  const bool written = file.write(bytes.data(), bytes.size()) == bytes.size();
  if (!file.close() || !written) {
    Storage.remove(path.c_str());
    return State::Pending;
  }
  return stateOf(code);
}

// Finds the body and accent families and copies their faces out of the book; the state file's code for what came
// of it (kCodeFailed for a failed read). `found` gets the families, `names` their names as the book declares them
// (the body's for a Family, then '\0' and each accent's that was copied), into `records`.
uint8_t examine(const Epub& epub, BuildArena* scratch, std::string& records, Families& found, std::string& names,
                bool& undecided) {
  // A book without faces is probed too: Accents needs its body's generic family (CssFontFamily).
  bool stale = false;
  if (!CssParser::loadFontFaces(epub.getCachePath(), records, &stale)) records.clear();
  if (stale) {  // a faces file of another version: not "no faces", the book is decided once the CSS is compiled again
    undecided = true;
    return kCodeNone;
  }
  bool failed = false;
  const Found families = detectFamilies(epub, scratch, &failed, records);
  if (failed) return kCodeFailed;
  found.body = families.body;
  Keys keys;
  uint8_t code = kCodeNone;
  uint8_t accents = 0;  // copied so far
  // The body family to fonts/, the accent families, the most used first, to fonts/accent0/ on; one refused gives
  // its place to the next.
  for (int i = -1; i < kMaxAccents; ++i) {
    const bool accent = i >= 0;
    CssParser::FontFace picked[4];
    pickFaces(records, accent ? families.accents[i] : CssFontFamily::identity(families.body), picked);
    uint8_t present = 0;
    for (uint8_t style = 0; style < 4; ++style) present |= picked[style].name.empty() ? 0 : 1u << style;
    if (present == 0) continue;  // not embedded
    // The faces of styles with little text are not copied: the engine draws those styles from the others.
    const uint32_t (&scores)[4] = accent ? families.accentStyles[i] : families.bodyStyles;
    const uint8_t kept = keptStyles(scores, present);
    for (uint8_t style = 0; style < 4; ++style) {
      if ((kept & (1u << style)) == 0) picked[style] = CssParser::FontFace{};
    }
    moveNearestToRegular(picked);
    const std::string dir = familyDir(epub.getCachePath(), accent ? accents : -1);
    Storage.mkdir(dir.c_str());
    std::string dest = dir + "/0.ttf";  // the style's digit goes in place of the 0
    Copy copy = Copy::Done;
    for (uint8_t style = 0; style < 4 && copy != Copy::Failed; ++style) {
      if (picked[style].name.empty()) continue;
      dest[dest.size() - 5] = static_cast<char>('0' + style);
      const std::string href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(std::string(picked[style].href)));
      copy = copyFace(epub, href, dest, keys, scratch);
      // A style refused falls back to regular; regular refused, the family keeps the reader's font.
      if (copy == Copy::Refused && style != 0) copy = Copy::Done;
      if (copy == Copy::Refused) break;
    }
    if (copy != Copy::Done) {
      Storage.removeDir(dir.c_str());
      if (copy == Copy::Failed) return kCodeFailed;
      continue;
    }
    LOG_INF("BKF", "%s font %.*s: %lu bytes of text (est.), styles 0x%x", accent ? "Accent" : "Body",
            static_cast<int>(picked[0].name.size()), picked[0].name.data(),
            static_cast<unsigned long>(
                std::min<uint64_t>(uint64_t{scores[0]} + scores[1] + scores[2] + scores[3], UINT32_MAX)),
            kept);
    if (accent) {
      found.accents[accents++] = families.accents[i];
      names.push_back('\0');
    } else {
      code = kCodeFamily;
    }
    names.append(picked[0].name);
  }
  return code;
}

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

// The keys and the obfuscation follow our web Font Manager's extraction (src/network/html/js/epub-fonts.js),
// which decodes the same two algorithms in the browser.
bool idpfKey(std::string uniqueIdentifier, uint8_t (&key)[20]) {
  size_t n = 0;  // the white space is squeezed out in place
  for (const char c : uniqueIdentifier) {
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') uniqueIdentifier[n++] = c;
  }
  return n != 0 && wc_ShaHash(reinterpret_cast<const byte*>(uniqueIdentifier.data()), static_cast<word32>(n), key) == 0;
}

bool adobeKey(const std::string_view uuid, uint8_t (&key)[16]) {
  constexpr std::string_view kPrefix = "urn:uuid:";
  if (uuid.size() < kPrefix.size() || strncasecmp(uuid.data(), kPrefix.data(), kPrefix.size()) != 0) return false;
  size_t bytes = 0;
  int high = -1;
  for (const char c : uuid.substr(kPrefix.size())) {
    if (c == '-') continue;
    const int v = hexValue(c);
    if (v < 0 || bytes == sizeof(key)) return false;
    if (high < 0) {
      high = v;
    } else {
      key[bytes++] = static_cast<uint8_t>((high << 4) | v);
      high = -1;
    }
  }
  return bytes == sizeof(key) && high < 0;
}

State read(const std::string& cachePath, std::string* name, Families* families, const int accent) {
  return stateOf(readCode(cachePath, name, families, accent));
}

uint8_t accentLimit(const std::string& cachePath, const uint32_t settingsKey) {
  const std::string path = cachePath + kLimitFile;
  std::string bytes;
  // Checked first: most books have none, and a failed open logs.
  if (!Storage.exists(path.c_str()) || !Storage.readFileToString("BKF", path, kLimitBytes, bytes)) return kMaxAccents;
  uint32_t key = 0;
  if (bytes.size() == kLimitBytes) std::memcpy(&key, &bytes[1], sizeof key);
  if (bytes.size() != kLimitBytes || bytes[0] != static_cast<char>(kVersion) || key != settingsKey) {
    Storage.remove(path.c_str());  // the settings changed since: every accent family is tried again
    return kMaxAccents;
  }
  return std::min(static_cast<uint8_t>(bytes[kLimitBytes - 1]), kMaxAccents);
}

void setAccentLimit(const std::string& cachePath, const uint32_t settingsKey, const uint8_t accents) {
  const std::string path = cachePath + kLimitFile;
  if (accents >= kMaxAccents) {
    if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
    return;
  }
  char bytes[kLimitBytes] = {static_cast<char>(kVersion)};
  std::memcpy(&bytes[1], &settingsKey, sizeof settingsKey);
  bytes[kLimitBytes - 1] = static_cast<char>(accents);
  HalFile file;
  if (!Storage.openFileForWrite("BKF", path, file)) return;
  const bool written = file.write(bytes, sizeof bytes) == sizeof bytes;
  if (!file.close() || !written) Storage.remove(path.c_str());
}

std::string familyDir(const std::string& cachePath, const int accent) {
  if (accent < 0) return cachePath + kFontsDir;
  std::string dir = cachePath + kAccentDir;
  dir.back() = static_cast<char>('0' + accent);
  return dir;
}

uint32_t addWeighted(const uint32_t sum, const uint32_t bytes, const uint32_t weight256) {
  const uint64_t total = sum + (static_cast<uint64_t>(bytes) * weight256 + 128) / 256;
  return static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX));
}

uint32_t probeWeight256(const size_t itemBytes, const size_t readBytes, const int spineCount, const int sampledCount) {
  const bool sized = itemBytes != 0 && readBytes != 0;
  const uint64_t weight = 256ull * (sized ? itemBytes : 1) * static_cast<uint64_t>(std::max(spineCount, 1)) /
                          ((sized ? readBytes : 1) * static_cast<uint64_t>(std::max(sampledCount, 1)));
  return static_cast<uint32_t>(std::min<uint64_t>(weight, UINT32_MAX));
}

uint8_t keptStyles(const uint32_t (&scores)[4], const uint8_t present) {
  constexpr uint8_t kOrder[4] = {0, 2, 1, 3};  // ties go to the regular, then italic, bold, bold italic
  int primary = -1;
  uint64_t total = 0;
  for (const uint8_t style : kOrder) {
    total += scores[style];
    if ((present & (1u << style)) != 0 && (primary < 0 || scores[style] > scores[primary])) primary = style;
  }
  if (primary < 0) return 0;
  uint8_t kept = static_cast<uint8_t>(1u << primary);
  for (uint8_t style = 0; style < 4; ++style) {
    const uint32_t score = scores[style];
    if ((present & (1u << style)) != 0 &&
        (score >= kMinStyleBytes || (score > 0 && uint64_t{score} * 100 >= total * kMinStylePercent))) {
      kept |= static_cast<uint8_t>(1u << style);
    }
  }
  if (primary != 3 && (kept & 0b0110) != 0b0110) kept &= static_cast<uint8_t>(~0b1000u);
  return kept;
}

Found detectFamilies(const Epub& epub, BuildArena* scratch, bool* failed, const std::string_view records) {
  *failed = false;
  CssParser* css = epub.getCssParser();
  if (!css) return {};
  // ~1.3 KB: the frames and the two tables. Not on the stack, which a section build shares.
  auto probe = makeUniqueNoThrow<BodyFontProbe>(*css);
  if (!probe) {
    LOG_ERR("BKF", "OOM: probe");
    *failed = true;
    return {};
  }
  const ParserState state(scratch);
  css->resetResolveStats();
  const unsigned long start = millis();
  Found found;
  // From the first item, not the text reference: a guide that points at the cover would hide the whole book.
  const int count = epub.getSpineItemsCount();
  const int sampled = std::min(count, kProbeMaxItems);
  const size_t cap =
      std::clamp(kProbeBudgetBytes / static_cast<size_t>(std::max(sampled, 1)), kProbeMinItemBytes, kProbeMaxItemBytes);
  for (int k = 0; k < sampled; ++k) {
    const int i = count <= kProbeMaxItems ? k : static_cast<int>(static_cast<int64_t>(k) * count / kProbeMaxItems);
    const size_t itemBytes = epub.getSpineItem(i).zipUncompressedSize;  // 0: not known
    // One SAX pass over the item's head, the parser state in `scratch` when there is room. A malformed document
    // reports what it parsed and counts as read.
    SaxParser parser;
    if (state.get()) parser.setExternalState(state.get(), SaxParser::stateBytes());
    SaxFeedSink sink(parser);
    const bool ready = parser.init(probe.get(), &BodyFontProbe::onStart, &BodyFontProbe::onEnd, &BodyFontProbe::onText,
                                   &BodyFontProbe::onDefault, /*htmlVoidTagRepair=*/true);
    if (ready) probe->beginParser(parser, probeWeight256(itemBytes, std::min(itemBytes, cap), count, sampled), cap);
    // One item that cannot be read is passed over: the others still sample the book.
    if (!ready || !epub.readSpineItemToStream(i, sink, sink.stopFlag(), scratch)) {
      LOG_ERR("BKF", "Cannot read spine item %d", i);
      continue;
    }
    ++found.itemsRead;
    found.bytesRead += static_cast<uint32_t>(parser.byteOffset());
  }
  if (sampled > 0 && found.itemsRead == 0) *failed = true;
  LOG_INF("BKF", "Probe: %u of %d spine items, %lu bytes, %lu ms", static_cast<unsigned>(found.itemsRead), count,
          static_cast<unsigned long>(found.bytesRead), millis() - start);
  // Short of heap, the resolver skips the rules it would read from the card: the families found would be
  // wrong, so this counts as a failed attempt rather than an answer.
  if (css->getResolveStats().lowHeapSkips != 0 && !css->isArenaResident()) {
    LOG_ERR("BKF", "Low heap: the probe missed CSS rules");
    *failed = true;
  }
  css->clearCaches(/*evictEverything=*/true);  // the resolver's caches go back to the heap
  found.body = probe->family();
  // An accent needs a face to be drawn, of any style (moveNearestToRegular makes it the regular).
  probe->accents(
      [records](const uint32_t family) {
        CssParser::FontFace faces[4];
        pickFaces(records, family, faces);
        return std::any_of(std::begin(faces), std::end(faces), [](const auto& face) { return !face.name.empty(); });
      },
      [records](const uint32_t family, const uint32_t bytes) {
        const std::string_view name = familyName(records, family);
        LOG_INF("BKF", "Left out %.*s: %lu bytes of text (est.)", static_cast<int>(name.size()), name.data(),
                static_cast<unsigned long>(bytes));
      },
      found);
  return found;
}

// Copying a book's faces into its own cache, for the reader to load from there, follows CrossInk (uxjulia/CrossInk)
// commit dfd2f31c (Julia Nguyen), MIT; here they are extracted once, de-obfuscated, and loaded as a TTF family.
State prepare(const Epub& epub, BuildArena* scratch) {
  const std::string& cachePath = epub.getCachePath();
  // The CSS was not loaded, or did not compile: nothing to decide from yet.
  if (!epub.getCssParser() || !epub.getCssParser()->cacheIsCurrent()) return State::Pending;
  const uint8_t last = readCode(cachePath, nullptr, nullptr, -1);
  const uint8_t attempts = last != kNoCode && last >= kCodeFailed ? static_cast<uint8_t>(last - kCodeFailed) : 0;
  Storage.removeDir((cachePath + kFontsDir).c_str());  // whatever an earlier attempt left

  std::string records;
  Families found;
  std::string names;
  bool undecided = false;
  const uint8_t code = examine(epub, scratch, records, found, names, undecided);
  if (undecided) return State::Pending;  // no record: the next open asks again
  return writeState(cachePath, code == kCodeFailed ? static_cast<uint8_t>(kCodeFailed + attempts + 1) : code, found,
                    names);
}

}  // namespace BookFonts
