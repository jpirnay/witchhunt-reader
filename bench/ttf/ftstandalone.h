#pragma once
/* THROWAWAY (TTF step 0): what ftgrays.c defines for its own standalone build, for the files
 * that include ftimage.h / ftgrays.h directly. */
#ifndef STANDALONE_
#define STANDALONE_
#endif
#ifndef ft_grays_raster
#define ft_grays_raster ittf_grays_raster
#endif
#ifndef FT_BEGIN_HEADER
#define FT_BEGIN_HEADER
#define FT_END_HEADER
#endif
#ifndef FT_STATIC_BYTE_CAST
#define FT_STATIC_BYTE_CAST(type, var) (type)(unsigned char)(var)
#endif
