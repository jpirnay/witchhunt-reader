/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/*----------------------------------------------*/
/*
 * CrossPoint configuration of ChaN's TJpgDec (vendored, see library.json).
 * Used as the baseline-JPEG decode engine (progressive stays on JPEGDEC).
 * Only this config file is modified from upstream; tjpgd.c / tjpgd.h are verbatim.
 */

#define JD_SZBUF 512
/* Specifies size of stream input buffer */

#define JD_FORMAT 2
/* Output pixel format. 2 = Grayscale (8-bit/pix) — matches the EIGHT_BIT_GRAYSCALE
/  pipeline (DirectPixelWriter/DirectCacheWriter/ditherers consume 8-bit gray). */

#define JD_USE_SCALE 1
/* Enable 1/1·1/2·1/4·1/8 output descaling (mapped from chooseJpegScale()). */

#define JD_TBLCLIP 1
/* Use table conversion for saturation arithmetic. A bit faster, +1 KB code. */

#define JD_FASTDECODE 1
/* Optimization level. 1 = + 32-bit barrel shifter, suitable for the 32-bit
/  ESP32-C3. (2 adds huffman LUTs that want 6<<HUFF_BIT bytes of RAM — avoided
/  on this RAM-constrained target; revisit only if decode time demands it.) */
