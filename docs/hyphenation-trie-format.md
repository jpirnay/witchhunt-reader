# Hypher Binary Tries

CrossPoint embeds the exact binary automata produced by
[Typst's `hypher`](https://github.com/typst/hypher). 

## File layout

Each `.bin` blob is a single self-contained automaton:

```
uint32_t root_addr_be;  // big-endian offset of the root node
uint8_t  levels[];      // shared "levels" tape (dist/score pairs)
uint8_t  nodes[];       // node records packed back-to-back
```

The size of the `levels` tape is implicit. Individual nodes reference slices
inside that tape via 12-bit offsets, so no additional pointers are required.

### Node encoding

Every node starts with a single control byte:

- Bit 7 – set when the node stores scores (`levels`).
- Bits 5-6 – stride of the target deltas (1, 2, or 3 bytes, big-endian).
- Bits 0-4 – transition count (values ≥ 31 spill into an extra byte).

If the `levels` flag is set, two more bytes follow. Together they encode a
12-bit offset into the global `levels` tape and a 4-bit length. Each byte in the
levels tape packs a distance/score pair as `dist * 10 + score`, where `dist`
counts how many UTF-8 bytes we advanced since the previous digit.

After the optional levels header come the transition labels (one byte per edge)
followed by the signed target deltas. Targets are stored as relative offsets
from the current node address. Deltas up to ±128 fit in a single byte, larger
distances grow to 2 or 3 bytes. The runtime walks the transitions with a simple
linear scan and materializes the absolute address by adding the decoded delta
to the current node’s base.

## Embedding blobs into the firmware

The helper script `scripts/generate_hyphenation_trie.py` acts as a thin
wrapper: it reads the hypher-generated `.bin` files, formats them as `constexpr`
byte arrays, and emits headers under
`lib/Epub/Epub/hyphenation/generated/`. Each header defines the raw data plus a
`SerializedHyphenationPatterns` descriptor so the reader can keep the automaton
in flash.

A convenient script `update_hyphenation.sh` is used to update all languages.
To use it, run:

```sh
./scripts/update_hyphenation.sh
```

## Hyphenation packs

Only English's trie is compiled into the firmware. Every other language's ships as a hyphenation pack on the SD
card, `hyph-<code>.cphyph` ([file-formats.md](file-formats.md#hyphenation-packs-cphy-and-the-store-whhs)), which the
Font Manager downloads from `assets/sd-fonts/Hyphenation-<Language>/`. `scripts/build_hyphenation_packs.py` writes
them: the nine languages that used to be compiled in from their generated headers here (the host tests compile them
in again with `HYPHENATION_ALL_BUILTIN` and check the packs hold the same bytes), most others straight from hypher's
`tries/<code>.bin`:

```sh
python3 scripts/build_hyphenation_packs.py --hypher-tries <hypher checkout>/tries
```

A language whose patterns hypher does not carry is built from a TeX pattern file in `assets/hyphenation-patterns/`
by `scripts/hyphenation_trie.py`, a port of hypher's trie builder that rebuilds all 48 of hypher's tries byte for
byte (`python3 scripts/hyphenation_trie.py --check <hypher checkout>`). Hungarian is one: hyph-utf8's patterns make a
354 KB trie, so its pack is built from the older, smaller Huhyphn v20031107 (78 KB, GPL-2;
[its README](../assets/hyphenation-patterns/hu/README.md)).

A new language needs its entry in `LanguageRegistry.cpp` (letters, case, minima, whether its breaks show a hyphen),
its code in `build_hyphenation_packs.py` and in `sd-fonts.yaml`, and its expected breaks for
`test/hyphenation_store`: `test/hyphenation_store/resources/generate_pack_reference.py` runs Liang's algorithm over
the language's patterns themselves. The language table holds 32 languages now, as many as `packMissing` has bits:
the next one widens it.

