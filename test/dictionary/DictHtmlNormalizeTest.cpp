// Host tests for the StarDict HTML normalizer.
//
// Two kinds of assertion. The exact-output ones pin the individual repairs, so
// a regression names itself. The ParsesCleanly ones feed the result to the real
// SaxParser, configured exactly as ChapterHtmlSlimParser configures it: that is
// the property the normalizer actually owes its caller, and it is what proves
// the repairs this file deliberately leaves to the parser (self-closing <br>,
// swallowing the </br> after it) really are covered there.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "DictHtmlNormalize.h"
#include "SaxParser/SaxParser.h"

namespace {

// Print sink that collects everything written, and can be told to start
// refusing writes after a byte budget -- the way the parser refuses once its
// XML parse has failed.
class StringSink : public Print {
 public:
  explicit StringSink(const size_t failAfter = 0) : failAfter(failAfter) {}

  size_t write(const uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, const size_t size) override {
    if (failAfter != 0 && text.size() + size > failAfter) return 0;
    text.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }

  std::string text;

 private:
  size_t failAfter;
};

std::string normalize(const std::string& html) {
  StringSink sink;
  EXPECT_TRUE(normalizeDictionaryHtml(html, sink)) << html;
  return sink.text;
}

// The body of the normalized document, so assertions read as the repair itself
// rather than as the wrapper plus the repair.
std::string body(const std::string& html) {
  const std::string full = normalize(html);
  const std::string open = "<html><body>";
  const std::string close = "</body></html>";
  EXPECT_EQ(full.compare(0, open.size(), open), 0) << full;
  EXPECT_GE(full.size(), open.size() + close.size());
  EXPECT_EQ(full.compare(full.size() - close.size(), close.size(), close), 0) << full;
  return full.substr(open.size(), full.size() - open.size() - close.size());
}

// Elements and text the parser saw, so a test can assert the markup survived as
// markup rather than as escaped text.
struct ParseTrace {
  bool ok = false;
  std::vector<std::string> starts;
  std::vector<std::string> ends;
  std::string text;
  std::string entities;  // raw named-entity references routed to the default handler
};

void onStart(void* user, const char* name, const char**) { static_cast<ParseTrace*>(user)->starts.emplace_back(name); }
void onEnd(void* user, const char* name) { static_cast<ParseTrace*>(user)->ends.emplace_back(name); }
void onChar(void* user, const char* s, const int len) {
  static_cast<ParseTrace*>(user)->text.append(s, static_cast<size_t>(len));
}
// yxml knows only the five XML entities. ChapterHtmlSlimParser registers a
// default handler so HTML named entities (&nbsp; and friends) reach it instead
// of aborting the parse; without one registered here the test would report a
// failure the real caller never sees.
void onDefault(void* user, const char* s, const int len) {
  static_cast<ParseTrace*>(user)->entities.append(s, static_cast<size_t>(len));
}

// Parse normalized output the way ChapterHtmlSlimParser::setup() does --
// htmlVoidTagRepair on, which is what this normalizer relies on for <br>.
ParseTrace parseNormalized(const std::string& html) {
  const std::string document = normalize(html);
  ParseTrace trace;
  SaxParser parser;
  if (!parser.init(&trace, onStart, onEnd, onChar, onDefault, /*htmlVoidTagRepair=*/true)) return trace;
  if (!parser.feed(reinterpret_cast<const uint8_t*>(document.data()), document.size())) return trace;
  trace.ok = parser.finalize();
  return trace;
}

TEST(DictHtmlNormalize, WrapsFragmentInARootElement) {
  // A definition is a fragment with several top-level elements. Without the
  // wrapper the parser stops at the first root's end tag and the rest is lost.
  EXPECT_EQ(normalize("<b>a</b><i>b</i>"), "<html><body><b>a</b><i>b</i></body></html>");
  EXPECT_EQ(normalize(""), "<html><body></body></html>");
  EXPECT_EQ(normalize("bare text"), "<html><body>bare text</body></html>");
}

TEST(DictHtmlNormalize, LowercasesElementAndAttributeNames) {
  EXPECT_EQ(body("<B>bold</B>"), "<b>bold</b>");
  EXPECT_EQ(body("<SPAN CLASS=\"Big\">x</SPAN>"), "<span class=\"Big\">x</span>");
  // Values keep their case: only the names are matched case-sensitively.
  EXPECT_EQ(body("<a HREF=\"bword://Word\">w</a>"), "<a href=\"bword://Word\">w</a>");
}

TEST(DictHtmlNormalize, QuotesUnquotedAndValuelessAttributes) {
  EXPECT_EQ(body("<span class=big>x</span>"), "<span class=\"big\">x</span>");
  EXPECT_EQ(body("<td nowrap>x</td>"), "<td nowrap=\"nowrap\">x</td>");
  EXPECT_EQ(body("<td nowrap class=c>x</td>"), "<td nowrap=\"nowrap\" class=\"c\">x</td>");
  EXPECT_EQ(body("<span class = 'big' >x</span>"), "<span class=\"big\">x</span>");
}

TEST(DictHtmlNormalize, EscapesTextTheXmlParserWouldReject) {
  EXPECT_EQ(body("Tom & Jerry"), "Tom &amp; Jerry");
  EXPECT_EQ(body("x < y"), "x &lt; y");
  EXPECT_EQ(body("a<"), "a&lt;");
  EXPECT_EQ(body("</>"), "&lt;/>");
}

TEST(DictHtmlNormalize, PassesWellFormedEntityReferencesThrough) {
  // The parser resolves HTML named entities through its default handler, so
  // rewriting these would lose the character entirely.
  EXPECT_EQ(body("a&nbsp;b"), "a&nbsp;b");
  EXPECT_EQ(body("&#160;"), "&#160;");
  EXPECT_EQ(body("&#x2014;"), "&#x2014;");
  EXPECT_EQ(body("&amp;"), "&amp;");
  // Not entity references, despite the '&'.
  EXPECT_EQ(body("&nbsp"), "&amp;nbsp");
  EXPECT_EQ(body("& "), "&amp; ");
  EXPECT_EQ(body("&;"), "&amp;;");
}

TEST(DictHtmlNormalize, EscapesInsideAttributeValues) {
  EXPECT_EQ(body("<a href=\"a&b\">x</a>"), "<a href=\"a&amp;b\">x</a>");
  EXPECT_EQ(body("<a href=\"a&amp;b\">x</a>"), "<a href=\"a&amp;b\">x</a>");
  // A double quote is legal inside a single-quoted HTML value and fatal inside
  // the double-quoted one this emits.
  EXPECT_EQ(body("<a title='say \"hi\"'>x</a>"), "<a title=\"say &quot;hi&quot;\">x</a>");
}

TEST(DictHtmlNormalize, DropsCommentsDoctypesAndProcessingInstructions) {
  EXPECT_EQ(body("a<!-- note -->b"), "ab");
  EXPECT_EQ(body("<!DOCTYPE html>a"), "a");
  EXPECT_EQ(body("<?xml version=\"1.0\"?>a"), "a");
  // Unterminated: drop to the end rather than emit a half construct.
  EXPECT_EQ(body("a<!-- never closed"), "a");
}

TEST(DictHtmlNormalize, KeepsTagStructureAcrossAwkwardMarkup) {
  // A '>' inside a quoted value must not end the tag early. It comes back
  // escaped -- legal either way in XML, and escaping costs nothing.
  EXPECT_EQ(body("<span title=\"a>b\">x</span>"), "<span title=\"a&gt;b\">x</span>");
  // An unterminated tag is not a tag.
  EXPECT_EQ(body("a <b"), "a &lt;b");
  // Self-closing is preserved as written.
  EXPECT_EQ(body("<br/>"), "<br/>");
  EXPECT_EQ(body("<br />"), "<br/>");
  // A bare void tag is left alone: SaxParser's htmlVoidTagRepair closes it.
  EXPECT_EQ(body("<br>"), "<br>");
}

TEST(DictHtmlNormalize, StopsWhenTheSinkRefusesAWrite) {
  // The parser sink returns 0 once its XML parse has failed. The normalizer has
  // to stop there rather than push the rest of the fragment through it.
  StringSink sink(/*failAfter=*/8);
  EXPECT_FALSE(normalizeDictionaryHtml(std::string(4096, 'x'), sink));
}

// --- the property that actually matters ------------------------------------

TEST(DictHtmlNormalize, ParsesCleanlyAsXml) {
  const std::vector<std::string> fragments = {
      "<b>quixotic</b> <i>adj.</i> exceedingly idealistic",
      "<B>UPPER</B> and <I>mixed</I>",
      "Tom & Jerry, x < y, 3 > 2",
      "<span class=big>unquoted</span>",
      "<td nowrap>boolean</td>",
      "line one<br>line two<br>line three",
      "<br></br>paired void",
      "<!-- comment --><!DOCTYPE html><p>after</p>",
      "<a href=\"bword://word\">see also</a>",
      "<a title='he said \"no\"'>quoted</a>",
      "&nbsp;&#160;&#x2014;&amp;&bare",
      "<p>nested <b>bold <i>italic</i></b> tail</p>",
      "<span title=\"a>b\">gt in attr</span>",
      "trailing <b",
      "",
  };
  for (const auto& fragment : fragments) {
    const ParseTrace trace = parseNormalized(fragment);
    EXPECT_TRUE(trace.ok) << "did not parse: " << fragment << "\n  -> " << normalize(fragment);
  }
}

TEST(DictHtmlNormalize, MarkupSurvivesAsMarkupNotAsText) {
  const ParseTrace trace = parseNormalized("<B>quixotic</B> <i>adj.</i> Tom & Jerry");
  ASSERT_TRUE(trace.ok);
  // Lowercased, so ChapterHtmlSlimParser's case-sensitive dispatch sees them.
  EXPECT_EQ(trace.starts, (std::vector<std::string>{"html", "body", "b", "i"}));
  EXPECT_EQ(trace.ends, (std::vector<std::string>{"b", "i", "body", "html"}));
  // The bare ampersand comes back as one character, not as "&amp;".
  EXPECT_EQ(trace.text, "quixotic adj. Tom & Jerry");
}

TEST(DictHtmlNormalize, NamedEntitiesReachTheParsersDefaultHandler) {
  // Definitions are full of &nbsp;. It has to arrive at the default handler as
  // a reference, while &amp; and an escaped bare '&' both arrive as text.
  const ParseTrace trace = parseNormalized("a&nbsp;b &amp; c &bare");
  ASSERT_TRUE(trace.ok);
  EXPECT_EQ(trace.entities, "&nbsp;");
  EXPECT_EQ(trace.text, "ab & c &bare");
}

TEST(DictHtmlNormalize, VoidTagsAreLeftToTheParserAndStillBalance) {
  // <br> and <br></br> both have to come out as a single balanced element, or
  // every definition using a line break falls back to plain text.
  for (const char* fragment : {"a<br>b", "a<br/>b", "a<br></br>b", "a<BR>b"}) {
    const ParseTrace trace = parseNormalized(fragment);
    ASSERT_TRUE(trace.ok) << fragment;
    EXPECT_EQ(trace.starts, (std::vector<std::string>{"html", "body", "br"})) << fragment;
    EXPECT_EQ(trace.ends, (std::vector<std::string>{"br", "body", "html"})) << fragment;
    EXPECT_EQ(trace.text, "ab") << fragment;
  }
}

}  // namespace
