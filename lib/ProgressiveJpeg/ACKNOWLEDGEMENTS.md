# Acknowledgements

The progressive decoding procedures (DC/AC first and refinement scans, EOB runs, restart
handling) follow ITU-T T.81 Annex G. The AC refinement routine mirrors the structure of
`decode_mcu_AC_refine` in the Independent JPEG Group's libjpeg (`jdphuff.c`), whose
correction-bit bookkeeping is the reference behaviour every encoder is tested against.

The reduced-size inverse DCT (keeping the top-left n x n coefficients and inverting with an
n-point IDCT) is the approach of libjpeg's scaled decoding.

The low-heap fallback -- the DC scans alone, resized to the output (`decodeResized`) -- continues
the progressive JPEG preview this firmware had as a separate DC-only decoder, whose idea was
inspired by the progressive JPEG fallbacks in
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) and
[FreeInkBook](https://github.com/freeink-project/freeink-sdk).

This is an independent implementation; it contains no libjpeg, libjpeg-turbo, JPEGDEC or
TJpgDec source code.
