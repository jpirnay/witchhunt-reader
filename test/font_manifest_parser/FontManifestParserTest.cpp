// FontManifestParser: the font manifest (assets/sd-fonts/fonts.json) read as a stream of family and
// file events, fed in pieces, without the whole document in memory.

#include <FontManifestParser.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

// Every event as one line of text, so a test compares the whole sequence at once.
struct Recorder {
  std::vector<std::string> events;

  static Recorder& self(void* ctx) { return *static_cast<Recorder*>(ctx); }

  static void familyBegin(void* ctx) { self(ctx).events.emplace_back("family{"); }
  static void familyName(void* ctx, const char* value, size_t len) {
    self(ctx).events.push_back("name=" + std::string(value, len));
  }
  static void familyDescription(void* ctx, const char* value, size_t len) {
    self(ctx).events.push_back("desc=" + std::string(value, len));
  }
  static void file(void* ctx, const FontManifestFile& f) {
    std::string line = "file=" + std::string(f.name, f.nameLen) + " size=" + std::to_string(f.size) +
                       " crc=" + (f.hasCrc32 ? std::to_string(f.crc32) : std::string("-"));
    if (f.nameOverflow) line += " overflow";
    self(ctx).events.push_back(line);
  }
  static void familyEnd(void* ctx) { self(ctx).events.emplace_back("}family"); }
  static void familyFormat(void* ctx, const char* value, size_t len) {
    self(ctx).events.push_back("format=" + std::string(value, len));
  }
  static void familyInDownloadAll(void* ctx, bool value) {
    self(ctx).events.push_back(std::string("inDownloadAll=") + (value ? "true" : "false"));
  }

  FontManifestCallbacks callbacks() {
    return FontManifestCallbacks{this, familyBegin, familyName,   familyDescription,
                                 file, familyEnd,   familyFormat, familyInDownloadAll};
  }
};

struct Parsed {
  std::vector<std::string> events;
  int version = -1;
  std::string baseUrl;
  bool baseUrlOverflow = false;
  bool error = false;
  bool complete = false;
};

// Feeds `json` in pieces of `chunk` bytes; 0 feeds it whole.
Parsed parse(const std::string& json, size_t chunk = 0) {
  Recorder rec;
  FontManifestParser parser(rec.callbacks());
  if (chunk == 0) chunk = json.size();
  for (size_t at = 0; at < json.size(); at += chunk) {
    parser.feed(json.data() + at, std::min(chunk, json.size() - at));
  }
  Parsed run;
  run.events = rec.events;
  run.version = parser.version();
  run.baseUrl = parser.baseUrl();
  run.baseUrlOverflow = parser.baseUrlOverflow();
  run.error = parser.hasError();
  run.complete = parser.complete();
  return run;
}

using Events = std::vector<std::string>;

const std::string kTwoFamilies = R"({
  "version": 2,
  "baseUrl": "https://example.com/fonts/",
  "families": [
    {
      "name": "Alpha",
      "description": "First family",
      "styles": ["regular", "bold"],
      "files": [
        {"name": "Alpha/Alpha_10.cpfont", "size": 1000, "crc32": 11},
        {"name": "Alpha/Alpha_12.cpfont", "size": 2000, "crc32": 22}
      ]
    },
    {
      "name": "Beta",
      "description": "Second family",
      "styles": ["regular"],
      "files": [
        {"name": "Beta/Beta_10.cpfont", "size": 3000, "crc32": 33}
      ]
    }
  ]
})";

const Events kTwoFamiliesEvents = {
    "family{",
    "name=Alpha",
    "desc=First family",
    "file=Alpha/Alpha_10.cpfont size=1000 crc=11",
    "file=Alpha/Alpha_12.cpfont size=2000 crc=22",
    "}family",
    "family{",
    "name=Beta",
    "desc=Second family",
    "file=Beta/Beta_10.cpfont size=3000 crc=33",
    "}family",
};

// A manifest whose one family has one file named `fileName`.
std::string withFileName(const std::string& fileName) {
  return R"({"version":2,"families":[{"name":"F","files":[{"name":")" + fileName + R"(","size":5,"crc32":6}]}]})";
}

}  // namespace

// 1. The common case: two families, their files, the version and the base URL.
TEST(FontManifestParserTest, ReadsAVersion2ManifestInDocumentOrder) {
  const Parsed run = parse(kTwoFamilies);
  EXPECT_EQ(run.events, kTwoFamiliesEvents);
  EXPECT_EQ(run.version, 2);
  EXPECT_EQ(run.baseUrl, "https://example.com/fonts/");
  EXPECT_FALSE(run.baseUrlOverflow);
  EXPECT_FALSE(run.error);
  EXPECT_TRUE(run.complete);
}

// 2. A version 1 manifest has no crc32: the file says so rather than reporting 0.
TEST(FontManifestParserTest, Version1FilesHaveNoCrc32) {
  const Parsed run = parse(R"({"version": 1, "baseUrl": "https://example.com/",
    "families": [{"name": "Old", "files": [{"name": "Old/Old_10.cpfont", "size": 42}]}]})");
  EXPECT_EQ(run.events, (Events{"family{", "name=Old", "file=Old/Old_10.cpfont size=42 crc=-", "}family"}));
  EXPECT_EQ(run.version, 1);
  EXPECT_TRUE(run.complete);
}

// 3. styles[] and unknown keys are skipped whole, however deep, even when what they hold looks like
// the manifest's own keys.
TEST(FontManifestParserTest, SkipsStylesAndUnknownKeysAtEveryLevel) {
  const Parsed run = parse(R"({
    "meta": {"families": [{"name": "Decoy"}], "version": 9, "list": [1, [2, {"baseUrl": "x"}]]},
    "version": 2,
    "families": [
      {
        "name": "A",
        "styles": ["regular", {"name": "Decoy"}],
        "extra": {"files": [{"name": "decoy", "size": 1}], "name": "Decoy", "description": "no"},
        "files": [
          {"name": "A/A_10.cpfont", "tags": ["x", {"name": "decoy"}], "meta": {"size": 99, "crc32": 1},
           "size": 10, "flag": true, "nothing": null, "crc32": 7}
        ],
        "notes": [[["deep", {"name": "Decoy"}]]]
      }
    ],
    "trailer": [{"version": 7, "baseUrl": "https://wrong/"}]
  })");
  EXPECT_EQ(run.events, (Events{"family{", "name=A", "file=A/A_10.cpfont size=10 crc=7", "}family"}));
  EXPECT_EQ(run.version, 2);
  EXPECT_EQ(run.baseUrl, "");
  EXPECT_FALSE(run.error);
  EXPECT_TRUE(run.complete);
}

// 4. No key order is assumed: files before the family's name, a file's name last, the version and
// base URL after the families.
TEST(FontManifestParserTest, AcceptsAnyKeyOrder) {
  const Parsed run =
      parse(R"({"families":[{"files":[{"crc32":5,"size":10,"name":"K/K_10.cpfont"}],"description":"Kay","name":"K"}],)"
            R"("baseUrl":"https://late.example/","version":1})");
  EXPECT_EQ(run.events, (Events{"family{", "file=K/K_10.cpfont size=10 crc=5", "desc=Kay", "name=K", "}family"}));
  EXPECT_EQ(run.version, 1);
  EXPECT_EQ(run.baseUrl, "https://late.example/");
  EXPECT_TRUE(run.complete);
}

// 5. Where the pieces are cut makes no difference.
TEST(FontManifestParserTest, ChunkingDoesNotChangeTheResult) {
  for (const size_t chunk :
       {size_t{1}, size_t{2}, size_t{3}, size_t{7}, size_t{13}, size_t{64}, size_t{511}, kTwoFamilies.size()}) {
    SCOPED_TRACE("chunk " + std::to_string(chunk));
    const Parsed run = parse(kTwoFamilies, chunk);
    EXPECT_EQ(run.events, kTwoFamiliesEvents);
    EXPECT_EQ(run.version, 2);
    EXPECT_EQ(run.baseUrl, "https://example.com/fonts/");
    EXPECT_FALSE(run.error);
    EXPECT_TRUE(run.complete);
  }
}

// 6. Escapes are what StreamingJsonParser makes of them: the simple ones decoded, \uXXXX passed
// through as its six characters (it does not decode to UTF-8).
TEST(FontManifestParserTest, EscapesInADescriptionArriveAsTheTokenParserDecodesThem) {
  const Parsed run =
      parse(R"({"version":2,"families":[{"name":"E","description":"say \"hi\" back\\slash sl\/ash tab\there caf)"
            "\\u00e9"
            R"("}]})");
  EXPECT_EQ(run.events,
            (Events{"family{", "name=E", "desc=say \"hi\" back\\slash sl/ash tab\there caf\\u00e9", "}family"}));

  // UTF-8 written as itself passes through untouched.
  EXPECT_EQ(parse("{\"families\":[{\"description\":\"caf\xC3\xA9\"}]}").events,
            (Events{"family{", "desc=caf\xC3\xA9", "}family"}));
}

// 7. A document cut short is not complete; one the token parser rejects is an error; so is a
// closing bracket that does not match the container the manifest has open.
TEST(FontManifestParserTest, TruncatedIsIncompleteAndMalformedIsAnError) {
  for (const size_t cut : {size_t{0}, size_t{1}, kTwoFamilies.size() / 2, kTwoFamilies.size() - 1}) {
    SCOPED_TRACE("cut at " + std::to_string(cut));
    const Parsed run = parse(kTwoFamilies.substr(0, cut));
    EXPECT_FALSE(run.complete);
    EXPECT_FALSE(run.error);
  }

  EXPECT_TRUE(parse(R"({"version": 2, "baseUrl": nope})").error);                           // a bad literal
  EXPECT_TRUE(parse(R"({"version": tru })").error);                                         // a cut literal
  EXPECT_TRUE(parse(R"(}{"version": 2})").error);                                           // a closer before the root
  EXPECT_TRUE(parse(R"({"families": [{"name": "A"]})").error);                              // ] closing a family
  EXPECT_TRUE(parse(R"({"families": [{"files": [{"name": "a"]}]}]})").error);               // ] closing a file
  EXPECT_TRUE(parse("{\"x\":" + std::string(40, '[') + std::string(40, ']') + "}").error);  // too deep
}

// What follows the root object is ignored, as a whole-document parse ignores it.
TEST(FontManifestParserTest, IgnoresContentAfterTheRootCloses) {
  const Parsed run = parse(R"({"version":2,"families":[]} {"families":[{"name":"Late"}],"version":3} nope)");
  EXPECT_TRUE(run.events.empty());
  EXPECT_EQ(run.version, 2);
  EXPECT_FALSE(run.error);
  EXPECT_TRUE(run.complete);
}

// 8. A file name that does not fit is reported, never handed over silently cut. Past the parser's
// own buffer it arrives truncated and flagged; past StreamingJsonParser's 512-byte token buffer the
// token parser drops it, and it arrives empty and flagged.
TEST(FontManifestParserTest, AFileNameTooLongForTheBufferIsReportedAsOverflow) {
  const size_t fits = FontManifestParser::FILE_NAME_BUF_SIZE - 1;

  const std::string exact(fits, 'a');
  EXPECT_EQ(parse(withFileName(exact)).events,
            (Events{"family{", "name=F", "file=" + exact + " size=5 crc=6", "}family"}));

  const std::string over(fits + 1, 'b');
  EXPECT_EQ(parse(withFileName(over)).events,
            (Events{"family{", "name=F", "file=" + over.substr(0, fits) + " size=5 crc=6 overflow", "}family"}));

  const std::string huge(600, 'c');
  const Parsed run = parse(withFileName(huge));
  EXPECT_EQ(run.events, (Events{"family{", "name=F", "file= size=5 crc=6 overflow", "}family"}));
  EXPECT_FALSE(run.error);
  EXPECT_TRUE(run.complete);
}

TEST(FontManifestParserTest, ADroppedFileNameIsReportedWhereverItSitsInTheObject) {
  const std::string huge(600, 'c');
  const Parsed run = parse(R"({"families":[{"name":"F","files":[{"size":5,"name":")" + huge + R"("}]}]})");
  EXPECT_EQ(run.events, (Events{"family{", "name=F", "file= size=5 crc=- overflow", "}family"}));
}

// A key too long for the token buffer is dropped with its value; the fields around it still count.
TEST(FontManifestParserTest, AnOverlongKeyIsIgnoredWithItsValue) {
  const std::string key(600, 'k');
  const Parsed run =
      parse(R"({"families":[{"name":"F","files":[{"name":"f.cpfont",")" + key + R"(":"zzz","size":5}]}]})");
  EXPECT_EQ(run.events, (Events{"family{", "name=F", "file=f.cpfont size=5 crc=-", "}family"}));
}

TEST(FontManifestParserTest, ABaseUrlTooLongIsReportedAsOverflow) {
  const size_t fits = FontManifestParser::BASE_URL_BUF_SIZE - 1;
  const auto withBaseUrl = [](const std::string& url) { return R"({"version":2,"baseUrl":")" + url + R"("})"; };

  Parsed run = parse(withBaseUrl(std::string(fits, 'u')));
  EXPECT_FALSE(run.baseUrlOverflow);
  EXPECT_EQ(run.baseUrl, std::string(fits, 'u'));

  run = parse(withBaseUrl(std::string(fits + 1, 'u')));
  EXPECT_TRUE(run.baseUrlOverflow);

  run = parse(withBaseUrl(std::string(600, 'u')));
  EXPECT_TRUE(run.baseUrlOverflow);
  EXPECT_EQ(run.baseUrl, "");
  EXPECT_TRUE(run.complete);
}

// A family name or description past the token buffer is dropped: no event, so the family's name
// stays empty and the caller's name check rejects it.
TEST(FontManifestParserTest, AnOverlongFamilyStringIsDropped) {
  const std::string huge(600, 'n');
  const Parsed run = parse(R"({"families":[{"name":")" + huge + R"(","description":")" + huge + R"("}]})");
  EXPECT_EQ(run.events, (Events{"family{", "}family"}));
}

// 9. size and crc32 count only as non-negative integers that fit uint32_t; anything else leaves
// size 0 and no crc32, as ArduinoJson's typed reads did.
TEST(FontManifestParserTest, NumbersMustBeNonNegativeIntegersThatFitUint32) {
  const Parsed run = parse(R"({"version":2,"families":[{"name":"N","files":[
    {"name":"a","size":4294967295,"crc32":4294967295},
    {"name":"b","size":-1,"crc32":-1},
    {"name":"c","size":1.5,"crc32":1.5},
    {"name":"d","size":1e3,"crc32":1e3},
    {"name":"e","size":4294967296,"crc32":4294967296},
    {"name":"f","size":"123","crc32":"123"},
    {"name":"g","size":0,"crc32":0},
    {"name":"h","size":null,"crc32":true},
    {"name":"i","size":99999999999999999999999,"crc32":18446744073709551617}
  ]}]})");
  EXPECT_EQ(run.events, (Events{
                            "family{",
                            "name=N",
                            "file=a size=4294967295 crc=4294967295",
                            "file=b size=0 crc=-",
                            "file=c size=0 crc=-",
                            "file=d size=0 crc=-",
                            "file=e size=0 crc=-",
                            "file=f size=0 crc=-",
                            "file=g size=0 crc=0",
                            "file=h size=0 crc=-",
                            "file=i size=0 crc=-",
                            "}family",
                        }));
  EXPECT_FALSE(run.error);
}

// version() is 0 until an integer version is read.
TEST(FontManifestParserTest, VersionIsZeroUnlessAnIntegerIsGiven) {
  EXPECT_EQ(parse(R"({"families":[]})").version, 0);
  EXPECT_EQ(parse(R"({"version":"2"})").version, 0);
  EXPECT_EQ(parse(R"({"version":2.0})").version, 0);
  EXPECT_EQ(parse(R"({"version":-1})").version, 0);
  EXPECT_EQ(parse(R"({"version":7})").version, 7);
}

// Elements of families[] and files[] that are not objects arrive as empty entries, which the
// caller's checks reject -- the way a whole-document parse saw them, as null objects.
TEST(FontManifestParserTest, NonObjectElementsArriveAsEmptyEntries) {
  const Parsed run = parse(
      R"({"version":2,"families":[1,"x",null,[{"name":"Hidden"}],{"name":"Real","files":[7,{"name":"f"},["g"]]}]})");
  EXPECT_EQ(run.events, (Events{
                            "family{",
                            "}family",
                            "family{",
                            "}family",
                            "family{",
                            "}family",
                            "family{",
                            "}family",
                            "family{",
                            "name=Real",
                            "file= size=0 crc=-",
                            "file=f size=0 crc=-",
                            "file= size=0 crc=-",
                            "}family",
                        }));
  EXPECT_TRUE(run.complete);
}

// families and files that are not arrays hold no entries; a root that is not an object holds no
// manifest, but is a complete document.
TEST(FontManifestParserTest, ContainersOfTheWrongKindHoldNothing) {
  EXPECT_TRUE(parse(R"({"version":2,"families":{"name":"X","files":[{"name":"x"}]}})").events.empty());
  EXPECT_EQ(parse(R"({"families":[{"name":"A","files":{"name":"x"}}]})").events,
            (Events{"family{", "name=A", "}family"}));
  const Parsed run = parse(R"([{"version":2,"families":[{"name":"A"}]}])");
  EXPECT_TRUE(run.events.empty());
  EXPECT_EQ(run.version, 0);
  EXPECT_TRUE(run.complete);
}

// reset() makes the parser good for a second pass over the same document.
TEST(FontManifestParserTest, ResetStartsAFreshDocument) {
  Recorder rec;
  FontManifestParser parser(rec.callbacks());
  parser.feed(R"({"version":1,"baseUrl":"https://a/","families":[{"name":"Cut")", 50);
  parser.reset();
  EXPECT_EQ(parser.version(), 0);
  EXPECT_STREQ(parser.baseUrl(), "");
  EXPECT_FALSE(parser.complete());
  rec.events.clear();
  parser.feed(kTwoFamilies.data(), kTwoFamilies.size());
  EXPECT_EQ(rec.events, kTwoFamiliesEvents);
  EXPECT_TRUE(parser.complete());
  EXPECT_FALSE(parser.hasError());
}

// Version 3: a family says its format and whether "Download all" takes it, in any order among its keys.
TEST(FontManifestParserTest, AVersion3FamilyGivesItsFormatAndItsPlaceInDownloadAll) {
  const Parsed run = parse(
      R"({"version":3,"baseUrl":"https://x/","families":[{"name":"NotoSansJP","inDownloadAll":false,"format":"otf",)"
      R"("files":[{"name":"NotoSansJP/NotoSansJP-Regular.otf","size":5,"crc32":6}],"description":"JP"},)"
      R"({"format":"cpfont","inDownloadAll":true,"name":"Alpha"}]})");
  EXPECT_EQ(run.events, (Events{
                            "family{",
                            "name=NotoSansJP",
                            "inDownloadAll=false",
                            "format=otf",
                            "file=NotoSansJP/NotoSansJP-Regular.otf size=5 crc=6",
                            "desc=JP",
                            "}family",
                            "family{",
                            "format=cpfont",
                            "inDownloadAll=true",
                            "name=Alpha",
                            "}family",
                        }));
  EXPECT_EQ(run.version, 3);
  EXPECT_TRUE(run.complete);
}

// inDownloadAll is a boolean or nothing: a string, a number or null is dropped, and the family still arrives.
TEST(FontManifestParserTest, AnInDownloadAllThatIsNotABooleanIsIgnored) {
  const Parsed run =
      parse(R"({"version":3,"families":[{"name":"A","inDownloadAll":"false"},{"name":"B","inDownloadAll":0},)"
            R"({"name":"C","inDownloadAll":null}]})");
  EXPECT_EQ(run.events, (Events{
                            "family{",
                            "name=A",
                            "}family",
                            "family{",
                            "name=B",
                            "}family",
                            "family{",
                            "name=C",
                            "}family",
                        }));
  EXPECT_TRUE(run.complete);
}

// A boolean that is an element of families[] is still the empty entry any non-object is.
TEST(FontManifestParserTest, ABooleanInAFamiliesArrayIsStillAnEmptyEntry) {
  EXPECT_EQ(parse(R"({"version":3,"families":[true]})").events, (Events{"family{", "}family"}));
}

// Only a family's own object says its format.
TEST(FontManifestParserTest, AFormatOutsideAFamilyIsIgnored) {
  const Parsed run =
      parse(R"({"version":3,"format":"otf","families":[{"name":"A","files":[{"name":"A/a.otf","format":"otf",)"
            R"("inDownloadAll":false}]}]})");
  EXPECT_EQ(run.events, (Events{"family{", "name=A", "file=A/a.otf size=0 crc=-", "}family"}));
}
