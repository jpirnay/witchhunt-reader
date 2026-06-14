/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/*----------------------------------------------*/
/*
 * CrossPoint configuration of ChaN's TJpgDec (vendored, see library.json).
 * Used as the baseline-JPEG decode engine (progressive renders a placeholder).
 * Modified from upstream here (config + the JD_FASTPATH macro below) and in tjpgd.c
 * (the JD_FASTPATH annotations on the hot decode functions, plus a BYTECLIP clamp
 * on the grayscale output path that fixes an upstream wrap-around bug — see the
 * note in mcu_output); tjpgd.h is verbatim.
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

/*----------------------------------------------*/
/* Hot-path placement (CrossPoint, ESP32-C3)    */
/*----------------------------------------------*/
/* On the C3 the per-MCU decode loop executes from flash through the instruction
/  cache, so the tight inner functions pay i-cache miss stalls. With
/  JD_FASTPATH_IRAM=1 the compute-bound functions are linked into IRAM, trading a
/  few KB of IRAM for the removal of those stalls. huffext/bitext stay out-of-line
/  and land in IRAM directly; block_idct, mcu_load and mcu_output get inlined into
/  the jd_decomp driver by -O2, so jd_decomp itself carries the attribute to pull
/  those inlined bodies into IRAM too. Set from platformio.ini build flags so the
/  benchmark can A/B it; default 0 keeps host/native builds (no esp_attr.h) clean. */
#ifndef JD_FASTPATH_IRAM
#define JD_FASTPATH_IRAM 0
#endif

#if JD_FASTPATH_IRAM
#include "esp_attr.h"
#define JD_FASTPATH IRAM_ATTR
#else
#define JD_FASTPATH
#endif
