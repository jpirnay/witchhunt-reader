"""Real-prose timing books for Hebrew: Bialik's story "Aryeh Baal Guf" (Bialik died in 1934, public domain) from
Hebrew Wikisource (the page 'אריה "בעל גוף"'), and a Latin twin with the same paragraph count and per-paragraph word
counts. Hebrew is the right-to-left path without shaping: the bidi passes and the TrueType measure that Arabic shares.
Usage: HE_WORDS=2500 python -I make_bialik_books.py <repo> <download dir> <out dir> <chapter epubs...>
HE_WORDS cuts the text at the first paragraph past that many words.
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
                if not any(0x5D0 <= ord(c) <= 0x5EA for c in text):
                    continue
                gen.check_text(text)
                paragraphs.append(html.escape(text, quote=False))
counts = [len(p.split(" ")) for p in paragraphs]
print(f"{len(sys.argv) - 4} chapters: {len(paragraphs)} paragraphs, {sum(counts)} words")
limit = int(os.environ.get("HE_WORDS", "0"))
if limit:
    kept, total = 0, 0
    while kept < len(paragraphs) and total < limit:
        total += counts[kept]
        kept += 1
    paragraphs, counts = paragraphs[:kept], counts[:kept]
    print(f"cut to {len(paragraphs)} paragraphs, {sum(counts)} words")

title = "אריה בעל גוף"
first = f"<h2>{title}</h2>\n<p>{paragraphs[0]}</p>\n"
second = "".join(f"<p>{p}</p>\n" for p in paragraphs)
hebrew = [gen.chapter(title, first, "he"), gen.chapter(title, second, "he")]
english, english_counts = gen.long_chapters(gen.ENGLISH, "en", counts)
assert english_counts == counts
os.makedirs(out, exist_ok=True)
gen.write(os.path.join(out, "test_bialik_he.epub"), gen.epub_files(title, "bialik-he", "he", hebrew))
gen.write(os.path.join(out, "test_bialik_en.epub"), gen.epub_files(gen.ENGLISH["title"], "bialik-en", "en", english))
