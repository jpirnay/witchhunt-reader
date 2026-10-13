/* THROWAWAY (TTF step 0): ftgrays.c standalone with a 8 KB cell pool, for the pool-size measurement. */
#define STANDALONE_
#define FALL_THROUGH ((void)0)
#define ft_grays_raster ittf_grays_raster_8k
#define ITTF_POOL_SIZE  8192L
#include "ftgrays.c"
