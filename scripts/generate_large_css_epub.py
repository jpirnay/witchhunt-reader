#!/usr/bin/env python3
from __future__ import annotations

import argparse
import zipfile
from pathlib import Path

DEFAULT_RULE_COUNT = 1500
DEFAULT_SELECTORS_PER_RULE = 1


def build_large_css(rule_count: int, selectors_per_rule: int) -> str:
    lines = ["/* Generated large CSS fixture */"]
    for i in range(rule_count):
        selectors = [f".rule{i}_{j}" for j in range(selectors_per_rule)]
        selector_list = ", ".join(selectors)
        margin = 1 + (i % 10)
        padding = 2 + (i % 8)
        font_size = 90 + (i % 40)
        line_height = 1.0 + ((i % 5) * 0.1)
        decoration = "underline" if i % 4 == 0 else "none"
        lines.append(
            f"{selector_list} {{ margin: {margin}px; padding: {padding}px; font-size: {font_size}%; line-height: {line_height:.1f}; text-decoration: {decoration}; }}"
        )
    return "\n".join(lines) + "\n"


def build_epub(epub_path: Path, rule_count: int, selectors_per_rule: int) -> None:
    css_content = build_large_css(rule_count, selectors_per_rule)
    chapter_html = """<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"""
    chapter_html += """<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">\n<head>\n  <title>Large CSS Benchmark</title>\n  <link rel=\"stylesheet\" href=\"styles/large.css\" type=\"text/css\"/>\n</head>\n<body>\n  <h1>Large CSS performance fixture</h1>\n  <p>Each paragraph uses an external stylesheet with many selectors.</p>\n</body>\n</html>\n"""

    content_opf = f"""<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"uid\" version=\"3.0\">\n  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n    <dc:identifier id=\"uid\">large-css-benchmark</dc:identifier>\n    <dc:title>Large CSS Benchmark Fixture</dc:title>\n    <dc:language>en</dc:language>\n  </metadata>\n  <manifest>\n    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n    <item id=\"css\" href=\"styles/large.css\" media-type=\"text/css\"/>\n    <item id=\"chapter1\" href=\"chapter1.xhtml\" media-type=\"application/xhtml+xml\"/>\n  </manifest>\n  <spine>\n    <itemref idref=\"chapter1\"/>\n  </spine>\n</package>\n"""

    nav_xhtml = """<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"""
    nav_xhtml += """<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n<head><title>Navigation</title></head>\n<body>\n  <nav epub:type=\"toc\">\n    <h1>Contents</h1>\n    <ol>\n      <li><a href=\"chapter1.xhtml\">Large CSS chapter</a></li>\n    </ol>\n  </nav>\n</body>\n</html>\n"""

    container_xml = """<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"""
    container_xml += """<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"""
    container_xml += """  <rootfiles>\n"""
    container_xml += """    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"""
    container_xml += """  </rootfiles>\n</container>\n"""

    with zipfile.ZipFile(epub_path, "w", compression=zipfile.ZIP_DEFLATED) as epub:
        epub.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        epub.writestr("META-INF/container.xml", container_xml)
        epub.writestr("OEBPS/styles/large.css", css_content)
        epub.writestr("OEBPS/chapter1.xhtml", chapter_html)
        epub.writestr("OEBPS/content.opf", content_opf)
        epub.writestr("OEBPS/nav.xhtml", nav_xhtml)


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate a large-CSS EPUB fixture for parser performance testing.")
    parser.add_argument("--output", default="test/fixtures/test_large_css.epub", help="Output EPUB path")
    parser.add_argument("--rule-count", type=int, default=DEFAULT_RULE_COUNT, help="Number of CSS rules to generate")
    parser.add_argument("--selectors-per-rule", type=int, default=DEFAULT_SELECTORS_PER_RULE, help="Selectors per CSS rule")
    args = parser.parse_args()

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    build_epub(output_path, args.rule_count, args.selectors_per_rule)
    print(f"Generated {output_path} with {args.rule_count} CSS rules.")


if __name__ == "__main__":
    main()
