#include "IndicReorder.h"

#include <cstring>

#include "IndicReorderSink.h"  // witchhunt: deviation 9
#include "IndicScripts.h"
#include "Utf8.h"

namespace {

using indic::CodepointRange;

// A vowel sign drawn wholly or partly before its consonant cluster: `pre` is
// drawn in front, `post` (up to two signs, 0 = none) after it.
struct PreBaseVowel {
  uint32_t sign;
  uint32_t pre;
  uint32_t post[2];
};

// A consonant + nukta sequence with a precomposed letter.
struct NuktaLetter {
  uint32_t base;
  uint32_t composed;
};

template <typename T>
struct Span {
  const T* data = nullptr;
  size_t count = 0;
  constexpr const T* begin() const { return data; }
  constexpr const T* end() const { return data + count; }
};
template <typename T, size_t N>
constexpr Span<T> span(const T (&array)[N]) {
  return {array, N};
}

struct ScriptRules {
  Span<CodepointRange> consonants;
  uint32_t ra;  // forms the reph when it starts a cluster
  // Whether a bare virama binds the next consonant into the cluster. Tamil
  // and Sinhala show the virama and start a new syllable unless a ZWJ asks
  // for a conjunct.
  bool viramaJoins;
  Span<PreBaseVowel> preBaseVowels;
  Span<NuktaLetter> nuktaLetters;
};

// --- Per-script rules -------------------------------------------------------

constexpr CodepointRange kDevanagariConsonants[] = {{0x0915, 0x0939}, {0x0958, 0x095F}, {0x0978, 0x097F}};
constexpr PreBaseVowel kDevanagariPreBase[] = {{0x093F, 0x093F, {}}, {0x094E, 0x094E, {}}};
constexpr NuktaLetter kDevanagariNukta[] = {{0x0915, 0x0958}, {0x0916, 0x0959}, {0x0917, 0x095A}, {0x091C, 0x095B},
                                            {0x0921, 0x095C}, {0x0922, 0x095D}, {0x0928, 0x0929}, {0x092B, 0x095E},
                                            {0x092F, 0x095F}, {0x0930, 0x0931}, {0x0933, 0x0934}};

constexpr CodepointRange kBengaliConsonants[] = {{0x0995, 0x09B9}, {0x09DC, 0x09DF}, {0x09F0, 0x09F1}};
constexpr PreBaseVowel kBengaliPreBase[] = {{0x09BF, 0x09BF, {}},
                                            {0x09C7, 0x09C7, {}},
                                            {0x09C8, 0x09C8, {}},
                                            {0x09CB, 0x09C7, {0x09BE}},
                                            {0x09CC, 0x09C7, {0x09D7}}};
constexpr NuktaLetter kBengaliNukta[] = {{0x09A1, 0x09DC}, {0x09A2, 0x09DD}, {0x09AF, 0x09DF}};

constexpr CodepointRange kGurmukhiConsonants[] = {{0x0A15, 0x0A39}, {0x0A59, 0x0A5E}};
constexpr PreBaseVowel kGurmukhiPreBase[] = {{0x0A3F, 0x0A3F, {}}};
constexpr NuktaLetter kGurmukhiNukta[] = {{0x0A16, 0x0A59}, {0x0A17, 0x0A5A}, {0x0A1C, 0x0A5B},
                                          {0x0A2B, 0x0A5E}, {0x0A32, 0x0A33}, {0x0A38, 0x0A36}};

constexpr CodepointRange kGujaratiConsonants[] = {{0x0A95, 0x0AB9}, {0x0AF9, 0x0AF9}};
constexpr PreBaseVowel kGujaratiPreBase[] = {{0x0ABF, 0x0ABF, {}}};

constexpr CodepointRange kOriyaConsonants[] = {{0x0B15, 0x0B39}, {0x0B5C, 0x0B5F}, {0x0B71, 0x0B71}};
constexpr PreBaseVowel kOriyaPreBase[] = {
    {0x0B47, 0x0B47, {}}, {0x0B48, 0x0B47, {0x0B56}}, {0x0B4B, 0x0B47, {0x0B3E}}, {0x0B4C, 0x0B47, {0x0B57}}};
constexpr NuktaLetter kOriyaNukta[] = {{0x0B21, 0x0B5C}, {0x0B22, 0x0B5D}};

constexpr CodepointRange kTamilConsonants[] = {{0x0B95, 0x0BB9}};
constexpr PreBaseVowel kTamilPreBase[] = {{0x0BC6, 0x0BC6, {}},       {0x0BC7, 0x0BC7, {}},
                                          {0x0BC8, 0x0BC8, {}},       {0x0BCA, 0x0BC6, {0x0BBE}},
                                          {0x0BCB, 0x0BC7, {0x0BBE}}, {0x0BCC, 0x0BC6, {0x0BD7}}};

// Telugu and Kannada draw every vowel sign above, below or after the base.
constexpr CodepointRange kTeluguConsonants[] = {{0x0C15, 0x0C39}, {0x0C58, 0x0C5A}};
constexpr CodepointRange kKannadaConsonants[] = {{0x0C95, 0x0CB9}, {0x0CDD, 0x0CDE}};

constexpr CodepointRange kMalayalamConsonants[] = {{0x0D15, 0x0D3A}};
constexpr PreBaseVowel kMalayalamPreBase[] = {{0x0D46, 0x0D46, {}},       {0x0D47, 0x0D47, {}},
                                              {0x0D48, 0x0D48, {}},       {0x0D4A, 0x0D46, {0x0D3E}},
                                              {0x0D4B, 0x0D47, {0x0D3E}}, {0x0D4C, 0x0D46, {0x0D57}}};

constexpr CodepointRange kSinhalaConsonants[] = {{0x0D9A, 0x0DC6}};
constexpr PreBaseVowel kSinhalaPreBase[] = {
    {0x0DD9, 0x0DD9, {}},       {0x0DDA, 0x0DD9, {0x0DCA}},         {0x0DDB, 0x0DDB, {}},
    {0x0DDC, 0x0DD9, {0x0DCF}}, {0x0DDD, 0x0DD9, {0x0DCF, 0x0DCA}}, {0x0DDE, 0x0DD9, {0x0DDF}}};

// Indexed like indic::SCRIPTS.
constexpr ScriptRules kRules[] = {
    {span(kDevanagariConsonants), 0x0930, true, span(kDevanagariPreBase), span(kDevanagariNukta)},
    {span(kBengaliConsonants), 0x09B0, true, span(kBengaliPreBase), span(kBengaliNukta)},
    {span(kGurmukhiConsonants), 0x0A30, true, span(kGurmukhiPreBase), span(kGurmukhiNukta)},
    {span(kGujaratiConsonants), 0x0AB0, true, span(kGujaratiPreBase), {}},
    {span(kOriyaConsonants), 0x0B30, true, span(kOriyaPreBase), span(kOriyaNukta)},
    {span(kTamilConsonants), 0x0BB0, false, span(kTamilPreBase), {}},
    {span(kTeluguConsonants), 0x0C30, true, {}, {}},
    {span(kKannadaConsonants), 0x0CB0, true, {}, {}},
    {span(kMalayalamConsonants), 0x0D30, true, span(kMalayalamPreBase), {}},
    {span(kSinhalaConsonants), 0x0DBB, false, span(kSinhalaPreBase), {}},
};
static_assert(sizeof(kRules) / sizeof(kRules[0]) == indic::SCRIPT_COUNT, "one rule set per indic::SCRIPTS entry");

// Every rule codepoint must lie in its own script's block.
constexpr bool rulesMatchScripts() {
  for (size_t i = 0; i < indic::SCRIPT_COUNT; i++) {
    const auto inBlock = [i](const uint32_t cp) {
      return cp == 0 || (cp >= indic::SCRIPTS[i].first && cp <= indic::SCRIPTS[i].last());
    };
    if (!inBlock(kRules[i].ra)) return false;
    for (const auto& r : kRules[i].consonants) {
      if (!inBlock(r.first) || !inBlock(r.last)) return false;
    }
    for (const auto& v : kRules[i].preBaseVowels) {
      if (!inBlock(v.sign) || !inBlock(v.pre) || !inBlock(v.post[0]) || !inBlock(v.post[1])) return false;
    }
    for (const auto& n : kRules[i].nuktaLetters) {
      if (!inBlock(n.base) || !inBlock(n.composed)) return false;
    }
  }
  return true;
}
static_assert(rulesMatchScripts(), "kRules is out of step with indic::SCRIPTS");

// --- Reordering -------------------------------------------------------------

using Cursor = const unsigned char*;

using indic_reorder::BoundedOut;  // witchhunt: deviation 9
using indic_reorder::next;        // witchhunt: deviation 9
using indic_reorder::peek;        // witchhunt: deviation 9
using indic_reorder::StringOut;   // witchhunt: deviation 9

const ScriptRules* rulesFor(const uint32_t cp) {
  const indic::ScriptInfo* script = indic::scriptOf(cp);
  return script ? &kRules[indic::indexOf(*script)] : nullptr;
}

bool isConsonant(const ScriptRules& rules, const uint32_t cp) {
  for (const auto& r : rules.consonants) {
    if (cp >= r.first && cp <= r.last) return true;
  }
  return false;
}

const PreBaseVowel* preBaseVowel(const ScriptRules& rules, const uint32_t cp) {
  for (const auto& v : rules.preBaseVowels) {
    if (v.sign == cp) return &v;
  }
  return nullptr;
}

uint32_t composeNukta(const ScriptRules& rules, const uint32_t base) {
  for (const auto& n : rules.nuktaLetters) {
    if (n.base == base) return n.composed;
  }
  return 0;
}

// Vowel signs and modifiers (candrabindu, anusvara, visarga, length marks):
// everything that follows a consonant cluster within one syllable.
bool isSyllableTail(const uint32_t cp) {
  return indic::isDependentSign(cp) && !indic::isNukta(cp) && !indic::isVirama(cp);
}

// Advances past one consonant and an optional nukta.
Cursor skipConsonant(Cursor p, const Cursor end) {  // witchhunt: deviation 9
  next(p, end);                                     // witchhunt: deviation 9
  Cursor q = p;
  return indic::isNukta(next(q, end)) ? q : p;  // witchhunt: deviation 9
}

// End of the consonant cluster starting at `p`: C[N] (H [ZWJ] C[N])* [H [ZWJ|ZWNJ]],
// where a bare H only continues the cluster in scripts whose virama joins.
Cursor clusterEnd(const ScriptRules& rules, Cursor p, const Cursor end) {  // witchhunt: deviation 9
  p = skipConsonant(p, end);                                               // witchhunt: deviation 9
  while (indic::isVirama(peek(p, end))) {                                  // witchhunt: deviation 9
    Cursor q = p;
    next(q, end);  // witchhunt: deviation 9
    Cursor afterJoiner = q;
    const bool zwj = next(afterJoiner, end) == indic::ZWJ;  // witchhunt: deviation 9
    if (!zwj) afterJoiner = q;
    if ((zwj || rules.viramaJoins) && isConsonant(rules, peek(afterJoiner, end))) {  // witchhunt: deviation 9
      p = skipConsonant(afterJoiner, end);                                           // witchhunt: deviation 9
      continue;
    }
    // Syllable-final virama, optionally with an explicit joiner.
    const uint32_t joiner = peek(q, end);                             // witchhunt: deviation 9
    if (joiner == indic::ZWJ || joiner == indic::ZWNJ) next(q, end);  // witchhunt: deviation 9
    return q;
  }
  return p;
}

Cursor tailEnd(Cursor p, const Cursor end) {          // witchhunt: deviation 9
  while (isSyllableTail(peek(p, end))) next(p, end);  // witchhunt: deviation 9
  return p;
}

// Copies [begin, end), composing nukta letters. Sets `changed` when it does.
template <typename Out>                                                                 // witchhunt: deviation 9
void appendCluster(const ScriptRules& rules, Cursor begin, const Cursor end, Out& out,  // witchhunt: deviation 9
                   bool& changed) {                                                     // witchhunt: deviation 9
  while (begin < end) {
    const Cursor start = begin;
    const uint32_t cp = next(begin, end);  // witchhunt: deviation 9
    if (const uint32_t composed = composeNukta(rules, cp);
        composed != 0 && begin < end && indic::isNukta(peek(begin, end))) {  // witchhunt: deviation 9
      next(begin, end);                                                      // witchhunt: deviation 9
      out.appendCodepoint(composed);                                         // witchhunt: deviation 9
      changed = true;
      continue;
    }
    out.append(reinterpret_cast<const char*>(start), begin - start);  // witchhunt: deviation 9
  }
}

// The text [begin, end) reordered into `out`; true when anything changed. witchhunt: deviation 9
template <typename Out>                                             // witchhunt: deviation 9
bool reorderInto(const Cursor begin, const Cursor end, Out& out) {  // witchhunt: deviation 9
  bool changed = false;

  Cursor p = begin;                    // witchhunt: deviation 9
  while (p < end) {                    // witchhunt: deviation 9
    const uint32_t cp = peek(p, end);  // witchhunt: deviation 9
    const ScriptRules* rules = rulesFor(cp);
    if (rules == nullptr || !isConsonant(*rules, cp)) {
      const Cursor start = p;
      next(p, end);                                                 // witchhunt: deviation 9
      out.append(reinterpret_cast<const char*>(start), p - start);  // witchhunt: deviation 9
      continue;
    }

    // Without a reph glyph, ra + virama stays a spacing prefix; the vowel
    // sign belongs in front of the consonant it is pronounced with.
    Cursor clusterBegin = p;
    {
      Cursor q = p;
      if (next(q, end) == rules->ra && indic::isVirama(next(q, end)) &&  // witchhunt: deviation 9
          isConsonant(*rules, peek(q, end))) {                           // witchhunt: deviation 9
        out.append(reinterpret_cast<const char*>(p), q - p);             // witchhunt: deviation 9
        clusterBegin = q;
      }
    }

    const Cursor clusterStop = clusterEnd(*rules, clusterBegin, end);  // witchhunt: deviation 9
    const Cursor tailStop = tailEnd(clusterStop, end);                 // witchhunt: deviation 9

    for (Cursor t = clusterStop; t < tailStop;) {
      if (const PreBaseVowel* vowel = preBaseVowel(*rules, next(t, end))) {  // witchhunt: deviation 9
        out.appendCodepoint(vowel->pre);                                     // witchhunt: deviation 9
        changed = true;
      }
    }
    appendCluster(*rules, clusterBegin, clusterStop, out, changed);
    for (Cursor t = clusterStop; t < tailStop;) {
      const uint32_t sign = next(t, end);  // witchhunt: deviation 9
      const PreBaseVowel* vowel = preBaseVowel(*rules, sign);
      if (vowel == nullptr) {
        out.appendCodepoint(sign);  // witchhunt: deviation 9
        continue;
      }
      for (const uint32_t post : vowel->post) {
        if (post != 0) out.appendCodepoint(post);  // witchhunt: deviation 9
      }
    }
    p = tailStop;
  }
  return changed;
}

// witchhunt: deviation 9 - moved: upstream closes the anonymous namespace before the std::string overload, whose body
// is reorderInto now; this line and the overload's first lines come after it.
}  // namespace

bool indicReorderForDisplay(const char* text, std::string& out) {    // witchhunt: deviation 9
  if (text == nullptr || !indic::containsIndic(text)) return false;  // witchhunt: deviation 9

  out.clear();                                            // witchhunt: deviation 9
  out.reserve(strlen(text) + 8);                          // witchhunt: deviation 9
  StringOut sink{out};                                    // witchhunt: deviation 9
  const Cursor begin = reinterpret_cast<Cursor>(text);    // witchhunt: deviation 9
  return reorderInto(begin, begin + strlen(text), sink);  // witchhunt: deviation 9
}  // witchhunt: deviation 9

// witchhunt: deviation 9 - the bounded form; see IndicReorder.h.
bool indicReorderForDisplay(const char* text, size_t len, char* out, size_t room,  // witchhunt: deviation 9
                            size_t* written) {                                     // witchhunt: deviation 9
  if (text == nullptr || out == nullptr || room == 0) return false;                // witchhunt: deviation 9
  const auto* bytes = reinterpret_cast<const unsigned char*>(text);                // witchhunt: deviation 9
  bool holdsIndic = false;                                                         // witchhunt: deviation 9
  for (size_t i = 0; i + 3 <= len && !holdsIndic; i++) {                           // witchhunt: deviation 9
    holdsIndic = indic::startsIndic(bytes + i);                                    // witchhunt: deviation 9
  }  // witchhunt: deviation 9
  if (!holdsIndic) return false;  // witchhunt: deviation 9

  BoundedOut sink{out, room};                                             // witchhunt: deviation 9
  if (!reorderInto(bytes, bytes + len, sink) || sink.full) return false;  // witchhunt: deviation 9
  out[sink.used] = '\0';                                                  // witchhunt: deviation 9
  *written = sink.used;                                                   // witchhunt: deviation 9
  return true;                                                            // witchhunt: deviation 9
}  // witchhunt: deviation 9
