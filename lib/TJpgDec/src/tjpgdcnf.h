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

#define JD_FASTDECODE 2
/* Optimization level. 2 = + table-driven huffman decode (32-bit barrel shifter
/  plus a 1<<HUFF_BIT LUT per huffman table). The LUTs are carved from the caller's
/  work pool (~6 KB extra for a colour JPEG), so TJPG_WORK_POOL_SIZE is sized for it.
/  Worth it here: baseline decode is huffman-bound on the single-core C3 and the RAM
/  is available. (lovyan03's dual-core decomp_multitask is N/A — C3 is single-core.) */
