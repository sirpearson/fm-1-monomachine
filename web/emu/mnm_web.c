/* SPDX-License-Identifier: GPL-3.0-only */
/* MNM-1 in the browser (FiMba-1's emulator, from FoMni's and X0X's): the whole app (UI, storage, engine)
 * against a simulated FM-1, compiled to WebAssembly without a C library (clang --target=wasm32, or
 * Emscripten). The page asks for audio; producing it advances the device's clock one millisecond at a
 * time, which runs the engine every 256 samples (the I2S half buffer) and the UI every 16 ms, as on the
 * FM-1. The page sends input (and MIDI from Web MIDI), and reads the screen, the lights and the saved
 * objects back through these exports.
 *
 *   web/emu/build.sh   ->  build/emu/mnm.wasm (+ the page) */
#define OM_HOST 1
#include <stdint.h>

/* the device's own libc: memset, memcpy, memcmp (the compiler may call them for struct copies) */
#include "../../firmware/src/libc.c"

/* ------------------------------------------------------------ screen --- */
#define __attribute__(x)                 /* (gfx.c's section attributes mean nothing here) */
static uint16_t fb[240 * 240];          /* RGB565, byte-swapped as the panel takes it (gfx.c) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sc = (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8));
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = sc;
}
static uint32_t blits;
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    blits++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = px[j * w + i];
}
#include "../../firmware/src/gfx.c"

/* ---------------------------------------------------------- platform --- */
#include "../../firmware/src/app/plat.h"

static uint32_t now_ms, held_btn, held_keys, master = 2800;
static int32_t enc_acc[NE];
static uint32_t lit_btn, lit_keys, dim_keys;

uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_buttons(void) { return held_btn; }
uint32_t plat_keys(void) { return held_keys; }
int32_t plat_enc(int role)
{
    int32_t v = enc_acc[role];
    enc_acc[role] = 0;
    return v;
}
uint32_t plat_master(void) { return master; }
void plat_leds(uint32_t b, uint32_t k, uint32_t d)
{
    lit_btn = b;
    lit_keys = k;
    dim_keys = d & ~k;
}

#define MQ 256
static uint32_t min_q[MQ], mi_w, mi_r;
#define MOQ 256
static uint32_t mout_q[MOQ], mo_w, mo_r;        /* MIDI out, for the page (Web MIDI out) */
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = min_q[mi_r++ % MQ];
    return 1;
}
void plat_midi_out(uint32_t pkt)
{
    if (mo_w - mo_r < MOQ)
        mout_q[mo_w++ % MOQ] = pkt;
}

/* flash: one buffer per object; the page keeps them (localStorage) */
static uint8_t store[OBJ_NOBJ][PLAT_STORE_MAX];
static int store_len[OBJ_NOBJ];
static uint32_t store_writes;
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    int n;
    if (obj >= OBJ_NOBJ || (n = store_len[obj]) <= 0)
        return -1;
    if ((uint32_t)n > max)
        n = (int)max;
    memcpy(dst, store[obj], (unsigned)n);
    return n;
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (obj >= OBJ_NOBJ || len > PLAT_STORE_MAX)
        return -1;
    memcpy(store[obj], src, len);
    store_len[obj] = (int)len;
    store_writes++;
    return 0;
}

static uint32_t cpu_pct;
uint32_t plat_cpu_pct(void) { return cpu_pct; }
uint32_t plat_xruns(void) { return 0; }

/* -------------------------------------------------------------- app --- */
#include "../../firmware/src/app/app.h"
#include "../../firmware/src/app/project.c"
#include "../../firmware/src/app/ui.c"

#undef __attribute__
#define EXPORT(name) __attribute__((used, visibility("default"), export_name(#name)))

/* --------------------------------------------------------- the device --- */
#define RING 4096u
static float ring[RING * 2];
static uint32_t ring_w, ring_r;
static double audio_due_ms;
static uint32_t ui_due;

static void run_ms(uint32_t ms)
{
    static int32_t blk[512];
    uint32_t i, k;
    for (i = 0; i < ms; i++) {
        now_ms++;
        while (audio_due_ms <= (double)now_ms) {      /* 256-frame blocks, as the device renders them */
            mn_render(blk, 256, master);
            for (k = 0; k < 256; k++) {
                uint32_t j = (ring_w++ % RING) * 2u;
                ring[j] = (float)blk[2 * k] * (1.0f / 8388608.0f);
                ring[j + 1] = (float)blk[2 * k + 1] * (1.0f / 8388608.0f);
            }
            audio_due_ms += 256.0 * 1000.0 / 44100.0;
        }
        if (now_ms >= ui_due) {
            ui_due = now_ms + 16;
            ui_frame();
        } else {
            ui_input_only();
        }
    }
}

static float out_l[1024], out_r[1024];
EXPORT(web_out_l) float *web_out_l(void) { return out_l; }
EXPORT(web_out_r) float *web_out_r(void) { return out_r; }

/* n frames (<= 1024) into out_l / out_r: the device runs until they exist */
EXPORT(web_render) void web_render(uint32_t n)
{
    uint32_t k;
    if (n > 1024u)
        n = 1024u;
    while (ring_w - ring_r < n)
        run_ms(1);
    for (k = 0; k < n; k++) {
        uint32_t i = (ring_r++ % RING) * 2u;
        out_l[k] = ring[i];
        out_r[k] = ring[i + 1];
    }
}

EXPORT(web_boot) void web_boot(void)
{
    mn_set_song(0);
    mn_init();
    project_load();
    project_apply();
    lcd_fill(0, 0, 240, 240, 0);
    ui_init();
}
EXPORT(web_buttons) void web_buttons(uint32_t m) { held_btn = m; }
EXPORT(web_keys) void web_keys(uint32_t m) { held_keys = m; }
EXPORT(web_enc) void web_enc(int role, int32_t n)
{
    if (role >= 0 && role < NE)
        enc_acc[role] += n;
}
/* one MIDI message from the page, as a USB-MIDI packet into the queue the device's USB and TRS fill */
EXPORT(web_midi) void web_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t cin = st >= 0xF0u ? 0x0Fu : st >> 4;
    if (mi_w - mi_r < MQ)
        min_q[mi_w++ % MQ] = cin | (st & 0xFFu) << 8 | (d1 & 0x7Fu) << 16 | (d2 & 0x7Fu) << 24;
}
/* MIDI out: the next packet, 0 when none */
EXPORT(web_midi_out) uint32_t web_midi_out(void) { return mo_r == mo_w ? 0u : mout_q[mo_r++ % MOQ]; }
EXPORT(web_master) void web_master(uint32_t v) { master = v > 4096u ? 4096u : v; }
EXPORT(web_lit_buttons) uint32_t web_lit_buttons(void) { return lit_btn; }
EXPORT(web_lit_keys) uint32_t web_lit_keys(void) { return lit_keys; }
EXPORT(web_dim_keys) uint32_t web_dim_keys(void) { return dim_keys; }
EXPORT(web_fb) uint16_t *web_fb(void) { return fb; }
EXPORT(web_blits) uint32_t web_blits(void) { return blits; }
EXPORT(web_now) uint32_t web_now(void) { return now_ms; }
EXPORT(web_set_cpu) void web_set_cpu(uint32_t pct) { cpu_pct = pct; }
EXPORT(web_playing) uint32_t web_playing(void) { return mn_playing; }
EXPORT(web_step) int32_t web_step(void) { return mn_step; }

EXPORT(web_store) uint8_t *web_store(uint32_t obj) { return obj < OBJ_NOBJ ? store[obj] : 0; }
EXPORT(web_store_len) int web_store_len(uint32_t obj) { return obj < OBJ_NOBJ ? store_len[obj] : -1; }
EXPORT(web_store_set_len) void web_store_set_len(uint32_t obj, int n)
{
    if (obj < OBJ_NOBJ)
        store_len[obj] = n < 0 ? -1 : n > (int)PLAT_STORE_MAX ? (int)PLAT_STORE_MAX : n;
}
EXPORT(web_store_writes) uint32_t web_store_writes(void) { return store_writes; }
EXPORT(web_nobj) uint32_t web_nobj(void) { return OBJ_NOBJ; }
EXPORT(web_store_max) uint32_t web_store_max(void) { return PLAT_STORE_MAX; }
EXPORT(web_init) void web_init(void)
{
    int o;
    for (o = 0; o < OBJ_NOBJ; o++)
        store_len[o] = -1;
}
