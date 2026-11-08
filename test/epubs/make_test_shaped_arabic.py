#!/usr/bin/env python3
"""Build the four shaped Arabic-script books: test_shaped_arabic.epub (ar), test_shaped_persian.epub (fa),
test_shaped_urdu.epub (ur) and test_shaped_bidi_indic.epub (he, Hindi words inside Hebrew paragraphs). ShapingRulesTest
checks its rules on every page of each, laid out with its font as the face of every font and style: the three Arabic
books with test/fixtures/fonts/NotoNaskhArabic-Book-Regular.ttf, the Hebrew one with NotoSansDevanagari-Book-Regular.ttf
(both made from the books by lib/EpdFont/scripts/make_shaped_book_fonts.py). Each has a golden shaped with its font;
the Arabic book also has an _unshaped golden, laid out with no face (the layout of a font that cannot shape: the line
pass's code points, as step 1 draws them), and a _1600 golden, laid out 1,600 px wide, where its lines pass the line
pass's 128 slots and take its fallback. BidiRulesTest runs its rules on the Arabic book at 460 and 1,600 px.

The texts are the Universal Declaration of Human Rights in each language, the UN's translation as the UDHR in XML
project keeps it, at eric-muller/udhr commit 588b3f4b2d0467aff54842a4b926551b69d5a66a:
  - Standard Arabic: https://raw.githubusercontent.com/eric-muller/udhr/588b3f4b2d0467aff54842a4b926551b69d5a66a/data/udhr/udhr_arb.xml
  - Persian:         https://raw.githubusercontent.com/eric-muller/udhr/588b3f4b2d0467aff54842a4b926551b69d5a66a/data/udhr/udhr_pes_1.xml
  - Urdu:            https://raw.githubusercontent.com/eric-muller/udhr/588b3f4b2d0467aff54842a4b926551b69d5a66a/data/udhr/udhr_urd.xml
  - Hebrew:          https://raw.githubusercontent.com/eric-muller/udhr/588b3f4b2d0467aff54842a4b926551b69d5a66a/data/udhr/udhr_heb.xml
The Hindi words of the Hebrew book are the Hindi book's (make_test_shaped_indic.py, udhr_hin.xml at the same commit),
so its font needs no code point the Devanagari book's does not. The texts are the files' paragraphs with their
whitespace collapsed, in NFC, held here as escapes: this script never fetches. Terms: OHCHR's UDHR Translation Project
lets its translations be reproduced with a link to it as the source ("Permissions", https://www.ohchr.org/en/human-rights/universal-declaration/universal-declaration-human-rights/about-universal-declaration-human-rights-translation-project),
and the UDHR in XML project, which carries them, calls them "part of our commons" (website/index.xml at that commit).
The vocalised Arabic paragraph (the basmala, then Articles 1 and 3 with their vowels written in) and the cases are this
repository's own.

Each Arabic-script book:
  - Chapter 1: the declaration's title (<h2>), the preamble's title (<h3>), and the preamble's first two paragraphs.
  - Chapter 2: Articles 1-3, each under its <h2> title. Article 1 sets a word <b>, a word <i> and a word in an inline
    font-size: 80% span. Then a paragraph of cases, and a Latin sentence (Article 1 in English, lang="en"), whose lines
    are laid out as any Latin book's. The cases, set off by the Arabic comma (a word glued to it):
    - a converter's same-style split of the language's name in brackets, (<span>..</span><span>..</span>): one word;
    - a word in brackets, which are mirrored and never shaped;
    - a bold letter inside a joined word (k<b>t</b>ab): three tokens, each shaped with its neighbours as context;
    - a lam-alef across a span (sal<span class="x">a</span>m). Noto Naskh Arabic, like Noto Sans Arabic, draws a
      lam-alef as two glyphs, so each token keeps its letter, in its joined form;
    - a mark-only span after a letter, in a tajweed-style colour and in <b>: merged into its letter's word, its style
      lost;
    - a superscript letter glued to a word, kept apart the same way;
    - tatweel inside a word;
    - digits beside words: the language's own (Arabic-Indic in Arabic, the extended ones in Persian and Urdu) and Latin
      ones, a year with its letter glued after it in Arabic; never shaped, ordered by bidi;
    - the AL question mark and semicolon glued to a word;
    - a Latin word inside the sentence, and a dir="ltr" span (an isolate inside the paragraph).
  - Arabic, beside those. Its one ligature across a style glue is Allah fully pointed (alef, lam, lam, shadda,
    superscript alef, heh: one glyph, U+FDF2, in the font's liga):
    - Allah split by a span after its first lam: drawn once, in the first token; the span's token keeps no glyph and
      has no word on its line;
    - Allah split after its alef, and before its heh: a glued Arabic neighbour gives its whole word as context (up to
      16 code points), so both tokens form the ligature alike and it is drawn once, by the token that holds its first
      letter; the other token keeps nothing and has no word on its line;
    - the lam-alef across a span with a U+0301 on the lam's token: the acute is a Text piece of its own, and the lam's
      run is drawn whole;
    - the lam-alef as a whole word, its alef the word's end, before a space (la shay');
    - a token that starts with marks before its letter, glued after a letter in another style (k<b>ata</b>ba): its
      marks are a Text piece;
    - a word stretched with tatweel past the 32 code points one shaping call takes (the piecewise loop);
    - a Latin paragraph (lang="en") with Arabic-Indic and Persian digits: no right-to-left letter, so no bidi, and no
      layout shapes them;
    - the vocalised paragraph: the basmala, its Allah split by a span after the first lam (the span's token keeps its
      kasra, a mark glyph on the first token's ligature), then Articles 1 and 3, with a lam-alef across a span inside a
      word. Its lines pass 128 slots at 1,600 px;
    - a paragraph at font-size 25% of Allah split by a span, 200 times: its lines take the line pass's fallback, and at
      1,600 px a line holds more than 255 pieces before the span tokens are dropped and fewer after, so it keeps no
      reading order (the guard);
    - Allah's first token with a Hebrew qamats after its lam (a mark of another script, transparent to bidi, in no
      Arabic run): the line pass gives the mark a slot of its own after its letter, which splits the run's segment, so
      the token is drawn as its code points; the span's token, the mark keeping it from its neighbour's context, is
      drawn from its own glyphs. In its own paragraph;
    - a 2-column table: its narrow cell holds a word wider than the cell (a forced split between letters), and in the
      second row Allah split by a span, glued to a bold number wider than the cell: the first token takes a line of its
      own (the breaker's fallback), drawing the ligature, and the span's token starts the next line with no word.
  - Persian, beside those: the translation's own ZWNJ words, mi-shavad as one token and with its ZWNJ starting a <b>
    (across an inline edge: kept, the tokens apart, the ZWNJ a Text piece the line pass absorbs), and a ZWJ after heh
    in an abbreviation (h.sh.).
  - Urdu, beside those: the translation's own full stops and extended digits.
  - No joiner in the Arabic book, so that its _unshaped golden is the layout master gives (which drops them).
The Hebrew book:
  - Chapter 1: the declaration's title and the preamble's first two paragraphs, a Hindi word in brackets in each.
  - Chapter 2: Articles 1-3, Article 1 ending with its first sentence in Hindi in quotation marks, after a colon (an
    RTL paragraph with a Hindi phrase and punctuation), Article 2 holding a Hindi word in brackets. Then a paragraph of
    cases: a Latin word wider than the page (split, its remainder taking no shaping) followed by the Hindi book's cases
    (a span split, a vowel sign in a <b>, a bold letter, a superscript letter); an English paragraph (lang="en",
    dir="ltr": a paragraph with letters of both directions runs the book's way otherwise) with one Hebrew word and
    Hindi words, whose lines without the Hebrew word run through the bidi pass's plain path; and the Latin sentence.

Regenerate with:  python -I test/epubs/make_test_shaped_arabic.py  (CPython 3.12; see write())
then the fonts (lib/EpdFont/scripts/make_shaped_book_fonts.py) and the goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import argparse
import os
import unicodedata
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

CONTAINER = (
    '<?xml version="1.0"?>\n'
    '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
    '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
    'media-type="application/oebps-package+xml"/></rootfiles>\n'
    "</container>\n"
)

CSS = """body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 1em }
.x { font-style: italic }
.small { font-size: 25% }
"""

# The letters, marks and signs the cases are built from, by name: no source holds Arabic or Hebrew literally.
ALEF, ALEF_HAMZA_ABOVE, ALEF_HAMZA_BELOW, ALEF_WASLA = "\u0627", "\u0623", "\u0625", "\u0671"
HAMZA, BEH, TEH, TEH_MARBUTA, JEEM, HAH, KHAH, DAL = "\u0621", "\u0628", "\u062a", "\u0629", "\u062c", "\u062d", "\u062e", "\u062f"
REH, SEEN, SHEEN, SAD, DAD, ZAH, AIN = "\u0631", "\u0633", "\u0634", "\u0635", "\u0636", "\u0638", "\u0639"
FEH, QAF, KAF, LAM, MEEM, NOON, HEH, WAW, YEH = "\u0641", "\u0642", "\u0643", "\u0644", "\u0645", "\u0646", "\u0647", "\u0648", "\u064a"
JEH, KEHEH, NOON_GHUNNA, FARSI_YEH, YEH_BARREE = "\u0698", "\u06a9", "\u06ba", "\u06cc", "\u06d2"
FATHATAN, KASRATAN, FATHA, DAMMA, KASRA, SHADDA, SUKUN = "\u064b", "\u064d", "\u064e", "\u064f", "\u0650", "\u0651", "\u0652"
HAMZA_ABOVE, SUPERSCRIPT_ALEF = "\u0654", "\u0670"
TATWEEL, COMMA, SEMICOLON, QUESTION, URDU_FULL_STOP = "\u0640", "\u060c", "\u061b", "\u061f", "\u06d4"
ARABIC_1948 = "\u0661\u0669\u0664\u0668"
PERSIAN_1948 = "\u06f1\u06f9\u06f4\u06f8"
PERSIAN_1402 = "\u06f1\u06f4\u06f0\u06f2"
COMBINING_ACUTE = "\u0301"
HEBREW_QAMATS = "\u05b8"  # a mark of another script: transparent to bidi, in no Arabic run
ZWNJ, ZWJ = "\u200c", "\u200d"

TAJWEED = '<span style="color: #c00000">'
SPAN_X = '<span class="x">'
DIR_LTR = '<span dir="ltr">UDHR 1948 (A/RES/217)</span>'

# Article 1's first sentence in English: the Latin sentence of every book.
LATIN_SENTENCE = "All human beings are born free and equal in dignity and rights."


def cases(items, end):
    """The cases paragraph: the items set off by the Arabic comma and a space."""
    return (COMMA + " ").join(items) + end


KITAB = KAF + TEH + ALEF + BEH  # kitab, book
SALAM = SEEN + LAM + SPAN_X + ALEF + "</span>" + MEEM  # salam, its alef in a span
# Allah, fully pointed (shadda and superscript alef on the second lam): the one word Noto Naskh Arabic draws as a
# ligature (its liga: alef, lam, lam, the two marks and heh in one glyph, U+FDF2). Its lam-alef is two glyphs, as in
# Noto Sans Arabic. Split by a span after its first lam, the ligature is drawn once, in the first token: the span's
# token keeps no glyph and has no word on its line.
ALLAH_TAIL = LAM + SHADDA + SUPERSCRIPT_ALEF + HEH
ALLAH = ALEF + LAM + SPAN_X + ALLAH_TAIL + "</span>"
PERSIAN_KITAB = KEHEH + TEH + ALEF + BEH

ARABIC = {
    "title": "\u0627\u0644\u0625\u0639\u0644\u0627\u0646 \u0627\u0644\u0639\u0627\u0644\u0645\u064a \u0644\u062d\u0642\u0648\u0642 \u0627\u0644\u0625\u0646\u0633\u0627\u0646",
    "preamble_title": "\u0627\u0644\u062f\u064a\u0628\u0627\u062c\u0629",
    "preamble": ["\u0644\u0645\u0651\u0627 \u0643\u0627\u0646 \u0627\u0644\u0627\u0639\u062a\u0631\u0627\u0641 \u0628\u0627\u0644\u0643\u0631\u0627\u0645\u0629 \u0627\u0644\u0645\u062a\u0623\u0635\u0644\u0629 \u0641\u064a \u062c\u0645\u064a\u0639 \u0623\u0639\u0636\u0627\u0621 \u0627\u0644\u0623\u0633\u0631\u0629 \u0627\u0644\u0628\u0634\u0631\u064a\u0629 \u0648\u0628\u062d\u0642\u0648\u0642\u0647\u0645 \u0627\u0644\u0645\u062a\u0633\u0627\u0648\u064a\u0629 \u0627\u0644\u062b\u0627\u0628\u062a\u0629 \u0647\u0648 \u0623\u0633\u0627\u0633 \u0627\u0644\u062d\u0631\u064a\u0629 \u0648\u0627\u0644\u0639\u062f\u0644 \u0648\u0627\u0644\u0633\u0644\u0627\u0645 \u0641\u064a \u0627\u0644\u0639\u0627\u0644\u0645.", "\u0648\u0644\u0645\u0627 \u0643\u0627\u0646 \u062a\u0646\u0627\u0633\u064a \u062d\u0642\u0648\u0642 \u0627\u0644\u0625\u0646\u0633\u0627\u0646 \u0648\u0627\u0632\u062f\u0631\u0627\u0624\u0647\u0627 \u0642\u062f \u0623\u0641\u0636\u064a\u0627 \u0625\u0644\u0649 \u0623\u0639\u0645\u0627\u0644 \u0647\u0645\u062c\u064a\u0629 \u0622\u0630\u062a \u0627\u0644\u0636\u0645\u064a\u0631 \u0627\u0644\u0625\u0646\u0633\u0627\u0646\u064a. \u0648\u0643\u0627\u0646 \u063a\u0627\u064a\u0629 \u0645\u0627 \u064a\u0631\u0646\u0648 \u0625\u0644\u064a\u0647 \u0639\u0627\u0645\u0629 \u0627\u0644\u0628\u0634\u0631 \u0627\u0646\u0628\u062b\u0627\u0642 \u0639\u0627\u0644\u0645 \u064a\u062a\u0645\u062a\u0639 \u0641\u064a\u0647 \u0627\u0644\u0641\u0631\u062f \u0628\u062d\u0631\u064a\u0629 \u0627\u0644\u0642\u0648\u0644 \u0648\u0627\u0644\u0639\u0642\u064a\u062f\u0629 \u0648\u064a\u062a\u062d\u0631\u0631 \u0645\u0646 \u0627\u0644\u0641\u0632\u0639 \u0648\u0627\u0644\u0641\u0627\u0642\u0629."],
    "articles": [
        ("\u0627\u0644\u0645\u0627\u062f\u0629 1", ["\u064a\u0648\u0644\u062f \u062c\u0645\u064a\u0639 \u0627\u0644\u0646\u0627\u0633 \u0623\u062d\u0631\u0627\u0631\u064b\u0627 \u0645\u062a\u0633\u0627\u0648\u064a\u0646 \u0641\u064a \u0627\u0644\u0643\u0631\u0627\u0645\u0629 \u0648\u0627\u0644\u062d\u0642\u0648\u0642. \u0648\u0642\u062f \u0648\u0647\u0628\u0648\u0627 \u0639\u0642\u0644\u0627\u064b \u0648\u0636\u0645\u064a\u0631\u064b\u0627 \u0648\u0639\u0644\u064a\u0647\u0645 \u0623\u0646 \u064a\u0639\u0627\u0645\u0644 \u0628\u0639\u0636\u0647\u0645 \u0628\u0639\u0636\u064b\u0627 \u0628\u0631\u0648\u062d \u0627\u0644\u0625\u062e\u0627\u0621."]),
        ("\u0627\u0644\u0645\u0627\u062f\u0629 2", ["\u0644\u0643\u0644 \u0625\u0646\u0633\u0627\u0646 \u062d\u0642 \u0627\u0644\u062a\u0645\u062a\u0639 \u0628\u0643\u0627\u0641\u0629 \u0627\u0644\u062d\u0642\u0648\u0642 \u0648\u0627\u0644\u062d\u0631\u064a\u0627\u062a \u0627\u0644\u0648\u0627\u0631\u062f\u0629 \u0641\u064a \u0647\u0630\u0627 \u0627\u0644\u0625\u0639\u0644\u0627\u0646\u060c \u062f\u0648\u0646 \u0623\u064a \u062a\u0645\u064a\u064a\u0632\u060c \u0643\u0627\u0644\u062a\u0645\u064a\u064a\u0632 \u0628\u0633\u0628\u0628 \u0627\u0644\u0639\u0646\u0635\u0631 \u0623\u0648 \u0627\u0644\u0644\u0648\u0646 \u0623\u0648 \u0627\u0644\u062c\u0646\u0633 \u0623\u0648 \u0627\u0644\u0644\u063a\u0629 \u0623\u0648 \u0627\u0644\u062f\u064a\u0646 \u0623\u0648 \u0627\u0644\u0631\u0623\u064a \u0627\u0644\u0633\u064a\u0627\u0633\u064a \u0623\u0648 \u0623\u064a \u0631\u0623\u064a \u0622\u062e\u0631\u060c \u0623\u0648 \u0627\u0644\u0623\u0635\u0644 \u0627\u0644\u0648\u0637\u0646\u064a \u0623\u0648 \u0627\u0644\u0625\u062c\u062a\u0645\u0627\u0639\u064a \u0623\u0648 \u0627\u0644\u062b\u0631\u0648\u0629 \u0623\u0648 \u0627\u0644\u0645\u064a\u0644\u0627\u062f \u0623\u0648 \u0623\u064a \u0648\u0636\u0639 \u0622\u062e\u0631\u060c \u062f\u0648\u0646 \u0623\u064a\u0629 \u062a\u0641\u0631\u0642\u0629 \u0628\u064a\u0646 \u0627\u0644\u0631\u062c\u0627\u0644 \u0648\u0627\u0644\u0646\u0633\u0627\u0621.", "\u0648\u0641\u0636\u0644\u0627\u064b \u0639\u0645\u0627 \u062a\u0642\u062f\u0645 \u0641\u0644\u0646 \u064a\u0643\u0648\u0646 \u0647\u0646\u0627\u0643 \u0623\u064a \u062a\u0645\u064a\u064a\u0632 \u0623\u0633\u0627\u0633\u0647 \u0627\u0644\u0648\u0636\u0639 \u0627\u0644\u0633\u064a\u0627\u0633\u064a \u0623\u0648 \u0627\u0644\u0642\u0627\u0646\u0648\u0646\u064a \u0623\u0648 \u0627\u0644\u062f\u0648\u0644\u064a \u0644\u0628\u0644\u062f \u0623\u0648 \u0627\u0644\u0628\u0642\u0639\u0629 \u0627\u0644\u062a\u064a \u064a\u0646\u062a\u0645\u064a \u0625\u0644\u064a\u0647\u0627 \u0627\u0644\u0641\u0631\u062f \u0633\u0648\u0627\u0621 \u0643\u0627\u0646 \u0647\u0630\u0627 \u0627\u0644\u0628\u0644\u062f \u0623\u0648 \u062a\u0644\u0643 \u0627\u0644\u0628\u0642\u0639\u0629 \u0645\u0633\u062a\u0642\u0644\u0627\u064b \u0623\u0648 \u062a\u062d\u062a \u0627\u0644\u0648\u0635\u0627\u064a\u0629 \u0623\u0648 \u063a\u064a\u0631 \u0645\u062a\u0645\u062a\u0639 \u0628\u0627\u0644\u062d\u0643\u0645 \u0627\u0644\u0630\u0627\u062a\u064a \u0623\u0648 \u0643\u0627\u0646\u062a \u0633\u064a\u0627\u062f\u062a\u0647 \u062e\u0627\u0636\u0639\u0629 \u0644\u0623\u064a \u0642\u064a\u062f \u0645\u0646 \u0627\u0644\u0642\u064a\u0648\u062f."]),
        ("\u0627\u0644\u0645\u0627\u062f\u0629 3", ["\u0644\u0643\u0644 \u0641\u0631\u062f \u0627\u0644\u062d\u0642 \u0641\u064a \u0627\u0644\u062d\u064a\u0627\u0629 \u0648\u0627\u0644\u062d\u0631\u064a\u0629 \u0648\u0633\u0644\u0627\u0645\u0629 \u0634\u062e\u0635\u0647."]),
    ],
    # Article 1's words set <b>, <i> and at 80%: jami', ahraran, mutasawin.
    "bold": JEEM + MEEM + YEH + AIN,
    "italic": ALEF_HAMZA_ABOVE + HAH + REH + ALEF + REH + FATHATAN + ALEF,
    "small": MEEM + TEH + SEEN + ALEF + WAW + YEH + NOON,
    "cases": cases([
        "(<span>" + ALEF + LAM + AIN + REH + "</span><span>" + BEH + YEH + TEH_MARBUTA + "</span>)",  # al-arabiyya
        "(" + KAF + LAM + MEEM + TEH_MARBUTA + ")",  # kalima
        KAF + "<b>" + TEH + "</b>" + ALEF + BEH,
        SALAM,
        SEEN + LAM + COMBINING_ACUTE + SPAN_X + ALEF + "</span>" + MEEM,  # an acute after the lam: a Text piece
        LAM + SPAN_X + ALEF + "</span> " + SHEEN + YEH + HAMZA,  # la shay', nothing
        KAF + TAJWEED + FATHA + "</span>" + TEH + BEH,  # kataba's first fatha coloured
        QAF + "<b>" + DAMMA + "</b>" + LAM,  # qul
        KITAB + "<sup>" + BEH + "</sup>",
        KAF + "<b>" + FATHA + TEH + FATHA + "</b>" + BEH + FATHA,  # kataba, its second syllable bold
        ALLAH,
        # Split elsewhere, the ligature is drawn once as well: after the alef, the alef's token draws it and the
        # span's token keeps nothing; before the heh, the first token draws it and the heh's token keeps nothing.
        ALEF + SPAN_X + LAM + ALLAH_TAIL + "</span>",
        ALEF + LAM + LAM + SHADDA + SUPERSCRIPT_ALEF + SPAN_X + HEH + "</span>",
        KAF + TATWEEL + TEH + TATWEEL + ALEF + BEH,
        JEEM + TATWEEL * 30 + MEEM + YEH + LAM,  # jamil, 34 code points
        SEEN + NOON + TEH_MARBUTA + " " + ARABIC_1948 + MEEM,  # sana 1948 m., the year of the Common Era
        SEEN + NOON + TEH_MARBUTA + " 1948" + MEEM,
        KITAB + QUESTION + " " + KITAB + SEMICOLON + " " + KAF + LAM + MEEM + TEH_MARBUTA + " Declaration " + FEH + YEH
        + " " + JEEM + MEEM + LAM + TEH_MARBUTA,  # kalima Declaration fi jumla: the word in a sentence
        DIR_LTR,
    ], "."),
    # The Latin paragraph whose digits no layout shapes: no right-to-left letter, so its paragraph has no bidi.
    "latin_digits": "The General Assembly adopted it in 1948, written " + ARABIC_1948 + " in Arabic and "
                    + PERSIAN_1948 + " in Persian.",
    # The basmala, then Articles 1 and 3 with their vowels: bismi llahi r-rahmani r-rahim. yuladu jami'u n-nasi ...
    # li-kulli fardin al-haqqu fi l-hayati wa-l-hurriyyati wa-salamati shakhsihi. Marks in canonical order (NFC).
    "vocalised": " ".join([
        BEH + KASRA + SEEN + SUKUN + MEEM + KASRA,
        ALEF + LAM + SPAN_X + ALLAH_TAIL + KASRA + "</span>",  # the ligature split by a span, a space after it
        ALEF_WASLA + LAM + REH + FATHA + SHADDA + HAH + SUKUN + MEEM + FATHA + SUPERSCRIPT_ALEF + NOON + KASRA,
        ALEF_WASLA + LAM + REH + FATHA + SHADDA + HAH + KASRA + YEH + MEEM + KASRA,
        YEH + DAMMA + WAW + LAM + FATHA + DAL + DAMMA,
        JEEM + FATHA + MEEM + KASRA + YEH + AIN + DAMMA,
        ALEF + LAM + NOON + FATHA + SHADDA + ALEF + SEEN + KASRA,
        ALEF_HAMZA_ABOVE + FATHA + HAH + SUKUN + REH + FATHA + ALEF + REH + FATHATAN + ALEF,
        MEEM + DAMMA + TEH + FATHA + SEEN + FATHA + ALEF + WAW + KASRA + YEH + NOON + FATHA,
        FEH + KASRA + YEH,
        ALEF + LAM + SUKUN + KAF + FATHA + REH + FATHA + ALEF + MEEM + FATHA + TEH_MARBUTA + KASRA,
        WAW + FATHA + ALEF + LAM + SUKUN + HAH + DAMMA + QAF + DAMMA + WAW + QAF + KASRA + ".",
        WAW + FATHA + QAF + FATHA + DAL + SUKUN,
        WAW + DAMMA + HEH + KASRA + BEH + DAMMA + WAW + ALEF,
        AIN + FATHA + QAF + SUKUN + LAM + FATHATAN + ALEF,
        WAW + FATHA + DAD + FATHA + MEEM + KASRA + YEH + REH + FATHATAN + ALEF,
        WAW + FATHA + AIN + FATHA + LAM + FATHA + YEH + SUKUN + HEH + KASRA + MEEM + SUKUN,
        ALEF_HAMZA_ABOVE + FATHA + NOON + SUKUN,
        YEH + DAMMA + AIN + FATHA + ALEF + MEEM + KASRA + LAM + FATHA,
        BEH + FATHA + AIN + SUKUN + DAD + DAMMA + HEH + DAMMA + MEEM + SUKUN,
        BEH + FATHA + AIN + SUKUN + DAD + FATHATAN + ALEF,
        BEH + KASRA + REH + DAMMA + WAW + HAH + KASRA,
        ALEF + LAM + SUKUN + ALEF_HAMZA_BELOW + KASRA + KHAH + FATHA + ALEF + HAMZA + KASRA + ".",
        LAM + KASRA + KAF + DAMMA + LAM + KASRA + SHADDA,
        FEH + FATHA + REH + SUKUN + DAL + KASRATAN,
        ALEF + LAM + SUKUN + HAH + FATHA + QAF + DAMMA + SHADDA,
        FEH + KASRA + YEH,
        ALEF + LAM + SUKUN + HAH + FATHA + YEH + FATHA + ALEF + TEH_MARBUTA + KASRA,
        WAW + FATHA + ALEF + LAM + SUKUN + HAH + DAMMA + REH + KASRA + SHADDA + YEH + FATHA + SHADDA + TEH_MARBUTA
        + KASRA,
        WAW + FATHA + SEEN + FATHA + LAM + FATHA + SPAN_X + ALEF + "</span>" + MEEM + FATHA + TEH_MARBUTA + KASRA,
        SHEEN + FATHA + KHAH + SUKUN + SAD + KASRA + HEH + KASRA + ".",
    ]),
    "stress": " ".join([ALLAH] * 200),
    # The ligature's first token with a mark of another script: the line pass gives the mark a slot of its own after
    # its letter, which splits the run's segment (the guard draws the token as its code points); the mark also keeps
    # the span's token from its neighbour's context, so that one is drawn from its own glyphs.
    "degraded": ALEF + LAM + HEBREW_QAMATS + SPAN_X + ALLAH_TAIL + "</span>" + " " + KITAB + ".",
    # The table: two article titles, and the title with the preamble's as one word, wider than its cell; in the second
    # row after a lam-alef across a span.
    "table": [
        ("\u0627\u0644\u0645\u0627\u062f\u0629 1", ("\u0627\u0644\u0625\u0639\u0644\u0627\u0646 \u0627\u0644\u0639\u0627\u0644\u0645\u064a \u0644\u062d\u0642\u0648\u0642 \u0627\u0644\u0625\u0646\u0633\u0627\u0646" + "\u0627\u0644\u062f\u064a\u0628\u0627\u062c\u0629").replace(" ", "")),
        ("\u0627\u0644\u0645\u0627\u062f\u0629 2", ALLAH + "<b>" + ARABIC_1948 * 10 + "</b>"),
    ],
}

PERSIAN = {
    "title": "\u0627\u0639\u0644\u0627\u0645\u06cc\u0647 \u062c\u0647\u0627\u0646\u06cc \u062d\u0642\u0648\u0642 \u0628\u0634\u0631",
    "preamble_title": "\u0645\u0642\u062f\u0645\u0647",
    "preamble": ["\u0627\u0632 \u0622\u0646\u062c\u0627 \u06a9\u0647 \u0634\u0646\u0627\u0633\u0627\u0626\u06cc \u062d\u06cc\u062b\u06cc\u062a \u0630\u0627\u062a\u06cc \u06a9\u0644\u06cc\u0647\u0654 \u0627\u0639\u0636\u0627\u06cc \u062e\u0627\u0646\u0648\u0627\u062f\u0647 \u0628\u0634\u0631\u06cc \u0648 \u062d\u0642\u0648\u0642 \u06cc\u06a9\u0633\u0627\u0646 \u0648 \u0627\u0646\u062a\u0642\u0627\u0644 \u0646\u0627\u067e\u0630\u06cc\u0631 \u0622\u0646\u0627\u0646 \u0627\u0633\u0627\u0633 \u0622\u0632\u0627\u062f\u06cc \u0648 \u0639\u062f\u0627\u0644\u062a \u0648 \u0635\u0644\u062d \u0631\u0627 \u062f\u0631 \u062c\u0647\u0627\u0646 \u062a\u0634\u06a9\u06cc\u0644 \u0645\u06cc\u062f\u0647\u062f\u060c", "\u0627\u0632 \u0622\u0646\u062c\u0627 \u06a9\u0647 \u0639\u062f\u0645 \u0634\u0646\u0627\u0633\u0627\u0626\u06cc \u0648 \u062a\u062d\u0642\u06cc\u0631 \u062d\u0642\u0648\u0642 \u0628\u0634\u0631 \u0645\u0646\u062a\u0647\u06cc \u0628\u0647 \u0627\u0639\u0645\u0627\u0644 \u0648\u062d\u0634\u06cc\u0627\u0646\u0647\u200c\u0627\u06cc \u06af\u0631\u062f\u06cc\u062f\u0647 \u0627\u0633\u062a \u06a9\u0647 \u0631\u0648\u062d \u0628\u0634\u0631\u06cc\u062a \u0631\u0627 \u0628\u0639\u0635\u06cc\u0627\u0646 \u0648\u0627\u062f\u0627\u0634\u062a\u0647 \u0648 \u0638\u0647\u0648\u0631 \u062f\u0646\u06cc\u0627\u0626\u06cc \u06a9\u0647 \u062f\u0631 \u0622\u0646 \u0627\u0641\u0631\u0627\u062f \u0628\u0634\u0631 \u062f\u0631 \u0628\u06cc\u0627\u0646 \u0648 \u0639\u0642\u06cc\u062f\u0647 \u0622\u0632\u0627\u062f \u0648 \u0627\u0632 \u062a\u0631\u0633 \u0648 \u0641\u0642\u0631 \u0641\u0627\u0631\u063a \u0628\u0627\u0634\u0646\u062f \u0628\u0639\u0646\u0648\u0627\u0646 \u0628\u0627\u0644\u0627\u062a\u0631\u06cc\u0646 \u0622\u0645\u0627\u0644 \u0628\u0634\u0631 \u0627\u0639\u0644\u0627\u0645 \u0634\u062f\u0647 \u0627\u0633\u062a\u060c"],
    "articles": [
        ("\u0645\u0627\u062f\u0647\u0654 \u0627\u0648\u0644", ["\u062a\u0645\u0627\u0645 \u0627\u0641\u0631\u0627\u062f \u0628\u0634\u0631 \u0622\u0632\u0627\u062f \u0628\u062f\u0646\u06cc\u0627 \u0645\u06cc\u0627\u06cc\u0646\u062f \u0648 \u0627\u0632 \u0644\u062d\u0627\u0638 \u062d\u06cc\u062b\u06cc\u062a \u0648 \u062d\u0642\u0648\u0642 \u0628\u0627 \u0647\u0645 \u0628\u0631\u0627\u0628\u0631\u0646\u062f. \u0647\u0645\u0647 \u062f\u0627\u0631\u0627\u06cc \u0639\u0642\u0644 \u0648 \u0648\u062c\u062f\u0627\u0646 \u0645\u06cc\u0628\u0627\u0634\u0646\u062f \u0648 \u0628\u0627\u06cc\u062f \u0646\u0633\u0628\u062a \u0628\u06cc\u06a9\u062f\u06cc\u06af\u0631 \u0628\u0627 \u0631\u0648\u062d \u0628\u0631\u0627\u062f\u0631\u06cc \u0631\u0641\u062a\u0627\u0631 \u06a9\u0646\u0646\u062f."]),
        ("\u0645\u0627\u062f\u0647\u0654 \u062f\u0648\u0645", ["\u0647\u0631 \u06a9\u0633 \u0645\u06cc\u062a\u0648\u0627\u0646\u062f \u0628\u062f\u0648\u0646 \u0647\u06cc\u0686\u06af\u0648\u0646\u0647 \u062a\u0645\u0627\u06cc\u0632 \u0645\u062e\u0635\u0648\u0635\u0627 \u0627\u0632 \u062d\u06cc\u062b \u0646\u0698\u0627\u062f\u060c \u0631\u0646\u06af\u060c \u062c\u0646\u0633\u060c \u0632\u0628\u0627\u0646\u060c \u0645\u0630\u0647\u0628\u060c \u0639\u0642\u06cc\u062f\u0647\u0654 \u0633\u06cc\u0627\u0633\u06cc \u06cc\u0627 \u0647\u0631 \u0639\u0642\u06cc\u062f\u0647 \u062f\u06cc\u06af\u0631 \u0648 \u0647\u0645\u0686\u0646\u06cc\u0646 \u0645\u0644\u06cc\u062a\u060c \u0648\u0636\u0639 \u0627\u062c\u062a\u0645\u0627\u0639\u06cc\u060c \u062b\u0631\u0648\u062a\u060c \u0648\u0644\u0627\u062f\u062a \u06cc\u0627 \u0647\u0631 \u0645\u0648\u0642\u0639\u06cc\u062a \u062f\u06cc\u06af\u0631\u060c \u0627\u0632 \u062a\u0645\u0627\u0645 \u062d\u0642\u0648\u0642 \u0648 \u06a9\u0644\u06cc\u0647\u0654 \u0622\u0632\u0627\u062f\u06cc\u200c\u0647\u0627\u0626\u06cc\u06a9\u0647 \u062f\u0631 \u0627\u0639\u0644\u0627\u0645\u06cc\u0647 \u0630\u06a9\u0631 \u062d\u0627\u0636\u0631 \u0634\u062f\u0647 \u0627\u0633\u062a\u060c \u0628\u0647\u0631\u0647\u200c\u0645\u0646\u062f \u06af\u0631\u062f\u062f.", "\u0628\u0639\u0644\u0627\u0648\u0647 \u0647\u06cc\u0686 \u062a\u0628\u0639\u06cc\u0636\u06cc \u0628\u0639\u0645\u0644 \u0646\u062e\u0648\u0627\u0647\u062f \u0622\u0645\u062f \u06a9\u0647 \u0645\u0628\u062a\u0646\u06cc \u0628\u0631 \u0648\u0636\u0639 \u0633\u06cc\u0627\u0633\u06cc\u060c \u0627\u062f\u0627\u0631\u06cc \u0648 \u0642\u0636\u0627\u0626\u06cc \u06cc\u0627 \u0628\u06cc\u0646\u200c\u0627\u0644\u0645\u0644\u0644\u06cc \u06a9\u0634\u0648\u0631 \u06cc\u0627 \u0633\u0631\u0632\u0645\u06cc\u0646\u06cc \u0628\u0627\u0634\u062f \u06a9\u0647 \u0634\u062e\u0635 \u0628\u0622\u0646 \u062a\u0639\u0644\u0642 \u062f\u0627\u0631\u062f\u060c \u062e\u0648\u0627\u0647 \u0627\u06cc\u0646 \u06a9\u0634\u0648\u0631 \u0645\u0633\u062a\u0642\u0644\u060c \u062a\u062d\u062a \u0642\u06cc\u0648\u0645\u06cc\u062a \u06cc\u0627 \u063a\u06cc\u0631 \u062e\u0648\u062f\u0645\u062e\u062a\u0627\u0631 \u0628\u0648\u062f\u0647 \u06cc\u0627 \u062d\u0627\u06a9\u0645\u06cc\u062a \u0622\u0646 \u0628\u0634\u06a9\u0644\u06cc \u0645\u062d\u062f\u0648\u062f \u0634\u062f\u0647 \u0628\u0627\u0634\u062f."]),
        ("\u0645\u0627\u062f\u0647\u0654 \u0633\u0648\u0645", ["\u0647\u0631 \u06a9\u0633 \u062d\u0642 \u0632\u0646\u062f\u06af\u06cc\u060c \u0622\u0632\u0627\u062f\u06cc \u0648 \u0627\u0645\u0646\u06cc\u062a \u0634\u062e\u0635\u06cc \u062f\u0627\u0631\u062f."]),
    ],
    # afrad, azad, heysiyat
    "bold": ALEF + FEH + REH + ALEF + DAL,
    "italic": "\u0622\u0632" + ALEF + DAL,
    "small": HAH + FARSI_YEH + "\u062b" + FARSI_YEH + TEH,
    "cases": cases([
        "(<span>" + FEH + ALEF + "</span><span>" + REH + SEEN + FARSI_YEH + "</span>)",  # farsi
        "(" + PERSIAN_KITAB + ")",
        KEHEH + "<b>" + TEH + "</b>" + ALEF + BEH,
        SALAM,
        PERSIAN_KITAB + TAJWEED + KASRA + "</span>",  # ketab-e, the ezafe's kasra coloured
        KHAH + ALEF + NOON + HEH + "<b>" + HAMZA_ABOVE + "</b>",  # khane-ye, the ezafe's hamza bold
        PERSIAN_KITAB + "<sup>" + BEH + "</sup>",
        KEHEH + TATWEEL + TEH + TATWEEL + ALEF + BEH,
        MEEM + FARSI_YEH + "<b>" + ZWNJ + SHEEN + WAW + DAL + "</b>",  # mi-shavad, its ZWNJ across the edge
        MEEM + FARSI_YEH + ZWNJ + SHEEN + WAW + DAL,
        SEEN + ALEF + LAM + " " + PERSIAN_1948,  # sal-e 1948
        SEEN + ALEF + LAM + " 1948",
        SEEN + ALEF + LAM + " " + PERSIAN_1402 + " " + HEH + ZWJ + "." + SHEEN,  # 1402 h.sh., the solar Hijri year
        PERSIAN_KITAB + QUESTION + " " + PERSIAN_KITAB + SEMICOLON + " " + WAW + ALEF + JEH + HEH + HAMZA_ABOVE
        + " Declaration " + DAL + REH + " " + JEEM + MEEM + LAM + HEH,  # vazhe-ye Declaration dar jomle
        DIR_LTR,
    ], "."),
}

URDU = {
    "title": "\u0627\u0646\u0633\u0627\u0646\u06cc \u062d\u0642\u0648\u0642 \u06a9\u0627 \u0639\u0627\u0644\u0645\u06cc \u0645\u0646\u0634\u0648\u0631",
    "preamble_title": "\u062a\u0645\u06c1\u06cc\u062f",
    "preamble": ["\u0686\u0648\u0646\u06a9\u06c1 \u06c1\u0631 \u0627\u0646\u0633\u0627\u0646 \u06a9\u06cc \u0630\u0627\u062a\u06cc \u0639\u0632\u062a \u0627\u0648\u0631 \u062d\u0631\u0645\u062a \u0627\u0648\u0631 \u0627\u0646\u0633\u0627\u0646\u0648\u06ba \u06a9\u06d2 \u0645\u0633\u0627\u0648\u06cc \u0627\u0648\u0631 \u0646\u0627\u0642\u0627\u0628\u0644\u0650 \u0627\u0646\u062a\u0642\u0627\u0644 \u062d\u0642\u0648\u0642 \u06a9\u0648 \u062a\u0633\u0644\u06cc\u0645 \u06a9\u0631\u0646\u0627 \u062f\u0646\u06cc\u0627 \u0645\u06cc\u06ba \u0622\u0632\u0627\u062f\u06cc\u060c \u0627\u0646\u0635\u0627\u0641 \u0627\u0648\u0631 \u0627\u0645\u0646 \u06a9\u06cc \u0628\u0646\u06cc\u0627\u062f \u06c1\u06d2\u060c", "\u0686\u0648\u0646\u06a9\u06c1 \u0627\u0646\u0633\u0627\u0646\u06cc \u062d\u0642\u0648\u0642 \u0633\u06d2 \u0644\u0627\u067e\u0631\u0648\u0627\u0626\u06cc \u0627\u0648\u0631 \u0627\u0646 \u06a9\u06cc \u0628\u06d2 \u062d\u0631\u0645\u062a\u06cc \u0627\u06a9\u062b\u0631 \u0627\u06cc\u0633\u06d2 \u0648\u062d\u0634\u06cc\u0627\u0646\u06c1 \u0627\u0641\u0639\u0627\u0644 \u06a9\u06cc \u0634\u06a9\u0644 \u0645\u06cc\u06ba \u0638\u0627\u06c1\u0631 \u06c1\u0648\u0626\u06cc \u06c1\u06d2 \u062c\u0646 \u0633\u06d2 \u0627\u0646\u0633\u0627\u0646\u06cc\u062a \u06a9\u06d2 \u0636\u0645\u06cc\u0631 \u06a9\u0648 \u0633\u062e\u062a \u0635\u062f\u0645\u06d2 \u067e\u06c1\u0646\u0686\u06d2 \u06c1\u06cc\u06ba \u0627\u0648\u0631 \u0639\u0627\u0645 \u0627\u0646\u0633\u0627\u0646\u0648\u06ba \u06a9\u06cc \u0628\u0644\u0646\u062f \u062a\u0631\u06cc\u0646 \u0622\u0631\u0632\u0648 \u06cc\u06c1 \u0631\u06c1\u06cc \u06c1\u06d2 \u06a9\u06c1 \u0627\u06cc\u0633\u06cc \u062f\u0646\u06cc\u0627 \u0648\u062c\u0648\u062f \u0645\u06cc\u06ba \u0622\u0626\u06d2 \u062c\u0633 \u0645\u06cc\u06ba \u062a\u0645\u0627\u0645 \u0627\u0646\u0633\u0627\u0646\u0648\u06ba \u06a9\u0648 \u0627\u067e\u0646\u06cc \u0628\u0627\u062a \u06a9\u06c1\u0646\u06d2 \u0627\u0648\u0631 \u0627\u067e\u0646\u06d2 \u0639\u0642\u06cc\u062f\u06d2 \u067e\u0631 \u0642\u0627\u0626\u0645 \u0631\u06c1\u0646\u06d2 \u06a9\u06cc \u0622\u0632\u0627\u062f\u06cc \u062d\u0627\u0635\u0644 \u06c1\u0648 \u0627\u0648\u0631 \u062e\u0648\u0641 \u0627\u0648\u0631 \u0627\u062d\u062a\u06cc\u0627\u062c \u0633\u06d2 \u0645\u062d\u0641\u0648\u0638 \u0631\u06c1\u06cc\u06ba\u060c"],
    "articles": [
        ("\u062f\u0641\u0639\u06c1 \u06f1\u06d4", ["\u062a\u0645\u0627\u0645 \u0627\u0646\u0633\u0627\u0646 \u0622\u0632\u0627\u062f \u0627\u0648\u0631 \u062d\u0642\u0648\u0642 \u0648 \u0639\u0632\u062a \u06a9\u06d2 \u0627\u0639\u062a\u0628\u0627\u0631 \u0633\u06d2 \u0628\u0631\u0627\u0628\u0631 \u067e\u06cc\u062f\u0627 \u06c1\u0648\u0626\u06d2 \u06c1\u06cc\u06ba\u06d4 \u0627\u0646\u06c1\u06cc\u06ba \u0636\u0645\u06cc\u0631 \u0627\u0648\u0631 \u0639\u0642\u0644 \u0648\u062f\u06cc\u0639\u062a \u06c1\u0648\u0626\u06cc \u06c1\u06d2\u06d4 \u0627\u0633 \u0644\u0626\u06d2 \u0627\u0646\u06c1\u06cc\u06ba \u0627\u06cc\u06a9 \u062f\u0648\u0633\u0631\u06d2 \u06a9\u06d2 \u0633\u0627\u062a\u06be \u0628\u06be\u0627\u0626\u06cc \u0686\u0627\u0631\u06d2 \u06a9\u0627 \u0633\u0644\u0648\u06a9 \u06a9\u0631\u0646\u0627 \u0686\u0627\u06c1\u06cc\u0626\u06d2\u06d4"]),
        ("\u062f\u0641\u0639\u06c1 \u06f2\u06d4", ["\u06c1\u0631 \u0634\u062e\u0635 \u0627\u0646 \u062a\u0645\u0627\u0645 \u0622\u0632\u0627\u062f\u06cc\u0648\u06ba \u0627\u0648\u0631 \u062d\u0642\u0648\u0642 \u06a9\u0627 \u0645\u0633\u062a\u062d\u0642 \u06c1\u06d2 \u062c\u0648 \u0627\u0633 \u0627\u0639\u0644\u0627\u0646 \u0645\u06cc\u06ba \u0628\u06cc\u0627\u0646 \u06a9\u0626\u06d2 \u06af\u0626\u06d2 \u06c1\u06cc\u06ba\u060c \u0627\u0648\u0631 \u0627\u0633 \u062d\u0642 \u067e\u0631 \u0646\u0633\u0644\u060c \u0631\u0646\u06af\u060c \u062c\u0646\u0633\u060c \u0632\u0628\u0627\u0646\u060c \u0645\u0630\u06c1\u0628 \u0627\u0648\u0631 \u0633\u06cc\u0627\u0633\u06cc \u062a\u0641\u0631\u06cc\u0642 \u06a9\u0627 \u06cc\u0627 \u06a9\u0633\u06cc \u0642\u0633\u0645 \u06a9\u06d2 \u0639\u0642\u06cc\u062f\u06d2\u060c \u0642\u0648\u0645\u060c \u0645\u0639\u0627\u0634\u0631\u06d2\u060c \u062f\u0648\u0644\u062a \u06cc\u0627 \u062e\u0627\u0646\u062f\u0627\u0646\u06cc \u062d\u06cc\u062b\u06cc\u062a \u0648\u063a\u06cc\u0631\u06c1 \u06a9\u0627 \u06a9\u0648\u0626\u06cc \u0627\u062b\u0631 \u0646\u06c1 \u067e\u0691\u06d2 \u06af\u0627\u06d4", "\u0627\u0633 \u06a9\u06d2 \u0639\u0644\u0627\u0648\u06c1 \u062c\u0633 \u0639\u0644\u0627\u0642\u06d2 \u06cc\u0627 \u0645\u0644\u06a9 \u0633\u06d2 \u062c\u0648 \u0634\u062e\u0635 \u062a\u0639\u0644\u0642 \u0631\u06a9\u06be\u062a\u0627 \u06c1\u06d2 \u0627\u0633 \u06a9\u06cc \u0633\u06cc\u0627\u0633\u06cc \u06a9\u06cc\u0641\u06cc\u062a \u062f\u0627\u0626\u0631\u06c1 \u0627\u062e\u062a\u06cc\u0627\u0631 \u06cc\u0627 \u0628\u06cc\u0646 \u0627\u0644\u0627\u0642\u0648\u0627\u0645\u06cc \u062d\u06cc\u062b\u06cc\u062a \u06a9\u06cc \u0628\u0646\u0627 \u067e\u0631 \u0627\u0633 \u0633\u06d2 \u06a9\u0648\u0626\u06cc \u0627\u0645\u062a\u06cc\u0627\u0632\u06cc \u0633\u0644\u0648\u06a9 \u0646\u06c1\u06cc\u06ba \u06a9\u06cc\u0627 \u062c\u0627\u0626\u06d2 \u06af\u0627\u06d4 \u0686\u0627\u06c1\u06d2 \u0648\u06c1 \u0645\u0644\u06a9 \u06cc\u0627 \u0639\u0644\u0627\u0642\u06c1 \u0622\u0632\u0627\u062f \u06c1\u0648 \u06cc\u0627 \u062a\u0648\u0644\u06cc\u062a\u06cc \u06c1\u0648 \u06cc\u0627 \u063a\u06cc\u0631 \u0645\u062e\u062a\u0627\u0631 \u06c1\u0648 \u06cc\u0627 \u0633\u06cc\u0627\u0633\u06cc \u0627\u0642\u062a\u062f\u0627\u0631 \u06a9\u06d2 \u0644\u062d\u0627\u0638 \u0633\u06d2 \u06a9\u0633\u06cc \u062f\u0648\u0633\u0631\u06cc \u0628\u0646\u062f\u0634 \u06a9\u0627 \u067e\u0627\u0628\u0646\u062f \u06c1\u0648\u06d4"]),
        ("\u062f\u0641\u0639\u06c1 \u06f3\u06d4", ["\u06c1\u0631 \u0634\u062e\u0635 \u06a9\u0648 \u0627\u067e\u0646\u06cc \u062c\u0627\u0646\u060c \u0622\u0632\u0627\u062f\u06cc \u0627\u0648\u0631 \u0630\u0627\u062a\u06cc \u062a\u062d\u0641\u0651\u0638 \u06a9\u0627 \u062d\u0642 \u06c1\u06d2\u06d4"]),
    ],
    # insan, azad, e'tibar
    "bold": ALEF + NOON + SEEN + ALEF + NOON,
    "italic": "\u0622" + "\u0632" + ALEF + DAL,
    "small": ALEF + AIN + TEH + BEH + ALEF + REH,
    "cases": cases([
        "(<span>" + ALEF + REH + "</span><span>" + DAL + WAW + "</span>)",  # urdu
        "(" + PERSIAN_KITAB + ")",
        KEHEH + "<b>" + TEH + "</b>" + ALEF + BEH,
        SALAM,
        HAH + QAF + WAW + QAF + TAJWEED + KASRA + "</span>",  # huquq-e, the izafat's zer coloured
        DAL + "<b>" + DAMMA + "</b>" + NOON + FARSI_YEH + ALEF,  # duniya, a bold pesh
        PERSIAN_KITAB + "<sup>" + BEH + "</sup>",
        KEHEH + TATWEEL + TEH + TATWEEL + ALEF + BEH,
        SEEN + ALEF + LAM + " " + PERSIAN_1948,
        SEEN + ALEF + LAM + " 1948",
        PERSIAN_KITAB + QUESTION + " " + PERSIAN_KITAB + SEMICOLON + " " + LAM + FEH + ZAH + " Declaration " + ALEF
        + FARSI_YEH + KEHEH + " " + JEEM + MEEM + LAM + YEH_BARREE + " " + MEEM + FARSI_YEH + NOON_GHUNNA,
        DIR_LTR,
    ], URDU_FULL_STOP),
}

# The Hindi book's words (make_test_shaped_indic.py): manav, adhikar, manushya, and Article 1's first sentence.
MANAV = "\u092e\u093e\u0928\u0935"
ADHIKAR = "\u0905\u0927\u093f\u0915\u093e\u0930"
MANUSHYA = "\u092e\u0928\u0941\u0937\u094d\u092f"
HINDI_SENTENCE = (
    "\u0938\u092d\u0940 \u092e\u0928\u0941\u0937\u094d\u092f\u094b\u0902 \u0915\u094b \u0917\u094c\u0930\u0935 "
    "\u0914\u0930 \u0905\u0927\u093f\u0915\u093e\u0930\u094b\u0902 \u0915\u0947 \u092e\u093e\u092e\u0932\u0947 "
    "\u092e\u0947\u0902 \u091c\u0928\u094d\u092e\u091c\u093e\u0924 \u0938\u094d\u0935\u0924\u0928\u094d\u0924\u094d"
    "\u0930\u0924\u093e \u0914\u0930 \u0938\u092e\u093e\u0928\u0924\u093e \u092a\u094d\u0930\u093e\u092a\u094d\u0924 "
    "\u0939\u0948"
)
DANDA = "\u0964"
OPEN_QUOTE, CLOSE_QUOTE = "\u00ab", "\u00bb"  # the Hebrew text's quotation marks
HINDI_CASES = (
    "(<span>\u0939\u093f</span><span>\u0928\u094d\u0926\u0940</span>), \u0915<b>\u093f</b>, \u0915<b>\u092e</b>\u0932,"
    " \u0928\u0940\u0924\u093f<sup>\u0915</sup>"
)

# Hebrew: ha-adam, enosh, li-zkhuyot, be-hindit, ha-mila, be-germanit, adam.
HA_ADAM = "\u05d4\u05d0\u05d3\u05dd"
ENOSH = "\u05d0\u05e0\u05d5\u05e9"
LI_ZKHUYOT = "\u05dc\u05d6\u05db\u05d5\u05d9\u05d5\u05ea"
BE_HINDIT = "\u05d1\u05d4\u05d9\u05e0\u05d3\u05d9\u05ea"
HA_MILA = "\u05d4\u05de\u05d9\u05dc\u05d4"
BE_GERMANIT = "\u05d1\u05d2\u05e8\u05de\u05e0\u05d9\u05ea"
ADAM = "\u05d0\u05d3\u05dd"
# A word wider than the page: split, and the Hindi words after it keep their store entries.
LONG_LATIN = "Donaudampfschifffahrtselektrizit\u00e4tenhauptbetriebswerkbauunterbeamtengesellschaft"

HEBREW = {
    "title": "\u05d4\u05db\u05e8\u05d6\u05d4 \u05dc\u05db\u05dc \u05d1\u05d0\u05d9 \u05e2\u05d5\u05dc\u05dd \u05d1\u05d3\u05d1\u05e8 \u05d6\u05db\u05d5\u05d9\u05d5\u05ea \u05d4\u05d0\u05d3\u05dd",
    "preamble_title": "",
    "preamble": ["\u05d4\u05d5\u05d0\u05d9\u05dc \u05d5\u05d4\u05db\u05e8\u05d4 \u05d1\u05db\u05d1\u05d5\u05d3 \u05d4\u05d8\u05d1\u05e2\u05d9 \u05d0\u05e9\u05e8 \u05dc\u05db\u05dc \u05d1\u05e0\u05d9 \u05de\u05e9\u05e4\u05d7\u05ea \u05d4\u05d0\u05d3\u05dd \u05d5\u05d1\u05d6\u05db\u05d5\u05d9\u05d5\u05ea\u05d9\u05d4\u05dd \u05d4\u05e9\u05d5\u05d5\u05ea \u05d5\u05d4\u05d1\u05dc\u05ea\u05d9 \u05e0\u05e4\u05e7\u05e2\u05d5\u05ea \u05d4\u05d5\u05d0 \u05d9\u05e1\u05d5\u05d3 \u05d4\u05d7\u05d5\u05e4\u05e9, \u05d4\u05e6\u05d3\u05e7 \u05d5\u05d4\u05e9\u05dc\u05d5\u05dd \u05d1\u05e2\u05d5\u05dc\u05dd.", "\u05d4\u05d5\u05d0\u05d9\u05dc \u05d5\u05d4\u05d6\u05dc\u05d6\u05d5\u05dc \u05d1\u05d6\u05db\u05d5\u05d9\u05d5\u05ea \u05d4\u05d0\u05d3\u05dd \u05d5\u05d1\u05d9\u05d6\u05d5\u05d9\u05df \u05d4\u05d1\u05e9\u05d9\u05dc\u05d5 \u05de\u05e2\u05e9\u05d9\u05dd \u05e4\u05e8\u05d0\u05d9\u05d9\u05dd \u05e9\u05e4\u05d2\u05e2\u05d5 \u05e7\u05e9\u05d4 \u05d1\u05de\u05e6\u05e4\u05d5\u05e0\u05d4 \u05e9\u05dc \u05d4\u05d0\u05e0\u05d5\u05e9\u05d5\u05ea; \u05d5\u05d1\u05e0\u05d9\u05df \u05e2\u05d5\u05dc\u05dd, \u05e9\u05d1\u05d5 \u05d9\u05d9\u05d4\u05e0\u05d5 \u05db\u05dc \u05d9\u05e6\u05d5\u05e8\u05d9 \u05d0\u05e0\u05d5\u05e9 \u05de\u05d7\u05d9\u05e8\u05d5\u05ea \u05d4\u05d3\u05d9\u05d1\u05d5\u05e8 \u05d5\u05d4\u05d0\u05de\u05d5\u05e0\u05d4 \u05d5\u05de\u05df \u05d4\u05d7\u05d9\u05e8\u05d5\u05ea \u05de\u05e4\u05d7\u05d3 \u05d5\u05de\u05de\u05d7\u05e1\u05d5\u05e8, \u05d4\u05d5\u05db\u05e8\u05d6 \u05db\u05e8\u05d0\u05e9 \u05e9\u05d0\u05d9\u05e4\u05d5\u05ea\u05d9\u05d5 \u05e9\u05dc \u05db\u05dc \u05d0\u05d3\u05dd."],
    "articles": [
        ("\u05e1\u05e2\u05d9\u05e3 \u05d0.", ["\u05db\u05dc \u05d1\u05e0\u05d9 \u05d0\u05d3\u05dd \u05e0\u05d5\u05dc\u05d3\u05d5 \u05d1\u05e0\u05d9 \u05d7\u05d5\u05e8\u05d9\u05df \u05d5\u05e9\u05d5\u05d5\u05d9\u05dd \u05d1\u05e2\u05e8\u05db\u05dd \u05d5\u05d1\u05d6\u05db\u05d5\u05d9\u05d5\u05ea\u05d9\u05d4\u05dd. \u05db\u05d5\u05dc\u05dd \u05d7\u05d5\u05e0\u05e0\u05d5 \u05d1\u05ea\u05d1\u05d5\u05e0\u05d4 \u05d5\u05d1\u05de\u05e6\u05e4\u05d5\u05df, \u05dc\u05e4\u05d9\u05db\u05da \u05d7\u05d5\u05d1\u05d4 \u05e2\u05dc\u05d9\u05d4\u05dd \u05dc\u05e0\u05d4\u05d5\u05d2 \u05d0\u05d9\u05e9 \u05d1\u05e8\u05e2\u05d4\u05d5 \u05d1\u05e8\u05d5\u05d7 \u05e9\u05dc \u05d0\u05d7\u05d5\u05d4."]),
        ("\u05e1\u05e2\u05d9\u05e3 \u05d1.", ["\u05db\u05dc \u05d0\u05d3\u05dd \u05d6\u05db\u05d0\u05d9 \u05dc\u05d6\u05db\u05d5\u05d9\u05d5\u05ea \u05d5\u05dc\u05d7\u05e8\u05d5\u05d9\u05d5\u05ea \u05e9\u05e0\u05e7\u05d1\u05e2\u05d5 \u05d1\u05d4\u05db\u05e8\u05d6\u05d4 \u05d6\u05d5 \u05dc\u05dc\u05d0 \u05d4\u05e4\u05dc\u05d9\u05d4 \u05db\u05dc\u05e9\u05d4\u05d9\u05d0 \u05de\u05d8\u05e2\u05de\u05d9 \u05d2\u05d6\u05e2, \u05e6\u05d1\u05e2, \u05de\u05d9\u05df, \u05dc\u05e9\u05d5\u05df, \u05d3\u05d7, \u05d3\u05e2\u05d4 \u05e4\u05d5\u05dc\u05d9\u05d8\u05d9\u05ea \u05d0\u05d5 \u05d3\u05e2\u05d4 \u05d1\u05d1\u05e2\u05d9\u05d5\u05ea \u05d0\u05d7\u05e8\u05d5\u05ea, \u05d1\u05d2\u05dc\u05dc \u05de\u05d5\u05e6\u05d0 \u05dc\u05d0\u05d5\u05de\u05d9 \u05d0\u05d5 \u05d7\u05d1\u05e8\u05ea\u05d9, \u05e7\u05e0\u05d9\u05df, \u05dc\u05d9\u05d3\u05d4 \u05d0\u05d5 \u05de\u05e2\u05de\u05d3 \u05d0\u05d7\u05e8.", "\u05d2\u05d3\u05d5\u05dc\u05d4 \u05de\u05d6\u05d5, \u05dc\u05d0 \u05d9\u05d5\u05e4\u05dc\u05d4 \u05d0\u05d3\u05dd \u05e2\u05dc \u05e4\u05d9 \u05de\u05e2\u05de\u05d3\u05d4 \u05d4\u05de\u05d3\u05d9\u05e0\u05d9, \u05e2\u05dc \u05e4\u05d9 \u05e1\u05de\u05db\u05d5\u05ea\u05d4 \u05d0\u05d5 \u05e2\u05dc \u05e4\u05d9 \u05de\u05e2\u05de\u05d3\u05d4 \u05d4\u05d1\u05d9\u05e0\u05dc\u05d0\u05d5\u05de\u05d9 \u05e9\u05dc \u05d4\u05de\u05d3\u05d9\u05e0\u05d4 \u05d0\u05d5 \u05d4\u05d0\u05e8\u05e5 \u05e9\u05d0\u05dc\u05d9\u05d4 \u05d4\u05d5\u05d0 \u05e9\u05d9\u05d9\u05da, \u05d3\u05d9\u05df \u05e9\u05d4\u05d0\u05e8\u05e5 \u05d4\u05d9\u05d0 \u05e2\u05e6\u05de\u05d0\u05d9\u05ea, \u05d5\u05d1\u05d9\u05df \u05e9\u05d4\u05d9\u05d0 \u05e0\u05ea\u05d5\u05e0\u05d4 \u05dc\u05e0\u05d0\u05de\u05e0\u05d5\u05ea, \u05d1\u05d9\u05df \u05e9\u05d4\u05d9\u05d0 \u05e0\u05d8\u05d5\u05dc\u05ea \u05e9\u05dc\u05d8\u05d5\u05df \u05e2\u05e6\u05de\u05d9 \u05d5\u05d1\u05d9\u05df \u05e9\u05e8\u05d9\u05d1\u05d5\u05e0\u05d5\u05ea\u05d4 \u05de\u05d5\u05d2\u05d1\u05dc\u05ea \u05db\u05dc \u05d4\u05d2\u05d1\u05dc\u05d4 \u05d0\u05d7\u05e8\u05ea."]),
        ("\u05e1\u05e2\u05d9\u05e3 \u05d2.", ["\u05db\u05dc \u05d0\u05d3\u05dd \u05d9\u05e9 \u05dc\u05d5 \u05d4\u05d6\u05db\u05d5\u05ea \u05dc\u05d7\u05d9\u05d9\u05dd, \u05dc\u05d7\u05e8\u05d5\u05ea \u05d5\u05dc\u05d1\u05d8\u05d7\u05d5\u05df \u05d0\u05d9\u05e9\u05d9."]),
    ],
}

BOOKS = [
    ("test_shaped_arabic.epub", "ar", ARABIC),
    ("test_shaped_persian.epub", "fa", PERSIAN),
    ("test_shaped_urdu.epub", "ur", URDU),
]

# The bidi controls no source may hold literally (U+200E/F, U+202A-202E, U+2066-2069, U+061C).
BIDI_CONTROLS = {0x200E, 0x200F, 0x061C} | set(range(0x202A, 0x202F)) | set(range(0x2066, 0x206A))


def check_text(text):
    if not unicodedata.is_normalized("NFC", text):
        raise SystemExit(f"not NFC: {text[:40].encode('unicode_escape')}")
    if any(ord(c) in BIDI_CONTROLS for c in text):
        raise SystemExit(f"a bidi control in: {text[:40].encode('unicode_escape')}")


def wrap(paragraph, word, open_tag, close_tag):
    """`paragraph` with its first word `word` (a whole word, between spaces) inside the tags."""
    words = paragraph.split(" ")
    if word not in words:
        raise SystemExit(f"{word.encode('unicode_escape')} is not a word of {paragraph[:40].encode('unicode_escape')}")
    i = words.index(word)
    words[i] = open_tag + word + close_tag
    return " ".join(words)


def insert_after(paragraph, word, text):
    """`paragraph` with `text` after its first word `word`, a space between."""
    return wrap(paragraph, word, "", " " + text)


def chapter(title, body, lang):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE html>\n'
        f'<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{lang}" lang="{lang}">\n'
        f'<head>\n  <title>{title}</title>\n  <link rel="stylesheet" type="text/css" href="style.css"/>\n</head>\n'
        f"<body>\n{body}</body>\n</html>\n"
    )


def opf(title, identifier, lang, chapters):
    items = "".join(
        f'    <item id="ch{i}" href="ch{i}.xhtml" media-type="application/xhtml+xml"/>\n'
        for i in range(1, len(chapters) + 1)
    )
    refs = "".join(f'    <itemref idref="ch{i}"/>\n' for i in range(1, len(chapters) + 1))
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        f"    <dc:title>{title}</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        f"    <dc:language>{lang}</dc:language>\n"
        f'    <dc:identifier id="bookid">{identifier}</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n"
        '    <item id="css" href="style.css" media-type="text/css"/>\n'
        f"{items}"
        "  </manifest>\n"
        f"  <spine>\n{refs}  </spine>\n"
        "</package>\n"
    )


def latin_paragraph(text, attributes=""):
    return f'<p lang="en" xml:lang="en"{attributes}>{text}</p>\n'


def book_chapters(text, lang):
    for value in [text["title"], text["preamble_title"], *text["preamble"], text["cases"]]:
        check_text(value)
    for title, paragraphs in text["articles"]:
        for value in [title, *paragraphs]:
            check_text(value)

    first = f"<h2>{text['title']}</h2>\n<h3>{text['preamble_title']}</h3>\n"
    first += "".join(f"<p>{p}</p>\n" for p in text["preamble"])

    second = ""
    for n, (title, paragraphs) in enumerate(text["articles"]):
        second += f"<h2>{title}</h2>\n"
        for p in paragraphs:
            if n == 0:
                p = wrap(p, text["bold"], "<b>", "</b>")
                p = wrap(p, text["italic"], "<i>", "</i>")
                p = wrap(p, text["small"], '<span style="font-size: 80%">', "</span>")
            second += f"<p>{p}</p>\n"
    second += f"<p>{text['cases']}</p>\n"
    second += latin_paragraph(LATIN_SENTENCE)
    if "latin_digits" in text:
        check_text(text["latin_digits"] + text["vocalised"] + text["stress"] + text["degraded"])
        second += latin_paragraph(text["latin_digits"])
        second += f"<p>{text['vocalised']}</p>\n"
        second += f'<p class="small">{text["stress"]}</p>\n'
        second += f"<p>{text['degraded']}</p>\n"
    if "table" in text:
        second += "<table>\n"
        for label, word in text["table"]:
            check_text(label + word)
            second += f"<tr><td>{label}</td><td>{word}</td></tr>\n"
        second += "</table>\n"
    return [chapter(text["title"], first, lang), chapter(text["articles"][0][0], second, lang)]


def hebrew_chapters():
    text = HEBREW
    first = f"<h2>{text['title']}</h2>\n"
    first += f"<p>{insert_after(text['preamble'][0], HA_ADAM, '(' + MANAV + ')')}</p>\n"
    first += f"<p>{insert_after(text['preamble'][1], ENOSH, '(' + MANUSHYA + ')')}</p>\n"

    (t1, (a1,)), (t2, (a2, a2b)), (t3, (a3,)) = text["articles"]
    second = f"<h2>{t1}</h2>\n<p>{a1} {BE_HINDIT}: {OPEN_QUOTE}{HINDI_SENTENCE} {DANDA}{CLOSE_QUOTE}.</p>\n"
    second += f"<h2>{t2}</h2>\n<p>{insert_after(a2, LI_ZKHUYOT, '(' + ADHIKAR + ')')}</p>\n<p>{a2b}</p>\n"
    second += f"<h2>{t3}</h2>\n<p>{a3}</p>\n"
    second += f"<p>{HA_MILA} {LONG_LATIN} ({BE_GERMANIT}), {BE_HINDIT}: {HINDI_CASES}.</p>\n"
    second += latin_paragraph(
        f"The Hebrew word {ADAM} means a human being, as {MANAV} and {MANUSHYA} do in Hindi. The Declaration says"
        f" {HINDI_SENTENCE} in its Hindi text, and {LATIN_SENTENCE[:-1]} in its English one.",
        ' dir="ltr"',
    )
    second += latin_paragraph(LATIN_SENTENCE)
    for c in (first, second):
        check_text(c)
    return [chapter(text["title"], first, "he"), chapter(t1, second, "he")]


def write(path, files):
    with zipfile.ZipFile(path, "w") as z:
        for name, data, method in files:
            # A fixed date keeps a regenerated book byte-identical, and a fixed system (Unix, as the first books were
            # written on) keeps it so on Windows. The deflated bytes are zlib's: zlib-ng (Python 3.14's) writes
            # others, so regenerate with CPython 3.12.
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 8, 0, 0, 0))
            info.create_system = 3
            info.compress_type = method
            z.writestr(info, data)
    print("wrote", path)


def epub_files(title, identifier, lang, chapters):
    stored, deflated = zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED
    files = [("mimetype", "application/epub+zip", stored), ("META-INF/container.xml", CONTAINER, deflated)]
    files.append(("OEBPS/content.opf", opf(title, identifier, lang, chapters), deflated))
    files.append(("OEBPS/style.css", CSS, deflated))
    files += [(f"OEBPS/ch{i}.xhtml", c, deflated) for i, c in enumerate(chapters, 1)]
    return files


def main():
    # The module's texts are escapes, but a console's code page may not print what they hold: --help keeps to ASCII.
    parser = argparse.ArgumentParser(description="Build the four shaped Arabic-script fixture books (see the header).")
    parser.parse_args()

    for name, lang, text in BOOKS:
        chapters = book_chapters(text, lang)
        write(os.path.join(HERE, name), epub_files(text["title"], name[: -len(".epub")], lang, chapters))
    write(os.path.join(HERE, "test_shaped_bidi_indic.epub"),
          epub_files(HEBREW["title"], "test_shaped_bidi_indic", "he", hebrew_chapters()))


if __name__ == "__main__":
    main()
