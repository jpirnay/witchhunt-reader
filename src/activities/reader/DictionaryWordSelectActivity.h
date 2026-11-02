#pragma once

// Ported from crosspoint-reader (PR #2583 by Uri Tauber, with #2706 by William
// Floyd and #2749 by Thiago Kenji Okada). Adapted to this device: no touch
// input, geometry from TextBlock::wordBox, and speculative resolve.

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "util/CjkLookupRun.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"

class FontCacheManager;

// Word selection over the current reader page: Left/Right step through words in
// reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader.
//
// A vertical page is a horizontal page in the turned frame (TurnedFrame.h): its words are boxed there, a row is a
// column, and the frame is mapped only at the edges -- a tap in, the highlight's boxes out, its text redrawn in the
// turned frame. Up/Down then step through the words, down the column, and Left/Right change column.
//
// Owns the Page it draws, so the reader can hand over the page it already has
// laid out instead of the overlay reloading it.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int fontId, int marginLeft, int marginTop)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        fontId(fontId),
        marginLeft(marginLeft),
        marginTop(marginTop) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
#if SERIAL_TEST_API
  // " sel=<selected> label=<the selected word's text> spec=<unknown|found|missing> span=<words>
  // match=<headword>"; nothing until the words are built.
  void appendDebugState(std::string& out) const override;
#endif

 private:
  // The font a piece was drawn with, and what follows from it. A page has one or two of these
  // -- body text, plus a heading or an inline size change -- so they are deduplicated into a
  // side table and a fragment stores a one-byte index instead of the fields themselves.
  struct DrawStyle {
    float scale;
    // Full width, NOT narrowed: font ids are 32-bit hashes (see fontIds.h, e.g. 1883578066), so
    // a 16-bit field truncates every one of them to a font that does not exist -- which draws
    // the highlight box and none of the glyphs inside it.
    int fontId;
    int16_t height;
    // How a vertical line draws the piece (the DRAW_* bits in the .cpp): down its column, with its marks centred, or
    // the word set upright as one block. 0 on a horizontal line.
    uint8_t flags;
  };

  // Screen box of one PIECE of a selectable word, in the frame the page draws in: the turned one on a vertical page,
  // where x runs down the column and y across it. Layout hands the page words already
  // fragmented: bionic reading splits "reading" into a bold "read" and a plain "ing",
  // hyphenation splits it across two lines as "read-" and "ing", and attached punctuation
  // arrives as its own token. `text` points into the owned Page's TextBlock arena, valid for
  // this activity's lifetime.
  //
  // Kept to 16 bytes on purpose. A bionic page carries ~500 of these and the array has to be ONE
  // contiguous block, alive while the cursor moves. It is freed for the definition's stay
  // (releaseWordArrays), so the definition has the heap these arrays held.
  struct Fragment {
    const char* text;
    int16_t x;
    int16_t y;
    int16_t width;
    // Bytes of `text` this piece covers. Usually the whole arena token, but a token holding an
    // em dash between two words ("swiftness-a") is cut at the dash, and those pieces are not
    // NUL-terminated where they end -- read them through fragmentText().
    uint16_t length;
    uint8_t drawStyle;  // index into drawStyles
    // Bold, italic, sub/superscript: this one really is per piece -- bionic reading alternates
    // bold and regular within a single word.
    EpdFontFamily::Style style;
  };
  static_assert(sizeof(Fragment) <= 16, "Fragment is on the reader's contiguous-heap budget");

  // One word as the reader sees it: a run of consecutive fragments. Selection, highlighting
  // and lookup all work on these, so a bionic-split or hyphenated word behaves like any other.
  struct Word {
    uint16_t firstFragment;
    uint8_t fragmentCount;
    // Fragment offset (from firstFragment) of the piece that ends on the hyphen layout added
    // when it broke this word across two lines, or NO_HYPHEN. Both the offset and the row span
    // are derived from it: fragments up to and including it sit on `row`, the rest on the row
    // below.
    uint8_t hyphenFragment;
    uint16_t row;
    // Extent along `row` (down the column on a vertical page) -- the part of the word the cursor navigates by. A
    // hyphen-joined word's tail on the next row is highlighted but does not steer navigation.
    int16_t x;
    int16_t width;
  };
  static constexpr uint8_t NO_HYPHEN = 0xFF;
  // Longest word handed to the dictionary, joined across fragments. Headwords far shorter
  // than this; the buffer is a stack local, so it stays well inside the task stack budget.
  static constexpr size_t WORD_TEXT_CAPACITY = 128;
  // Largest block a lookup may need: dictzip's 32 KB inflate window plus its ~3 KB chunk scratch.
  static constexpr size_t LOOKUP_CONTIGUOUS_BYTES = 36 * 1024;

  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void extractWords();
  // Exact-ish capacity for the two arrays, counted before anything is allocated. Growing them
  // mid-scan would hold the old and the new block at once, which is what actually destroys the
  // contiguous run a definition needs. Returns tokens; sets `wordEstimate` to the group count.
  size_t countPageTokens(size_t& wordEstimate) const;
  uint8_t internDrawStyle(const TextBlock::WordBox& geometry, uint8_t flags);
  // Fuse each line-final word ending in a hyphen with the first word of the line below.
  // Layout does not record that it hyphenated (the flag never reaches the page cache), so the
  // shape of the break is the evidence: a word that ends the line on '-' with a word starting
  // the next line is the split of one word in every book that is not deliberately odd.
  void joinHyphenatedWords();
  // Concatenate word `index`'s fragments into `buf` (NUL-terminated), returning the length.
  // dropJoinHyphen leaves out the hyphen layout inserted at a line break, which is what the
  // dictionary should see first; a compound that legitimately breaks at its own hyphen is
  // retried with it.
  size_t buildWordText(int index, char* buf, size_t capacity, bool dropJoinHyphen) const;
  // NUL-terminated view of one fragment: the arena pointer itself when the piece ends on the
  // token's own NUL, otherwise a copy in `buf`.
  static const char* fragmentText(const Fragment& fragment, char* buf, size_t capacity);
  const DrawStyle& drawStyleOf(const Fragment& fragment) const { return drawStyles[fragment.drawStyle]; }
  bool isHyphenJoined(const int index) const { return words[index].hyphenFragment != NO_HYPHEN; }
  // Open the dictionary on first use. Idempotent; false when there is none
  // configured or it will not open.
  bool ensureDictionaryOpen();
  // Resolve the highlighted word's LOCATION once the cursor has been still for
  // SPECULATIVE_DEBOUNCE_MS. Index search only -- no inflate, no definition
  // buffer -- so it costs a handful of SD reads and no large allocation.
  void speculateForSelection();
  // The script of word `index`'s first kept codepoint: Han or kana and Hangul take the prefix
  // search (spec 5.1), anything else the exact lookup.
  CjkLookupRun::Script scriptAt(int index) const;
  // Feeds `run` the words from `index` on, in reading order and so across a line end, until it closes.
  void buildRun(int index, CjkLookupRun& run) const;
  // Searches `run`, built from `selected`, for its dictionary prefixes and records the outcome as
  // the speculation for `selected`: the hits, the verdict, and the words the longest hit covers.
  // False, with the old speculation dropped and nothing recorded, when abortCheck gave up between
  // probes. `result` gets the search's outcome.
  bool resolveRun(const CjkLookupRun& run, Dictionary::AbortCheck abortCheck, Dictionary::LookupResult& result);
  // Confirm's half for a CJK word: the run from `selected`, searched unless the speculation already
  // has it, then every entry of its matches read into `definition`. Logs the lookup. `ok` is false
  // when the dictionary could not be opened or indexed.
  bool lookUpRun(bool ok, std::string& definition, Dictionary::LookupResult& result);
  // Noto Sans for a Latin lookup, the reader's own font (`fontId`) for one that started on CJK text
  // (spec 5.3). The one place that chooses; roadmap step 6 replaces it.
  int definitionFontId(CjkLookupRun::Script script) const;
  // Move to another installed dictionary and look the same word up again.
  // Persists the choice: this IS the dictionary setting, and a reader who
  // switched mid-book means it.
  void switchDictionary(int direction);
  // The dictionaries on the card, discovered once on first need. Empty until
  // then; a single entry means there is nothing to switch between.
  const std::vector<DictionaryEntry>& installedDictionaries();
  int closestInRow(uint16_t row, int centerX) const;
  void moveVertical(int direction);

  // The word under a point in logical pixels, or -1 for a miss.
  //
  // Walks the fragments rather than matching a recorded set of rectangles: a page carries
  // hundreds of selectable words, far past what TapTargets holds, and the boxes are already
  // here -- the same argument that has the RecentBooks grid invert its own layout instead of
  // recording cells. Per word, per fragment, once per tap; nothing runs per frame.
  //
  // The box tested is the fragment's exact advance box, which is the highlight's box before it
  // is grown by 2px for air. Tapping is therefore very slightly tighter than what lights up,
  // and neighbouring words can never claim each other's pixels.
  int wordAt(int px, int py) const;

  // What a tap did to the selection this pass, so loop() can keep its one copy of the
  // move-then-speculate bookkeeping instead of the touch path growing a second one.
  enum class WordTouch : uint8_t {
    None,      // nothing here for touch; the buttons get their turn
    Consumed,  // claimed but deliberately inert (a contact still down)
    Moved,     // the cursor is now on the tapped word
    Activate,  // the tap landed on the word already selected, or inside the match lit from it
  };
  WordTouch consumeWordTouch();
  void performLookup();
  // Gives the three arrays and the highlight snapshot back to the heap while a definition is
  // shown; extractWords() builds them again, identically, when the page is drawn on return.
  // `selected` and the speculation fields stay, so the cursor comes back where it was.
  void releaseWordArrays();
  // Words the highlight covers from `selected`: the span of the CJK match the speculation found
  // for it, otherwise 1 -- the word alone, which is every Latin word.
  int spanWordCount() const;
  // Saves the pixels under the highlight over words [selected, selected + spanWordCount()) and
  // draws it, one box per row. False when the boxes could not all be saved: the highlight is
  // drawn regardless, and the next cursor move takes the full repaint.
  bool drawHighlightWithSnapshot();
  // The fast path's glyph load for the highlight, one batch per font and style.
  void prewarmHighlight(FontCacheManager& fcm) const;
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int fontId;
  const int marginLeft;
  const int marginTop;

  std::vector<DrawStyle> drawStyles;
  std::vector<Fragment> fragments;
  std::vector<Word> words;
  // Word boxes are measured once the font cache has been prewarmed for this
  // page, which only happens inside render(); onEnter() has no framebuffer to
  // scan against. Until then there is nothing to highlight.
  bool wordsExtracted = false;
  // Set by the first successful extractWords(): a later one is a rebuild, which keeps `selected`.
  bool wordsBuiltOnce = false;
  int selected = 0;
  uint16_t rowCount = 0;

  Dictionary dict;
  std::vector<DictionaryEntry> dictionaries;
  bool dictionariesDiscovered = false;
  // Set from the definition viewer's result and acted on in loop(), not in the
  // handler itself: the handler runs while the activity stack is being popped,
  // and starting another activity from inside that is asking for trouble.
  int8_t pendingDictionarySwitch = 0;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  bool dictNeedsIndex = false;

  // What the speculative resolve found for the word the cursor is sitting on.
  // Two things come out of it: Confirm skips straight to reading the definition
  // instead of searching the index again, and the highlight can show, before
  // the user presses anything, that a word has no entry.
  enum class Speculation : uint8_t {
    Unknown,  // not resolved yet (still settling, or speculation is off)
    Found,
    Missing,  // the search reached a verdict and the word is not in the dictionary
  };
  static constexpr unsigned long SPECULATIVE_DEBOUNCE_MS = 300;
  Speculation speculation = Speculation::Unknown;
  int speculationIdx = -1;  // word `speculation` describes; -1 = none
  DictLocation speculationLocation;
  std::string speculationHeadword;
  // The CJK speculation's hits for speculationIdx; a Latin word's are the two fields above.
  DictPrefixMatch speculationMatch;
  // Words the longest hit covers from speculationIdx; 1 otherwise, and always 1 for a Latin word.
  uint8_t speculationSpan = 1;
  // Records the speculation for word `index` (-1: none). Under the render lock: the render task
  // sizes and styles the highlight from these three fields, and a half-written set would draw one
  // word's span from another. Loop task only -- the render task already holds the lock, and it is
  // not recursive.
  void setSpeculation(int index, Speculation verdict, uint8_t span);
  unsigned long lastMoveMs = 0;
  // Speculation is off for this session: no dictionary, it would not open, or
  // its index still has to be built (a multi-second pass that must stay on the
  // explicit Confirm, behind its popup, rather than firing while idle). Confirm
  // turns it back on once it has built the index.
  bool speculationDisabled = false;
  // A verdict that changes the box -- an outline, a span -- arrived for the word the cursor is
  // already on, so the highlight has to be redrawn even though the cursor did not move. Set
  // under the render lock, which the render task holds when it clears it.
  bool speculationRepaintPending = false;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight,
  // so a cursor move restores them and repaints only the two affected highlights
  // instead of re-running the full two-pass page render (which also reloads
  // every SD-font glyph on the page). snapshotIdx is the word the saved
  // highlight is drawn from; -1 means the framebuffer no longer holds a clean page (popup
  // drawn, sub-activity shown) and the next render must be a full one.
  //
  // The highlight covers one rectangle per row it occupies -- two for a hyphen-joined word, or
  // for a CJK match that crosses a line end, whose halves sit at the end of one line and the
  // start of the next. Saving their bounding box instead would mean saving both whole lines and
  // everything between them, so the runs are packed back to back into the one buffer. A
  // highlight over more rows or pixels than this holds is drawn without being saved, and the
  // next move takes the full repaint (spec 4).
  struct SnapshotRun {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
    uint16_t offset;  // byte offset of this run's pixels within `snapshot`
  };
  static constexpr size_t SNAPSHOT_CAPACITY = 2048;  // packed 1bpp: 16384 pixels
  static constexpr uint8_t MAX_SNAPSHOT_RUNS = 2;
  std::unique_ptr<uint8_t[]> snapshot;
  SnapshotRun snapshotRuns[MAX_SNAPSHOT_RUNS] = {};
  uint8_t snapshotRunCount = 0;
  int snapshotIdx = -1;
};
