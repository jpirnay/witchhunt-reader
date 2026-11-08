"""Real-prose timing books: Premchand's Gaban (1931, public domain), chapters from Hindi Wikisource (Premchand
Rachnavali vol. 5), and a Latin twin with the same paragraph count and per-paragraph word counts.

Usage: python -I make_gaban_books.py <repo> <download dir> <out dir> <chapter epubs...>
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
                if not any(0x900 <= ord(c) <= 0x97F for c in text):
                    continue
                gen.check_text(text)
                paragraphs.append(html.escape(text, quote=False))
counts = [len(p.split(" ")) for p in paragraphs]
print(f"{len(sys.argv) - 4} chapters: {len(paragraphs)} paragraphs, {sum(counts)} words")
limit = int(os.environ.get("GABAN_WORDS", "0"))
if limit:
    kept, total = 0, 0
    while kept < len(paragraphs) and total < limit:
        total += counts[kept]
        kept += 1
    paragraphs, counts = paragraphs[:kept], counts[:kept]
    print(f"cut to {len(paragraphs)} paragraphs, {sum(counts)} words")

title = "गबन"  # Gaban
first = f"<h2>{title}</h2>\n<p>{paragraphs[0]}</p>\n"
second = "".join(f"<p>{p}</p>\n" for p in paragraphs)
hindi = [gen.chapter(title, first, "hi"), gen.chapter(title, second, "hi")]
english, english_counts = gen.long_chapters(gen.ENGLISH, "en", counts)
assert english_counts == counts
os.makedirs(out, exist_ok=True)
gen.write(os.path.join(out, "test_gaban_hi.epub"), gen.epub_files(title, "gaban-hi", "hi", hindi))
gen.write(os.path.join(out, "test_gaban_en.epub"), gen.epub_files(gen.ENGLISH["title"], "gaban-en", "en", english))
