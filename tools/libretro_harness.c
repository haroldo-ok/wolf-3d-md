/* libretro_harness.c — headless Genesis point-to-point test host.
 *
 * Loads a libretro core (Genesis Plus GX), runs a ROM for a scripted number of
 * frames with scripted joypad input, dumps frames as PPM images, and prints the
 * first bytes of cartridge SRAM so a wrapper can assert on a self-test marker.
 * No display or audio device is required, so it runs identically in CI.
 *
 * Adapted from the harness in haroldo-ok/jazz-jackrabbit-for-sega-genesis
 * (the reference implementation — cross-check there if a core build differs).
 *
 * Build:
 *   cc -O2 -o harness libretro_harness.c \
 *      -I<gpgx>/libretro/libretro-common/include -ldl -lm
 *
 * Usage:
 *   harness CORE.so ROM.bin TOTAL_FRAMES SCRIPT OUT_PREFIX
 *
 * SCRIPT is a comma-separated list of directives:
 *   <first>-<last>:<button>   hold button over the inclusive frame range
 *   shot@<frame>              dump the frame to OUT_PREFIX<frame>.ppm
 * Buttons: up down left right a b c start
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "libretro.h"

#define MAX_DIRECTIVES 128

typedef struct { unsigned first, last, id; } Hold;
typedef struct { unsigned frame; } Shot;

static Hold  holds[MAX_DIRECTIVES];
static Shot  shots[MAX_DIRECTIVES];
static unsigned holdCount, shotCount;
static unsigned currentFrame;
static const char *outPrefix;

static enum retro_pixel_format pixFmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint8_t lastRGB[1024 * 512 * 3];
static unsigned lastW, lastH, haveFrame;

/* Map DSL button names to RetroPad ids. D-pad and START are unambiguous.
 * A/B/C follow Genesis Plus GX's default Mega Drive mapping
 * (Genesis A=RetroPad Y, B=RetroPad B, C=RetroPad A); swap here if your core
 * build maps differently. */
static int button_id(const char *name)
{
    if (!strcmp(name, "up"))    return RETRO_DEVICE_ID_JOYPAD_UP;
    if (!strcmp(name, "down"))  return RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (!strcmp(name, "left"))  return RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (!strcmp(name, "right")) return RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (!strcmp(name, "start")) return RETRO_DEVICE_ID_JOYPAD_START;
    if (!strcmp(name, "a"))     return RETRO_DEVICE_ID_JOYPAD_Y;   /* Genesis A */
    if (!strcmp(name, "b"))     return RETRO_DEVICE_ID_JOYPAD_B;   /* Genesis B */
    if (!strcmp(name, "c"))     return RETRO_DEVICE_ID_JOYPAD_A;   /* Genesis C */
    fprintf(stderr, "unknown button '%s'\n", name);
    return -1;
}

static void parse_script(char *s)
{
    for (char *tok = strtok(s, ","); tok; tok = strtok(NULL, ",")) {
        if (!strncmp(tok, "shot@", 5)) {
            if (shotCount < MAX_DIRECTIVES)
                shots[shotCount++].frame = (unsigned)atoi(tok + 5);
        } else {
            char btn[16]; unsigned a, b;
            if (sscanf(tok, "%u-%u:%15s", &a, &b, btn) == 3 && holdCount < MAX_DIRECTIVES) {
                int id = button_id(btn);
                if (id >= 0) { holds[holdCount].first = a; holds[holdCount].last = b;
                               holds[holdCount].id = (unsigned)id; holdCount++; }
            }
        }
    }
}

/* --- libretro frontend callbacks --- */

static void cb_log(enum retro_log_level lvl, const char *fmt, ...)
{
    (void)lvl; va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

static bool cb_env(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        pixFmt = *(const enum retro_pixel_format *)data; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = cb_log; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = "."; return true;
    default:
        return false;   /* decline everything else */
    }
}

static void put_rgb(uint8_t *dst, uint8_t r, uint8_t g, uint8_t b)
{ dst[0] = r; dst[1] = g; dst[2] = b; }

static void cb_video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data) return;                         /* duped frame: keep last */
    if (w > 1024 || h > 512) return;
    lastW = w; lastH = h; haveFrame = 1;
    const uint8_t *src = (const uint8_t *)data;
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            uint8_t *o = &lastRGB[(y * w + x) * 3];
            if (pixFmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                const uint32_t p = ((const uint32_t *)(src + y * pitch))[x];
                put_rgb(o, (p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF);
            } else if (pixFmt == RETRO_PIXEL_FORMAT_RGB565) {
                const uint16_t p = ((const uint16_t *)(src + y * pitch))[x];
                put_rgb(o, ((p >> 11) & 0x1F) << 3, ((p >> 5) & 0x3F) << 2, (p & 0x1F) << 3);
            } else { /* 0RGB1555 (default) */
                const uint16_t p = ((const uint16_t *)(src + y * pitch))[x];
                put_rgb(o, ((p >> 10) & 0x1F) << 3, ((p >> 5) & 0x1F) << 3, (p & 0x1F) << 3);
            }
        }
    }
}

static void cb_poll(void) {}

static int16_t cb_input(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    for (unsigned i = 0; i < holdCount; i++)
        if (holds[i].id == id && currentFrame >= holds[i].first && currentFrame <= holds[i].last)
            return 1;
    return 0;
}

static FILE *wavf; static size_t wavframes;
static size_t cb_audio_batch(const int16_t *d, size_t frames)
{ if (wavf) { fwrite(d, 4, frames, wavf); wavframes += frames; } return frames; }
static void   cb_audio(int16_t l, int16_t r) { int16_t b[2] = {l, r}; if (wavf) { fwrite(b, 4, 1, wavf); wavframes++; } }

static void write_ppm(unsigned frame)
{
    if (!haveFrame) return;
    char path[512];
    snprintf(path, sizeof(path), "%s%u.ppm", outPrefix, frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", lastW, lastH);
    fwrite(lastRGB, 3, (size_t)lastW * lastH, f);
    fclose(f);
    fprintf(stderr, "wrote %s (%ux%u)\n", path, lastW, lastH);
}

/* Function-pointer typedefs so a plain-identifier LOAD macro works. */
typedef void   (*fn_set_env)(retro_environment_t);
typedef void   (*fn_set_video)(retro_video_refresh_t);
typedef void   (*fn_set_asample)(retro_audio_sample_t);
typedef void   (*fn_set_abatch)(retro_audio_sample_batch_t);
typedef void   (*fn_set_ipoll)(retro_input_poll_t);
typedef void   (*fn_set_istate)(retro_input_state_t);
typedef void   (*fn_void)(void);
typedef bool   (*fn_load)(const struct retro_game_info *);
typedef void   (*fn_set_ctrl)(unsigned, unsigned);
typedef void  *(*fn_mem_data)(unsigned);
typedef size_t (*fn_mem_size)(unsigned);
typedef unsigned (*fn_getreg)(int);
static fn_getreg g_getreg;
static void print_cpu(unsigned frame)
{
    if (!g_getreg) return;
    fprintf(stderr, "frame %u: PC=%06X SR=%04X A7=%06X D0=%08X A0=%08X\n", frame, g_getreg(16) & 0xFFFFFF, g_getreg(17), g_getreg(15) & 0xFFFFFF, g_getreg(0), g_getreg(8));
}
static unsigned *g_prof; static unsigned profStart;
static fn_mem_data g_mem_data; static fn_mem_size g_mem_size;
static void dump_ram(unsigned frame)
{
    char path[512];
    uint8_t *ram = (uint8_t *)g_mem_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t n = g_mem_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || !n) return;
    snprintf(path, sizeof(path), "%s%u.ram", outPrefix, frame);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fwrite(ram, 1, n, f); fclose(f);
}

#define LOAD(T, var, sym) T var = (T)dlsym(h, sym); \
    if (!var) { fprintf(stderr, "missing symbol %s\n", sym); return 2; }

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s CORE.so ROM.bin TOTAL_FRAMES SCRIPT OUT_PREFIX\n", argv[0]);
        return 1;
    }
    const char *corePath = argv[1], *romPath = argv[2];
    unsigned total = (unsigned)atoi(argv[3]);
    char scriptBuf[4096]; strncpy(scriptBuf, argv[4], sizeof(scriptBuf) - 1);
    scriptBuf[sizeof(scriptBuf) - 1] = 0;
    outPrefix = argv[5];
    parse_script(scriptBuf);

    void *h = dlopen(corePath, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

    LOAD(fn_set_env,     set_environment, "retro_set_environment");
    LOAD(fn_set_video,   set_video,       "retro_set_video_refresh");
    LOAD(fn_set_asample, set_asample,     "retro_set_audio_sample");
    LOAD(fn_set_abatch,  set_abatch,      "retro_set_audio_sample_batch");
    LOAD(fn_set_ipoll,   set_ipoll,       "retro_set_input_poll");
    LOAD(fn_set_istate,  set_istate,      "retro_set_input_state");
    LOAD(fn_void,        r_init,          "retro_init");
    LOAD(fn_load,        r_load_game,     "retro_load_game");
    LOAD(fn_void,        r_run,           "retro_run");
    LOAD(fn_set_ctrl,    set_ctrl,        "retro_set_controller_port_device");
    LOAD(fn_mem_data,    mem_data,        "retro_get_memory_data");
    LOAD(fn_mem_size,    mem_size,        "retro_get_memory_size");

    g_mem_data = mem_data; g_mem_size = mem_size;
    if (getenv("WOLF_CPU")) {
        /* m68k_get_reg is a local symbol in the core: derive its address from retro_run */
        char *base = (char *)dlsym(h, "retro_run");
        g_getreg = (fn_getreg)(base - strtoul(getenv("WOLF_RUNSYM"), 0, 16) + strtoul(getenv("WOLF_REGSYM"), 0, 16));
    }
    if (getenv("WOLF_WAV")) wavf = fopen(getenv("WOLF_WAV"), "wb");
    set_environment(cb_env);
    set_video(cb_video);
    set_asample(cb_audio);
    set_abatch(cb_audio_batch);
    set_ipoll(cb_poll);
    set_istate(cb_input);
    r_init();

    /* Load ROM both by path and from memory so the core can use either. */
    FILE *rf = fopen(romPath, "rb");
    if (!rf) { fprintf(stderr, "cannot open %s\n", romPath); return 2; }
    fseek(rf, 0, SEEK_END); long sz = ftell(rf); fseek(rf, 0, SEEK_SET);
    void *rom = malloc((size_t)sz);
    if (fread(rom, 1, (size_t)sz, rf) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(rf);

    struct retro_game_info gi;
    memset(&gi, 0, sizeof(gi));
    gi.path = romPath; gi.data = rom; gi.size = (size_t)sz; gi.meta = NULL;
    if (!r_load_game(&gi)) { fprintf(stderr, "retro_load_game failed\n"); return 2; }
    set_ctrl(0, RETRO_DEVICE_JOYPAD);

    if (getenv("WOLF_PROFSYM")) {
        char *b2 = (char *)dlsym(h, "retro_run");
        g_prof = (unsigned *)(b2 - strtoul(getenv("WOLF_RUNSYM"), 0, 16) + strtoul(getenv("WOLF_PROFSYM"), 0, 16));
        profStart = getenv("WOLF_PROFSTART") ? (unsigned)atoi(getenv("WOLF_PROFSTART")) : 0;
    }
    for (currentFrame = 0; currentFrame < total; currentFrame++) {
        if (g_prof && currentFrame == profStart) memset(g_prof, 0, 0x80000 * 4);
        r_run();
        for (unsigned i = 0; i < shotCount; i++)
            if (shots[i].frame == currentFrame) { write_ppm(currentFrame); dump_ram(currentFrame); print_cpu(currentFrame); }
    }

    /* Print the first SRAM bytes for an on-target self-test marker.
       Genesis SRAM is on odd bytes; some cores interleave with 0xFF. */
    uint8_t *sram = (uint8_t *)mem_data(RETRO_MEMORY_SAVE_RAM);
    size_t   ssz  = mem_size(RETRO_MEMORY_SAVE_RAM);
    if (sram && ssz) {
        printf("SRAM:");
        for (size_t i = 0; i < ssz && i < 16; i++) printf(" %02X", sram[i]);
        printf("\n");
    }

    if (g_prof) { FILE *pf = fopen(getenv("WOLF_PROFOUT") ? getenv("WOLF_PROFOUT") : "/tmp/prof.bin", "wb"); fwrite(g_prof, 4, 0x80000, pf); fclose(pf); }
    if (wavf) { fclose(wavf); fprintf(stderr, "audio frames: %zu\n", wavframes); }
    free(rom);
    return 0;
}
