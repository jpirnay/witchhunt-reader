"""Downloads a Wikisource page as an EPUB through ws-export into <outdir>/<name>.epub and prints its URL and sha256.
Usage: ws_export.py <lang> <title> <outdir> <name>  (the title may be given as \\u escapes)."""
import hashlib
import os
import sys
import urllib.parse
import urllib.request

lang, title, outdir, name = sys.argv[1:5]
if title.isascii():
    title = title.encode("ascii").decode("unicode_escape")
url = "https://ws-export.wmcloud.org/?" + urllib.parse.urlencode(
    {"format": "epub-3", "lang": lang, "page": title, "fonts": "", "images": "false"})
req = urllib.request.Request(url, headers={"User-Agent": "witchhunt-reader-research/1.0 (token-length survey)"})
with urllib.request.urlopen(req, timeout=900) as r:
    data = r.read()
path = os.path.join(outdir, name + ".epub")
with open(path, "wb") as f:
    f.write(data)
sys.stdout.reconfigure(encoding="utf-8")
print(url)
print(path, len(data), hashlib.sha256(data).hexdigest())
