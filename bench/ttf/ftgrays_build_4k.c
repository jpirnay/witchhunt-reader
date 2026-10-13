/* THROWAWAY (TTF step 0): ftgrays.c standalone with a 4 KB cell pool, for the pool-size measurement. */
#define STANDALONE_
#define FALL_THROUGH ((void)0)
#define ft_grays_raster ittf_grays_raster_4k
#define ITTF_POOL_SIZE  4096L
#include "ftgrays.c"
