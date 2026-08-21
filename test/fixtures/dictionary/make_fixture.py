#!/usr/bin/env python3
"""Generate the StarDict fixture dictionary used by test/dictionary.

Writes /dictionaries-style files for two dictionaries into this directory:

  plain/plain.{ifo,idx,dict,syn}   -- uncompressed .dict
  zipped/zipped.{ifo,idx,dict.dz}  -- real dictzip .dict.dz, same content

Both carry the same headwords and definitions, so a test can assert that the
compressed path returns byte-identical text to the plain one.

The fixture deliberately exceeds one SAMPLE_INTERVAL (256) of index entries so
the .qidx sidecar holds several samples and the binary descent is actually
exercised, and it uses a small dictzip chunk length so definitions straddle
chunk boundaries.

Regenerate with:  python3 test/fixtures/dictionary/make_fixture.py
"""

import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

# Small enough that the fixture stays a few hundred KB while still producing
# many chunks; real dictzip uses 58315.
CHUNK_LENGTH = 4096

ENTRY_COUNT = 3000


def fold(word):
    """The ordering StarDict indexes use (glib g_ascii_strcasecmp)."""
    return bytes(c + 32 if 65 <= c <= 90 else c for c in word.encode("utf-8"))


def make_entries():
    """Headword -> definition. Mixed case on purpose: a case-sensitive sort
    would put 'Zebra' before 'apple', and the descent would then miss."""
    entries = {}
    for i in range(ENTRY_COUNT):
        # Four letters, so words interleave across the alphabet rather than
        # clustering, and every 7th is capitalised.
        word = "{}{}{}{}".format(
            chr(ord("a") + i % 26),
            chr(ord("a") + (i // 26) % 26),
            chr(ord("a") + (i // 7) % 26),
            chr(ord("a") + (i // 3) % 26),
        )
        if i % 7 == 0:
            word = word.capitalize()
        # Repeated text so each definition compresses via back-references, which
        # is what makes the inflate ring buffer matter.
        entries[word] = "definition of {}. ".format(word) * (1 + i % 3)

    # One definition long enough to span several dictzip chunks.
    entries["longword"] = "a very long definition. " * 900
    # Distinctive short one for the exact-match assertions.
    entries["quixotic"] = "exceedingly idealistic; unrealistic and impractical"
    # Case pair that only sorts correctly under case-insensitive comparison.
    entries["Quorum"] = "the minimum number of members required to be present"
    # Hyphenated-compound fallback. "well-being" is a headword in its own right,
    # so the compound must win outright; "billionaire" and "mother" exist only as
    # parts, so a compound built from them has to fall back to the longest part.
    entries["well-being"] = "the state of being comfortable, healthy, or happy"
    entries["billionaire"] = "a person with assets worth a billion or more"
    entries["mother"] = "a woman in relation to her child or children"
    return entries


def write_dictionary(folder, stem, entries, synonyms, compressed, sametypesequence="m"):
    root = os.path.join(HERE, folder)
    os.makedirs(root, exist_ok=True)
    base = os.path.join(root, stem)

    ordered = sorted(entries.items(), key=lambda kv: fold(kv[0]))
    keys = [fold(word) for word, _ in ordered]
    assert len(set(keys)) == len(keys), "two headwords fold to the same key: the scan's first hit would be arbitrary"

    dict_blob = bytearray()
    idx = bytearray()
    ordinals = {}
    for ordinal, (word, definition) in enumerate(ordered):
        payload = definition.encode("utf-8")
        idx += word.encode("utf-8") + b"\0" + struct.pack(">II", len(dict_blob), len(payload))
        dict_blob += payload
        ordinals[word] = ordinal

    with open(base + ".idx", "wb") as f:
        f.write(idx)

    if compressed:
        with open(base + ".dict.dz", "wb") as f:
            f.write(dictzip(bytes(dict_blob), os.path.basename(base) + ".dict"))
    else:
        with open(base + ".dict", "wb") as f:
            f.write(dict_blob)

    if synonyms:
        # .syn: synonym\0 + BE32 ordinal of the headword it points at, sorted
        # the same way the index is.
        syn = bytearray()
        for alias, target in sorted(synonyms.items(), key=lambda kv: fold(kv[0])):
            syn += alias.encode("utf-8") + b"\0" + struct.pack(">I", ordinals[target])
        with open(base + ".syn", "wb") as f:
            f.write(syn)

    with open(base + ".ifo", "w", encoding="utf-8") as f:
        f.write("StarDict's dict ifo file\n")
        f.write("version=2.4.2\n")
        f.write("bookname={}\n".format(stem))
        f.write("wordcount={}\n".format(len(ordered)))
        f.write("idxfilesize={}\n".format(len(idx)))
        if synonyms:
            f.write("synwordcount={}\n".format(len(synonyms)))
        f.write("sametypesequence={}\n".format(sametypesequence))
    return ordered


def dictzip(data, name):
    """gzip container with the dictzip 'RA' random-access extra field.

    Each chunk is terminated with Z_FULL_FLUSH, which resets the compressor's
    history and byte-aligns the output -- that is what lets a decompressor start
    a fresh inflate at any chunk boundary. This is how real dictzip files are
    built, so the reader is exercised against the same shape it meets on an SD
    card rather than a simplified one.
    """
    chunks = [data[i : i + CHUNK_LENGTH] for i in range(0, len(data), CHUNK_LENGTH)]
    assert len(chunks) <= 0xFFFF, "too many chunks for the 16-bit count"

    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    compressed_chunks = []
    for i, chunk in enumerate(chunks):
        last = i == len(chunks) - 1
        part = co.compress(chunk)
        part += co.flush(zlib.Z_FINISH if last else zlib.Z_FULL_FLUSH)
        assert len(part) <= 0xFFFF, "compressed chunk exceeds the 16-bit length field"
        compressed_chunks.append(part)

    extra = b"RA" + struct.pack("<H", 6 + 2 * len(chunks))
    extra += struct.pack("<HHH", 1, CHUNK_LENGTH, len(chunks))
    extra += b"".join(struct.pack("<H", len(c)) for c in compressed_chunks)

    out = bytearray()
    out += b"\x1f\x8b\x08"
    out += struct.pack("<B", 0x04 | 0x08)  # FEXTRA | FNAME
    out += struct.pack("<IBB", 0, 2, 3)  # mtime, XFL, OS=Unix
    out += struct.pack("<H", len(extra))
    out += extra
    out += name.encode("utf-8") + b"\0"
    out += b"".join(compressed_chunks)
    out += struct.pack("<II", zlib.crc32(data) & 0xFFFFFFFF, len(data) & 0xFFFFFFFF)
    return bytes(out)


def main():
    entries = make_entries()
    synonyms = {
        # Alternate spelling and an irregular form, the two things a .syn is for.
        "quixotical": "quixotic",
        "quorums": "Quorum",
    }
    ordered = write_dictionary("plain", "plain", entries, synonyms, compressed=False)
    write_dictionary("zipped", "zipped", entries, None, compressed=True)

    # A machine-readable manifest, so the C++ test asserts against the generator
    # rather than against numbers copied by hand into two places.
    with open(os.path.join(HERE, "manifest.txt"), "w", encoding="utf-8") as f:
        f.write("entry_count={}\n".format(len(ordered)))
        f.write("first_word={}\n".format(ordered[0][0]))
        f.write("last_word={}\n".format(ordered[-1][0]))
        f.write("chunk_length={}\n".format(CHUNK_LENGTH))
    print("wrote {} entries ({} synonyms)".format(len(ordered), len(synonyms)))


if __name__ == "__main__":
    main()
