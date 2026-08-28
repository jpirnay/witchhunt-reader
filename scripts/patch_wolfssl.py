"""
PlatformIO pre-build hook: inject WitchReader's trimmed wolfSSL configuration.

The wolfSSL Arduino library ships a user_settings.h tuned for a broad default.
For this firmware we want a lean CLIENT-ONLY TLS 1.3/1.2 profile to save flash
(the app partition is ~95% full) and stack. Rather than edit the vendored file
in place (it lives under .pio/libdeps and is re-fetched on clean builds), we
append our overrides under a unique marker, idempotently.

Runs as a PlatformIO pre: hook and standalone (`python scripts/patch_wolfssl.py`).
No-op when the wolfSSL libdep is absent (flag-off / non-network envs), so it is
safe to register globally.
"""

import glob
import os

try:
    Import("env")  # noqa: F821 -- provided by PlatformIO when run as a build hook
    PROJECT_DIR = env["PROJECT_DIR"]  # noqa: F821
except (NameError, Exception):  # not under PlatformIO -> standalone invocation
    PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MARKER = "/* ---- CrossPoint wolfSSL client-only overrides ---- */"
END_MARKER = "/* ---- end CrossPoint wolfSSL overrides ---- */"

# Lean client-only profile. Enables TLS 1.3 + 1.2, ECDHE (X25519/P-256), the AEAD
# suites our hosts use (AES-GCM + ChaCha20-Poly1305), RSA-PSS verify, SNI, HKDF.
# Disables the server side, static DH/FFDHE, legacy ciphers, filesystem, OCSP/CRL.
OVERRIDES = """

{marker}
#ifndef CROSSPOINT_WOLFSSL_TUNED
#define CROSSPOINT_WOLFSSL_TUNED

/* Optimize buffer sizing for a small embedded client (wolfSSL asks for this). */
#ifndef WOLFSSL_CLIENT_EXAMPLE
#define WOLFSSL_CLIENT_EXAMPLE
#endif

/* TLS versions: 1.3 preferred, 1.2 fallback; drop everything older. */
#undef  WOLFSSL_TLS13
#define WOLFSSL_TLS13
#undef  NO_OLD_TLS
#define NO_OLD_TLS            /* removes TLS 1.0/1.1 + SSLv3 */

/* Client only. */
#ifndef NO_WOLFSSL_SERVER
#define NO_WOLFSSL_SERVER
#endif

/* Key exchange: ECDHE only. Drop static/ephemeral finite-field DH entirely. */
#ifndef NO_DH
#define NO_DH
#endif
#undef  HAVE_FFDHE_2048
#undef  HAVE_FFDHE_3072
#undef  HAVE_FFDHE_4096

/* Curves + KDF needed for TLS 1.3 ECDHE. */
#ifndef HAVE_ECC
#define HAVE_ECC
#endif
#ifndef HAVE_CURVE25519
#define HAVE_CURVE25519
#endif
#ifndef HAVE_SUPPORTED_CURVES
#define HAVE_SUPPORTED_CURVES
#endif
#ifndef HAVE_TLS_EXTENSIONS
#define HAVE_TLS_EXTENSIONS
#endif
#ifndef HAVE_HKDF
#define HAVE_HKDF
#endif
#ifndef HAVE_SNI
#define HAVE_SNI
#endif

/* Hashes: SHA-256 always; SHA-384/512 are REQUIRED by TLS 1.3 (AES-256-GCM-SHA384
 * suite + P-384 key schedule) even when the leaf cert is SHA-256. Omitting them
 * makes the 1.3 handshake fail with HASH_TYPE_E (-232). */
#ifndef WOLFSSL_SHA384
#define WOLFSSL_SHA384
#endif
#ifndef WOLFSSL_SHA512
#define WOLFSSL_SHA512
#endif

/* AEAD ciphers our hosts negotiate. */
#ifndef HAVE_AESGCM
#define HAVE_AESGCM
#endif
#ifndef HAVE_CHACHA
#define HAVE_CHACHA
#endif
#ifndef HAVE_POLY1305
#define HAVE_POLY1305
#endif

/* RSA verify for RSA-chained certs (USERTrust RSA); PSS for TLS 1.3. */
#ifndef WC_RSA_PSS
#define WC_RSA_PSS
#endif

/* Math backend: use SP (single-precision) math instead of TFM fast-math so the
 * SP ECC fast path (P-256/P-384) + WOLFSSL_SP_RISCV32 assembly kick in. Our
 * hosts are all ECDHE/ECDSA, so accelerating ECC is the biggest handshake win.
 * SP-ECC auto-enables only when USE_FAST_MATH is OFF, so undef it here. */
#undef  USE_FAST_MATH
#define WOLFSSL_SP_MATH_ALL      /* SP math for all key sizes (RSA verify too) */
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_SP_384           /* P-384 (Sectigo/GitHub ECDSA chains use it) */
/* P-256 is on by default with SP-ECC; keep RSA via SP too for USERTrust RSA. */
#define WOLFSSL_HAVE_SP_RSA

/* Bignum ceiling: sized for RSA-4096 chains (no FFDHE), half of upstream's 16384. */
#undef  FP_MAX_BITS
#define FP_MAX_BITS 8192
/* SP integer max bits (SP-math analogue of FP_MAX_BITS): cover RSA-4096. */
#ifndef SP_INT_BITS
#define SP_INT_BITS 4096
#endif

/* Trim unused primitives / features to reclaim flash. */
#ifndef NO_DSA
#define NO_DSA
#endif
#ifndef NO_RC4
#define NO_RC4
#endif
#ifndef NO_MD4
#define NO_MD4
#endif
#ifndef NO_DES3
#define NO_DES3
#endif
#ifndef NO_PSK
#define NO_PSK
#endif
#ifndef NO_PWDBASED
#define NO_PWDBASED
#endif
#ifndef NO_WOLFSSL_MEMORY
/* keep wolfSSL memory hooks so heap stats work */
#endif

/* No filesystem on device: certs come from a buffer, not files. */
#ifndef NO_FILESYSTEM
#define NO_FILESYSTEM
#endif

/* Small session cache: we open few concurrent sessions. */
#ifndef SMALL_SESSION_CACHE
#define SMALL_SESSION_CACHE
#endif

/* Instrumentation only, off unless -DCROSSPOINT_TLS_VERIFY_TIMING is in build_flags.
 * wolfSSL issues the verify callback ONLY on a verification error by default, so a
 * successful handshake reports nothing and per-certificate cost cannot be measured. These two
 * make it fire for every certificate in the chain (intermediates first, leaf last), which is
 * what SecureClient's callback times. Semantics are unchanged for us: the callback returns
 * `preverify` untouched on success, which is exactly what wolfSSL does with no callback. */
#ifdef CROSSPOINT_TLS_VERIFY_TIMING
#ifndef WOLFSSL_ALWAYS_VERIFY_CB
#define WOLFSSL_ALWAYS_VERIFY_CB
#endif
#ifndef WOLFSSL_VERIFY_CB_ALL_CERTS
#define WOLFSSL_VERIFY_CB_ALL_CERTS
#endif
#endif

/* Some cross-signed chains (GTS R4 via GlobalSign, USERTrust) verify more
 * reliably when alternate chain building is allowed. */
#ifndef WOLFSSL_ALT_CERT_CHAINS
#define WOLFSSL_ALT_CERT_CHAINS
#endif

#endif /* CROSSPOINT_WOLFSSL_TUNED */
{end_marker}
"""


def patch(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    # Strip any prior injection (idempotent). Our block is delimited by MARKER at
    # the start and END_MARKER at the end so we can remove it wherever it sits.
    if MARKER in text and END_MARKER in text:
        head = text.split(MARKER, 1)[0]
        tail = text.split(END_MARKER, 1)[1]
        text = head + tail
    # Prepend so our defines (esp. WOLFSSL_CLIENT_EXAMPLE) are seen BEFORE the
    # library's own #if checks near the top of user_settings.h, avoiding a
    # spurious "define WOLFSSL_CLIENT_EXAMPLE" #warning.
    block = OVERRIDES.format(marker=MARKER, end_marker=END_MARKER).lstrip() + "\n"
    text = block + text.lstrip()
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print("Patched wolfSSL user_settings: {}".format(os.path.relpath(path, PROJECT_DIR)))


def main():
    patterns = [
        os.path.join(PROJECT_DIR, ".pio", "libdeps", "*", "Arduino-wolfSSL", "src", "user_settings.h"),
        os.path.join(PROJECT_DIR, ".pio", "libdeps", "*", "wolfssl", "src", "user_settings.h"),
    ]
    found = False
    for pat in patterns:
        for path in glob.glob(pat):
            patch(path)
            found = True
    if not found:
        # Normal for flag-off / non-network envs; not an error.
        print("patch_wolfssl: no wolfSSL user_settings.h found (skipping)")


main()
