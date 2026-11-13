/* snd.c - sound effects (Z80 4-channel PCM driver) and music (YM2612 + SN76489 streams) */
#include <genesis.h>
#include "wl_def.h"
#include "snd.h"
#include "vid.h"
#include "gen/audio.h"

SDSMode DigiMode = sds_SoundBlaster;
boolean SoundOn = TRUE, MusicOn = TRUE;

#define YM_A0   ((vu8 *)0xA04000)
#define PSG_P   ((vu8 *)0xC00011)

/* ---------------------------------------------------------------- effects */

#if PCM_EXTERN
/* The sound data lives behind the first 4 MB of the ROM (pcm.bin, appended by tools/packrom.py), in
 * 512 KB pages 8 and up.  A cartridge with the SSF mapper shows one page at a time in the window at
 * 0x380000 (region 7), which nothing else uses; emulators that map larger ROMs linearly simply have the
 * data at 0x400000.  SD_Init finds out which of the two applies by looking for the marker. */
#define PCM_PAGE_BITS   19
#define PCM_FIRST_PAGE  8
#define PCM_WINDOW      0x380000
#define PCM_LINEAR      0x400000

static u8 pcm_mode;                 /* 0 = not found (no digitised/AdLib effects), 1 = window, 2 = linear */
static s16 pcm_page = -1;

static const u8 *pcm_ptr(u32 ofs)
{
    if (pcm_mode == 1)
    {
        s16 page = ofs >> PCM_PAGE_BITS;
        if (page != pcm_page)
        {
            SND_PCM4_stopPlay(SOUND_PCM_CH1);       /* the driver must not read the old page while it is swapped out */
            SYS_setBank(7, PCM_FIRST_PAGE + page);
            pcm_page = page;
        }
        return (const u8 *)(PCM_WINDOW + (ofs & ((1UL << PCM_PAGE_BITS) - 1)));
    }
    return (const u8 *)(PCM_LINEAR + ofs);
}

static void pcm_detect(void)
{
    pcm_mode = 0;
    SYS_setBank(7, PCM_FIRST_PAGE);
    pcm_page = 0;
    if (*(vu32 *)PCM_WINDOW == pcm_marker)
        pcm_mode = 1;
    else
    {
        SYS_setBank(7, 7);
        pcm_page = -1;
        if (*(vu32 *)PCM_LINEAR == pcm_marker)
            pcm_mode = 2;
    }
}
#else
#define pcm_ptr(ofs)    (pcm_data + (ofs))
#endif

static s16 sfx_cur = -1;
static u16 sfx_prio;
static u32 sfx_end;
static boolean pos_next, pos_active;
static s16 pos_l, pos_r;
static u8 sfx_vol = 15;

static u8 pos_volume(s16 l, s16 r)
{
    s16 a = l < r ? l : r;
    if (a < 0) a = 0;
    if (a > 8) a = 8;
    return 15 - (a * 13) / 8;
}

static boolean sfx_active(void)
{
    if (sfx_cur < 0) return FALSE;
    if ((s32)(vid_frames - sfx_end) >= 0)
    {
        sfx_cur = -1;
        pos_active = FALSE;
        return FALSE;
    }
    return TRUE;
}

boolean SD_PlaySound(u16 sound)
{
    const snd_t *t;
    const u8 *data;
    u32 len, ofs;
    boolean positioned = pos_next;

    pos_next = FALSE;
    if (sound >= NUMSOUNDS_G) return FALSE;
    t = &snd_tab[sound];
#if PCM_EXTERN
    if (!pcm_mode) return FALSE;
#endif
    if (DigiMode != sds_Off && t->digi_len)
    {
        ofs = t->digi_ofs;
        len = t->digi_len;
    }
    else if (SoundOn)
    {
        ofs = t->adlib_ofs;
        len = t->adlib_len;
    }
    else
        return FALSE;

    if (sfx_active() && t->priority < sfx_prio)
        return FALSE;

    sfx_vol = positioned ? pos_volume(pos_l, pos_r) : 15;
    SYS_disableInts();
    data = pcm_ptr(ofs);        /* may switch the ROM page: only now that the old sound is being replaced */
    SND_PCM4_startPlay(data, len, SOUND_PCM_CH1, FALSE);
    SND_PCM4_setVolume(SOUND_PCM_CH1, sfx_vol);
    SYS_enableInts();
    sfx_cur = sound;
    sfx_prio = t->priority;
    sfx_end = vid_frames + (len * (IS_PAL_SYSTEM ? 50 : 60)) / PCM_RATE_HZ + 1;
    pos_active = positioned;
    return TRUE;
}

void SD_StopSound(void)
{
    SYS_disableInts();
    SND_PCM4_stopPlay(SOUND_PCM_CH1);
    SYS_enableInts();
    sfx_cur = -1;
    pos_active = FALSE;
}

void SD_WaitSoundDone(void)
{
    u16 guard = 600;
    while (sfx_active() && guard--)
        vid_frame();
}

u16 SD_SoundPlaying(void)
{
    return sfx_active() ? (u16)(sfx_cur + 1) : 0;
}

void SD_PositionSound(s16 leftvol, s16 rightvol)
{
    pos_l = leftvol;
    pos_r = rightvol;
    pos_next = TRUE;
}

void SD_SetPosition(s16 leftvol, s16 rightvol)
{
    u8 v;
    if (!pos_active) return;
    v = pos_volume(leftvol, rightvol);
    if (v != sfx_vol)
    {
        sfx_vol = v;
        SYS_disableInts();
        SND_PCM4_setVolume(SOUND_PCM_CH1, v);
        SYS_enableInts();
    }
}

boolean SD_SoundPositioned(void)
{
    return pos_active && sfx_active();
}

/* ------------------------------------------------------------------ music */

static const u8 *mus_ptr, *mus_start;
static u16 mus_wait, mus_acc;
static boolean mus_on;

static inline void ym_busy(void)
{
    while (*YM_A0 & 0x80)
        ;
}

static void ym_w(u8 part, u8 reg, u8 val)
{
    vu8 *p = YM_A0 + part * 2;
    ym_busy();
    p[0] = reg;
    ym_busy();
    p[1] = val;
}

static void music_silence(void)
{
    static const u8 chn[6] = { 0, 1, 2, 4, 5, 6 };
    u8 i;
    Z80_requestBus(TRUE);
    for (i = 0; i < 5; i++)
        ym_w(0, 0x28, chn[i]);
    ym_busy();
    *YM_A0 = 0x2A;                  /* the PCM driver only writes the data port */
    *PSG_P = 0x9F; *PSG_P = 0xBF; *PSG_P = 0xDF; *PSG_P = 0xFF;
    Z80_releaseBus();
}

void SD_MusicOff(void)
{
    mus_on = FALSE;
    music_silence();
}

void SD_StartMusic(u16 song)
{
    const mus_t *m;
    if (!MusicOn || song >= NUMSONGS_G) return;
    m = &music_tab[song];
    if (!m->len) return;
    SD_MusicOff();
    SYS_disableInts();
    Z80_requestBus(TRUE);
    ym_w(0, 0x22, 0);
    ym_w(0, 0x27, 0);
    ym_busy();
    *YM_A0 = 0x2A;
    Z80_releaseBus();
    mus_start = mus_ptr = music_data + m->ofs;
    mus_wait = mus_ptr[0];
    mus_acc = 0;
    mus_on = TRUE;
    SYS_enableInts();
}

void SD_StartLevelMusic(u16 song)
{
    SD_StartMusic(song);
}

void SD_FadeOutMusic(void)
{
    SD_MusicOff();
}

void snd_tick(void)
{
    u16 ticks;
    if (!mus_on) return;
    mus_acc += IS_PAL_SYSTEM ? 3584 : 2987;       /* 700 Hz ticks per frame, 8.8 fixed */
    ticks = mus_acc >> 8;
    mus_acc &= 255;
    if (mus_wait > ticks)
    {
        mus_wait -= ticks;
        return;
    }
    Z80_requestBus(TRUE);
    while (ticks >= mus_wait)
    {
        ticks -= mus_wait;
        switch (mus_ptr[1])
        {
        case 1: ym_w(0, mus_ptr[2], mus_ptr[3]); break;
        case 2: ym_w(1, mus_ptr[2], mus_ptr[3]); break;
        case 3: *PSG_P = mus_ptr[2]; break;
        case 4:
        {
            u8 i;
            static const u8 chn[5] = { 0, 1, 2, 4, 5 };
            for (i = 0; i < 5; i++) ym_w(0, 0x28, chn[i]);
            *PSG_P = 0x9F; *PSG_P = 0xBF; *PSG_P = 0xDF;
            mus_ptr = mus_start - 4;
            break;
        }
        default: break;
        }
        mus_ptr += 4;
        mus_wait = mus_ptr[0];
    }
    mus_wait -= ticks;
    ym_busy();
    *YM_A0 = 0x2A;
    Z80_releaseBus();
}

void SD_Init(void)
{
#if PCM_EXTERN
    pcm_detect();
#endif
    SND_PCM4_loadDriver(TRUE);
    sfx_cur = -1;
    mus_on = FALSE;
}
