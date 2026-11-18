#!/usr/bin/env python3
"""Encrypt an EPUB the way the protected books the reader opens are encrypted, with a known key.

For device and host tests of protected-content reading (docs/future_work/protected-content.md):

  adobe  AES-128-CBC, the plaintext raw DEFLATE, PKCS#7 padding, no sizes in encryption.xml
         (an Adobe ADEPT book after fulfilment).
  lcp    AES-256-CBC, xmlenc padding (random fill, the count last), Compression Method and
         OriginalLength in encryption.xml (a Readium LCP book).

Every entry is encrypted but the mimetype, META-INF/* and the package document (OPF), which OCF
keeps readable. Each encrypted entry is stored in the archive (zip method 0) as a 16-byte IV and
the ciphertext. AES runs through the openssl command line, so nothing beyond Python is needed.

  python3 -I tools/make_protected_epub.py in.epub out.epub --key 000102...0f [--scheme lcp]

The key goes to the device with POST /api/book-key {"path": ..., "key": <base64>}.
"""

import argparse
import os
import subprocess
import sys
import zipfile
import zlib
from xml.sax.saxutils import quoteattr
from urllib.parse import quote

ALGORITHMS = {
    "adobe": "http://www.w3.org/2001/04/xmlenc#aes128-cbc",
    "lcp": "http://www.w3.org/2001/04/xmlenc#aes256-cbc",
}


def aes_cbc(key, iv, data):
    """AES-CBC of `data` (whole blocks), key length picks AES-128 or AES-256."""
    cipher = "-aes-128-cbc" if len(key) == 16 else "-aes-256-cbc"
    result = subprocess.run(
        ["openssl", "enc", cipher, "-nopad", "-K", key.hex(), "-iv", iv.hex()],
        input=data,
        capture_output=True,
        check=True,
    )
    return result.stdout


def pad(data, xmlenc):
    count = 16 - len(data) % 16
    fill = os.urandom(count - 1) if xmlenc else bytes([count]) * (count - 1)
    return data + fill + bytes([count])


def raw_deflate(data):
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    return compressor.compress(data) + compressor.flush()


def stays_plain(name, opf_path):
    return name == "mimetype" or name.startswith("META-INF/") or name == opf_path


def find_opf(source):
    container = source.read("META-INF/container.xml").decode("utf-8")
    marker = 'full-path="'
    start = container.index(marker) + len(marker)
    return container[start : container.index('"', start)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source")
    parser.add_argument("target")
    parser.add_argument("--key", required=True, help="hex: 16 bytes (adobe) or 32 (lcp)")
    parser.add_argument("--scheme", choices=sorted(ALGORITHMS), default="adobe")
    args = parser.parse_args()

    key = bytes.fromhex(args.key)
    if len(key) != (16 if args.scheme == "adobe" else 32):
        sys.exit("adobe takes a 16-byte key, lcp a 32-byte one")
    lcp = args.scheme == "lcp"

    with zipfile.ZipFile(args.source) as source:
        opf_path = find_opf(source)
        names = [info.filename for info in source.infolist() if not info.is_dir()]
        if "META-INF/encryption.xml" in names:
            sys.exit("the source has an encryption.xml already")
        encrypted = []
        with zipfile.ZipFile(args.target, "w") as target:
            target.writestr(zipfile.ZipInfo("mimetype"), source.read("mimetype"), zipfile.ZIP_STORED)
            for name in names:
                if name == "mimetype":
                    continue
                data = source.read(name)
                if stays_plain(name, opf_path):
                    target.writestr(name, data, zipfile.ZIP_DEFLATED)
                    continue
                iv = os.urandom(16)
                blob = iv + aes_cbc(key, iv, pad(raw_deflate(data), lcp))
                target.writestr(zipfile.ZipInfo(name), blob, zipfile.ZIP_STORED)
                encrypted.append((name, len(data)))

            entries = []
            for name, size in encrypted:
                compression = ""
                if lcp:
                    compression = (
                        "<EncryptionProperties><EncryptionProperty>"
                        '<Compression xmlns="http://www.idpf.org/2016/encryption#compression" '
                        f'Method="8" OriginalLength="{size}"/></EncryptionProperty></EncryptionProperties>'
                    )
                entries.append(
                    '<EncryptedData xmlns="http://www.w3.org/2001/04/xmlenc#">'
                    f'<EncryptionMethod Algorithm="{ALGORITHMS[args.scheme]}"/>'
                    '<KeyInfo xmlns="http://www.w3.org/2000/09/xmldsig#"/>'
                    f"<CipherData><CipherReference URI={quoteattr(quote(name))}/></CipherData>"
                    f"{compression}</EncryptedData>"
                )
            xml = (
                '<?xml version="1.0" encoding="UTF-8"?>'
                '<encryption xmlns="urn:oasis:names:tc:opendocument:xmlns:container">' + "".join(entries) + "</encryption>"
            )
            target.writestr("META-INF/encryption.xml", xml, zipfile.ZIP_DEFLATED)
    print(f"{args.target}: {len(encrypted)} entries encrypted ({args.scheme})")


if __name__ == "__main__":
    main()
