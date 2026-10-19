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

static inline u16 divu32_16(u32 n, u16 d)
{
    u32 r = n;
    u32 dd = d;
    __asm__ volatile("divu.w %1,%0" : "+d"(r) : "d"(dd) : "cc");
    return (u16)r;
}

static inline s16 calc_height(s32 xint, s32 yint)
{
    s32 nx = FixedByFrac(xint - viewx, viewcos) - FixedByFrac(yint - viewy, viewsin);
    if (nx < MINDIST) nx = MINDIST;
    return divu32_16((u32)proj_heightnumerator, (u16)(nx >> 8));
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

static void cast_rays(void)
{
    s16 pixx;
    s16 midangle = viewangle * (FINEANGLES / ANGLES);
    u16 xpd = (u16)viewx, xpu = (u16)(0x10000 - (u16)viewx);
    u16 ypd = (u16)viewy, ypu = (u16)(0x10000 - (u16)viewy);
    u8 *tm = &tilemap[0][0];
    u8 *sv = &spotvis[0][0];

    for (pixx = 0; pixx < NRAYS; pixx++)
    {
        s16 angle = midangle + pixelangle[pixx];
        s16 xtilestep, ytilestep;
        s32 xstep, ystep, xint, yint, ex, ey, mid;
        u16 xpartial, ypartial;
        s16 xtile, ytile, col, page, dn;
        u16 xspot, yspot;
        u8 tilehit;

        if (angle < 0) angle += FINEANGLES;
        else if (angle >= ANG360) angle -= FINEANGLES;

        if (angle < ANG90)
        {
            xtilestep = 1; ytilestep = -1;
            xstep = finetangent[ANG90 - 1 - angle];
            ystep = -finetangent[angle];
            xpartial = xpu; ypartial = ypd;
        }
        else if (angle < ANG180)
        {
            xtilestep = -1; ytilestep = -1;
            xstep = -finetangent[angle - ANG90];
            ystep = -finetangent[ANG180 - 1 - angle];
            xpartial = xpd; ypartial = ypd;
        }
        else if (angle < ANG270)
        {
            xtilestep = -1; ytilestep = 1;
            xstep = -finetangent[ANG270 - 1 - angle];
            ystep = finetangent[angle - ANG180];
            xpartial = xpd; ypartial = ypu;
        }
        else
        {
            xtilestep = 1; ytilestep = 1;
            xstep = finetangent[angle - ANG270];
            ystep = finetangent[ANG360 - 1 - angle];
            xpartial = xpu; ypartial = ypu;
        }

        yint = viewy + mulfrac(ystep, xpartial);
        xtile = focaltx + xtilestep;
        xspot = (xtile << 6) + (yint >> 16);
        xint = viewx + mulfrac(xstep, ypartial);
        ytile = focalty + ytilestep;
        yspot = ((xint >> 16) << 6) + ytile;

vertcheck:
        if (ytilestep < 0 ? ((yint >> 16) <= ytile) : ((yint >> 16) >= ytile))
            goto horizentry;
vertentry:
        tilehit = tm[xspot];
        if (tilehit)
            goto hitvert;
passvert:
        sv[xspot] = 1;
        xtile += xtilestep;
        yint += ystep;
        xspot = (xtile << 6) + (yint >> 16);
        goto vertcheck;

hitvert:
        if (tilehit & 0x80)
        {
            if (tilehit & 0x40)
            {
                /* sliding push wall */
                mid = yint + mulfrac(ystep, pwallpos << 10);
                if ((mid >> 16) != (yint >> 16))
                    goto passvert;
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
                goto passvert;
            if ((u16)mid < doorposition[dn])
                goto passvert;
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

horizcheck:
        if (xtilestep < 0 ? ((xint >> 16) <= xtile) : ((xint >> 16) >= xtile))
            goto vertentry;
horizentry:
        tilehit = tm[yspot];
        if (tilehit)
            goto hithoriz;
passhoriz:
        sv[yspot] = 1;
        ytile += ytilestep;
        xint += xstep;
        yspot = ((xint >> 16) << 6) + ytile;
        goto horizcheck;

hithoriz:
        if (tilehit & 0x80)
        {
            if (tilehit & 0x40)
            {
                mid = xint + mulfrac(xstep, pwallpos << 10);
                if ((mid >> 16) != (xint >> 16))
                    goto passhoriz;
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
                goto passhoriz;
            if ((u16)mid < doorposition[dn])
                goto passhoriz;
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

/* ------------------------------------------------------------ drawing */

/* scaled column of wall texels, one byte (two pixels) wide */
static void draw_wall(u16 c, const u8 *tex, u16 H)
{
    u8 *dst = screenbuf + (c >> 2) * VIEW_COLBYTES + (c & 3);
    u32 step, pos;
    u16 n;

    if (H > 512) H = 512;
    step = recip_step[H];
    if (H >= VIEWH)
    {
        n = VIEWH;
        pos = (u32)((H - VIEWH) >> 1) * step + (step >> 1);
    }
    else
    {
        n = H;
        dst += ((VIEWH - H) >> 1) << 2;
        pos = step >> 1;
    }
    {
        register u32 i = pos >> 16, f = pos & 0xFFFF;
        register u32 si = step >> 16, sf = step & 0xFFFF;
        while (n >= 8)
        {
            __asm__ volatile(
                "move.b (%[t],%[i].w),0(%[d])\n\t add.w %[sf],%[f]\n\t addx.w %[si],%[i]\n\t"
                "move.b (%[t],%[i].w),4(%[d])\n\t add.w %[sf],%[f]\n\t addx.w %[si],%[i]\n\t"
                "move.b (%[t],%[i].w),8(%[d])\n\t add.w %[sf],%[f]\n\t addx.w %[si],%[i]\n\t"
                "move.b (%[t],%[i].w),12(%[d])\n\t add.w %[sf],%[f]\n\t addx.w %[si],%[i]\n\t"
                "move.b (%[t],%[i].w),16(%[d])\n\t add.w %[sf],%[f]\n\t addx.w %[si],%[i]\n\t"
                "move.b (%[t],%[i].w),20(%[d])\n\t add.w %[sf],%[f]\n\t addx.w %[si],%[i]\n\t"
                "move.b (%[t],%[i].w),24(%[d])\n\t add.w %[sf],%[f]\n\t addx.w %[si],%[i]\n\t"
                "move.b (%[t],%[i].w),28(%[d])\n\t add.w %[sf],%[f]\n\t addx.w %[si],%[i]\n\t"
                : [i] "+d"(i), [f] "+d"(f)
                : [t] "a"(tex), [d] "a"(dst), [si] "d"(si), [sf] "d"(sf)
                : "memory", "cc");
            dst += 32;
            n -= 8;
        }
        while (n--)
        {
            *dst = tex[i & 0xFFFF];
            dst += 4;
            f += sf;
            i += si + (f >> 16);
            f &= 0xFFFF;
        }
    }
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
        v->tpos = (u32)(u16)(2 * c + 1 - xl) * v->step;
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
        r0 = top + ((y0 * H + 63) >> 6);
        r1 = top + ((y1 * H + 63) >> 6);
        {
            const u8 *pix = p + 2 - y0;
            p += 2 + (y1 - y0);
            if (r0 < 0) r0 = 0;
            if (r1 > VIEWH) r1 = VIEWH;
            if (r0 >= r1) continue;
            tp = (u32)(u16)(r0 - top) * v->step;
            d = col + (r0 << 2);
            r1 -= r0;
            while (r1--)
            {
                *d = pix[tp >> 16];
                d += 4;
                tp += v->step;
            }
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

    for (sp = statobjlist; sp != laststatobj; sp++)
    {
        if (sp->shapenum == -1) continue;
        if (!spotvis[sp->tilex][sp->tiley]) continue;
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
        if (*visspot
            || (*(visspot - 1) && !*(tilespot - 1))
            || (*(visspot + 1) && !*(tilespot + 1))
            || (*(visspot - 65) && !*(tilespot - 65))
            || (*(visspot - 64) && !*(tilespot - 64))
            || (*(visspot - 63) && !*(tilespot - 63))
            || (*(visspot + 65) && !*(tilespot + 65))
            || (*(visspot + 64) && !*(tilespot + 64))
            || (*(visspot + 63) && !*(tilespot + 63)))
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

static u32 colseq[VIEWC];

static void render_columns(void)
{
    s16 k, r, i;
    for (k = 0; k < VIEWC; k++)
    {
        u8 *base = screenbuf + k * VIEW_COLBYTES;
        vid_wait_job(colseq[k]);       /* previous frame's upload of this column */
        memset(base, 0x11, (VIEWH / 2) * 4);
        memset(base + (VIEWH / 2) * 4, 0x00, (VIEWH / 2) * 4);
        for (r = 4 * k; r < 4 * k + 4; r++)
            draw_wall(r, rc_tex[r], wallheight[r] >> 2);
        for (i = 0; i < numvis; i++)
        {
            vis_t *v = vorder[i];
            s16 a, b;
            if (v->rc1 < 4 * k || v->rc0 > 4 * k + 3) continue;
            a = v->rc0 > 4 * k ? v->rc0 : 4 * k;
            b = v->rc1 < 4 * k + 3 ? v->rc1 : 4 * k + 3;
            for (r = a; r <= b; r++)
            {
                if (!v->nocull && wallheight[r] >= v->viewheight)
                    continue;
                sprite_col(v, r);
            }
        }
        colseq[k] = vid_queue(base, (vid_target ? TILE_VIEW1 : TILE_VIEW0) * 32 + k * VIEW_COLBYTES, VIEW_COLBYTES);
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
    viewx = player->x - FixedByFrac(FOCALLENGTH, viewcos);
    viewy = player->y + FixedByFrac(FOCALLENGTH, viewsin);
    focaltx = viewx >> TILESHIFT;
    focalty = viewy >> TILESHIFT;

    memset(spotvis, 0, sizeof(spotvis));
    spotvis[player->tilex][player->tiley] = 1;
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
void ThreeDRefresh(void)
{
    dbg_frames++;
    viewangle = player->angle;
    if (gamestate.weapon >= 0 && gamestate.weapon <= wp_chaingun)
        weapon_spr = weaponscale[gamestate.weapon] + gamestate.weaponframe;
    else
        weapon_spr = -1;
    R_Frame();
    vid_flush(&statsurf);
    fizzlein = FALSE;
}
