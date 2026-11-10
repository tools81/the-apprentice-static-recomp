/* Cartoon HD / Cartoon HD+ upscalers (HD edition). */
#pragma once
#include <stdint.h>

enum { CARTOON_OFF, CARTOON_HD, CARTOON_HDPLUS, CARTOON_MODES };

void cartoon_init(void);
/* src: 768x240 picture.  Returns the upscaled picture (1536x960, or src
   itself for CARTOON_OFF); valid until the next call. */
uint32_t *cartoon_upscale(uint32_t *src, int w, int h, int mode, int *ow, int *oh);

/* rows [0, rows) split over the worker threads */
typedef void (*RowFn)(void *ctx, int y0, int y1);
void parallel_rows(int rows, RowFn fn, void *ctx);
