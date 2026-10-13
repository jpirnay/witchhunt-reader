"""THROWAWAY (TTF step 0): cut the bench's CJK sample out of a full CJK TrueType font.

usage: make_cjk_subset.py SRC DST [COLLECTION_INDEX]   (the index selects a member of a .ttc)"""
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

src, dst = sys.argv[1], sys.argv[2]
number = int(sys.argv[3]) if len(sys.argv) > 3 else -1
han = list(range(0x4E00, 0x9FA6, 41))
font = TTFont(src, fontNumber=number) if number >= 0 else TTFont(src)
if "glyf" not in font:
    sys.exit(f"{src} has no 'glyf' table (CFF outlines?): the TTF design reads TrueType outlines only")
opts = subset.Options()
opts.hinting = False
opts.layout_features = []
sub = subset.Subsetter(opts)
sub.populate(unicodes=list(range(0x20, 0x7F)) + han)
sub.subset(font)
font.save(dst)
cmap = font.getBestCmap()
print(f"{dst}: {sum(1 for c in han if c in cmap)} of {len(han)} sampled Han codepoints present")
