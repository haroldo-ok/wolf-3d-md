/* r_asm.h - layout shared by render.c and the 68000 routines in r_asm.s */
#ifndef R_ASM_H
#define R_ASM_H

#define R_VIEWH         128     /* = VIEWH */
#define R_VIEWC         32      /* = VIEWC, 8 pixel tile columns of the view */
#define R_NRAYS         128     /* 2 pixel wide rays */
#define R_COLBYTES      512     /* = VIEW_COLBYTES */
#define R_BG_TABLES     26880   /* offset of render.c's background tables in screenbuf */
#define R_SPOTVIS       22784   /* offset of spotvis in screenbuf (wl_def.h) */
#define R_MINDIST8      0x58    /* = MINDIST >> 8 */
#define R_DOORWALL      98      /* = DOORWALL */
#define R_SPR_SWEEP     28160   /* offset of the sprite sweep scratch in screenbuf (render.c) */
#define R_MAXVIS        64      /* = MAXVIS */
#define R_JOBSIZE       10      /* = VID_JOBSIZE */
#ifndef R_JOBCOLS
#define R_JOBCOLS       4       /* tile columns that may share one upload job */
#endif
#ifndef R_JOBGAP
#define R_JOBGAP        512     /* bytes of unchanged background worth uploading to merge two jobs */
#endif
#define R_JOBRING       640     /* = VID_MAXJOBS * VID_JOBSIZE */

/* vis_t (render.c) */
#define V_VIEWHEIGHT    0
#define V_NOCULL        2
#define V_XL            4
#define V_TOP           6
#define V_STEP          8
#define V_ROWTAB        12
#define V_COLOFS        16
#define V_DATA          20
#define V_LEFT          24
#define V_RIGHT         26
#define V_H             28
#define V_RC0           30
#define V_RC1           32
#define V_SIZE          34

#endif
