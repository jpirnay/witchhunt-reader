"""Real-prose timing books for Arabic: Jurji Zaydan's travel letters (Zaydan died in 1914, public domain), the England
chapter from Arabic Wikisource, and a Latin twin with the same paragraph count and per-paragraph word counts.

Usage: AR_WORDS=2500 python -I make_zaydan_books.py <repo> <download dir> <out dir> <chapter epubs...>
AR_WORDS cuts the text at the first paragraph past that many words (#656 measured 31 paragraphs, 2,529 words).
The downloaded EPUBs are untrusted data: only their <p> text is read, escaped, and checked.
"""
import html
import os
import re
import sys
import zipfile

repo, dl, out = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, os.path.join(repo, "test", "epubs"))
import make_test_shaped_indic as gen  # noqa: E402  (the repo's own generator)

sys.stdout.reconfigure(encoding="utf-8")
paragraphs = []
for name in sys.argv[4:]:
    with zipfile.ZipFile(os.path.join(dl, name)) as z:
        for entry in z.namelist():
            if not entry.endswith(".xhtml") or "/c0_" not in entry:
                continue
            body = z.read(entry).decode("utf-8", "replace")
            for raw in re.findall(r"<p[^>]*>(.*?)</p>", body, flags=re.S):
                text = html.unescape(re.sub(r"<[^>]+>", "", raw))
                text = " ".join(text.split())
                if not any(0x620 <= ord(c) <= 0x64A for c in text):
                    continue
                gen.check_text(text)
                paragraphs.append(html.escape(text, quote=False))
counts = [len(p.split(" ")) for p in paragraphs]
print(f"{len(sys.argv) - 4} chapters: {len(paragraphs)} paragraphs, {sum(counts)} words")
limit = int(os.environ.get("AR_WORDS", "0"))
if limit:
    kept, total = 0, 0
    while kept < len(paragraphs) and total < limit:
        total += counts[kept]
        kept += 1
    paragraphs, counts = paragraphs[:kept], counts[:kept]
    print(f"cut to {len(paragraphs)} paragraphs, {sum(counts)} words")

title = "رحلة إلى أوربا"
first = f"<h2>{title}</h2>\n<p>{paragraphs[0]}</p>\n"
second = "".join(f"<p>{p}</p>\n" for p in paragraphs)
arabic = [gen.chapter(title, first, "ar"), gen.chapter(title, second, "ar")]
english, english_counts = gen.long_chapters(gen.ENGLISH, "en", counts)
assert english_counts == counts
os.makedirs(out, exist_ok=True)
gen.write(os.path.join(out, "test_zaydan_ar.epub"), gen.epub_files(title, "zaydan-ar", "ar", arabic))
gen.write(os.path.join(out, "test_zaydan_en.epub"), gen.epub_files(gen.ENGLISH["title"], "zaydan-en", "en", english))
