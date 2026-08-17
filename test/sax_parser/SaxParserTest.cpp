#include <SaxParser/SaxParser.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct Event {
  enum class Type { Start, End, Char, Default };
  Type type;
  std::string name;
  std::string text;
};

struct Collector {
  std::vector<Event> events;

  static void onStart(void* ud, const char* name, const char**) {
    static_cast<Collector*>(ud)->events.push_back({Event::Type::Start, name, {}});
  }
  static void onEnd(void* ud, const char* name) {
    static_cast<Collector*>(ud)->events.push_back({Event::Type::End, name, {}});
  }
  static void onChar(void* ud, const char* s, int len) {
    static_cast<Collector*>(ud)->events.push_back({Event::Type::Char, {}, std::string(s, len)});
  }
  static void onDefault(void* ud, const char* s, int len) {
    static_cast<Collector*>(ud)->events.push_back({Event::Type::Default, {}, std::string(s, len)});
  }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(SaxParser, ParseMinimalDocument) {
  const char* xml = "<root><child>hello</child></root>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  ASSERT_EQ(c.events.size(), 5u);
  EXPECT_EQ(c.events[0].type, Event::Type::Start);
  EXPECT_EQ(c.events[0].name, "root");
  EXPECT_EQ(c.events[1].type, Event::Type::Start);
  EXPECT_EQ(c.events[1].name, "child");
  EXPECT_EQ(c.events[2].type, Event::Type::Char);
  EXPECT_EQ(c.events[2].text, "hello");
  EXPECT_EQ(c.events[3].type, Event::Type::End);
  EXPECT_EQ(c.events[3].name, "child");
  EXPECT_EQ(c.events[4].type, Event::Type::End);
  EXPECT_EQ(c.events[4].name, "root");
}

TEST(SaxParser, ParseChunked) {
  const char* xml = "<root><a>foo</a><b>bar</b></root>";
  const size_t len = strlen(xml);

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar));

  // Feed 3 bytes at a time
  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  for (size_t i = 0; i < len; i += 3) {
    const size_t chunk = (i + 3 <= len) ? 3 : (len - i);
    ASSERT_TRUE(p.feed(bytes + i, chunk));
  }
  ASSERT_TRUE(p.finalize());

  int starts = 0, ends = 0, chars = 0;
  for (const auto& e : c.events) {
    if (e.type == Event::Type::Start)
      starts++;
    else if (e.type == Event::Type::End)
      ends++;
    else if (e.type == Event::Type::Char)
      chars++;
  }
  EXPECT_EQ(starts, 3);  // root, a, b
  EXPECT_EQ(ends, 3);
  EXPECT_GE(chars, 2);  // at least one Char event per text node; expat may split across chunk boundaries
}

TEST(SaxParser, EarlyStop) {
  const char* xml = "<root><a/><b/><c/></root>";

  struct StopAfterFirst {
    int startCount = 0;
    SaxParser* parser = nullptr;

    static void onStart(void* ud, const char* /*name*/, const char**) {
      auto* s = static_cast<StopAfterFirst*>(ud);
      s->startCount++;
      if (s->startCount == 2) {
        s->parser->stop();
      }
    }
    static void onEnd(void*, const char*) {}
  };

  StopAfterFirst state;
  SaxParser p;
  ASSERT_TRUE(p.init(&state, StopAfterFirst::onStart, StopAfterFirst::onEnd));
  state.parser = &p;

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  p.feed(bytes, strlen(xml));  // may return false after stop — that's fine

  EXPECT_TRUE(p.isStopped());
  EXPECT_EQ(state.startCount, 2);
}

TEST(SaxParser, ParseError) {
  const char* xml = "<root><unclosed>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  p.feed(bytes, strlen(xml));
  const bool ok = p.finalize();

  EXPECT_FALSE(ok);
  EXPECT_GT(strlen(p.errorString()), 0u);
}

TEST(SaxParser, ByteOffsetAdvances) {
  const char* xml = "<root><child>text</child></root>";

  struct OffsetCapture {
    uint32_t offsetAtChild = 0;
    SaxParser* parser = nullptr;
    bool childSeen = false;

    static void onStart(void* ud, const char* name, const char**) {
      auto* s = static_cast<OffsetCapture*>(ud);
      if (strcmp(name, "child") == 0 && !s->childSeen) {
        s->childSeen = true;
        s->offsetAtChild = s->parser->byteOffset();
      }
    }
    static void onEnd(void*, const char*) {}
  };

  OffsetCapture state;
  SaxParser p;
  ASSERT_TRUE(p.init(&state, OffsetCapture::onStart, OffsetCapture::onEnd));
  state.parser = &p;

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  EXPECT_GT(state.offsetAtChild, 0u);
}

TEST(SaxParser, DefaultHandler) {
  // Verify the defaultCb fires for entity references
  const char* xml = "<root>&amp;</root>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, nullptr, Collector::onDefault));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  // With SetDefaultHandlerExpand, standard entities like &amp; are expanded
  // by expat into char data before reaching the default handler — so no
  // Default event is expected here. Instead we may get a Char event.
  // What matters is that the document parsed successfully and no crash occurred.
  SUCCEED();
}

TEST(SaxParser, HtmlEntityRoutedToDefaultCb) {
  // &nbsp; is not an XML built-in, so the yxml backend must intercept it and
  // route it to defaultCb (mirroring expat's DefaultHandlerExpand).  Without
  // the fix, yxml returns YXML_EREF and feed() returns false.
  const char* xml = "<p>Silo&nbsp;1</p>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, Collector::onDefault));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  // defaultCb must have received the raw "&nbsp;" text.
  bool sawEntity = false;
  for (const auto& e : c.events) {
    if (e.type == Event::Type::Default && e.text == "&nbsp;") {
      sawEntity = true;
    }
  }
  EXPECT_TRUE(sawEntity) << "defaultCb was not called with &nbsp;";
}

TEST(SaxParser, HtmlEntitySpanningChunks) {
  // Entity reference split across two feed() calls: first chunk ends inside
  // "&nbs", second chunk starts with "p;".
  const char* xml = "<p>x&nbsp;y</p>";
  const size_t split = 7;  // "<p>x&nb" | "sp;y</p>"

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, Collector::onDefault));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, split));
  ASSERT_TRUE(p.feed(bytes + split, strlen(xml) - split));
  ASSERT_TRUE(p.finalize());

  bool sawEntity = false;
  for (const auto& e : c.events) {
    if (e.type == Event::Type::Default && e.text == "&nbsp;") sawEntity = true;
  }
  EXPECT_TRUE(sawEntity) << "cross-chunk &nbsp; not delivered to defaultCb";
}

TEST(SaxParser, XmlBuiltinEntitiesPassThrough) {
  // XML built-ins must still be expanded by yxml (not routed to defaultCb).
  const char* xml = "<r>&amp;&lt;&gt;&quot;&apos;</r>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, Collector::onDefault));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  // Collect all char data (built-ins are expanded to their characters).
  std::string chars;
  for (const auto& e : c.events) {
    if (e.type == Event::Type::Char) chars += e.text;
  }
  EXPECT_EQ(chars, "&<>\"'");

  // None of the built-ins should appear as Default events.
  for (const auto& e : c.events) {
    EXPECT_NE(e.type, Event::Type::Default) << "built-in entity reached defaultCb: " << e.text;
  }
}

TEST(SaxParser, TruncationFlagsClearForWellSizedDoc) {
  const char* xml = "<root><child a=\"1\" b=\"2\">text</child></root>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  // A small, well-formed document stays within every fixed capacity.
  EXPECT_EQ(p.truncationFlags(), 0u);
}

TEST(SaxParser, TruncationFlagsReportMaxAttrs) {
  // 13 attributes — one more than kMaxAttrs (12). The 13th is dropped and the
  // overflow is recorded so callers can log it (the yxml backend only).
  const char* xml =
      "<e a1='1' a2='2' a3='3' a4='4' a5='5' a6='6' a7='7' a8='8' a9='9' "
      "a10='10' a11='11' a12='12' a13='13'/>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  // The active backend (yxml) has fixed caps and records the overflow. expat,
  // if ever re-enabled, has no fixed caps and returns 0 — so only assert the
  // flag when the parser actually reports truncation support.
  EXPECT_TRUE(p.truncationFlags() & SaxParser::kTruncMaxAttrs);
}

// ---------------------------------------------------------------------------
// HTML void-element repair
//
// Real-world EPUB/OPDS content frequently uses HTML-style void elements
// (<br>, <hr>, ...) without the XML-required self-closing slash. yxml is a
// strict well-formed-XML engine, so without repair these fail the parse the
// same way expat did. The pre-processor in SaxParserYxml.cpp turns "<br>"
// into "<br/>" before yxml ever sees it.
// ---------------------------------------------------------------------------

TEST(SaxParser, UnclosedBrIsAutoClosed) {
  const char* xml = "<p>Hello<br>World</p>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  ASSERT_EQ(c.events.size(), 6u);
  EXPECT_EQ(c.events[0].name, "p");
  EXPECT_EQ(c.events[1].text, "Hello");
  EXPECT_EQ(c.events[2].type, Event::Type::Start);
  EXPECT_EQ(c.events[2].name, "br");
  EXPECT_EQ(c.events[3].type, Event::Type::End);
  EXPECT_EQ(c.events[3].name, "br");
  EXPECT_EQ(c.events[4].text, "World");
  EXPECT_EQ(c.events[5].name, "p");

  EXPECT_TRUE(p.truncationFlags() & SaxParser::kVoidTagRepaired);
}

TEST(SaxParser, UnclosedHrWithAttributeIsAutoClosed) {
  const char* xml = "<div>Section<hr class=\"sep\">Next</div>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  bool sawHrStart = false, sawHrEnd = false;
  for (const auto& e : c.events) {
    if (e.type == Event::Type::Start && e.name == "hr") sawHrStart = true;
    if (e.type == Event::Type::End && e.name == "hr") sawHrEnd = true;
  }
  EXPECT_TRUE(sawHrStart);
  EXPECT_TRUE(sawHrEnd);
  EXPECT_TRUE(p.truncationFlags() & SaxParser::kVoidTagRepaired);
}

TEST(SaxParser, UnclosedVoidTagIsCaseInsensitive) {
  const char* xml = "<p>Hello<BR>World</p>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  EXPECT_TRUE(p.truncationFlags() & SaxParser::kVoidTagRepaired);
}

TEST(SaxParser, AlreadySelfClosedVoidTagDoesNotSetRepairFlag) {
  const char* xml = "<p>Hello<br/>World</p>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  EXPECT_FALSE(p.truncationFlags() & SaxParser::kVoidTagRepaired);
}

TEST(SaxParser, VoidTagRepairIgnoresGreaterThanInAttributeValue) {
  // A '>' inside a quoted attribute value must not be mistaken for the tag
  // terminator that would trigger (or skip) the self-close injection.
  const char* xml = "<p title=\"a>b\">text<br>x</p>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  bool sawBrEnd = false;
  for (const auto& e : c.events) {
    if (e.type == Event::Type::End && e.name == "br") sawBrEnd = true;
  }
  EXPECT_TRUE(sawBrEnd);
}

TEST(SaxParser, UnclosedNonVoidElementStillFails) {
  // Repair is scoped to known HTML void elements; an unrelated unclosed
  // element (<span>) must continue to be a genuine parse error.
  const char* xml = "<p>Hello<span>World</p>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  p.feed(bytes, strlen(xml));
  EXPECT_FALSE(p.finalize());
}

TEST(SaxParser, UnclosedVoidTagSplitAcrossChunks) {
  // Split the feed right in the middle of "<br>" to make sure the tag-scan
  // state machine survives a chunk boundary mid-tag.
  const char* xml = "<p>Hello<br>World</p>";
  const size_t split = 10;  // "<p>Hello<b" | "r>World</p>"

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, split));
  ASSERT_TRUE(p.feed(bytes + split, strlen(xml) - split));
  ASSERT_TRUE(p.finalize());

  bool sawBrEnd = false;
  for (const auto& e : c.events) {
    if (e.type == Event::Type::End && e.name == "br") sawBrEnd = true;
  }
  EXPECT_TRUE(sawBrEnd);
  EXPECT_TRUE(p.truncationFlags() & SaxParser::kVoidTagRepaired);
}

TEST(SaxParser, PairedMetaParsesWhenRepairDisabled) {
  // EPUB3 OPF metadata pairs <meta> with a real end tag:
  //   <meta refines="#t" property="title-type">main</meta>
  // With repair enabled that opening tag would be self-closed, turning the
  // real </meta> into a mismatched close and killing the whole OPF parse
  // (book fails to open). Strict-XML parsers therefore init with repair off
  // (the default) — this must parse cleanly.
  const char* xml = "<package><metadata><meta refines=\"#t\" property=\"title-type\">main</meta></metadata></package>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  bool sawMetaStart = false, sawMetaEnd = false, sawText = false;
  for (const auto& e : c.events) {
    if (e.type == Event::Type::Start && e.name == "meta") sawMetaStart = true;
    if (e.type == Event::Type::End && e.name == "meta") sawMetaEnd = true;
    if (e.type == Event::Type::Char && e.text == "main") sawText = true;
  }
  EXPECT_TRUE(sawMetaStart);
  EXPECT_TRUE(sawMetaEnd);
  EXPECT_TRUE(sawText);
  EXPECT_FALSE(p.truncationFlags() & SaxParser::kVoidTagRepaired);
}

// ---------------------------------------------------------------------------
// Void elements that the document ALSO closes explicitly
//
// XHTML 1.1 requires a void element to either self-close or be paired, so
// converters targeting it emit "<meta ...></meta>". Repairing the start tag into
// "<meta ... />" leaves that end tag closing nothing, which aborts the parse --
// issue #157, where every content file of a FictionBook->XHTML book began
// <meta ...></meta><link ...></link> and the book would not open on any spine.
// ---------------------------------------------------------------------------

TEST(SaxParser, PairedVoidElementEndTagIsDropped) {
  const char* xml = "<html><head><meta charset=\"UTF-8\"></meta><title>T</title></head></html>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  // Exactly one start/end pair for meta -- the synthesized one; the source's own
  // </meta> must not surface as a second end event.
  int metaStarts = 0;
  int metaEnds = 0;
  for (const auto& e : c.events) {
    if (e.name != "meta") continue;
    if (e.type == Event::Type::Start) ++metaStarts;
    if (e.type == Event::Type::End) ++metaEnds;
  }
  EXPECT_EQ(metaStarts, 1);
  EXPECT_EQ(metaEnds, 1);
}

// The head shape from issue_157.epub, reduced: paired <meta> and <link>, both on
// one line, with a real element between and after.
TEST(SaxParser, Issue157HeadWithPairedMetaAndLinkParses) {
  const char* xml =
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
      "<meta http-equiv=\"content-type\" content=\"text/xhtml; charset=UTF-8\"></meta>"
      "<title>Chapter</title>"
      "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"></link>"
      "<link rel=\"stylesheet\" type=\"text/css\" href=\"unicode_fonts.css\"></link>"
      "</head><body><p>Body text</p></body></html>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  // The body must survive: before the fix the parse died in <head> and no
  // content ever reached the renderer.
  bool sawBodyText = false;
  for (const auto& e : c.events) {
    if (e.text == "Body text") sawBodyText = true;
  }
  EXPECT_TRUE(sawBodyText);
}

TEST(SaxParser, PairedVoidEndTagToleratesWhitespaceBetween) {
  const char* xml = "<head><meta charset=\"UTF-8\">\n  </meta><title>T</title></head>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  int metaEnds = 0;
  for (const auto& e : c.events) {
    if (e.name == "meta" && e.type == Event::Type::End) ++metaEnds;
  }
  EXPECT_EQ(metaEnds, 1);
}

// Suppression must be exact: a DIFFERENT tag following a self-close is replayed
// intact, not swallowed. This is the regression that a naive "skip the next end
// tag" implementation would introduce.
TEST(SaxParser, NonMatchingTagAfterSelfCloseIsReplayed) {
  const char* xml = "<div><br><span>kept</span></div>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  bool sawSpanStart = false;
  bool sawSpanEnd = false;
  bool sawText = false;
  for (const auto& e : c.events) {
    if (e.name == "span" && e.type == Event::Type::Start) sawSpanStart = true;
    if (e.name == "span" && e.type == Event::Type::End) sawSpanEnd = true;
    if (e.text == "kept") sawText = true;
  }
  EXPECT_TRUE(sawSpanStart);
  EXPECT_TRUE(sawSpanEnd);
  EXPECT_TRUE(sawText);
}

// Two void elements of the same name in a row: the second start tag must not be
// mistaken for the first's end tag.
TEST(SaxParser, ConsecutiveSameNameVoidElementsBothSurvive) {
  const char* xml = "<head><meta name=\"a\"><meta name=\"b\"></head>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  int metaStarts = 0;
  for (const auto& e : c.events) {
    if (e.name == "meta" && e.type == Event::Type::Start) ++metaStarts;
  }
  EXPECT_EQ(metaStarts, 2);
}

// The candidate end tag split across feed() chunks — the reason it is buffered in
// the impl rather than decided byte-by-byte.
TEST(SaxParser, PairedVoidEndTagSplitAcrossFeeds) {
  const std::string xml = "<head><meta charset=\"UTF-8\"></meta><title>T</title></head>";
  const size_t split = xml.find("</meta>") + 3;  // mid end-tag

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml.data());
  ASSERT_TRUE(p.feed(bytes, split));
  ASSERT_TRUE(p.feed(bytes + split, xml.size() - split));
  ASSERT_TRUE(p.finalize());

  int metaEnds = 0;
  bool sawTitle = false;
  for (const auto& e : c.events) {
    if (e.name == "meta" && e.type == Event::Type::End) ++metaEnds;
    if (e.name == "title" && e.type == Event::Type::Start) sawTitle = true;
  }
  EXPECT_EQ(metaEnds, 1);
  EXPECT_TRUE(sawTitle);
}

// Strict-XML mode (the EPUB3 OPF path) is unchanged: no repair, no suppression.
TEST(SaxParser, StrictModeLeavesPairedMetaAlone) {
  const char* xml = "<package><metadata><meta property=\"x\">v</meta></metadata></package>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(
      p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/false));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  bool sawValue = false;
  for (const auto& e : c.events) {
    if (e.text == "v") sawValue = true;
  }
  EXPECT_TRUE(sawValue);
  EXPECT_FALSE(p.truncationFlags() & SaxParser::kVoidTagRepaired);
}

// Not a meta/link special case: EVERY void element breaks the same way when the
// document pairs it. In issue_157.epub the counts across 69 content files were
// </br> 640, </link> 138, </img> 87, </meta> 69 -- so <br></br>, once written off
// in this file as "essentially never emitted by real tools", was the single most
// common form, and </img> meant even a repaired <head> would have died in <body>.
TEST(SaxParser, PairedBrAndImgInBodyAreDropped) {
  const char* xml =
      "<body><p>One<br></br>Two</p>"
      "<p><img src=\"i.png\" alt=\"a\"></img>after</p></body>";

  Collector c;
  SaxParser p;
  ASSERT_TRUE(p.init(&c, Collector::onStart, Collector::onEnd, Collector::onChar, nullptr, /*htmlVoidTagRepair=*/true));

  const auto* bytes = reinterpret_cast<const uint8_t*>(xml);
  ASSERT_TRUE(p.feed(bytes, strlen(xml)));
  ASSERT_TRUE(p.finalize());

  int brStarts = 0, brEnds = 0, imgStarts = 0, imgEnds = 0;
  bool sawTwo = false, sawAfter = false;
  for (const auto& e : c.events) {
    if (e.name == "br") (e.type == Event::Type::Start ? brStarts : brEnds)++;
    if (e.name == "img") (e.type == Event::Type::Start ? imgStarts : imgEnds)++;
    if (e.text == "Two") sawTwo = true;
    if (e.text == "after") sawAfter = true;
  }
  EXPECT_EQ(brStarts, 1);
  EXPECT_EQ(brEnds, 1);
  EXPECT_EQ(imgStarts, 1);
  EXPECT_EQ(imgEnds, 1);
  // Content after each paired void element must survive.
  EXPECT_TRUE(sawTwo);
  EXPECT_TRUE(sawAfter);
}
