/* render.c - the 3-D view: ray casting, wall and sprite scaling.
 *
 * The ray casting loop is a C transcription of WL_DR_A.ASM / WL_DRAW.C.
 * Rays are two pixels wide (VIEWW/2 rays); a first pass casts every ray and
 * records its wall column and height, the second pass renders the picture
 * one 8-pixel tile column at a time (floor/ceiling fill, walls, sprites,
 * weapon) and hands each finished column to the DMA queue, so the VRAM
 * upload overlaps the CPU work and no second RAM frame buffer is needed.
 */
#include "wl_def.h"
#include "vid.h"
#include <string.h>

#define NRAYS       (VIEWW / 2)
#define MAXVIS      64
void GetBonus(statobj_t *check);

s32 viewx, viewy;
s16 viewangle;
s32 viewsin, viewcos;
s16 centerx = VIEWW / 2;
u16 wallheight[NRAYS];
s16 weapon_spr = -1;
s16 weapon_dy;
u16 game_pal0[16];
const s16 dirangle[9] = {0, ANGLES / 8, 2 * ANGLES / 8, 3 * ANGLES / 8, 4 * ANGLES / 8,
                         5 * ANGLES / 8, 6 * ANGLES / 8, 7 * ANGLES / 8, 0};

static const u8 *rc_tex[NRAYS];
static s16 horizwall[MAXWALLTILES], vertwall[MAXWALLTILES];
static s16 focaltx, focalty;

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

static s16 vc15, vs15;          /* viewcos / viewsin as signed 1.15 */

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

static s16 c_midangle;
static u16 c_xpd, c_xpu, c_ypd, c_ypu;
static u16 rc_face[NRAYS];      /* which plain wall face a ray hit (0xFFFF = door/push wall) */

/* spotvis marks the tiles the rays crossed this frame.  Instead of clearing the whole
   64x64 map every frame, a tile counts as seen when it holds this frame's stamp
   (1..255); the map is only cleared when the stamp wraps or the video mode changes
   (spotvis lives inside screenbuf, which the menus and intermission screens reuse). */
static u8 vis_stamp = 1;
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

/* one ray (WL_DRAW.C: AsmRefresh) */
static void cast_ray(s16 pixx)
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

static inline s16 divs32_16(s32 n, s16 d)
{
    s32 r = n;
    __asm__("divs.w %1,%0" : "+d"(r) : "dm"(d) : "cc");
    return (s16)r;
}

/* Ray m lies halfway between rays l and r.  If those hit the same wall face
   the in-between ray is interpolated instead of cast: on a flat wall 1/z is
   linear in screen x, so the projected height is the mean of the two, and the
   texture column is interpolated perspective-correctly (u/z is linear too). */
static boolean interp_ray(s16 m, s16 l, s16 r)
{
    u16 h1, h2;
    s16 dc, u;
    const u8 *p1, *p2;
#ifdef NOINTERP
    return FALSE;
#endif

    if (rc_face[l] == 0xFFFF || rc_face[l] != rc_face[r])
        return FALSE;
    h1 = wallheight[l];
    h2 = wallheight[r];
    p1 = rc_tex[l];
    p2 = rc_tex[r];
    wallheight[m] = (u16)(((u32)h1 + h2) >> 1);
    dc = (s16)((p2 - p1) >> 6);
    if (dc)
    {
        if (h1 > 8192) h1 = 8192;
        if (h2 > 8192) h2 = 8192;
        /* dc is 16 bit and h2 <= 8192: one muls.w instead of a __mulsi3 call */
        u = divs32_16(muls16(dc, (s16)h2), (s16)(h1 + h2));
        p1 += (s16)(u << 6);
    }
    rc_tex[m] = p1;
    rc_face[m] = rc_face[l];
    return TRUE;
}

/* Rays 0,4,8.. are cast in full; the rest are interpolated wherever the rays
   around them hit the same wall face, and cast otherwise. */
static void cast_rays(void)
{
    s16 a;
    c_midangle = viewangle * (FINEANGLES / ANGLES);
    c_xpd = (u16)viewx; c_xpu = (u16)(0x10000 - (u16)viewx);
    c_ypd = (u16)viewy; c_ypu = (u16)(0x10000 - (u16)viewy);
    for (a = 0; a < NRAYS; a += 4)
        cast_ray(a);
    cast_ray(NRAYS - 2);
    cast_ray(NRAYS - 1);
    for (a = 0; a < NRAYS - 4; a += 4)
        if (!interp_ray(a + 2, a, a + 4))
            cast_ray(a + 2);
    for (a = 0; a < NRAYS - 3; a += 2)
        if (!interp_ray(a + 1, a, a + 2))
            cast_ray(a + 1);
}

/* ------------------------------------------------------------ drawing */

/* scaled column of wall texels, one byte (two pixels) wide: runs a precompiled
   scaler routine (see tools/scalers.py) for this wall height */
static inline void draw_wall(u8 *dst, const u8 *tex, u16 H)
{
    register const u8 *t __asm__("a0");
    register u8 *d __asm__("a1");
    u16 idx;

    if (H > 512) H = 512;
    d = dst;
    if (H <= VIEWH)
    {
        d += (u16)(((VIEWH - H) >> 1) << 2);
        idx = H;
    }
    else
        idx = VIEWH + ((H - (VIEWH - 1)) >> 1);
    t = tex;
    /* the scaler routines use d0 as a scratch register (tools/scalers.py) */
    __asm__ volatile("jsr (%2)"
                     : "+a"(t), "+a"(d)
                     : "a"((const u8 *)scaler_code + scaler_ofs[idx])
                     : "memory", "cc", "d0");
}

typedef struct
{
    s16 shapenum;
    s16 viewx;
    u16 viewheight;
    u16 H;
    s16 xl, rc0, rc1;
    s16 nextc;
    u32 step, tpos;
    const sprhdr_t *hdr;
    boolean nocull;
} vis_t;

static vis_t vislist[MAXVIS];
static s16 spr_top, spr_bot;    /* rows written by sprites in the current tile column */
static vis_t *vorder[MAXVIS];
static s16 numvis;

static void vis_setup(vis_t *v, s16 shapenum, s16 vx, u16 height, boolean nocull, u16 forceH)
{
    s16 c1;
    u16 H = forceH ? forceH : (height >> 2);
    v->shapenum = shapenum;
    v->viewx = vx;
    v->viewheight = height;
    v->nocull = nocull;
    v->hdr = &spr_hdr[shapenum];
    if (H == 0 || v->hdr->data_ofs == 0xFFFFFFFF)
    {
        v->rc0 = 1; v->rc1 = 0;
        return;
    }
    if (H > 512) H = 512;
    v->H = H;
    v->xl = vx - (H >> 1);
    v->step = recip_step[H];
    v->rc0 = v->xl >> 1;
    c1 = ((v->xl + H) >> 1) - 1;
    v->rc1 = c1 >= NRAYS ? NRAYS - 1 : c1;
    if (v->rc0 < 0) v->rc0 = 0;
    v->nextc = -1;
    {
        /* trim the ray range to the shape's non-empty texel columns [left, right]
           (sprite_col draws nothing outside them; the texel column grows with the ray) */
        const sprhdr_t *h = v->hdr;
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

/* n rows of a scaled texel run, 16.16 position tp stepping by step */
static inline void scale_run(u8 *dst, const u8 *tex, u32 pos, u32 step, u16 n)
{
    register u32 i = pos >> 16, f = pos & 0xFFFF;
    register u32 si = step >> 16, sf = step & 0xFFFF;
    if (step == 0x8000)
    {
        /* exactly 2x (the weapon, always): every texel covers two rows */
        register const u8 *t __asm__("a0") = tex + i;
        register u8 *d __asm__("a1") = dst;
        if (f & 0x8000)
        {
            /* the first row is the second half of a texel */
            *d = *t++;
            d += 4;
            n--;
        }
        while (n >= 8)
        {
            __asm__ volatile(
                "move.b (%[t])+,%%d0\n\t move.b %%d0,0(%[d])\n\t move.b %%d0,4(%[d])\n\t"
                "move.b (%[t])+,%%d0\n\t move.b %%d0,8(%[d])\n\t move.b %%d0,12(%[d])\n\t"
                "move.b (%[t])+,%%d0\n\t move.b %%d0,16(%[d])\n\t move.b %%d0,20(%[d])\n\t"
                "move.b (%[t])+,%%d0\n\t move.b %%d0,24(%[d])\n\t move.b %%d0,28(%[d])\n\t"
                "lea 32(%[d]),%[d]"
                : [t] "+a"(t), [d] "+a"(d)
                :
                : "memory", "cc", "d0");
            n -= 8;
        }
        while (n >= 2)
        {
            u8 b = *t++;
            d[0] = b;
            d[4] = b;
            d += 8;
            n -= 2;
        }
        if (n)
            *d = *t;
        return;
    }
    {
        /* 8 rows per pass, then 4/2/1: the remainder used to run through a C loop at
           about twice the cost per row, and most sprite posts are short */
        register u8 *d = dst;
        u16 c;
        __asm__ volatile(
        "   move.w  %[n],%[c]\n"
        "   lsr.w   #3,%[c]\n"
        "   bra.w   2f\n"
        "1:\n"
        "   move.b  (%[t],%[i].w),0(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   move.b  (%[t],%[i].w),4(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   move.b  (%[t],%[i].w),8(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   move.b  (%[t],%[i].w),12(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   move.b  (%[t],%[i].w),16(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   move.b  (%[t],%[i].w),20(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   move.b  (%[t],%[i].w),24(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   move.b  (%[t],%[i].w),28(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   lea     32(%[d]),%[d]\n"
        "2: dbra    %[c],1b\n"
        "   btst    #2,%[n]\n"
        "   beq.s   3f\n"
        "   move.b  (%[t],%[i].w),0(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   move.b  (%[t],%[i].w),4(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   move.b  (%[t],%[i].w),8(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   move.b  (%[t],%[i].w),12(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   lea     16(%[d]),%[d]\n"
        "3: btst    #1,%[n]\n"
        "   beq.s   4f\n"
        "   move.b  (%[t],%[i].w),0(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   move.b  (%[t],%[i].w),4(%[d])\n   add.w   %[sf],%[f]\n   addx.w  %[si],%[i]\n"
        "   addq.w  #8,%[d]\n"
        "4: btst    #0,%[n]\n"
        "   beq.s   5f\n"
        "   move.b  (%[t],%[i].w),(%[d])\n"
        "5:\n"
            : [i] "+d"(i), [f] "+d"(f), [d] "+a"(d), [c] "=&d"(c)
            : [t] "a"(tex), [n] "d"(n), [si] "d"(si), [sf] "d"(sf)
            : "memory", "cc");
    }
}

/* draw ray column c of a sprite */
static void sprite_col(vis_t *v, u16 c)
{
    s16 xl = v->xl;
    u32 pos;
    s16 tcol;
    const sprhdr_t *h = v->hdr;
    const u8 *p;
    u8 *col;
    s16 top;
    u16 H = v->H;

    if (v->nextc != (s16)c)
        v->tpos = mul16x32((u16)(2 * c + 1 - xl), v->step);
    pos = v->tpos;
    v->tpos = pos + (v->step << 1);
    v->nextc = c + 1;

    tcol = pos >> 16;
    if (tcol < h->left || tcol > h->right)
        return;
    p = spr_data + h->data_ofs + spr_colofs[h->colidx + (tcol - h->left)];
    col = screenbuf + (c >> 2) * VIEW_COLBYTES + (c & 3);
    top = (s16)(VIEWH - H) >> 1;
    for (;;)
    {
        u8 y0 = p[0], y1;
        s16 r0, r1;
        u32 tp;
        u8 *d;
        if (y0 == 0xFF) break;
        y1 = p[1];
        r0 = top + ((mulu16(y0, H) + 63) >> 6);
        r1 = top + ((mulu16(y1, H) + 63) >> 6);
        {
            const u8 *pix = p + 2 - y0;
            p += 2 + (y1 - y0);
            if (r0 < 0) r0 = 0;
            if (r1 > VIEWH) r1 = VIEWH;
            if (r0 >= r1) continue;
            if (r0 < spr_top) spr_top = r0;
            if (r1 > spr_bot) spr_bot = r1;
            tp = mul16x32((u16)(r0 - top), v->step);
            d = col + (r0 << 2);
            r1 -= r0;
            scale_run(d, pix, tp, v->step, r1);
        }
    }
}

static void build_vislist(void)
{
    s16 i, j, least;
    statobj_t *sp;
    objtype *ob;
    s16 vx, vh;
    vis_t *v = vislist;
    vis_t *vend = vislist + MAXVIS;

    /* most statics are out of sight: find the next one on a tile seen this frame (tight
       loop: tilex:tiley read as one word, spot = (tilex << 6) | tiley since tiley < 64) */
    for (sp = statobjlist; ; sp++)
    {
        {
            register statobj_t *q __asm__("a0") = sp;
            u16 w, x;
            __asm__ volatile(
                "1: cmpa.l  %[end],%[q]\n"
                "   beq.s   2f\n"
                "   move.w  (%[q]),%[w]\n"
                "   move.w  %[w],%[x]\n"
                "   lsr.w   #2,%[x]\n"
                "   and.w   #0xFFC0,%[x]\n"
                "   or.b    %[w],%[x]\n"
                "   cmp.b   0(%[sv],%[x].w),%[st]\n"
                "   beq.s   2f\n"
                "   addq.l  #6,%[q]\n"
                "   bra.s   1b\n"
                "2:\n"
                : [q] "+a"(q), [w] "=&d"(w), [x] "=&d"(x)
                : [end] "a"(laststatobj), [sv] "a"(&spotvis[0][0]), [st] "d"(vis_stamp)
                : "cc", "memory");
            sp = q;
        }
        if (sp == laststatobj) break;
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
    /* the player's weapon, always on top */
    if (weapon_spr >= 0)
    {
        v = &vislist[MAXVIS - 1];
        vis_setup(v, weapon_spr, VIEWW / 2, 0, TRUE, VIEWH);
        vorder[numvis++] = v;
    }
}

/* store 'n' longwords (one row of 4 rays each) */
static inline void fill_rows(u32 *p, u32 v, u16 n)
{
    /* move.l Dn,(An)+ is 12 cycles; GCC stored the constant as an immediate with a
       displacement, 24 cycles per longword */
    u16 c;
    __asm__ volatile(
        "   move.w  %[n],%[c]\n"
        "   lsr.w   #3,%[c]\n"
        "   bra.s   2f\n"
        "1: move.l  %[v],(%[p])+\n"
        "   move.l  %[v],(%[p])+\n"
        "   move.l  %[v],(%[p])+\n"
        "   move.l  %[v],(%[p])+\n"
        "   move.l  %[v],(%[p])+\n"
        "   move.l  %[v],(%[p])+\n"
        "   move.l  %[v],(%[p])+\n"
        "   move.l  %[v],(%[p])+\n"
        "2: dbra    %[c],1b\n"
        "   and.w   #7,%[n]\n"
        "   bra.s   4f\n"
        "3: move.l  %[v],(%[p])+\n"
        "4: dbra    %[n],3b\n"
        : [p] "+a"(p), [n] "+d"(n), [c] "=&d"(c)
        : [v] "d"(v)
        : "memory", "cc");
}

static u32 colseq[VIEWC];

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
/* per frame scratch of render_columns' sprite sweep, also in the unused tail of screenbuf
   (after the VT_SEQ test checksums) */
#define SPR_SWEEP       (screenbuf + 28160)
_Static_assert(26880 + 6 * VIEWC <= 27136 && 27136 + 240 * 4 <= 28160, "screenbuf tail overflow");
_Static_assert(28160 + VIEWC + 2 * MAXVIS <= SCREEN_BYTES, "screenbuf tail overflow");

#define CEIL4   0x11111111UL

static void render_columns(void)
{
    s16 k, r, i;
    u16 bank = vid_target ? 1 : 0;
    u16 vram = (bank ? TILE_VIEW1 : TILE_VIEW0) * 32;
    u8 *vtop = bg_vtop[bank], *vbot = bg_vbot[bank];
    u8 *bfirst = SPR_SWEEP, *bnext = SPR_SWEEP + VIEWC, *act = SPR_SWEEP + VIEWC + MAXVIS;
    u16 nact = 0;

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
    }
    for (k = 0; k < VIEWC; k++)
    {
        u8 *base = screenbuf + k * VIEW_COLBYTES;
        const u16 *wh = &wallheight[4 * k];
        const u8 *const *tx = &rc_tex[4 * k];
        u16 st = 0, en = VIEWH;             /* background rows: [0, st) ceiling, [en, VIEWH) floor */
        u16 wt = VIEWH / 2, wb = VIEWH / 2; /* rows covered by walls: [wt, wb) */
        u16 lo, hi, ct, cb;

        vid_wait_job(colseq[k]);       /* previous frame's upload of this column */
        for (r = 0; r < 4; r++)
        {
            u16 H = wh[r] >> 2, a, b;
            if (H >= VIEWH) { wt = 0; wb = VIEWH; continue; }
            if (!H) a = b = VIEWH / 2;
            else
            {
                a = (VIEWH - H) >> 1;
                b = a + H;
                if (a < wt) wt = a;
                if (b > wb) wb = b;
            }
            if (a > st) st = a;
            if (b < en) en = b;
        }
        ct = bg_ctop[k];
        if (st > ct) fill_rows((u32 *)base + ct, CEIL4, st - ct);
        cb = bg_cbot[k];
        if (en < cb) fill_rows((u32 *)base + en, 0UL, cb - en);

        for (r = 0; r < 4; r++)
        {
            u16 H = wh[r] >> 2;
            if (H) draw_wall(base + r, tx[r], H);
        }
        spr_top = VIEWH; spr_bot = 0;
        if (numvis)
        {
            /* sweep: retire sprites that ended left of this column, then insert the ones
               starting here, keeping the active list in vorder (far to near) order */
            u16 j, m = 0;
            u8 x;
            for (j = 0; j < nact; j++)
                if ((vorder[act[j]]->rc1 >> 2) >= k)
                    act[m++] = act[j];
            nact = m;
            for (x = bfirst[k]; x != 0xFF; x = bnext[x])
            {
                j = nact++;
                while (j > 0 && act[j - 1] > x)
                {
                    act[j] = act[j - 1];
                    j--;
                }
                act[j] = x;
            }
            for (j = 0; j < nact; j++)
            {
                vis_t *v = vorder[act[j]];
                s16 a, b;
                a = v->rc0 > 4 * k ? v->rc0 : 4 * k;
                b = v->rc1 < 4 * k + 3 ? v->rc1 : 4 * k + 3;
                for (r = a; r <= b; r++)
                {
                    if (!v->nocull && wallheight[r] >= v->viewheight)
                        continue;
                    sprite_col(v, r);
                }
            }
        }
        if ((u16)spr_top < wt) wt = spr_top;
        if ((u16)spr_bot > wb) wb = spr_bot;
        bg_ctop[k] = wt;
        bg_cbot[k] = wb;

        /* upload only the tile rows that differ from what this VRAM bank already holds */
        ct = wt >> 3;
        cb = (wb + 7) >> 3;
        lo = vtop[k] < ct ? vtop[k] : ct;
        hi = vbot[k] > cb ? vbot[k] : cb;
        vtop[k] = ct;
        vbot[k] = cb;
        if (lo < hi)
            colseq[k] = vid_queue(base + lo * 32, vram + k * VIEW_COLBYTES + lo * 32, (hi - lo) * 32);
    }
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
    memcpy(game_pal0, view_palette, 32);
}

volatile u32 dbg_t0, dbg_t1, dbg_t2, dbg_t3;
volatile u32 dbg_d0, dbg_d1, dbg_d2;
void R_Frame(void)
{
    viewsin = sintable[viewangle];
    viewcos = sintable[viewangle + ANGLES / 4];
    vc15 = viewcos >> 1; if (vc15 > 32767) vc15 = 32767;
    vs15 = viewsin >> 1; if (vs15 > 32767) vs15 = 32767;
    viewx = player->x - FixedByFrac(FOCALLENGTH, viewcos);
    viewy = player->y + FixedByFrac(FOCALLENGTH, viewsin);
    focaltx = viewx >> TILESHIFT;
    focalty = viewy >> TILESHIFT;

    if (vis_serial != vid_mode_serial || ++vis_stamp == 0)
    {
        /* first frame in this video mode, or the stamp wrapped: forget every old mark */
        vis_serial = vid_mode_serial;
        memset(spotvis, 0, sizeof(spotvis));
        vis_stamp = 1;
    }
    spotvis[player->tilex][player->tiley] = vis_stamp;
    dbg_t0 = vid_frames;
    cast_rays();
    dbg_t1 = vid_frames;
    build_vislist();
    dbg_t2 = vid_frames;
    render_columns();
    dbg_t3 = vid_frames;
    dbg_d0 = dbg_t1 - dbg_t0; dbg_d1 = dbg_t2 - dbg_t1; dbg_d2 = dbg_t3 - dbg_t2;
    /* flip once everything is uploaded */
    vid_flip_queue();
}

/* ------------------------------------------------------------------ */
/* ThreeDRefresh: one frame of the 3-D view + status bar (WL_DRAW.C)   */

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
        s = (s << 1 | s >> 31) ^ *(volatile u16 *)VDP_DATA_PORT;
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
    viewangle = player->angle;
    if (gamestate.weapon >= 0 && gamestate.weapon <= wp_chaingun)
        weapon_spr = weaponscale[gamestate.weapon] + gamestate.weaponframe;
    else
        weapon_spr = -1;
    R_Frame();
    vid_flush(&statsurf);
    fizzlein = FALSE;
#ifdef VT_SEQ
    vt_check();
#endif
}
