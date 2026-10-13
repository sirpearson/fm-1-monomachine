/* SPDX-License-Identifier: GPL-3.0-only */
/* MNM-1 app: the global settings, the pattern slots, and the app's entry points. Included by the unity
 * build after gfx.c (drawing helpers in scope) and by the host simulator. (FiMba-1's app.h, reshaped.) */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "../dsp/mnm.h"
#include "../dsp/fastmath.h"

#ifndef OM_VERSION                       /* a release build passes its own (tools/build.py --release) */
#define OM_VERSION "DEV"
#endif
#define PROJ_MAGIC 0x474D4E4Du           /* "MNMG": never another firmware's project */
#define PROJ_FORMAT 1u

/* the global settings (OBJ_PROJ); the patterns and kits are in their slots (OBJ_PAT0 + slot) */
typedef struct {
    uint32_t magic, format;
    uint16_t tempo10;                    /* BPM x 10 */
    uint8_t slot;                        /* the pattern slot last playing */
    uint8_t midi_base;                   /* the channel of track 1 (0..10): tracks 1..6 on base..base+5 */
    uint8_t auto_ch;                     /* 0: off, 1..16: the channel that plays the track selected */
    uint8_t clock_in, clock_out, midi_out;
    uint8_t theme;
    uint8_t follow;                      /* the grid follows the step playing */
    uint8_t rsv[22];                     /* room to grow: zeros in an older project */
} project_t;

extern project_t proj;
#define NSLOT PLAT_NPATSLOT

void project_defaults(void);
int project_load(void);                  /* 0 = loaded; else defaults are in place */
int project_save(void);                  /* 0 = saved and verified */
void project_apply(void);                /* the settings to the engine (boot, load) */
int song_load(uint32_t slot, mn_song_t *s);       /* 0 = loaded; else a new song (slot 0: the demo) */
int song_save(uint32_t slot, const mn_song_t *s); /* 0 = saved and verified */

void ui_init(void);
void ui_frame(void);
void ui_input_only(void);
int ui_dirty(void);
void ui_say(const char *a, const char *b);
