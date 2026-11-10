# Hungarian hyphenation patterns

`huhyphen.tex` is **Huhyphn v20031107**, the Hungarian hyphenation patterns by NAGY Bence, as published on CTAN:
<https://mirrors.ctan.org/language/hyphenation/huhyphen.tex> (SHA-256
`c43826996c331ff3b1147d69a485f64bf802dcb794af842435b4491e4567db5b`), byte for byte: 8-bit Cork (T1) encoding,
13,459 patterns.

Copyright (C) 2003, NAGY Bence. Distributed under the GNU General Public License version 2 ([`COPYING`](COPYING)),
as the file's header says. It is not compiled into the firmware: `scripts/build_hyphenation_packs.py` turns it into
the downloadable pack `assets/sd-fonts/Hyphenation-Hungarian/hyph-hu.cphyph` (a hypher trie of these patterns,
`scripts/hyphenation_trie.py --cork`), which is under the same licence. This file is that pack's source.

Why this version: hyph-utf8's current `hyph-hu.tex` (huhyphn by the same author, 62,851 patterns with compound-word
patterns) makes a 354 KB trie, nearly all of the reader's 384 KB hyphenation store on the X3 and X4. This set makes a
78 KB one. On 22,865 words from three Hungarian novels (Project Gutenberg) it breaks 95.6% of words exactly as the current set
does; measured against the current set's breaks, its precision and its recall are both 98.6%.
