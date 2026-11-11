#ifndef WEAPON_H
#define WEAPON_H
#include "wl_types.h"
#define WP_FRAMES 20
#define WP_FIRST_SPR 416
#define WP_ZERO_IDX 2
#define WP_MAXTILES 152
#define WP_TILE_BASE 1226
/* tiles: VDP tile data; sat[bank]: sprite table entries using tile bank 0 / 1 */
typedef struct { const u8 *tiles; u16 tile_bytes; const u16 *sat[2]; u16 sat_bytes; } wpframe_t;
extern const wpframe_t wp_frames[WP_FRAMES];
#endif
