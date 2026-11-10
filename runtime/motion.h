/* Motion-compensated picture interpolation (HD edition sprite interpolation). */
#pragma once
#include <stdint.h>

/* p: picture shown now, n: the next picture (768x240 each) */
void motion_prepare(const uint32_t *p, const uint32_t *n);
/* the picture at fraction alpha (0..1) of the way from p to n */
void motion_render(uint32_t *out, const uint32_t *p, const uint32_t *n, float alpha);
