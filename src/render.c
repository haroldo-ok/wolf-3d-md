/* render.c - the 3-D view: ray casting, wall and sprite scaling.
 *
 * The ray casting loop is a C transcription of WL_DR_A.ASM / WL_DRAW.C.
 * Rays are two pixels wide (VIEWW/2 rays); a first pass casts every ray and
 * records its wall column and height, the second pass renders the picture
 * one 8-pixel tile column at a time (floor/ceiling fill, walls, sprites)
 * and hands each finished column to the DMA queue, so the VRAM
 * upload overlaps the CPU work and no second RAM frame buffer is needed.
 */
#include "wl_def.h"
#include "vid.h"
#include "r_asm.h"
#include "gen/rtables.h"
#include "gen/weapon.h"
#include <string.h>
#define offsetof(t, m) __builtin_offsetof(t, m)

#define NRAYS       (VIEWW / 2)
#define MAXVIS      64
void GetBonus(statobj_t *check);

s32 viewx, viewy;
s16 viewangle;
s32 viewsin, viewcos;
s16 centerx = VIEWW / 2;
s16 weapon_spr = -1;
/* Detail: Low in the menu.  Only the even rays are cast and drawn, each 4 pixels wide (the
   picture is the high detail one with every odd ray replaced by the ray to its left).  The
   weapon and the status bar are not affected. */
#ifdef VT_LOWDETAIL
u8 r_lowdetail = 1;
#else
u8 r_lowdetail;
#endif
s16 weapon_dy;
u16 game_pal0[16];
const s16 dirangle[9] = {0, ANGLES / 8, 2 * ANGLES / 8, 3 * ANGLES / 8, 4 * ANGLES / 8,
                         5 * ANGLES / 8, 6 * ANGLES / 8, 7 * ANGLES / 8, 0};

/* per ray results of the caster; defined in r_asm.s so one pointer reaches all three */
extern const u8 *rc_tex[NRAYS];
extern u16 rc_face[NRAYS];      /* which plain wall face a ray hit (0xFFFF = door/push wall) */
static s16 horizwall[MAXWALLTILES], vertwall[MAXWALLTILES];
s16 focaltx, focalty;
/* wall texture of every tile value per orientation, for r_asm.s; entry 64 is the door frame */
const u8 *rc_wallptr_h[65], *rc_wallptr_v[65];

/* --------------------------------------------------------------- math */

static inline u32 mulu16(u16 a, u16 b)
{
    u32 r = a;
    __asm__("mulu.w %1,%0" : "+d"(r) : "dm"(b) : "cc");
    return r;
}

s32 FixedByFrac(s32 a, s32 b)
{
    boolean neg = FALSE;
    u32 ua, r;
    u16 ub;
    if (a < 0) { a = -a; neg = TRUE; }
    if (b < 0) { b = -b; neg = !neg; }
    ua = (u32)a;
    ub = (u16)b;
    r = mulu16(ua >> 16, ub) + (mulu16(ua, ub) >> 16);
    return neg ? -(s32)r : (s32)r;
}

/* a * f / 65536 for a fraction f < 1 (as a 16 bit value) */
static inline s32 mulfrac(s32 a, u16 f)
{
    u32 ua;
    u32 r;
    if (a < 0)
    {
        ua = (u32)-a;
        r = mulu16(ua >> 16, f) + (mulu16(ua, f) >> 16);
        return -(s32)r;
    }
    ua = (u32)a;
    r = mulu16(ua >> 16, f) + (mulu16(ua, f) >> 16);
    return (s32)r;
}

/* a (16 bit) * b (32 bit), result 32 bit.  a (a small row/column count here) is the
   source operand: mulu.w takes 38 + 2 cycles per set bit of its source. */
static inline u32 mul16x32(u16 a, u32 b)
{
    u32 lo = mulu16((u16)b, a);
    u16 hi = b >> 16;
    if (hi)
        lo += mulu16(hi, a) << 16;
    return lo;
}

/* (t * f) >> 16 for unsigned t */
static inline u32 mulhi(u32 t, u16 f)
{
    return mulu16(t >> 16, f) + (mulu16(t, f) >> 16);
}

static inline u16 divu32_16(u32 n, u16 d)
{
    u32 r = n;
    u32 dd = d;
    __asm__ volatile("divu.w %1,%0" : "+d"(r) : "d"(dd) : "cc");
    return (u16)r;
}

s16 vc15, vs15;          /* viewcos / viewsin as signed 1.15 */

static inline s32 muls16(s16 a, s16 b)
{
    s32 r = a;
    __asm__("muls.w %1,%0" : "+d"(r) : "dm"(b) : "cc");
    return r;
}

/* perpendicular distance -> projected wall height.  The deltas are taken in
   1/512 tile units so that two signed 16x16 multiplies give nx as 8.8 in the
   high word of the sum. */
static inline s16 calc_height(s32 xint, s32 yint)
{
    s16 dx = (s16)((xint - viewx) >> 7);
    s16 dy = (s16)((yint - viewy) >> 7);
    s32 sum = muls16(dx, vc15) - muls16(dy, vs15);
    s16 nx = (s16)(sum >> 16);
    if (nx < (MINDIST >> 8)) nx = (MINDIST >> 8);
    return divu32_16((u32)proj_heightnumerator, (u16)nx);
}

/* ------------------------------------------------------- transformations */

void TransformActor(objtype *ob)
{
    s32 gx, gy, gxt, gyt, nx, ny;

    gx = ob->x - viewx;
    gy = ob->y - viewy;
    gxt = FixedByFrac(gx, viewcos);
    gyt = FixedByFrac(gy, viewsin);
    nx = gxt - gyt - 0x2000;            /* ACTORSIZE */
    gxt = FixedByFrac(gx, viewsin);
    gyt = FixedByFrac(gy, viewcos);
    ny = gyt + gxt;
    ob->transx = nx;
    if (nx < MINDIST)
    {
        ob->viewheight = 0;
        return;
    }
    ob->viewx = centerx + (s16)((ny * proj_scale) / nx);
    ob->viewheight = divu32_16((u32)proj_heightnumerator, (u16)(nx >> 8));
}

boolean TransformTile(s16 tx, s16 ty, s16 *dispx, s16 *dispheight)
{
    s32 gx, gy, gxt, gyt, nx, ny;

    gx = ((s32)tx << TILESHIFT) + 0x8000 - viewx;
    gy = ((s32)ty << TILESHIFT) + 0x8000 - viewy;
    gxt = FixedByFrac(gx, viewcos);
    gyt = FixedByFrac(gy, viewsin);
    nx = gxt - gyt - 0x2000;
    gxt = FixedByFrac(gx, viewsin);
    gyt = FixedByFrac(gy, viewcos);
    ny = gyt + gxt;
    if (nx < MINDIST)
    {
        *dispheight = 0;
        return FALSE;
    }
    *dispx = centerx + (s16)((ny * proj_scale) / nx);
    *dispheight = divu32_16((u32)proj_heightnumerator, (u16)(nx >> 8));
    if (nx < TILEGLOBAL && ny > -TILEGLOBAL / 2 && ny < TILEGLOBAL / 2)
        return TRUE;
    return FALSE;
}

s16 CalcRotate(objtype *ob)
{
    s16 angle, va;

    va = player->angle + (centerx - ob->viewx) / 8;
    if (ob->obclass == rocketobj || ob->obclass == hrocketobj)
        angle = (va - 180) - ob->angle;
    else
        angle = (va - 180) - dirangle[ob->dir];
    angle += ANGLES / 16;
    while (angle >= ANGLES) angle -= ANGLES;
    while (angle < 0) angle += ANGLES;
    if (ob->state->rotate == 2)
        return 4 * (angle / (ANGLES / 2));
    return angle / (ANGLES / 8);
}

/* ------------------------------------------------------------- casting */

static inline const u8 *wallpage(s16 page, s16 col)
{
    u32 o = wall_ofs[page];
    if (o == 0xFFFFFFFF) o = 0;
    return wall_data + o + (col << 6);
}

static inline const u8 *doorpage(s16 doornum, boolean vert)
{
    s16 p;
    switch (doorobjlist[doornum].lock)
    {
    case dr_normal: p = DOORWALL; break;
    case dr_elevator: p = DOORWALL + 4; break;
    default: p = DOORWALL + 6; break;
    }
    return wall_data + wall_ofs[p + (vert ? 1 : 0)];
}

s16 c_midangle;
u16 c_xpd, c_xpu, c_ypd, c_ypu;

/* spotvis marks the tiles the rays crossed this frame.  Instead of clearing the whole
   64x64 map every frame, a tile counts as seen when it holds this frame's stamp
   (1..255); the map is only cleared when the stamp wraps or the video mode changes
   (spotvis lives inside screenbuf, which the menus and intermission screens reuse). */
u8 vis_stamp = 1;
static u16 vis_serial = 0xFFFF;
#define TILE_SEEN(p)    (*(p) == vis_stamp)

typedef struct
{
    s32 xint, yint, xstep, ystep;
    s16 xtile, ytile;
    u16 xspot, yspot;
    u8 vert;
} dda_t;

/* The grid walk of the ray caster: advance tile by tile until a nonzero tile is
   entered.  entry 0 = start, 1 = continue after a rejected vertical hit,
   2 = after a rejected horizontal hit.  Specialised per quadrant so the
   direction tests are constants; returns the tile value hit. */
static inline __attribute__((always_inline)) u8 dda_body(dda_t *d, u8 entry, s16 xs, s16 ys)
{
    register s32 xint = d->xint, yint = d->yint, xstep = d->xstep, ystep = d->ystep;
    register s16 xtile = d->xtile, ytile = d->ytile;
    register u16 xspot = d->xspot, yspot = d->yspot;
    u8 *tm = &tilemap[0][0];
    u8 *sv = &spotvis[0][0];
    u8 stamp = vis_stamp;
    u8 hit;

    if (entry == 1) goto passvert;
    if (entry == 2) goto passhoriz;
vertcheck:
    if (ys < 0 ? ((s16)(yint >> 16) <= ytile) : ((s16)(yint >> 16) >= ytile))
        goto horizentry;
vertentry:
    hit = tm[xspot];
    if (hit) { d->vert = 1; goto done; }
passvert:
    sv[xspot] = stamp;
    xtile += xs;
    yint += ystep;
    xspot = (xtile << 6) + (s16)(yint >> 16);
    goto vertcheck;
horizcheck:
    if (xs < 0 ? ((s16)(xint >> 16) <= xtile) : ((s16)(xint >> 16) >= xtile))
        goto vertentry;
horizentry:
    hit = tm[yspot];
    if (hit) { d->vert = 0; goto done; }
passhoriz:
    sv[yspot] = stamp;
    ytile += ys;
    xint += xstep;
    yspot = ((s16)(xint >> 16) << 6) + ytile;
    goto horizcheck;
done:
    d->xint = xint; d->yint = yint; d->xtile = xtile; d->ytile = ytile;
    d->xspot = xspot; d->yspot = yspot;
    return hit;
}

static u8 dda_pp(dda_t *d, u8 e) { return dda_body(d, e, 1, 1); }
static u8 dda_pm(dda_t *d, u8 e) { return dda_body(d, e, 1, -1); }
static u8 dda_mp(dda_t *d, u8 e) { return dda_body(d, e, -1, 1); }
static u8 dda_mm(dda_t *d, u8 e) { return dda_body(d, e, -1, -1); }

static inline u8 dda_run(dda_t *d, u8 entry, s16 xs, s16 ys)
{
    if (xs > 0) return ys > 0 ? dda_pp(d, entry) : dda_pm(d, entry);
    return ys > 0 ? dda_mp(d, entry) : dda_mm(d, entry);
}

/* one ray (WL_DRAW.C: AsmRefresh).  r_asm.s casts the rays; this C version is the reference
   it was written from, and still casts the rays that hit a sliding push wall. */
__attribute__((used, noinline)) void cast_ray_c(s16 pixx)
{
    s16 midangle = c_midangle;
    u16 xpd = c_xpd, xpu = c_xpu;
    u16 ypd = c_ypd, ypu = c_ypu;
    u8 *tm = &tilemap[0][0];

    rc_face[pixx] = 0xFFFF;
    {

        s16 angle = midangle + pixelangle[pixx];
        s16 xtilestep, ytilestep;
        s32 xstep, ystep, xint, yint, ex, ey, mid;
        s16 xtile, ytile, col, page, dn;
        u16 xspot, yspot;
        u8 tilehit, entry;
        dda_t st;

        if (angle < 0) angle += FINEANGLES;
        else if (angle >= ANG360) angle -= FINEANGLES;

        if (angle < ANG90)
        {
            u32 t1 = finetangent[ANG90 - 1 - angle], t2 = finetangent[angle];
            xtilestep = 1; ytilestep = -1;
            xstep = t1; ystep = -(s32)t2;
            xint = viewx + mulhi(t1, ypd);
            yint = viewy - mulhi(t2, xpu);
        }
        else if (angle < ANG180)
        {
            u32 t1 = finetangent[angle - ANG90], t2 = finetangent[ANG180 - 1 - angle];
            xtilestep = -1; ytilestep = -1;
            xstep = -(s32)t1; ystep = -(s32)t2;
            xint = viewx - mulhi(t1, ypd);
            yint = viewy - mulhi(t2, xpd);
        }
        else if (angle < ANG270)
        {
            u32 t1 = finetangent[ANG270 - 1 - angle], t2 = finetangent[angle - ANG180];
            xtilestep = -1; ytilestep = 1;
            xstep = -(s32)t1; ystep = t2;
            xint = viewx - mulhi(t1, ypu);
            yint = viewy + mulhi(t2, xpd);
        }
        else
        {
            u32 t1 = finetangent[angle - ANG270], t2 = finetangent[ANG360 - 1 - angle];
            xtilestep = 1; ytilestep = 1;
            xstep = t1; ystep = t2;
            xint = viewx + mulhi(t1, ypu);
            yint = viewy + mulhi(t2, xpu);
        }
        xtile = focaltx + xtilestep;
        xspot = (xtile << 6) + (yint >> 16);
        ytile = focalty + ytilestep;
        yspot = ((xint >> 16) << 6) + ytile;

        st.xint = xint; st.yint = yint; st.xstep = xstep; st.ystep = ystep;
        st.xtile = xtile; st.ytile = ytile; st.xspot = xspot; st.yspot = yspot;
        entry = 0;
dda_again:
        tilehit = dda_run(&st, entry, xtilestep, ytilestep);
        xint = st.xint; yint = st.yint; xtile = st.xtile; ytile = st.ytile;
        xspot = st.xspot; yspot = st.yspot;
        if (!st.vert)
            goto hithoriz;

hitvert:
        if (tilehit & 0x80)
        {
            if (tilehit & 0x40)
            {
                /* sliding push wall */
                mid = yint + mulfrac(ystep, pwallpos << 10);
                if ((mid >> 16) != (yint >> 16))
                    { entry = 1; goto dda_again; }
                yint = mid;
                ex = (s32)xtile << 16;
                if (xtilestep == -1)
                {
                    col = 63 - ((yint >> 10) & 63);
                    ex += TILEGLOBAL - ((s32)pwallpos << 10);
                }
                else
                {
                    col = (yint >> 10) & 63;
                    ex += (s32)pwallpos << 10;
                }
                wallheight[pixx] = calc_height(ex, yint);
                rc_tex[pixx] = wallpage(vertwall[tilehit & 63], col);
                goto nextpix;
            }
            dn = tilehit & 0x7f;
            mid = yint + (ystep >> 1);
            if ((mid >> 16) != (yint >> 16))
                { entry = 1; goto dda_again; }
            if ((u16)mid < doorposition[dn])
                { entry = 1; goto dda_again; }
            yint = mid;
            ex = ((s32)xtile << 16) + 0x8000;
            col = ((yint - doorposition[dn]) >> 10) & 63;
            wallheight[pixx] = calc_height(ex, yint);
            rc_tex[pixx] = doorpage(dn, TRUE) + (col << 6);
            goto nextpix;
        }
        ex = (s32)xtile << 16;
        col = (yint >> 10) & 63;
        if (xtilestep == -1)
        {
            col = 63 - col;
            ex += TILEGLOBAL;
        }
        wallheight[pixx] = calc_height(ex, yint);
        rc_face[pixx] = xspot;
        if (tilehit & 0x40)
        {
            if (tm[((xtile - xtilestep) << 6) + (yint >> 16)] & 0x80)
                page = DOORWALL + 3;
            else
                page = vertwall[tilehit & ~0x40];
        }
        else
            page = vertwall[tilehit];
        rc_tex[pixx] = wallpage(page, col);
        goto nextpix;

hithoriz:
        if (tilehit & 0x80)
        {
            if (tilehit & 0x40)
            {
                mid = xint + mulfrac(xstep, pwallpos << 10);
                if ((mid >> 16) != (xint >> 16))
                    { entry = 2; goto dda_again; }
                xint = mid;
                ey = (s32)ytile << 16;
                col = (xint >> 10) & 63;
                if (ytilestep == -1)
                    ey += TILEGLOBAL - ((s32)pwallpos << 10);
                else
                {
                    col = 63 - col;
                    ey += (s32)pwallpos << 10;
                }
                wallheight[pixx] = calc_height(xint, ey);
                rc_tex[pixx] = wallpage(horizwall[tilehit & 63], col);
                goto nextpix;
            }
            dn = tilehit & 0x7f;
            mid = xint + (xstep >> 1);
            if ((mid >> 16) != (xint >> 16))
                { entry = 2; goto dda_again; }
            if ((u16)mid < doorposition[dn])
                { entry = 2; goto dda_again; }
            xint = mid;
            ey = ((s32)ytile << 16) + 0x8000;
            col = ((xint - doorposition[dn]) >> 10) & 63;
            wallheight[pixx] = calc_height(xint, ey);
            rc_tex[pixx] = doorpage(dn, FALSE) + (col << 6);
            goto nextpix;
        }
        ey = (s32)ytile << 16;
        col = (xint >> 10) & 63;
        if (ytilestep == -1)
            ey += TILEGLOBAL;
        else
            col = 63 - col;
        wallheight[pixx] = calc_height(xint, ey);
        rc_face[pixx] = yspot | 0x8000;
        if (tilehit & 0x40)
        {
            if (tm[((xint >> 16) << 6) + ytile - ytilestep] & 0x80)
                page = DOORWALL + 2;
            else
                page = horizwall[tilehit & ~0x40];
        }
        else
            page = horizwall[tilehit];
        rc_tex[pixx] = wallpage(page, col);

nextpix:
        ;
    }
}

/* r_asm.s: cast all rays.  Rays 0,4,8.. are cast in full.  A ray halfway between two rays
   that hit the same wall face is interpolated instead: on a flat wall 1/z is linear in
   screen x, so the projected height is the mean of the two, and the texture column is
   interpolated perspective-correctly (u/z is linear too).  The rest are cast. */
void rc_cast(void);

static void cast_rays(void)
{
    c_midangle = viewangle * (FINEANGLES / ANGLES);
    c_xpd = (u16)viewx; c_xpu = (u16)(0x10000 - (u16)viewx);
    c_ypd = (u16)viewy; c_ypu = (u16)(0x10000 - (u16)viewy);
    rc_cast();
}

/* ------------------------------------------------------------ drawing */

/* a visible sprite.  The layout is known to r_asm.s (r_asm.h). */
typedef struct
{
    u16 viewheight;         /* sort key, and compared with wallheight[] to cull */
    u16 nocull;             /* drawn over the walls whatever its distance (unused: the weapon is hardware sprites now) */
    s16 xl;                 /* screen x of the left edge */
    s16 top;                /* screen row of the top edge (negative when taller than the view) */
    u32 step;               /* texels per pixel, 16.16 */
    const u16 *rowtab;      /* spr_rowtab row of this height */
    const u16 *colofs;      /* colofs[texel column] = offset of its posts in data */
    const u8 *data;
    s16 left, right;        /* non-empty texel columns */
    u16 H;
    s16 rc0, rc1;           /* ray columns to draw */
} vis_t;
_Static_assert(offsetof(vis_t, viewheight) == V_VIEWHEIGHT && offsetof(vis_t, nocull) == V_NOCULL &&
               offsetof(vis_t, xl) == V_XL && offsetof(vis_t, top) == V_TOP &&
               offsetof(vis_t, step) == V_STEP && offsetof(vis_t, rowtab) == V_ROWTAB &&
               offsetof(vis_t, colofs) == V_COLOFS && offsetof(vis_t, data) == V_DATA &&
               offsetof(vis_t, left) == V_LEFT && offsetof(vis_t, right) == V_RIGHT &&
               offsetof(vis_t, H) == V_H && offsetof(vis_t, rc0) == V_RC0 &&
               offsetof(vis_t, rc1) == V_RC1 && sizeof(vis_t) == V_SIZE && VIEWH == R_VIEWH,
               "vis_t does not match r_asm.h");

/* the sprite list is rebuilt every frame: it lives in the tail of screenbuf that the game
   mode leaves unused (see BG_TABLES below), not in the scarce work RAM */
#define VISLIST_OFS     28320
#define vislist         ((vis_t *)(screenbuf + VISLIST_OFS))
_Static_assert(VISLIST_OFS + MAXVIS * V_SIZE <= SCREEN_BYTES, "screenbuf tail overflow");
s16 spr_top, spr_bot;           /* rows written by sprites in the current tile column */
vis_t *vorder[MAXVIS];
s16 numvis;

static void vis_setup(vis_t *v, s16 shapenum, s16 vx, u16 height, boolean nocull, u16 forceH)
{
    s16 c1;
    u16 H = forceH ? forceH : (height >> 2);
    const sprhdr_t *h = &spr_hdr[shapenum];
    v->viewheight = height;
    v->nocull = nocull;
    if (H == 0 || h->data_ofs == 0xFFFFFFFF)
    {
        v->rc0 = 1; v->rc1 = 0;
        return;
    }
    if (H > 512) H = 512;
    v->H = H;
    v->xl = vx - (H >> 1);
    v->top = (s16)(VIEWH - H) >> 1;
    v->step = recip_step[H];
    v->rowtab = spr_rowtab + mulu16(H, 65);
    v->left = h->left;
    v->right = h->right;
    v->colofs = spr_colofs + h->colidx - h->left;
    v->data = spr_data + h->data_ofs;
    v->rc0 = v->xl >> 1;
    c1 = ((v->xl + H) >> 1) - 1;
    v->rc1 = c1 >= NRAYS ? NRAYS - 1 : c1;
    if (v->rc0 < 0) v->rc0 = 0;
    {
        /* trim the ray range to the shape's non-empty texel columns [left, right]
           (nothing is drawn outside them; the texel column grows with the ray) */
        u32 st2 = v->step << 1, pos;
        if (v->rc0 > v->rc1) return;
        pos = mul16x32((u16)(2 * v->rc0 + 1 - v->xl), v->step);
        while ((s16)(pos >> 16) < h->left)
        {
            if (++v->rc0 > v->rc1) return;
            pos += st2;
        }
        pos = mul16x32((u16)(2 * v->rc1 + 1 - v->xl), v->step);
        while ((s16)(pos >> 16) > h->right)
        {
            if (--v->rc1 < v->rc0) return;
            pos -= st2;
        }
    }
}

/* tilemap index of every static object, in the unused tail of screenbuf after vislist */
#define STATSPOT_OFS    (VISLIST_OFS + MAXVIS * V_SIZE)
#define statspot        ((u16 *)(screenbuf + STATSPOT_OFS))
_Static_assert(STATSPOT_OFS + 2 * (MAXSTATS + 1) <= SCREEN_BYTES, "screenbuf tail overflow");
static u16 statspot_serial = 0xFFFF, statspot_mode = 0xFFFF;

static void build_vislist(void)
{
    s16 i, j, least, nstat;
    const u16 *qs;
    statobj_t *sp;
    objtype *ob;
    s16 vx, vh;
    vis_t *v = vislist;
    vis_t *vend = vislist + MAXVIS;

    /* Most statics are out of sight, so finding the ones on a tile seen this frame has to
       be quick: statspot caches the tilemap index of every static, ending with the player's
       own tile, which is always marked seen and so stops the scan without a counter. */
    nstat = laststatobj - statobjlist;
    if (statspot_serial != stat_serial || statspot_mode != vid_mode_serial)
    {
        statspot_serial = stat_serial;
        statspot_mode = vid_mode_serial;
        for (i = 0; i < nstat; i++)
            statspot[i] = (statobjlist[i].tilex << 6) + statobjlist[i].tiley;
    }
    statspot[nstat] = (player->tilex << 6) + player->tiley;
    for (qs = statspot; ; )
    {
        {
            register const u16 *q __asm__("a0") = qs;
            u16 x;
            __asm__ volatile(
                "1: move.w  (%[q])+,%[x]\n"
                "   cmp.b   0(%[sv],%[x].w),%[st]\n"
                "   bne.s   1b\n"
                : [q] "+a"(q), [x] "=&d"(x)
                : [sv] "a"(&spotvis[0][0]), [st] "d"(vis_stamp)
                : "cc", "memory");
            qs = q;
        }
        i = qs - statspot - 1;
        if (i >= nstat) break;
        sp = &statobjlist[i];
        if (sp->shapenum == -1) continue;
        if (TransformTile(sp->tilex, sp->tiley, &vx, &vh) && (sp->flags & FL_BONUS))
        {
            GetBonus(sp);
            continue;
        }
        if (!vh) continue;
        if (v < vend)
        {
            vis_setup(v, sp->shapenum, vx, vh, FALSE, 0);
            v++;
        }
    }

    for (ob = player->next; ob; ob = ob->next)
    {
        s16 shapenum = ob->state->shapenum;
        u8 *visspot, *tilespot;
        u16 spotloc;
        if (!shapenum) continue;
        spotloc = (ob->tilex << 6) + ob->tiley;
        visspot = &spotvis[0][0] + spotloc;
        tilespot = &tilemap[0][0] + spotloc;
        {
            /* most actors are nowhere near a tile seen this frame: rule those out with
               nine compares before the full test below */
            u16 near;
            __asm__(
                "   cmp.b   (%[v]),%[st]\n     beq.s 1f\n"
                "   cmp.b   -1(%[v]),%[st]\n   beq.s 1f\n"
                "   cmp.b   1(%[v]),%[st]\n    beq.s 1f\n"
                "   cmp.b   -65(%[v]),%[st]\n  beq.s 1f\n"
                "   cmp.b   -64(%[v]),%[st]\n  beq.s 1f\n"
                "   cmp.b   -63(%[v]),%[st]\n  beq.s 1f\n"
                "   cmp.b   63(%[v]),%[st]\n   beq.s 1f\n"
                "   cmp.b   64(%[v]),%[st]\n   beq.s 1f\n"
                "   cmp.b   65(%[v]),%[st]\n   beq.s 1f\n"
                "   moveq   #0,%[r]\n          bra.s 2f\n"
                "1: moveq   #1,%[r]\n"
                "2:\n"
                : [r] "=&d"(near)
                : [v] "a"(visspot), [st] "d"(vis_stamp)
                : "cc");
            if (!near)
            {
                ob->flags &= ~FL_VISABLE;
                continue;
            }
        }
        if (TILE_SEEN(visspot)
            || (TILE_SEEN(visspot - 1) && !*(tilespot - 1))
            || (TILE_SEEN(visspot + 1) && !*(tilespot + 1))
            || (TILE_SEEN(visspot - 65) && !*(tilespot - 65))
            || (TILE_SEEN(visspot - 64) && !*(tilespot - 64))
            || (TILE_SEEN(visspot - 63) && !*(tilespot - 63))
            || (TILE_SEEN(visspot + 65) && !*(tilespot + 65))
            || (TILE_SEEN(visspot + 64) && !*(tilespot + 64))
            || (TILE_SEEN(visspot + 63) && !*(tilespot + 63)))
        {
            ob->active = ac_yes;
            TransformActor(ob);
            if (!ob->viewheight) continue;
            if (shapenum == -1) shapenum = ob->temp1;
            if (ob->state->rotate)
                shapenum += CalcRotate(ob);
            if (v < vend)
            {
                vis_setup(v, shapenum, ob->viewx, ob->viewheight, FALSE, 0);
                v++;
            }
            ob->flags |= FL_VISABLE;
        }
        else
            ob->flags &= ~FL_VISABLE;
    }

    numvis = 0;
    for (i = 0; &vislist[i] < v; i++)
        if (vislist[i].rc0 <= vislist[i].rc1)
            vorder[numvis++] = &vislist[i];
    /* far to near (smallest height first): insertion sort */
    for (i = 1; i < numvis; i++)
    {
        vis_t *t = vorder[i];
        j = i - 1;
        while (j >= 0 && vorder[j]->viewheight > t->viewheight)
        {
            vorder[j + 1] = vorder[j];
            j--;
        }
        vorder[j + 1] = t;
    }
    (void)least;
}

u32 colseq[VIEWC];              /* upload job of each tile column (vid_jdone when it is in VRAM) */

/* Background bookkeeping, so the ceiling and floor are neither refilled nor re-uploaded
   where they already hold plain background:
   RAM:       rows [0, bg_ctop) of tile column k are pure ceiling, rows [bg_cbot, VIEWH) pure floor
   VRAM bank: tile rows [0, bg_vtop) are pure ceiling tiles, [bg_vbot, VIEWR) pure floor tiles
   Anything else that draws into the view (vid_mode_serial) resets it to "nothing known".
   The tables live in the tail of screenbuf that the game mode leaves unused (after the
   view, the status bar and spotvis), not in the scarce work RAM: whatever else reuses
   that memory bumps vid_mode_serial first. */
#define BG_TABLES       (screenbuf + 26880)
#define bg_ctop         (BG_TABLES)                     /* [VIEWC] */
#define bg_cbot         (BG_TABLES + VIEWC)             /* [VIEWC] */
#define bg_vtop         ((u8 (*)[VIEWC])(BG_TABLES + 2 * VIEWC))    /* [2][VIEWC] */
#define bg_vbot         ((u8 (*)[VIEWC])(BG_TABLES + 4 * VIEWC))    /* [2][VIEWC] */
static u16 bg_serial = 0xFFFF;
_Static_assert(VIEW_BYTES + STAT_BYTES + MAPSIZE * MAPSIZE <= 26880, "BG_TABLES overlaps spotvis");
_Static_assert(R_BG_TABLES == 26880 && R_VIEWC == VIEWC && R_NRAYS == NRAYS && R_COLBYTES == VIEW_COLBYTES &&
               R_JOBSIZE == VID_JOBSIZE && R_MAXVIS == MAXVIS && R_MINDIST8 == (MINDIST >> 8) && R_DOORWALL == DOORWALL &&
               ANG90 == 900 && R_SPOTVIS == 22784 && dr_normal == 0 && dr_elevator == 5 && R_JOBRING == VID_MAXJOBS * VID_JOBSIZE, "r_asm.h out of date");
/* per frame scratch of render_columns' sprite sweep, also in the unused tail of screenbuf
   (after the VT_SEQ test checksums) */
#define SPR_SWEEP       (screenbuf + R_SPR_SWEEP)
_Static_assert(26880 + 6 * VIEWC <= 27136 && 27136 + 240 * 4 <= 28160, "screenbuf tail overflow");
_Static_assert(28160 + VIEWC + 2 * MAXVIS <= SCREEN_BYTES, "screenbuf tail overflow");

#define CEIL4   0x11111111UL

/* r_asm.s: the per tile column loop (background fill, wall scalers, sprites, upload) */
void rc_columns(u8 *vtop, u16 vram);
void rcl_columns(u8 *vtop, u16 vram);     /* the low detail version */

u16 spr_nact;                   /* sprites in rc_columns' active list */

static void render_columns(void)
{
    s16 i;
    u16 bank = vid_target ? 1 : 0;
    u8 *bfirst = SPR_SWEEP, *bnext = SPR_SWEEP + VIEWC;

    if (bg_serial != vid_mode_serial)
    {
        bg_serial = vid_mode_serial;
        memset(bg_ctop, 0, VIEWC);
        memset(bg_cbot, VIEWH, VIEWC);
        memset(bg_vtop, 0, 2 * VIEWC);
        memset(bg_vbot, VIEWR, 2 * VIEWC);
    }
    if (numvis)
    {
        /* bucket the sprites by their first tile column, each bucket in vorder order */
        memset(bfirst, 0xFF, VIEWC);
        for (i = numvis - 1; i >= 0; i--)
        {
            u16 c = vorder[i]->rc0 >> 2;
            bnext[i] = bfirst[c];
            bfirst[c] = i;
        }
        spr_nact = 0;
    }
    if (r_lowdetail)
        rcl_columns(bg_vtop[bank], (bank ? TILE_VIEW1 : TILE_VIEW0) * 32);
    else
        rc_columns(bg_vtop[bank], (bank ? TILE_VIEW1 : TILE_VIEW0) * 32);
}

void R_SetLevelColors(u16 ceilcolor)
{
    memcpy(game_pal0, view_palette, 32);
    game_pal0[CEIL_PALIDX] = ceilcolor;
}

void R_Init(void)
{
    s16 i;
    for (i = 1; i < MAXWALLTILES; i++)
    {
        horizwall[i] = (i - 1) * 2;
        vertwall[i] = (i - 1) * 2 + 1;
    }
    horizwall[0] = vertwall[0] = 0;
    for (i = 0; i < 64; i++)
    {
        rc_wallptr_h[i] = horizwall[i] < NUMWALLPAGES ? wallpage(horizwall[i], 0) : wall_data;
        rc_wallptr_v[i] = vertwall[i] < NUMWALLPAGES ? wallpage(vertwall[i], 0) : wall_data;
    }
    rc_wallptr_h[64] = wallpage(DOORWALL + 2, 0);
    rc_wallptr_v[64] = wallpage(DOORWALL + 3, 0);
    memcpy(game_pal0, view_palette, 32);
}

static u32 last_world, last_sprites;

static inline u32 sig_add(u32 h, u32 v)
{
    return ((h << 5) | (h >> 27)) ^ v;
}

/* everything the walls of the view depend on: the viewpoint, the doors, the push wall */
static u32 world_sig(void)
{
    u32 h = (u32)viewx;
    const u16 *dp = doorposition, *de = doorposition + (lastdoorobj - doorobjlist);
    h = sig_add(h, (u32)viewy);
    h = sig_add(h, ((u32)(u16)viewangle << 16) | pwallstate);
    h = sig_add(h, r_lowdetail);
    h = sig_add(h, ((u32)pwallpos << 16) | (pwallx << 8) | pwally);
    while (dp < de)
        h = sig_add(h, *dp++);
    return h;
}

void R_Frame(void)
{
    u32 world, sprites;
    boolean recast;
    s16 i;
    viewsin = sintable[viewangle];
    viewcos = sintable[viewangle + ANGLES / 4];
    vc15 = viewcos >> 1; if (vc15 > 32767) vc15 = 32767;
    vs15 = viewsin >> 1; if (vs15 > 32767) vs15 = 32767;
    viewx = player->x - FixedByFrac(FOCALLENGTH, viewcos);
    viewy = player->y + FixedByFrac(FOCALLENGTH, viewsin);
    focaltx = viewx >> TILESHIFT;
    focalty = viewy >> TILESHIFT;

    /* Nothing in the picture depends on anything but the state hashed below (the weapon
       and the status bar are not part of the view), so when it is the same as in the last
       frame drawn, that frame is simply left on screen.  The rays are still valid then;
       only the sprite list has to be rebuilt to see whether a sprite moved or changed. */
    world = world_sig();
    recast = world != last_world || vis_serial != vid_mode_serial;
    if (recast)
    {
        if (vis_serial != vid_mode_serial || ++vis_stamp == 0)
        {
            /* first frame in this video mode, or the stamp wrapped: forget every old mark */
            vis_serial = vid_mode_serial;
            memset(spotvis, 0, sizeof(spotvis));
            vis_stamp = 1;
        }
        spotvis[player->tilex][player->tiley] = vis_stamp;
        cast_rays();
    }
    build_vislist();
    sprites = numvis;
    for (i = 0; i < numvis; i++)
    {
        const vis_t *v = vorder[i];
        sprites = sig_add(sprites, ((u32)v->viewheight << 16) | (u16)v->xl);
        sprites = sig_add(sprites, (u32)v->data);
    }
#ifndef VT_TOUR     /* the benchmark draws every frame, to stay comparable */
    if (!recast && sprites == last_sprites)
        return;
#endif
    last_world = world;
    last_sprites = sprites;
    render_columns();
    /* flip once everything is uploaded */
    vid_flip_queue();
}

/* ------------------------------------------------------------------ */
/* ThreeDRefresh: one frame of the 3-D view + status bar (WL_DRAW.C)   */

_Static_assert(WP_FIRST_SPR == SPR_KNIFEREADY && SPR_CHAINATK4 == WP_FIRST_SPR + WP_FRAMES - 1, "weapon.h out of date");
static const s16 weaponscale[4] = { SPR_KNIFEREADY, SPR_PISTOLREADY, SPR_MACHINEGUNREADY, SPR_CHAINREADY };

volatile u32 dbg_frames;

#if defined(VT_SEQ) || defined(VT_TOUR)
/* test hook: render a fixed sequence of views and record a checksum of the VRAM view bank
   shown after each frame, so two builds can be compared frame by frame.  Frames 0..79 turn
   in the start room (slow runs mixed with jumps); later frames stand the player next to a
   series of static objects (actors are frozen by PlayLoop, so every view is reproducible). */
#define VT_FRAMES 240
/* kept in the part of screenbuf the game mode leaves unused, not in (tight) work RAM */
#define vt_sum ((volatile u32 *)(screenbuf + 27136))
volatile u32 vt_t8, vt_t80, vt_t240;    /* VT_TOUR benchmark: vblank count at frames 8, 80 and 240 */
static void vt_check(void)
{
    u32 n = dbg_frames - 1, s = 0, i;
    u16 vram;
    if (n >= VT_FRAMES) return;
    vid_wait_idle();
    vram = (vid_front ? TILE_VIEW1 : TILE_VIEW0) * 32;
    SYS_disableInts();
    *(volatile u32 *)VDP_CTRL_PORT = VDP_READ_VRAM_ADDR((u32)vram);
    for (i = 0; i < VIEW_BYTES / 2; i++)
    {
        u16 w = *(volatile u16 *)VDP_DATA_PORT;
#ifdef VT_EVENSUM
        /* checksum of the picture low detail has to show: every odd ray = the ray to its left */
        w = (w & 0xFF00) | (w >> 8);
#endif
        s = (s << 1 | s >> 31) ^ w;
    }
    SYS_enableInts();
    vt_sum[n] = s;
}

static void vt_place(u32 n)
{
    static const s8 off[4][3] = {{2, 0, 2}, {-2, 0, 0}, {0, 2, 1}, {0, -2, 3}};    /* dx, dy, quarter turns */
    s16 cnt = laststatobj - statobjlist, tries;
    if (n == 8) vt_t8 = vid_frames;
    if (n == 80) vt_t80 = vid_frames;
    if (n == VT_FRAMES) vt_t240 = vid_frames;
    if (n > VT_FRAMES) n = VT_FRAMES;
    if (n < 80 || cnt <= 0)
    {
        player->angle = (n & 8) ? (s16)((n * 3) % ANGLES) : (s16)((n * n * 37 + n * 53) % ANGLES);
        return;
    }
    for (tries = 0; tries < 200; tries++)
    {
        u32 m = n * 13 + tries * 7;
        const statobj_t *sp = &statobjlist[m % cnt];
        const s8 *o = off[(m / cnt + n) & 3];
        s16 tx = sp->tilex + o[0], ty = sp->tiley + o[1];
        if (sp->shapenum == -1 || tx < 1 || ty < 1 || tx > MAPSIZE - 2 || ty > MAPSIZE - 2)
            continue;
        if (tilemap[tx][ty] || tilemap[sp->tilex + o[0] / 2][sp->tiley + o[1] / 2])
            continue;
        player->tilex = tx;
        player->tiley = ty;
        player->x = ((s32)tx << TILESHIFT) + 0x8000;
        player->y = ((s32)ty << TILESHIFT) + 0x8000;
        player->angle = (s16)((o[2] * (ANGLES / 4) + (n % 5) * 9 + ANGLES - 18) % ANGLES);
        return;
    }
}
#endif

void ThreeDRefresh(void)
{
    dbg_frames++;
#ifdef VT_ANGLE
    player->angle = VT_ANGLE;
#endif
#if defined(VT_SEQ) || defined(VT_TOUR)
    vt_place(dbg_frames);
#endif
#ifdef VT_DOORS
    { void vt_doors(u32 n); vt_doors(dbg_frames); }
#endif
    viewangle = player->angle;
    if (gamestate.weapon >= 0 && gamestate.weapon <= wp_chaingun)
        weapon_spr = weaponscale[gamestate.weapon] + gamestate.weaponframe;
    else
        weapon_spr = -1;
#ifdef VT_NOWEAPON
    weapon_spr = -1;
#endif
#ifdef VT_WEAPON
    { extern const volatile u8 vt_weapon_sel; weapon_spr = SPR_KNIFEREADY + vt_weapon_sel; }
#endif
    /* the weapon is drawn by hardware sprites over the view */
    vid_weapon(weapon_spr >= 0 ? weapon_spr - WP_FIRST_SPR : -1);
    R_Frame();
    vid_flush(&statsurf);
    fizzlein = FALSE;
#ifdef VT_SEQ
    vt_check();
#endif
}
