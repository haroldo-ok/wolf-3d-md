/* vt_doors.c - test hooks (see README "Measuring and checking").
 * -DVT_SEQ -DVT_DOORS:
 * replaces the views of the VT sequence with ones that look at doors in every state of
 * opening and at a sliding push wall, which the normal sequence hardly shows. */
#include "wl_def.h"

#ifdef VT_WEAPON
/* -DVT_SEQ -DVT_WEAPON: show this weapon frame (0..19) all the time.  The test patches this
   byte in the ROM image to go through all of them with one build. */
__attribute__((used)) const volatile u8 vt_weapon_sel = 0;
#endif

#ifdef VT_DOORS

static u16 vt_pw = 0xFFFF;      /* tile made a push wall by the previous frame */
static u8 vt_pwold;

void vt_doors(u32 n)
{
    static const s8 dir[4][3] = {{1, 0, 2}, {-1, 0, 0}, {0, 1, 1}, {0, -1, 3}};    /* dx, dy, quarter turns facing back */
    u8 *tm = &tilemap[0][0];
    s16 nd = lastdoorobj - doorobjlist, i, tries;

    if (vt_pw != 0xFFFF)
    {
        tm[vt_pw] = vt_pwold;
        vt_pw = 0xFFFF;
        pwallstate = 0;
    }
    if (n > 240) n = 240;
    for (i = 0; i < nd; i++)
        doorposition[i] = ((n + i) & 3) ? (u16)(n * 2749 + i * 9973) : (((n + i) & 4) ? 0xFFFF : 0);
    if (n < 80)
    {
        /* the start room: turn the wall the player faces into a push wall on the move */
        s16 a = player->angle, dx = 0, dy = 0, x = player->tilex, y = player->tiley;
        if (a < 45 || a >= 315) dx = 1;
        else if (a < 135) dy = -1;
        else if (a < 225) dx = -1;
        else dy = 1;
        for (i = 0; i < 12; i++)
        {
            x += dx; y += dy;
            if (x < 1 || y < 1 || x > MAPSIZE - 2 || y > MAPSIZE - 2) return;
            if (tm[(x << 6) + y]) break;
        }
        if (i < 12 && tm[(x << 6) + y] < 0x40 && (n & 1))
        {
            vt_pw = (x << 6) + y;
            vt_pwold = tm[vt_pw];
            tm[vt_pw] |= 0xc0;
            pwallpos = (n * 5) & 63;
            pwallstate = 1;         /* the renderer skips frames in which nothing moved */
        }
        return;
    }
    if (nd <= 0) return;
    for (tries = 0; tries < 64; tries++)
    {
        const doorobj_t *d = &doorobjlist[(n * 7 + tries) % nd];
        const s8 *o = dir[(n + tries) & 3];
        s16 dist = 1 + ((n >> 2) & 1);
        s16 tx = d->tilex + o[0] * dist, ty = d->tiley + o[1] * dist;
        if (tx < 1 || ty < 1 || tx > MAPSIZE - 2 || ty > MAPSIZE - 2) continue;
        if (tm[(tx << 6) + ty] || tm[((d->tilex + o[0]) << 6) + d->tiley + o[1]]) continue;
        player->tilex = tx;
        player->tiley = ty;
        player->x = ((s32)tx << TILESHIFT) + 0x8000 + (s32)((n * 37) % 41 - 20) * 0x600;
        player->y = ((s32)ty << TILESHIFT) + 0x8000 + (s32)((n * 53) % 41 - 20) * 0x600;
        player->angle = (s16)((o[2] * (ANGLES / 4) + (n % 9) * 10 + ANGLES - 40) % ANGLES);
        return;
    }
}
#endif
