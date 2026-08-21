#pragma once

#include <Print.h>

#include <string>

// Rewrite a StarDict HTML definition into the well-formed XHTML document the
// chapter parser will accept, streaming the result into `out`.
//
// StarDict definitions are HTML fragments -- tag soup, in the worst case --
// while the layout path is driven by a strict XML parser. This bridges the two:
//
//   - wraps the fragment in <html><body>, because a fragment routinely has more
//     than one top-level element and the parser stops at the first root's end
//     tag (SaxParser::kTrailingDataIgnored);
//   - lowercases element and attribute names, because both the XML parser's
//     start/end matching and ChapterHtmlSlimParser's strcmp dispatch are
//     case-sensitive, so <B>bold</b> is both a parse error and an unstyled word;
//   - quotes unquoted and valueless attribute values (class=foo, <td nowrap>),
//     legal in HTML and fatal in XML;
//   - escapes bare '&' and stray '<' in text, while passing well-formed entity
//     references through untouched -- the parser resolves HTML named entities
//     like &nbsp; through its default handler, so they must survive intact;
//   - drops comments, doctypes and processing instructions.
//
// Deliberately NOT done here: self-closing void elements (<br> -> <br/>) and
// dropping the </br> that may follow one. SaxParser's htmlVoidTagRepair already
// rewrites the byte stream for both, and ChapterHtmlSlimParser enables it. The
// residue is an unpaired </br> with no <br> before it, which stays a parse error
// and falls back to plain text.
//
// Returns false when `out` refused a write -- which for the parser sink means
// the fragment failed to parse, so the caller falls back to plain text. Damage
// this cannot repair (mismatched or crossed tags) surfaces the same way.
bool normalizeDictionaryHtml(const std::string& html, Print& out);
