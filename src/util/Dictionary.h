#pragma once

// Ported from crosspoint-reader's StarDict reader (PR #2583 by Uri Tauber,
// with #2696/#2706/#2733/#2791 by William Floyd and #2877 by Uri Tauber).
// Reworked here for this heap: session-held inflate window, buffered index
// scans, one descent per lookup, and no temp files. See docs/dictionary.md.

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "DictZip.h"

// Result of an index search — file location of a definition without reading it.
struct DictLocation {
  uint32_t offset = 0;  // byte offset in .dict data
  uint32_t size = 0;    // byte length in .dict data
  bool found = false;
  // Set when the search was cut short by an .idx open or seek failure rather than
  // reaching a verdict, so a failed search isn't reported as a genuine miss.
  bool readError = false;
};

// One headword found at the start of a run of CJK text, with every .idx entry it has. StarDict's
// order puts entries with byte-equal headwords next to each other, so they are a run in the .idx:
// where it starts and how long it is are all a reader needs to read them all.
struct DictPrefixHit {
  DictLocation location;   // the run's FIRST entry, in .idx order
  uint8_t runBytes = 0;    // bytes of the run of text it covers, a codepoint boundary
  uint8_t entryCount = 0;  // entries in the run, at most DictPrefixMatch::MAX_ENTRIES_PER_HIT
  std::string headword;    // the .idx headword, also for a .syn hit; SSO up to 15 bytes (5 CJK chars)
  uint32_t idxStart = 0;   // byte offset in .idx of that first entry
};

// What Dictionary::resolvePrefixes() found. ~140 bytes on the ESP32; a member or stack local, never
// on the heap.
struct DictPrefixMatch {
  static constexpr uint8_t MAX_HITS = 3;
  static constexpr uint8_t MAX_ENTRIES_PER_HIT = 8;  // more same-headword entries are not listed
  DictPrefixHit hits[MAX_HITS];                      // longest first
  uint8_t count = 0;
  bool aborted = false;  // gave up between probes because the AbortCheck said so: no verdict
  uint8_t probes = 0;    // window scans made, .idx and .syn together
  uint16_t reads = 0;    // file reads made: scan-buffer refills plus sidecar sample reads
};

// Slim StarDict reader: exact-match lookup with a synonym and mini stemming
// fallback.
//
// Expects /dictionaries/<folder>/<stem>.idx (uncompressed) plus <stem>.dict or
// <stem>.dict.dz, and an optional <stem>.syn synonym index. Lookups
// binary-search a lazily built sampled-offset sidecar (<stem>.qidx, byte offset
// of every SAMPLE_INTERVAL-th .idx entry), then linear-scan at most
// SAMPLE_INTERVAL entries. A present .syn gets a parallel <stem>.sidx sidecar;
// its entries carry an ordinal (the N-th .idx entry) resolved back to a byte
// offset via the same fixed-interval .qidx samples. Everything streams from SD;
// no index is held in RAM.
//
// One Dictionary instance is one lookup session: it holds a small, bounded set
// of reusable buffers (see releaseCaches()) so repeated lookups cost no
// allocation at all. Destroy it, or call releaseCaches(), when the user leaves
// the dictionary UI.
class Dictionary {
 public:
  // Why a lookup did not return a definition — so the UI can tell a genuine
  // miss apart from a real failure, and name the failure.
  enum class LookupResult : uint8_t {
    Found,       // hit — definition filled
    NotFound,    // the word is genuinely not in the dictionary
    LowMemory,   // found, but an allocation failed: the ~32KB .dict.dz inflate
                 // window, the scan buffers, or the definition text buffer
                 // couldn't be obtained from the fragmented heap.
    Decompress,  // found, but decompression genuinely failed (corrupt/truncated
                 // .dict.dz — a read/inflate error, not a memory shortage)
    ReadError,   // found, but a file open/bounds/IO error prevented reading it
  };

  // Resolve the dictionary folder and validate its files. Rejects
  // dictionaries with 64-bit index offsets (idxoffsetbits=64 in .ifo).
  bool open(const char* folderName);
  bool isOpen() const { return !basePath.empty(); }

  // Release the session buffers: the scan buffers and the dictzip inflate
  // window plus its cached chunk table. The dictionary stays open and a later
  // lookup takes them again. Call this when the reader leaves the dictionary so
  // a reading session does not carry ~35KB it no longer needs.
  void releaseCaches();

  // True when the .ifo declares sametypesequence=h — definitions are HTML and
  // the viewer may lay them out through the EPUB rendering pipeline.
  bool definitionsAreHtml() const { return htmlDefinitions; }

  // True when the .qidx sidecar (or the .sidx sidecar of a present .syn) is
  // missing or stale — call buildIndex() first so the UI can show an
  // "Indexing…" message for the slow first pass.
  bool needsIndex();

  // Why an index build failed — the scan buffer is a heap allocation, so the
  // same fragmentation that breaks lookups can break indexing, and it deserves
  // the same "Not enough memory" rather than a generic error.
  enum class IndexResult : uint8_t {
    Ok,
    LowMemory,  // the scan buffer couldn't be allocated
    ReadError,  // source open/read or sidecar write failure
  };

  // One streaming pass over .idx writing the .qidx sidecar, plus a pass over
  // .syn writing the .sidx sidecar when a synonym file is present. Each sidecar
  // is rebuilt only when actually stale. yieldFn (optional) is called every
  // ~64KB consumed to feed the watchdog / repaint the UI. *outResult (if
  // provided) reports why a failed build failed; only the mandatory .idx pass
  // can fail the build (a failed .syn pass just disables synonyms).
  bool buildIndex(void (*yieldFn)(void*) = nullptr, void* ctx = nullptr, IndexResult* outResult = nullptr);

  // Clean the word, look it up, and on a miss retry dictionary-authored
  // synonyms then mini stem variants (-'s/-s/-es/-ies/-ed/-ing). On a hit fills
  // the definition text (capped at MAX_DEFINITION_BYTES) and the headword as
  // stored in the index. Returns true on a hit. *outResult (if provided)
  // reports the precise outcome so the UI can distinguish a genuine miss from a
  // decompression / low-memory / read failure.
  bool lookup(const char* word, std::string& definitionOut, std::string& matchedHeadwordOut,
              LookupResult* outResult = nullptr);

  // The index half of lookup(): find WHERE the definition is, without reading
  // it. Same cleaning, synonym and stemming behaviour, but it stops at the
  // location -- no inflate, no 32 KB window, no definition buffer, nothing
  // larger than the session scan buffers.
  //
  // Cheap enough to run speculatively while the user is still choosing a word,
  // which pre-pays the search for the lookup that follows and lets the UI say
  // in advance that a word has no entry. Feed the result to readResolved().
  bool resolve(const char* word, DictLocation& locationOut, std::string& matchedHeadwordOut,
               LookupResult* outResult = nullptr);

  // Read the definition for a location resolve() returned. Separated so a
  // caller that already resolved does not search the index twice.
  bool readResolved(const DictLocation& location, std::string& definitionOut, LookupResult* outResult = nullptr);

  using AbortCheck = bool (*)();
  // The search of spec 5.2 for a lookup that starts on CJK text. `run` is searched as given: the
  // caller has cleaned it (CjkLookupRun). Up to MAX_HITS headwords that are prefixes of `run`,
  // longest first, at most one per length, each with its whole run of same-headword entries, each
  // run listed once. None of resolve()'s fallbacks. Only the first MAX_PREFIX_RUN_BYTES of `run`
  // are searched, cut back to a codepoint boundary.
  // abortCheck (optional) is asked before every probe after the first; when it says yes the
  // search returns false with matchOut.aborted set, count 0 and *outResult NotFound. outResult as
  // for resolve(): Found, NotFound, LowMemory (no scan buffers), ReadError (no verdict reached).
  bool resolvePrefixes(const char* run, DictPrefixMatch& matchOut, LookupResult* outResult = nullptr,
                       AbortCheck abortCheck = nullptr);

  // The longest run resolvePrefixes() searches: CjkLookupRun's longest, 12 codepoints of up to 4
  // bytes (CjkLookupRun.cpp asserts it). A stack buffer of this size holds each probe's target.
  static constexpr size_t MAX_PREFIX_RUN_BYTES = 48;

  // The definitions of match.hits[0..count), in one string sized once from the index sizes (one
  // allocation, as readDefinition's). Every entry of every hit, in this order: hit by hit, longest
  // first; within a hit, its entryCount entries read from .idx at idxStart, in .idx order. So the
  // longest match's entries all come before the shorter matches. Plain: "headword\n" + definition
  // per entry (a duplicate headword repeats its line), entries joined by "\n\n". HTML
  // (definitionsAreHtml()): "<p><b>" + headword with & < > escaped + "</b></p>" + definition per
  // entry, no separator. At most MAX_DEFINITION_BYTES in all: the very first entry's definition is
  // cut to fit; every later entry is taken only whole, and at the first one that does not fit, it
  // and everything after it are left out. Refused with LowMemory, like readDefinition, when
  // ESP.getMaxAllocHeap() < total + 8 KB; a .dict.dz takes its inflate window first.
  bool readPrefixDefinitions(const DictPrefixMatch& match, std::string& definitionOut,
                             LookupResult* outResult = nullptr);

  static std::string cleanWord(const char* word);

  // A short fixed name for a lookup outcome ("found", "not_found", ...), for log lines and the
  // serial CMD:ACT:DICT reply.
  static const char* resultName(LookupResult result);
  // The one log line per lookup, from the dictionary screen and the serial lookup alike:
  // dictionary, word, outcome (a resultName() or why the dictionary could not be searched) and,
  // on a hit, the headword it matched (null otherwise).
  static void logLookup(const char* dictionaryName, const char* word, const char* outcome, const char* headword);

  static constexpr uint32_t MAX_DEFINITION_BYTES = 64 * 1024;

 private:
  static constexpr uint32_t SAMPLE_INTERVAL = 256;

  // Refill size for the buffered .idx / .syn scans. One sample window is at most
  // SAMPLE_INTERVAL entries — roughly 3KB for a typical dictionary — so 512
  // bytes turns a scan of ~3000 single-byte reads into ~6 reads, and the bisect's
  // per-probe headword read into one.
  static constexpr size_t SCAN_BUF_BYTES = 512;

  // Length of the longest suffix appended to basePath (".dict.dz"): open() reserves
  // pathScratch for basePath plus this.
  static constexpr size_t LONGEST_SUFFIX_LEN = sizeof(".dict.dz") - 1;

  // "<basePath><suffix>", composed in pathScratch, whose room open() reserved, so
  // the lookup path builds its paths without transient heap and at any length.
  // Valid until the next call. A fixed 160-byte stack buffer refused any base
  // path over 151 chars: a folder and stem of some length on the card, and every
  // dictionary of the host test once its build directory was long.
  const char* buildPath(const char* suffix);

  // Take the reusable session buffers if they aren't held yet: SCAN_BUF_BYTES
  // for the .idx scan and the same again for .syn. Allocated once per session,
  // not once per lookup, so a lookup adds nothing to the heap's fragmentation.
  bool ensureScanBuffers();

  // The window of the sampled index one descent resolved to, remembered so the
  // stem-variant probes that follow can skip the descent entirely.
  //
  // A lookup probes the exact word, then up to ~6 stem variants — and a stem
  // variant differs from the word only in its tail, so it almost always sorts
  // into the same SAMPLE_INTERVAL-entry window. Upstream re-ran the whole
  // bisect for each one: log2(samples) sidecar reads plus a seek and a headword
  // read into .idx, per variant, for an answer it had already computed. Caching
  // the window's bounds makes every variant after the first a pure comparison.
  struct SampleWindow {
    bool valid = false;
    uint32_t startByte = 0;
    std::string low;   // headword at startByte — the window's inclusive lower bound
    std::string high;  // headword at the next sample — exclusive upper bound
    bool hasHigh = false;
  };

  // Buffered forward reader over one index file. Owns the handle's position
  // outright: every read goes through here, so the buffer is always in sync and
  // a refill only re-seeks after an explicit seekSet().
  class ScanReader {
   public:
    void attach(HalFile* file, uint8_t* buf, uint32_t fileSize);
    bool attached() const { return file != nullptr; }
    bool seekSet(uint32_t offset);
    uint32_t position() const { return chunkStart + static_cast<uint32_t>(cursor); }
    // Read a NUL-terminated word into out (max outSize-1 chars). Returns the
    // number of characters read (excluding the NUL), or -1 on EOF/error.
    // Over-long words are truncated but the stream stays in sync.
    int readWord(char* out, size_t outSize);
    // Read exactly n bytes. False on EOF/error.
    bool readBytes(void* dst, size_t n);
    // File reads made since attach(), for DictPrefixMatch::reads.
    uint32_t reads() const { return refills; }

   private:
    bool refill();

    HalFile* file = nullptr;
    uint8_t* buf = nullptr;
    uint32_t fileSize = 0;
    uint32_t chunkStart = 0;  // file offset of buf[0]
    int filled = 0;           // valid bytes in buf
    int cursor = 0;           // read position within buf
    bool posDirty = true;     // the handle needs a seek before the next refill
    uint32_t refills = 0;     // file reads refill() has issued
  };

  // The .idx / .qidx handles shared by every locate() call in one lookup. A
  // lookup probes up to ~6 stem variants; opening the two files per probe cost
  // ~12 SD opens and ~12 std::string path temporaries per word, churning the
  // same heap whose fragmentation makes lookups fail mid-session. Opened once
  // per lookup instead, with the paths built via buildPath(). The .syn / .sidx
  // handles are opened lazily by locateSynonym() — only an exact miss consults
  // them, so a hit never pays for two extra SD opens.
  struct LookupSession {
    HalFile idx;
    HalFile qidx;
    HalFile syn;
    HalFile sidx;
    ScanReader idxScan;
    ScanReader synScan;
    SampleWindow idxWindow;  // cached descent over .qidx/.idx
    SampleWindow synWindow;  // the same over .sidx/.syn, for resolvePrefixes()'s .syn probes
    uint32_t idxSize = 0;
    uint32_t sampleCount = 0;  // 0 when the sidecar is absent, stale or empty
    uint32_t entryCount = 0;   // .idx entries the sidecar was built over; 0 = unknown
    uint32_t synSize = 0;
    uint32_t synSampleCount = 0;
    bool synOpened = false;  // openSynonyms() has run (success or failure)
    // A .syn exists but couldn't be searched (open failure, or no usable .sidx),
    // so a miss is an unfinished search rather than a verdict. Distinct from
    // "this dictionary has no .syn", which is a legitimate miss.
    bool synFailed = false;
  };

  // Open .idx (required) and .qidx (optional — locate() falls back to a full
  // scan without it). False when the dictionary is closed or .idx won't open.
  bool openSession(LookupSession& session);

  // Open .syn / .sidx into the session on first use. Idempotent; returns false
  // when there is no usable synonym index — either because no .syn exists, or
  // because it couldn't be opened / its .sidx is unusable, which sets
  // session.synFailed and releases both handles (an unindexed .syn is never
  // scanned linearly).
  bool openSynonyms(LookupSession& session);

  // Bisect a sampled-offset sidecar (.qidx over .idx, .sidx over .syn) to the
  // byte offset of the last sampled entry whose word is <= target, so the caller
  // only has to linear-scan at most SAMPLE_INTERVAL entries from there. Returns
  // 0 — scan source from the start — when sampleCount is 0 or a sample is
  // unreadable. `window`, when given, both short-circuits the descent for a
  // target inside the last resolved window and records the new one. Clobbers
  // wordBuf. With strictlyBelow, the last sample whose word is < target instead,
  // and `window` is neither read nor recorded: a window bounds a <= descent.
  uint32_t bisectSamples(HalFile& sidecar, ScanReader& source, uint32_t sampleCount, const char* target,
                         SampleWindow* window, bool strictlyBelow = false);

  // readSampleOffset(), counted in sampleReads.
  bool readSample(HalFile& sidecar, uint32_t sampleIndex, uint32_t* out);

  DictLocation locate(LookupSession& session, const char* target, std::string* matchedHeadwordOut);

  // resolvePrefixes() stops a file's window scans at this many once the file has a hit. Before its
  // first hit there is no cap: one could call a run absent whose match lies a few windows down.
  static constexpr uint8_t PREFIX_PROBES_PER_FILE = 3;

  // What the probes of one resolvePrefixes() call share, and the .syn hits its .syn scans keep
  // (each its run bytes and ordinal only, until followed). Defined in Dictionary.cpp.
  struct PrefixSearch;
  struct SynonymHits;

  // One file's half of resolvePrefixes() (spec 5.2), into `out`: matchOut's hits for .idx, a
  // SynonymHits for .syn. A window scan per probe, each probe's target a shorter prefix of the
  // run than the last, until the run's prefixes in that file are all seen or enough of them are.
  // An .idx hit whose first entry opens a window past byte 0 gets locateRun(): its run may begin
  // in the window before.
  template <typename Hits>
  void collectPrefixes(LookupSession& session, PrefixSearch& search, Hits& out);

  // The .idx run of hit.headword from its first entry: the last sample strictly below the
  // headword, then forward to the first byte-equal entry, counting it and those that follow.
  // Sets location, idxStart and entryCount; false, leaving the hit alone, when it is not found.
  bool locateRun(LookupSession& session, DictPrefixHit& hit);

  // Merges the .syn hits into matchOut's .idx hits: longest first, one hit per length with the
  // .idx hit on a tie; then a hit whose first entry a longer kept hit already has is dropped; at
  // most MAX_HITS. Each .syn hit that could still be kept is followed, one probe each:
  // locateByOrdinal() gives its .idx headword, whose entry may lie in the middle of its run, and
  // locateRun() finds the run's start.
  void mergeSynonymHits(LookupSession& session, PrefixSearch& search, SynonymHits& syn, DictPrefixMatch& matchOut);

  // Resolve an ordinal (the N-th .idx entry, 0-based) to its .dict location via
  // the .qidx samples. Used to follow a .syn synonym back to its headword.
  DictLocation locateByOrdinal(LookupSession& session, uint32_t ordinal, std::string* matchedHeadwordOut);

  // Bisect the .syn/.sidx synonym index for target; on a hit follow its ordinal
  // through locateByOrdinal(). Returns not-found when no .syn exists.
  DictLocation locateSynonym(LookupSession& session, const char* target, std::string* matchedHeadwordOut);

  // One streaming pass over sourcePath writing a sampled-offset sidecar. Each
  // source entry is a NUL-terminated word followed by suffixBytes fixed bytes
  // (8 for .idx: offset+size; 4 for .syn: ordinal). magic tags the sidecar.
  // *outResult (if provided) reports why a failed pass failed.
  bool buildSidecar(const std::string& sourcePath, const std::string& sidecarPath, uint32_t magic, uint32_t suffixBytes,
                    void (*yieldFn)(void*), void* ctx, IndexResult* outResult);

  // True when sidecarPath must be (re)built from sourcePath: missing/unreadable/
  // wrong-version sidecar, or a source-size mismatch. Shared by needsIndex() and
  // buildIndex() so each sidecar is rebuilt only when actually stale.
  bool sidecarIsStale(const std::string& sourcePath, const std::string& sidecarPath, uint32_t magic);

  // Read size bytes of the definition at location into dst (room for size bytes). On failure
  // returns false and, if outResult is given, sets the reason (Decompress / LowMemory / ReadError).
  // The caller has taken the inflate window of a .dict.dz and checked the heap.
  bool readEntryInto(const DictLocation& location, uint32_t size, char* dst, LookupResult* outResult);

  // The next .idx entry of match's hits in reading order: hit by hit, each hit's entryCount entries
  // from its idxStart. `hit` and `entry` are the position of the entry to read, advanced past it.
  // False when there are none left or the .idx could not be read (`ioError`).
  bool nextPrefixEntry(LookupSession& session, const DictPrefixMatch& match, uint8_t& hit, uint8_t& entry,
                       DictLocation& locationOut, bool& ioError);

  // Read the definition at location straight into out. On failure returns false and, if outResult
  // is given, sets it to the specific reason (Decompress / LowMemory / ReadError).
  bool readDefinition(const DictLocation& location, std::string& out, LookupResult* outResult = nullptr);
  static void stemVariants(const std::string& word, std::vector<std::string>& out);
  // The word after a French elision ("l'histoire" -> "histoire"); empty when
  // the word starts with none.
  static std::string elisionRemainder(const std::string& word);
  // Components of a hyphenated compound, longest first; empty when there is no
  // hyphen. The last resort when the compound itself is not a headword.
  static void hyphenParts(const std::string& word, std::vector<std::string>& out);

  std::string basePath;     // "/dictionaries/<folder>/<stem>", empty when not open
  std::string pathScratch;  // buildPath()'s result; lookups are single-threaded, as for wordBuf
  bool hasPlainDict = false;
  bool hasSyn = false;  // a <stem>.syn synonym index exists next to the .idx
  bool htmlDefinitions = false;

  // Session-scoped buffers, taken on first use and freed by releaseCaches().
  // One allocation, two named halves. Previously a single array indexed by hand
  // (`scanBuf.get() + SCAN_BUF_BYTES` for the synonym half), which read as
  // pointer arithmetic on an untyped buffer and drew a portability warning.
  struct ScanBuffers {
    uint8_t idx[SCAN_BUF_BYTES];
    uint8_t syn[SCAN_BUF_BYTES];
  };
  std::unique_ptr<ScanBuffers> scanBuf;
  DictZip::Scratch dzScratch;  // inflate window + cached .dz chunk table

  // Shared scan buffer: lookups are single-threaded and this avoids a
  // 256-byte array on the stack of every locate() call.
  char wordBuf[256] = {};

  // Sidecar sample reads (readSample()) since resolvePrefixes() started, for DictPrefixMatch::reads.
  uint32_t sampleReads = 0;
};
