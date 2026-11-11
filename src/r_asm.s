// r_asm.s - hand written 68000 inner loops of the 3-D view (see render.c)
//
// GCC's 68000 code for these loops spent most of its time shuffling values between the
// stack and registers; they are small enough to write by hand.  Every routine draws exactly
// what the C code it replaced drew (tools/vtbench.py cmp).

#include "r_asm.h"

    .text

// ---------------------------------------------------------------------------------------
// sd_draw: a0 = sprite (vis_t), d0 = a, d1 = b
//
// Draws the ray columns a..b (all inside one 8 pixel tile column) of sprite v, skipping
// rays whose wall is nearer than the sprite.
//
//   a0 post data            a1 destination       a2 row table of this height
//   a3 screen column - 516  a4 texels of a post  a5/a6 first / last row written (spr_top/bot)
//   d2 texel fraction       d3 texel index       d5/d6 texel step (integer / fraction)
//   d7 row of the sprite's top
// ---------------------------------------------------------------------------------------

    .equ    L_TPOS, 0
    .equ    L_STEP2, 4
    .equ    L_WH, 8
    .equ    L_COLOFS, 12
    .equ    L_DATA, 16
    .equ    L_VH, 20
    .equ    L_NOCULL, 22
    .equ    L_LEFT, 24
    .equ    L_RIGHT, 26
    .equ    L_CNT, 28
    .equ    L_IS2X, 30
    .equ    L_SIZE, 32

// \p = label prefix; \lo = 1 for the low detail version: only the even rays are drawn, each
// two bytes (4 pixels) wide
    .macro  SD_DRAW p, lo
    .if \lo
    .set    SD_ROWSIZE, 16
    .set    SD_ROW2SIZE, 18
    .else
    .set    SD_ROWSIZE, 10
    .set    SD_ROW2SIZE, 10
    .endif
\p\()_draw:                            // a0 = v, d0 = a, d1 = b; uses every register
    lea     -L_SIZE(sp),sp
    .if \lo
    addq.w  #1,d0                   // only the even rays exist
    and.w   #-2,d0
    cmp.w   d0,d1
    blt.w   \p\()_ret
    .endif
    sub.w   d0,d1
    .if \lo
    lsr.w   #1,d1
    .endif
    move.w  d1,L_CNT(sp)            // rays - 1

    move.l  V_STEP(a0),d6
    move.l  d6,d5
    swap    d5                      // d5.w = integer part of the step, d6.w = fraction
    // texel column position of ray a: (2a + 1 - xl) * step
    move.w  d0,d2
    add.w   d2,d2
    addq.w  #1,d2
    sub.w   V_XL(a0),d2
    move.w  d2,d3
    mulu.w  d6,d3
    tst.w   d5
    beq.s   1f
    mulu.w  d5,d2
    swap    d2
    clr.w   d2
    add.l   d2,d3
1:  move.l  d3,L_TPOS(sp)
    move.l  d6,d2
    add.l   d2,d2
    .if \lo
    add.l   d2,d2
    .endif
    move.l  d2,L_STEP2(sp)          // texel columns from one ray to the next

    lea     wallheight,a1
    move.w  d0,d2
    add.w   d2,d2
    adda.w  d2,a1
    move.l  a1,L_WH(sp)
    // screen column of ray a, biased by -516: the row blocks address row r as 4r+4(a1), which keeps every
    // displacement non-zero and so every row the same size
    move.w  d0,d2
    and.w   #3,d2
    lsr.w   #2,d0
    lsl.w   #8,d0
    add.w   d0,d0                   // tile column * 512
    lea     screenbuf-516,a3
    adda.w  d0,a3
    adda.w  d2,a3

    move.l  V_COLOFS(a0),L_COLOFS(sp)
    move.l  V_DATA(a0),L_DATA(sp)
    move.w  V_VIEWHEIGHT(a0),L_VH(sp)
    move.w  V_NOCULL(a0),L_NOCULL(sp)
    move.w  V_LEFT(a0),L_LEFT(sp)
    move.w  V_RIGHT(a0),L_RIGHT(sp)
    move.w  V_TOP(a0),d7
    move.l  V_ROWTAB(a0),a2
    cmp.l   #0x8000,d6
    seq     L_IS2X(sp)
    movea.w spr_top,a5
    movea.w spr_bot,a6

\p\()_ray:
    move.l  L_WH(sp),a1
    move.w  (a1)+,d0
    .if \lo
    addq.l  #2,a1
    .endif
    move.l  a1,L_WH(sp)
    tst.w   L_NOCULL(sp)
    bne.s   1f
    cmp.w   L_VH(sp),d0
    bhs.w   \p\()_next                 // the wall of this ray is nearer
1:  move.w  L_TPOS(sp),d0           // texel column
    cmp.w   L_LEFT(sp),d0
    blt.w   \p\()_next
    cmp.w   L_RIGHT(sp),d0
    bgt.w   \p\()_next
    move.l  L_COLOFS(sp),a0
    add.w   d0,d0
    moveq   #0,d1
    move.w  (a0,d0.w),d1
    move.l  L_DATA(sp),a0
    adda.l  d1,a0
    tst.b   L_IS2X(sp)
    bne.w   \p\()_post2

// ---- posts of one column, any scale
\p\()_post:
    moveq   #0,d0
    move.b  (a0)+,d0                // first texel row of the post
    cmp.b   #0xFF,d0
    beq.w   \p\()_next
    moveq   #0,d1
    move.b  (a0)+,d1                // texel row after its last
    movea.l a0,a4
    suba.w  d0,a4                   // a4[texel row] = pixel
    suba.w  d0,a0
    adda.w  d1,a0                   // next post
    add.w   d0,d0
    move.w  (a2,d0.w),d0
    add.w   d1,d1
    move.w  (a2,d1.w),d1
    add.w   d7,d0                   // first screen row
    bpl.s   1f
    moveq   #0,d0
1:  add.w   d7,d1                   // screen row after the last
    cmp.w   #R_VIEWH,d1
    ble.s   2f
    move.w  #R_VIEWH,d1
2:  cmp.w   d1,d0
    bge.s   \p\()_post
    cmpa.w  d0,a5
    ble.s   3f
    movea.w d0,a5
3:  cmpa.w  d1,a6
    bge.s   4f
    movea.w d1,a6
4:  // texel position of the first row: (row - top) * step, 16.16
    move.w  d0,d3
    sub.w   d7,d3
    move.w  d3,d2
    mulu.w  d6,d2
    tst.w   d5
    beq.s   5f
    mulu.w  d5,d3
    swap    d2
    add.w   d2,d3
    swap    d2
    bra.s   6f
5:  swap    d2
    move.w  d2,d3
    swap    d2
6:  // enter the row block so that it ends on the post's last row
    move.w  d1,d4
    lsl.w   #2,d4
    lea     (a3,d4.w),a1
    sub.w   d0,d1                   // rows
    add.w   d1,d1
    move.w  \p\()_jtab(pc,d1.w),d4
    jmp     \p\()_jtab(pc,d4.w)
\p\()_jtab:
    .set    n, 0
    .rept   R_VIEWH + 1
    .word   \p\()_rows_end - \p\()_jtab - SD_ROWSIZE * n
    .set    n, n + 1
    .endr

    .set    n, 0
    .rept   R_VIEWH
    .if \lo
    move.b  (a4,d3.w),d0
    move.b  d0,4*n+4(a1)
    move.b  d0,4*n+5(a1)
    .else
    move.b  (a4,d3.w),4*n+4(a1)
    .endif
    add.w   d6,d2
    addx.w  d5,d3
    .set    n, n + 1
    .endr
\p\()_rows_end:
    bra.w   \p\()_post

// ---- posts of one column at exactly 2x: every texel covers two rows,
//      the sprite starts on row 0 and is never clipped
\p\()_post2:
    moveq   #0,d0
    move.b  (a0)+,d0
    cmp.b   #0xFF,d0
    beq.w   \p\()_next
    moveq   #0,d1
    move.b  (a0)+,d1
    sub.w   d0,d1                   // texels
    ble.s   \p\()_post2
    add.w   d0,d0                   // first screen row
    cmpa.w  d0,a5
    ble.s   1f
    movea.w d0,a5
1:  move.w  d1,d4
    add.w   d4,d4
    add.w   d0,d4                   // screen row after the last
    cmpa.w  d4,a6
    bge.s   2f
    movea.w d4,a6
2:  lsl.w   #2,d4
    lea     (a3,d4.w),a1
    add.w   d1,d1
    move.w  \p\()_jtab2(pc,d1.w),d4
    jmp     \p\()_jtab2(pc,d4.w)
\p\()_jtab2:
    .set    n, 0
    .rept   R_VIEWH / 2 + 1
    .word   \p\()_rows2_end - \p\()_jtab2 - SD_ROW2SIZE * n
    .set    n, n + 1
    .endr

    .set    n, 0
    .rept   R_VIEWH / 2
    move.b  (a0)+,d0
    move.b  d0,8*n+4(a1)
    move.b  d0,8*n+8(a1)
    .if \lo
    move.b  d0,8*n+5(a1)
    move.b  d0,8*n+9(a1)
    .endif
    .set    n, n + 1
    .endr
\p\()_rows2_end:
    bra.w   \p\()_post2

\p\()_next:
    move.l  L_STEP2(sp),d0
    add.l   d0,L_TPOS(sp)
    addq.l  #1+\lo,a3
    subq.w  #1,L_CNT(sp)
    bpl.w   \p\()_ray

    move.w  a5,spr_top
    move.w  a6,spr_bot
\p\()_ret:
    lea     L_SIZE(sp),sp
    rts
    .endm

    SD_DRAW sd, 0
    SD_DRAW sdl, 1

// ---------------------------------------------------------------------------------------
// Per ray results of the ray caster (render.c); kept together so one pointer reaches all.
// ---------------------------------------------------------------------------------------
    .section .bss
    .balign 2
    .globl  wallheight
    .globl  rc_tex
    .globl  rc_face
wallheight: .space  2 * R_NRAYS     // u16: projected wall height, 4 * pixels
rc_tex:     .space  4 * R_NRAYS     // texture column of the wall
rc_face:    .space  2 * R_NRAYS     // which wall face the ray hit (render.c)

    .text

// ---------------------------------------------------------------------------------------
// void rc_columns(u8 *vtop, u16 vram)
//
// Draws the view one 8 pixel tile column at a time and queues each finished column for
// upload (render.c: render_columns explains the bookkeeping).  vtop = bg_vtop[bank],
// vram = VRAM address of the view buffer being drawn.
//
//   a3 scaler_tab     a4 rc_tex[4k]       a5 wallheight[4k]   a6 screen column
//   d2 st  d3 en      rows [0, st) get ceiling, [en, VIEWH) floor before the walls go in
//   d4 wt  d5 wb      rows [wt, wb) are not plain background when the column is done
//   d7 = VIEWH
// ---------------------------------------------------------------------------------------

    .equ    RL_VTOP, 0
    .equ    RL_VRAM, 4
    .equ    RL_K, 6
    .equ    RL_WT, 8
    .equ    RL_WB, 10
    .equ    RL_J, 12
    .equ    RL_M, 14
    .equ    RL_REGS, 16
    .equ    RL_PSRC, 28
    .equ    RL_PEND, 32
    .equ    RL_PCNT, 36
    .equ    RL_TEXK, 38
    .equ    RL_SIZE, 42
    .equ    RARGS, RL_SIZE + 44 + 4

// rows covered by one ray: update d2-d5
    .macro  RC_EXTENT
    move.w  (a5)+,d0
    lsr.w   #2,d0                   // wall height in pixels
    cmp.w   d7,d0
    bhs.s   8f
    move.w  d7,d1
    sub.w   d0,d1
    lsr.w   #1,d1                   // first row
    add.w   d1,d0                   // row after the last
    cmp.w   d4,d1
    bhs.s   1f
    move.w  d1,d4
1:  cmp.w   d5,d0
    bls.s   2f
    move.w  d0,d5
2:  cmp.w   d2,d1
    bls.s   3f
    move.w  d1,d2
3:  cmp.w   d3,d0
    bhs.s   9f
    move.w  d0,d3
    bra.s   9f
8:  moveq   #0,d4                   // fills the whole column
    move.w  d7,d5
9:
    .endm

// one ray of wall: run the compiled scaler for its height
    .macro  RC_WALL r
    movea.l (a4)+,a0
    move.w  (a5)+,d0
    lsr.w   #2,d0
    beq.s   9f
    cmp.w   #512,d0
    bls.s   1f
    move.w  #512,d0
1:  lsl.w   #3,d0
    lea     \r(a6),a1
    adda.w  4(a3,d0.w),a1
    movea.l (a3,d0.w),a2
    jsr     (a2)
9:
    .endm

// low detail: one ray of wall, 2 bytes wide, from the textures with doubled texels
    .macro  RC_WALL2 r
    move.l  (a4)+,d6
    addq.l  #4,a4                   // (the odd rays are not used)
    move.w  (a5)+,d0
    addq.l  #2,a5
    lsr.w   #2,d0
    beq.s   9f
    cmp.w   #512,d0
    bls.s   1f
    move.w  #512,d0
1:  lsl.w   #3,d0
    add.l   d6,d6
    add.l   RL_TEXK(sp),d6
    movea.l d6,a0
    lea     \r(a6),a1
    adda.w  4(a3,d0.w),a1
    movea.l (a3,d0.w),a2
    jsr     (a2)
9:
    .endm

// append the pending upload RL_PSRC..RL_PEND to the upload ring (vid.c).  Uses d0-d3, a0, a1.
    .macro  RC_FLUSH
3:  move.w  vid_jhead,d2
    move.w  d2,d3
    add.w   #R_JOBSIZE,d3
    cmp.w   #R_JOBRING,d3
    bne.s   4f
    moveq   #0,d3
4:  cmp.w   vid_jtail,d3
    bne.s   5f
    jsr     vid_frame               // ring full
    bra.s   3b
5:  lea     vid_jobs,a0
    adda.w  d2,a0
    move.l  RL_PSRC(sp),d0
    move.l  d0,(a0)+
    move.l  RL_PEND(sp),d1
    sub.l   d0,d1
    sub.l   #screenbuf,d0
    add.w   RL_VRAM(sp),d0
    move.w  d0,(a0)+
    move.w  d1,(a0)+
    clr.w   (a0)
    move.w  d3,vid_jhead
    addq.l  #1,vid_jqueued
    clr.w   RL_PCNT(sp)
    .endm

    // d1 longwords of d0 at (a1)+, 1 <= d1 <= VIEWH
    .rept   R_VIEWH
    move.l  d0,(a1)+
    .endr
rc_fill_end:
    rts
rc_fill:
    add.w   d1,d1
    neg.w   d1
    jmp     rc_fill_end(pc,d1.w)

// \p = label prefix, \sd = prefix of the sprite drawer to use, \lo = 1 for low detail: two rays
// per tile column, each drawn 4 pixels wide by the word scalers
    .macro  RC_COLUMNS p, sd, lo
    .globl  \p\()_columns
\p\()_columns:
    movem.l d2-d7/a2-a6,-(sp)
    lea     -RL_SIZE(sp),sp
    move.l  RARGS(sp),RL_VTOP(sp)
    move.w  RARGS+6(sp),RL_VRAM(sp)
    clr.w   RL_K(sp)
    clr.w   RL_PCNT(sp)
    lea     screenbuf,a6
    lea     wallheight,a5
    lea     rc_tex,a4
    .if \lo
    lea     scaler_tab2,a3
    move.l  #wall_data,d0           // texel t of the doubled textures - 2 * (texel t of the plain ones)
    add.l   d0,d0
    move.l  #wall_data2,d1
    sub.l   d0,d1
    move.l  d1,RL_TEXK(sp)
    .else
    lea     scaler_tab,a3
    .endif
    move.w  #R_VIEWH,d7

\p\()_col:
    // the previous frame's upload of this column must be done before it is redrawn
    move.w  RL_K(sp),d0
    add.w   d0,d0
    add.w   d0,d0
    lea     colseq,a0
    move.l  (a0,d0.w),d6
1:  cmp.l   vid_jdone,d6
    bls.s   2f
    jsr     vid_frame
    bra.s   1b
2:
    moveq   #0,d2
    move.w  d7,d3
    moveq   #R_VIEWH / 2,d4
    moveq   #R_VIEWH / 2,d5
    .if \lo
    RC_EXTENT
    addq.l  #2,a5
    RC_EXTENT
    addq.l  #2,a5
    .else
    RC_EXTENT
    RC_EXTENT
    RC_EXTENT
    RC_EXTENT
    .endif

    // ceiling and floor, where the column does not already hold them
    lea     screenbuf+R_BG_TABLES,a0
    adda.w  RL_K(sp),a0
    moveq   #0,d0
    move.b  (a0),d0                 // bg_ctop[k]
    move.w  d2,d1
    sub.w   d0,d1
    bls.s   1f
    lsl.w   #2,d0
    lea     (a6,d0.w),a1
    move.l  #0x11111111,d0
    bsr.w   rc_fill
    lea     screenbuf+R_BG_TABLES,a0
    adda.w  RL_K(sp),a0
1:  moveq   #0,d1
    move.b  R_VIEWC(a0),d1          // bg_cbot[k]
    sub.w   d3,d1
    bls.s   2f
    lsl.w   #2,d3
    lea     (a6,d3.w),a1
    moveq   #0,d0
    bsr.w   rc_fill
2:
    subq.l  #8,a5
    .if \lo
    RC_WALL2 0
    RC_WALL2 2
    .else
    RC_WALL 0
    RC_WALL 1
    RC_WALL 2
    RC_WALL 3
    .endif

    tst.w   numvis
    beq.w   3f
    // ---- sprites of this tile column, far to near.  The active list (indices into
    // vorder, ascending = far to near) is swept left to right: add the sprites that start
    // in this column (bucketed by render_columns), draw, drop the ones that end here.
    move.w  d4,RL_WT(sp)
    move.w  d5,RL_WB(sp)
    movem.l a4-a6,RL_REGS(sp)
    move.w  d7,spr_top
    clr.w   spr_bot
    lea     screenbuf+R_SPR_SWEEP,a0    // bfirst[VIEWC], bnext[MAXVIS], act[MAXVIS]
    move.w  RL_K(sp),d0
    moveq   #0,d1
    move.b  (a0,d0.w),d1
    lea     R_VIEWC+R_MAXVIS(a0),a1
    move.w  spr_nact,d2
    bra.s   \p\()_ins_test
\p\()_ins:
    move.w  d2,d3
    addq.w  #1,d2
1:  tst.w   d3
    beq.s   2f
    move.b  -1(a1,d3.w),d0
    cmp.b   d1,d0
    bls.s   2f
    move.b  d0,(a1,d3.w)
    subq.w  #1,d3
    bra.s   1b
2:  move.b  d1,(a1,d3.w)
    move.b  R_VIEWC(a0,d1.w),d1
\p\()_ins_test:
    cmp.b   #0xFF,d1
    bne.s   \p\()_ins
    move.w  d2,spr_nact
    clr.w   RL_J(sp)
    clr.w   RL_M(sp)
    bra.s   \p\()_spr_test
\p\()_spr:
    lea     screenbuf+R_SPR_SWEEP+R_VIEWC+R_MAXVIS,a1
    moveq   #0,d3
    move.b  (a1,d0.w),d3
    addq.w  #1,RL_J(sp)
    move.w  d3,d0
    lsl.w   #2,d0
    lea     vorder,a0
    movea.l (a0,d0.w),a0
    move.w  RL_K(sp),d0
    lsl.w   #2,d0                   // first and last ray of the tile column
    move.w  d0,d1
    addq.w  #3,d1
    cmp.w   V_RC1(a0),d1
    bge.s   1f
    move.w  RL_M(sp),d2             // goes on into the next column
    move.b  d3,(a1,d2.w)
    addq.w  #1,RL_M(sp)
    bra.s   2f
1:  move.w  V_RC1(a0),d1
2:  cmp.w   V_RC0(a0),d0
    bge.s   4f
    move.w  V_RC0(a0),d0
4:  bsr.w   \sd\()_draw
\p\()_spr_test:
    move.w  RL_J(sp),d0
    cmp.w   spr_nact,d0
    blo.s   \p\()_spr
    move.w  RL_M(sp),spr_nact
    movem.l RL_REGS(sp),a4-a6
    .if \lo
    lea     scaler_tab2,a3
    .else
    lea     scaler_tab,a3
    .endif
    move.w  #R_VIEWH,d7
    move.w  RL_WT(sp),d4
    move.w  RL_WB(sp),d5
    move.w  spr_top,d0
    cmp.w   d4,d0
    bhs.s   1f
    move.w  d0,d4
1:  move.w  spr_bot,d0
    cmp.w   d5,d0
    bls.s   3f
    move.w  d0,d5
3:
    lea     screenbuf+R_BG_TABLES,a0
    adda.w  RL_K(sp),a0
    move.b  d4,(a0)                 // bg_ctop[k], bg_cbot[k]
    move.b  d5,R_VIEWC(a0)

    // upload only the tile rows that differ from what this VRAM bank already holds
    lsr.w   #3,d4
    addq.w  #7,d5
    lsr.w   #3,d5
    movea.l RL_VTOP(sp),a0
    adda.w  RL_K(sp),a0
    moveq   #0,d0
    move.b  (a0),d0                 // bg_vtop[bank][k]
    cmp.w   d4,d0
    bls.s   1f
    move.w  d4,d0
1:  moveq   #0,d1
    move.b  2*R_VIEWC(a0),d1        // bg_vbot[bank][k]
    cmp.w   d5,d1
    bhs.s   2f
    move.w  d5,d1
2:  move.b  d4,(a0)
    move.b  d5,2*R_VIEWC(a0)
    sub.w   d0,d1
    bls.w   \p\()_nextcol
    lsl.w   #5,d0
    lsl.w   #5,d1
    lea     (a6,d0.w),a0            // first byte to upload
    lea     (a0,d1.w),a1            // and the byte after the last
    // Tile columns are adjacent in RAM and in VRAM alike: a column that starts within
    // R_JOBGAP bytes of the previous upload extends that job instead of costing the V-int
    // another one.  The bytes in between are plain background on both sides anyway.  At
    // most R_JOBCOLS columns share a job, so uploading still overlaps the drawing.
    tst.w   RL_PCNT(sp)
    beq.w   \p\()_newjob
    cmp.w   #R_JOBCOLS,RL_PCNT(sp)
    bhs.s   \p\()_flushjob
    move.l  a0,d0
    sub.l   RL_PEND(sp),d0
    cmp.l   #R_JOBGAP,d0
    bhi.s   \p\()_flushjob
    move.l  a1,RL_PEND(sp)
    addq.w  #1,RL_PCNT(sp)
    bra.s   \p\()_setseq
\p\()_flushjob:
    move.l  a0,d4
    move.l  a1,d5
    RC_FLUSH
    movea.l d4,a0
    movea.l d5,a1
\p\()_newjob:
    move.l  a0,RL_PSRC(sp)
    move.l  a1,RL_PEND(sp)
    move.w  #1,RL_PCNT(sp)
\p\()_setseq:
    move.l  vid_jqueued,d1          // the job this column will go out with
    addq.l  #1,d1
    move.w  RL_K(sp),d0
    add.w   d0,d0
    add.w   d0,d0
    lea     colseq,a0
    move.l  d1,(a0,d0.w)

\p\()_nextcol:
    lea     R_COLBYTES(a6),a6
    addq.w  #1,RL_K(sp)
    cmp.w   #R_VIEWC,RL_K(sp)
    bne.w   \p\()_col

    tst.w   RL_PCNT(sp)
    beq.s   1f
    RC_FLUSH
1:
    lea     RL_SIZE(sp),sp
    movem.l (sp)+,d2-d7/a2-a6
    rts
    .endm

    RC_COLUMNS rc, sd, 0
    RC_COLUMNS rcl, sdl, 1

// ---------------------------------------------------------------------------------------
// void rc_cast(void)
//
// The ray caster (WL_DRAW.C: AsmRefresh).  Rays 0,4,8.. are cast in full; the others are
// interpolated wherever the rays around them hit the same wall face, and cast otherwise
// (render.c explains why that is exact).  Results go to wallheight / rc_tex / rc_face.
//
// The grid walk exists once per quadrant so that every direction test is a constant:
//   d0 xint   d1 yint     16.16 intersection with the next horizontal / vertical grid line
//   d2 xtile  d5 ytile    grid line being tested
//   d4 xtile << 6
//   d6 xspot  d3 yspot    tilemap index of the tile behind that line (clean upper word)
//   d7 visibility stamp
//   a0 tilemap  a1 spotvis  a2 xstep  a3 ystep  a4 64 * x direction
// A ray that hits a sliding push wall is rare: it is handed to the C version, cast_ray_c.
// ---------------------------------------------------------------------------------------

    .section .bss
    .balign 2
rc_pix:     .space  2               // ray being cast, * 2
    .text

// d0 = dx, d1 = dy (wall - viewer, 1/512 tile): store the projected height of ray rc_pix
// (calc_height in render.c), leave rc_pix in d5.  Uses d0, d1, d5, a1.
    .macro  RC_HEIGHT
    muls.w  vc15,d0
    muls.w  vs15,d1
    sub.l   d1,d0
    swap    d0                      // distance, 8.8
    cmp.w   #R_MINDIST8,d0
    bge.s   1f
    moveq   #R_MINDIST8,d0
1:  moveq   #0,d1
    move.w  d0,d1
    add.l   d1,d1
    lea     rc_heighttab,a1
    move.w  (a1,d1.l),d0            // proj_heightnumerator / distance
    move.w  rc_pix,d5
    lea     wallheight,a1
    move.w  d0,(a1,d5.w)
    .endm

// finish a ray that hit a plain wall: d0/d1 = dx/dy, d2 = face id, d3 = texture column * 64,
// d6 = tilemap index of the wall, d7 = its tile, a4 = step back to the tile the ray came
// from, a5 = wall texture table of this orientation
rc_tail:
    RC_HEIGHT
    lea     rc_face,a1
    move.w  d2,(a1,d5.w)
    btst    #6,d7
    beq.s   2f
    sub.w   a4,d6                   // next to a door: is the ray coming out of the doorway?
    tst.b   (a0,d6.l)
    bpl.s   2f
    movea.l 4*64(a5),a2             // then this is the door frame
    bra.s   3f
2:  and.w   #0x3F,d7
    lsl.w   #2,d7
    movea.l (a5,d7.w),a2
3:  adda.w  d3,a2
    lea     rc_tex,a1
    add.w   d5,d5
    move.l  a2,(a1,d5.w)
    rts

// finish a ray that hit a door: d0/d1 = dx/dy, d2 = 1 for a vertical door, d4 = texture
// column * 64, d7 = door number
rc_doortail:
    RC_HEIGHT
    lea     doorobjlist,a1
    move.w  d7,d3
    lsl.w   #3,d3
    move.b  3(a1,d3.w),d3           // lock
    moveq   #R_DOORWALL,d6
    tst.b   d3
    beq.s   2f
    moveq   #R_DOORWALL + 4,d6
    cmp.b   #5,d3                   // dr_elevator
    beq.s   2f
    moveq   #R_DOORWALL + 6,d6
2:  add.w   d2,d6
    lsl.w   #2,d6
    lea     wall_ofs,a1
    move.l  (a1,d6.w),d6
    add.l   #wall_data,d6
    movea.l d6,a2
    adda.w  d4,a2
    lea     rc_tex,a1
    add.w   d5,d5
    move.l  a2,(a1,d5.w)
    rts

rc_pushwall:
    moveq   #0,d0
    move.w  rc_pix,d0
    lsr.w   #1,d0
    move.l  d0,-(sp)
    jsr     cast_ray_c
    addq.l  #4,sp
    rts

    .macro  STEP1 xs, reg
    .if \xs > 0
    addq.w  #1,\reg
    .else
    subq.w  #1,\reg
    .endif
    .endm

// d0 = angle inside the quadrant (0..899)
    .macro  RC_QUAD name, xs, ys, t1first, ypart, xpart
\name:
    lea     finetangent,a5
    lsl.w   #2,d0
    move.w  #4*899,d1
    sub.w   d0,d1
    .if \t1first
    move.l  (a5,d0.w),d2            // tangent: x step per tile of y
    move.l  (a5,d1.w),d3            // cotangent: y step per tile of x
    .else
    move.l  (a5,d1.w),d2
    move.l  (a5,d0.w),d3
    .endif
    // xint = viewx +- xstep * (distance to the first horizontal line)
    move.w  \ypart,d4
    move.w  d2,d0
    mulu.w  d4,d0
    clr.w   d0
    swap    d0
    move.l  d2,d5
    swap    d5
    mulu.w  d4,d5
    add.l   d5,d0
    .if \xs < 0
    neg.l   d0
    neg.l   d2
    .endif
    add.l   viewx,d0
    movea.l d2,a2
    // yint = viewy +- ystep * (distance to the first vertical line)
    move.w  \xpart,d4
    move.w  d3,d1
    mulu.w  d4,d1
    clr.w   d1
    swap    d1
    move.l  d3,d5
    swap    d5
    mulu.w  d4,d5
    add.l   d5,d1
    .if \ys < 0
    neg.l   d1
    neg.l   d3
    .endif
    add.l   viewy,d1
    movea.l d3,a3

    move.w  focaltx,d2
    STEP1   \xs, d2
    move.w  d2,d4
    lsl.w   #6,d4
    movea.w #64*\xs,a4
    move.w  focalty,d5
    STEP1   \ys, d5
    move.l  d0,d3
    clr.w   d3
    swap    d3
    lsl.w   #6,d3
    add.w   d5,d3                   // yspot
    lea     tilemap,a0
    lea     screenbuf+R_SPOTVIS,a1
    move.b  vis_stamp,d7
    bra.s   \name\()_vc

\name\()_he0:
    add.w   d4,d6                   // keep xspot valid
    bra.w   \name\()_he
    // ---- walk along x: vertical grid lines
\name\()_pv:
    move.b  d7,(a1,d6.l)            // seen, step to the next line
    STEP1   \xs, d2
    add.w   a4,d4
    add.l   a3,d1
\name\()_vc:
    move.l  d1,d6
    clr.w   d6
    swap    d6
    cmp.w   d5,d6                   // crossed the next horizontal line first?
    .if \ys < 0
    ble.s   \name\()_he0
    .else
    bge.s   \name\()_he0
    .endif
    add.w   d4,d6                   // xspot
\name\()_ve:
    tst.b   (a0,d6.l)
    beq.s   \name\()_pv

    // hit a vertical face
    moveq   #0,d7
    move.b  (a0,d6.l),d7
    bmi.s   \name\()_vspecial
    move.w  d1,d3
    lsr.w   #4,d3
    and.w   #0x0FC0,d3              // texture column * 64
    .if \xs < 0
    eor.w   #0x0FC0,d3
    addq.w  #1,d2                   // the east face of the tile
    .endif
    sub.l   viewy,d1
    asr.l   #7,d1
    swap    d2
    clr.w   d2
    sub.l   viewx,d2
    asr.l   #7,d2
    move.l  d2,d0
    move.w  d6,d2
    lea     rc_wallptr_v,a5
    bra.w   rc_tail

\name\()_vspecial:
    btst    #6,d7
    bne.w   rc_pushwall
    // a door, half a tile further on: does the ray stay in this tile and hit the leaf?
    movem.l d0-d6,-(sp)
    and.w   #0x7F,d7
    move.l  a3,d2
    asr.l   #1,d2
    add.l   d1,d2
    move.l  d2,d3
    eor.l   d1,d3
    swap    d3
    tst.w   d3
    bne.s   \name\()_vpass
    lea     doorposition,a5
    move.w  d7,d3
    add.w   d3,d3
    move.w  (a5,d3.w),d3
    cmp.w   d3,d2
    blo.s   \name\()_vpass
    move.w  d2,d4
    sub.w   d3,d4
    lsr.w   #4,d4
    and.w   #0x0FC0,d4
    move.l  d2,d1
    sub.l   viewy,d1
    asr.l   #7,d1
    move.w  10(sp),d0               // xtile
    swap    d0
    move.w  #0x8000,d0
    sub.l   viewx,d0
    asr.l   #7,d0
    lea     28(sp),sp
    moveq   #1,d2
    bra.w   rc_doortail
\name\()_vpass:
    movem.l (sp)+,d0-d6
    move.b  vis_stamp,d7
    bra.w   \name\()_pv

    // ---- walk along y: horizontal grid lines
\name\()_ve0:
    lsl.w   #6,d3
    add.w   d5,d3                   // keep yspot valid
    bra.w   \name\()_ve
\name\()_ph:
    move.b  d7,(a1,d3.l)
    STEP1   \ys, d5
    add.l   a2,d0
    move.l  d0,d3
    clr.w   d3
    swap    d3
    cmp.w   d2,d3                   // crossed the next vertical line first?
    .if \xs < 0
    ble.s   \name\()_ve0
    .else
    bge.s   \name\()_ve0
    .endif
    lsl.w   #6,d3
    add.w   d5,d3                   // yspot
\name\()_he:
    tst.b   (a0,d3.l)
    beq.s   \name\()_ph

    // hit a horizontal face
    moveq   #0,d7
    move.b  (a0,d3.l),d7
    bmi.s   \name\()_hspecial
    move.w  d0,d4
    lsr.w   #4,d4
    and.w   #0x0FC0,d4
    .if \ys > 0
    eor.w   #0x0FC0,d4
    .else
    addq.w  #1,d5                   // the south face of the tile
    .endif
    sub.l   viewx,d0
    asr.l   #7,d0
    swap    d5
    clr.w   d5
    sub.l   viewy,d5
    asr.l   #7,d5
    move.l  d5,d1
    move.w  d3,d2
    or.w    #0x8000,d2
    move.l  d3,d6
    move.w  d4,d3
    movea.w #\ys,a4
    lea     rc_wallptr_h,a5
    bra.w   rc_tail

\name\()_hspecial:
    btst    #6,d7
    bne.w   rc_pushwall
    movem.l d0-d6,-(sp)
    and.w   #0x7F,d7
    move.l  a2,d2
    asr.l   #1,d2
    add.l   d0,d2
    move.l  d2,d3
    eor.l   d0,d3
    swap    d3
    tst.w   d3
    bne.s   \name\()_hpass
    lea     doorposition,a5
    move.w  d7,d3
    add.w   d3,d3
    move.w  (a5,d3.w),d3
    cmp.w   d3,d2
    blo.s   \name\()_hpass
    move.w  d2,d4
    sub.w   d3,d4
    lsr.w   #4,d4
    and.w   #0x0FC0,d4
    move.l  d2,d0
    sub.l   viewx,d0
    asr.l   #7,d0
    move.w  22(sp),d1               // ytile
    swap    d1
    move.w  #0x8000,d1
    sub.l   viewy,d1
    asr.l   #7,d1
    lea     28(sp),sp
    moveq   #0,d2
    bra.w   rc_doortail
\name\()_hpass:
    movem.l (sp)+,d0-d6
    move.b  vis_stamp,d7
    bra.w   \name\()_ph
    .endm

// cast ray rc_pix
rc_ray:
    move.w  rc_pix,d1
    lea     rc_face,a0
    move.w  #-1,(a0,d1.w)
    lea     pixelangle,a0
    move.w  (a0,d1.w),d0
    add.w   c_midangle,d0
    bpl.s   1f
    add.w   #3600,d0
    bra.s   2f
1:  cmp.w   #3600,d0
    blt.s   2f
    sub.w   #3600,d0
2:  cmp.w   #900,d0
    blt.w   rc_q0
    sub.w   #900,d0
    cmp.w   #900,d0
    blt.w   rc_q1
    sub.w   #900,d0
    cmp.w   #900,d0
    blt.w   rc_q2
    sub.w   #900,d0
    bra.w   rc_q3

    RC_QUAD rc_q0,  1, -1, 0, c_ypd, c_xpu
    RC_QUAD rc_q1, -1, -1, 1, c_ypd, c_xpd
    RC_QUAD rc_q2, -1,  1, 0, c_ypu, c_xpd
    RC_QUAD rc_q3,  1,  1, 1, c_ypu, c_xpu

// One ray from the rays s2 / 2 to its left and right, or cast it.
// a4 / a5 / a6 = its entry in rc_face / wallheight / rc_tex.  Heights are at most 2048
// (rc_heighttab), so 16 bit sums cannot overflow.
    .macro  RC_INTERP s2
    move.w  -\s2(a4),d1
    cmp.w   #-1,d1
    beq.w   8f
    cmp.w   \s2(a4),d1
    bne.w   8f
    move.w  d1,(a4)                 // same wall face on both sides
    move.w  -\s2(a5),d2
    move.w  \s2(a5),d3
    add.w   d3,d2                   // h1 + h2
    move.w  d2,d1
    lsr.w   #1,d1
    move.w  d1,(a5)
    movea.l -2*\s2(a6),a1
    move.w  2*\s2+2(a6),d1
    sub.w   a1,d1                   // 64 * texture columns between the two
    beq.s   3f
    // columns from the left ray = dc * h2 / (h1 + h2), rounded towards zero.  dc is
    // mostly tiny: count the quotient up instead of paying for muls + divs
    move.w  d1,d4
    bpl.s   4f
    neg.w   d4
4:  cmp.w   #4*64,d4
    bhi.s   7f
    moveq   #0,d5
5:  add.w   d3,d5
    sub.w   #64,d4
    bne.s   5b
6:  sub.w   d2,d5
    blo.s   6f
    add.w   #64,d4
    bra.s   6b
6:  tst.w   d1
    bpl.s   6f
    neg.w   d4
6:  adda.w  d4,a1
    bra.s   3f
7:  asr.w   #6,d1
    muls.w  d3,d1
    divs.w  d2,d1
    lsl.w   #6,d1
    adda.w  d1,a1
3:  move.l  a1,(a6)
    bra.s   9f
8:  move.l  a4,d0
    sub.l   #rc_face,d0
    move.w  d0,rc_pix
    movem.l d7/a4-a6,-(sp)
    bsr.w   rc_ray
    movem.l (sp)+,d7/a4-a6
9:
    .endm

    .globl  rc_cast
rc_cast:
    movem.l d2-d7/a2-a6,-(sp)
    moveq   #0,d0
1:  move.w  d0,rc_pix
    bsr.w   rc_ray
    move.w  rc_pix,d0
    addq.w  #8,d0
    cmp.w   #2*R_NRAYS,d0
    blo.s   1b
    move.w  #2*R_NRAYS-4,rc_pix
    bsr.w   rc_ray
    move.w  #2*R_NRAYS-2,rc_pix
    bsr.w   rc_ray
    lea     rc_face+2*2,a4          // rays 2, 6, .. 122 from the rays 2 away
    lea     wallheight+2*2,a5
    lea     rc_tex+4*2,a6
    moveq   #(R_NRAYS-8)/4,d7
rc_cast2:
    RC_INTERP 4
    addq.l  #8,a4
    addq.l  #8,a5
    lea     16(a6),a6
    dbra    d7,rc_cast2
    tst.b   r_lowdetail             // low detail draws the even rays only
    bne.w   rc_cast_done
    lea     rc_face+2,a4            // rays 1, 3, .. 125 from their neighbours
    lea     wallheight+2,a5
    lea     rc_tex+4,a6
    moveq   #(R_NRAYS-4)/2,d7
rc_cast3:
    RC_INTERP 2
    addq.l  #4,a4
    addq.l  #4,a5
    addq.l  #8,a6
    dbra    d7,rc_cast3
rc_cast_done:
    movem.l (sp)+,d2-d7/a2-a6
    rts
