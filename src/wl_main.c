/* wl_main.c - startup, new game, main loop (port of WL_MAIN.C) */
#include "wl_def.h"
#include "snd.h"
#include "vid.h"
#include "input.h"
#include "hud.h"
#include "pic.h"
#include "menu.h"
#include "inter.h"
#include "save.h"
#include <string.h>

static u16 rndindex;

void US_InitRndT(boolean randomize)
{
    if (randomize)
        rndindex = (TimeCount + vid_frames) & 0xFF;
    else
        rndindex = 0;
}

u16 US_RndT(void)
{
    rndindex = (rndindex + 1) & 0xFF;
    return rndtable[rndindex];
}

void Quit(const char *error)
{
    SYS_disableInts();
    VDP_setEnable(TRUE);
    VDP_clearPlane(BG_A, TRUE);
    PAL_setColor(15, 0xEEE);
    VDP_drawText(error ? error : "QUIT", 1, 12);
    for (;;) ;
}

void NewGame(s16 difficulty, s16 episode)
{
    memset(&gamestate, 0, sizeof(gamestate));
    gamestate.difficulty = difficulty;
    gamestate.weapon = gamestate.bestweapon = gamestate.chosenweapon = wp_pistol;
    gamestate.health = 100;
    gamestate.ammo = STARTAMMO;
    gamestate.lives = 3;
    gamestate.nextextra = EXTRAPOINTS;
    gamestate.episode = episode;
    startgame = FALSE;
    loadedgame = FALSE;
}

int main(bool hardReset)
{
#ifdef STACKPAINT
    {
        /* test hook: fill the free stack area with a marker so a RAM dump shows the deepest
           stack use (the lowest overwritten byte above the end of .bss) */
        extern u8 _bend[];
        u8 here, *p = _bend;
        while (p < &here - 64) *p++ = 0x5A;
    }
#endif
    vid_init();
    IN_Init();
    HUD_Init();
    R_Init();
    SD_Init();
    Scores_Load();

#if defined(WOLF_TEST) && WOLF_TEST == 3
    NewGame(gd_medium, 0);
    gamestate.mapon = 2;
    CheckHighScore(23456, 3);
#elif defined(WOLF_TEST) && WOLF_TEST == 4
    /* sound test: -DWOLF_SNDS=<sound numbers>; plays them one after the other, 260 frames apart */
    {
        static const u16 snds[] = { WOLF_SNDS };
        u16 i;
        for (i = 0; i < sizeof(snds) / sizeof(snds[0]); i++)
        {
            vid_wait(60);
            SD_PlaySound(snds[i]);
            vid_wait(200);
        }
        for (;;)
            vid_frame();
    }
#elif defined(WOLF_TEST)
    /* test builds: -DWOLF_EP=<episode 0..5> -DWOLF_MAP=<floor 0..9> start there */
#ifndef WOLF_EP
#define WOLF_EP 0
#endif
#ifndef WOLF_MAP
#define WOLF_MAP 0
#endif
    NewGame(gd_medium, WOLF_EP);
    gamestate.mapon = WOLF_MAP;
    startgame = TRUE;
    GameLoop();
#endif
    PG13();
    for (;;)
    {
        startgame = loadedgame = FALSE;
        AttractLoop();
        US_ControlPanel();
        if (startgame || loadedgame)
            GameLoop();
    }
    return 0;
}
