/* SPDX-License-Identifier: GPL-3.0-only */
/* MNM-1: a six-track monophonic synthesizer and step sequencer, after the Elektron Monomachine.
 *
 *   TRACKS   MN_NT tracks, one mono voice each. A track is a MACHINE (the oscillator: eleven of them,
 *            mn_machine_*), then the same strip on every track: a high-pass and a low-pass filter
 *            (Base / Width, each with its resonance, and an attack-decay envelope on both), an amp
 *            envelope (attack, hold, decay, release), distortion, a peak EQ, sample-rate reduction,
 *            volume and pan, and a delay of its own (tempo-synced, with feedback and filters).
 *   PARAMS   MN_NP values per track, 0..max each (mn_par_info), on eight pages of eight:
 *            SYN (the machine's own), AMP, FILT, FX, LFO1, LFO2, LFO3, TRK (arpeggiator, gate, accent,
 *            probability, transpose). A KIT is the six machines and their values.
 *   LFOS     three per track, each aimed at any value of any track (its page and place), with seven
 *            waves, five trigger modes and a tempo-synced speed.
 *   PATTERN  up to 64 steps per track: a trig (with accent and slide), a note, and PARAMETER LOCKS:
 *            any value of any track set for one step (MN_NLOCK lock rows a pattern). A lock on a step
 *            without a trig changes the sound without playing a note (a trigless lock). Length, speed
 *            and swing are the pattern's; the tempo is global.
 *   SONG     what a slot in flash holds: a kit and a pattern. The one playing can be replaced by
 *            another at the end of its pattern (mn_queue).
 *
 * Threads: mn_render() runs in the audio interrupt. The song the engine plays is shared memory: the
 * UI edits it in place (byte writes: a value changes between two blocks, never half-written). Notes,
 * transport and clock go through a command queue, drained at the start of each block. The state
 * marked (UI) is written by the render and only read elsewhere. Float only, no libm: fastmath.h. */
#pragma once
#include <stdint.h>

#define MN_SR 44100.0f
#define MN_NT 6                         /* tracks */
#define MN_NP 64                        /* values per track */
#define MN_NSTEP 64                     /* steps per pattern, at most */
#define MN_NLOCK 32                     /* lock rows per pattern: a (track, value) each, a value per step */
#define MN_NPAGE 8

/* pages: P_* is page * 8 + place */
enum { PG_SYN, PG_AMP, PG_FILT, PG_FX, PG_LFO1, PG_LFO2, PG_LFO3, PG_TRK };
enum {
    P_SYN0 = 0,                                                      /* the machine's eight */
    P_ATK = 8, P_HOLD, P_DEC, P_REL, P_DIST, P_VOL, P_PAN, P_PORT,   /* AMP */
    P_BASE = 16, P_WDTH, P_HPQ, P_LPQ, P_FATK, P_FDEC, P_BOFS, P_WOFS, /* FILT */
    P_EQF = 24, P_EQG, P_SRR, P_DTIM, P_DSND, P_DFB, P_DHP, P_DLP,  /* FX */
    P_LFO1 = 32, P_LFO2 = 40, P_LFO3 = 48,                           /* + L_* */
    P_ARPM = 56, P_ASPD, P_ARNG, P_ACHD, P_GATE, P_ACNT, P_PROB, P_TRNS   /* TRK */
};
enum { L_TRK, L_DEST, L_WAVE, L_TRIG, L_MULT, L_SPD, L_PHAS, L_DPTH };   /* an LFO's eight */
#define MN_NMOD 56                      /* LFO destinations: every value but the TRK page's */

enum { MC_SAW, MC_PULSE, MC_ENS, MC_SID, MC_FM, MC_FMDYN, MC_WAVE, MC_DRUM, MC_NOISE, MC_PLUCK, MC_SINE, MC_N };
enum { LW_TRI, LW_SIN, LW_SQR, LW_SAW, LW_RAMP, LW_EXP, LW_RND, LW_N };
enum { LT_FREE, LT_TRIG, LT_HOLD, LT_ONE, LT_HALF, LT_N };
enum { ARP_OFF, ARP_UP, ARP_DOWN, ARP_UPDN, ARP_RAND, ARP_N };
#define MN_NCHORD 12
#define MN_NSPEED 5                     /* pattern speed: 1/2x 3/4x 1x 3/2x 2x */

/* how a value is shown */
enum { PK_NUM, PK_BIP, PK_ENUM, PK_SPECIAL };
typedef struct {
    char name[5];
    uint8_t max, def, kind;
} mn_pinfo_t;
const mn_pinfo_t *mn_par_info(int machine, int p);
void mn_par_text(int machine, int p, int v, char *buf);   /* the value as the screen shows it (12 bytes) */
const char *mn_machine_name(int m);                       /* "SAW", "FM" ... (5 bytes) */
const char *mn_machine_long(int m);                       /* "Supersaw" ... */
const char *mn_page_name(int pg);                         /* "SYN", "AMP" ... */
void mn_dest_text(const uint8_t *machines, int self, int trk, int dest, char *buf);   /* an LFO's target */

/* ---- the song: a kit and a pattern (what a flash slot holds) */
#define MN_TRIG 1u
#define MN_ACC 2u
#define MN_SLIDE 4u
typedef struct {
    uint8_t machine[MN_NT];
    uint8_t par[MN_NT][MN_NP];
} mn_kit_t;
typedef struct {
    uint8_t trig[MN_NT][MN_NSTEP];      /* MN_TRIG | MN_ACC | MN_SLIDE */
    uint8_t note[MN_NT][MN_NSTEP];      /* MIDI note */
    uint8_t ltrk[MN_NLOCK], lpar[MN_NLOCK];   /* a lock row's track (0xFF: the row is free) and value */
    uint8_t lval[MN_NLOCK][MN_NSTEP];   /* its value on each step, 0xFF: none */
    uint8_t len;                        /* 1..64 */
    uint8_t speed;                      /* 0..MN_NSPEED-1 */
    uint8_t swing;                      /* 50..80 (%) */
    uint8_t rsv;
} mn_pat_t;
#define MN_SONG_MAGIC 0x314D4E4Du        /* "MNM1" */
#define MN_SONG_FORMAT 1u
typedef struct {
    uint32_t magic, format;
    mn_kit_t kit;
    mn_pat_t pat;
} mn_song_t;

void mn_kit_init(mn_kit_t *k);                            /* the default kit */
void mn_track_init(mn_kit_t *k, int t, int machine);      /* one track: the machine and its defaults */
void mn_machine_set(mn_kit_t *k, int t, int machine);     /* the machine, and its SYN page's defaults */
void mn_pat_clear(mn_pat_t *p);
void mn_song_init(mn_song_t *s);                          /* the default kit, an empty pattern */
void mn_song_demo(mn_song_t *s);                          /* the default kit and a beat to start from */
int mn_song_valid(mn_song_t *s);                          /* magic and format; values out of range fixed: 1 */
/* locks: -1 when none */
int mn_lock_get(const mn_pat_t *p, int t, int par, int step);
int mn_lock_set(mn_pat_t *p, int t, int par, int step, int v);   /* 0, or -1: every row is in use */
void mn_lock_clear_step(mn_pat_t *p, int t, int step);           /* every lock of track t on the step */
void mn_lock_clear_track(mn_pat_t *p, int t);
int mn_step_locks(const mn_pat_t *p, int t, int step);            /* how many values the step locks */
int mn_lock_rows_free(const mn_pat_t *p);

/* ---- the engine */
void mn_init(void);
void mn_set_song(mn_song_t *s);         /* play this song from now on (stopped, or a slot loaded at once) */
void mn_queue(mn_song_t *s);            /* playing: at the end of the pattern (stopped: now) */
mn_song_t *mn_song(void);               /* the song playing */
mn_song_t *mn_queued(void);             /* the one waiting for the end of the pattern, or 0 */

void mn_play(int on);                   /* start (from step 1) / stop */
void mn_note_on(int t, int note, int vel);    /* live: a key, MIDI */
void mn_note_off(int t, int note);
void mn_all_off(void);                  /* every live note up, every gate closed */
void mn_panic(void);                    /* everything quiet, now (the delays too) */
/* MIDI clock in: tick (F8), start (FA), continue (FB), stop (FC). When the clock input is on, ticks
 * (the last within ~0.5 s) lead the sequencer, 6 a step at 1x */
enum { MN_CLK_TICK, MN_CLK_START, MN_CLK_CONTINUE, MN_CLK_STOP };
void mn_clock(int msg);

/* settings (main loop -> render, read every block) */
extern volatile uint16_t mn_tempo10;    /* BPM x 10, 300..3000 */
extern volatile uint8_t mn_mute;        /* bit t: track t muted (the sequencer does not play it) */
extern volatile uint8_t mn_clock_in, mn_clock_out, mn_midi_out_on, mn_midi_base;   /* base: channel 0..15 of track 1 */
/* MIDI out from the render (sequencer notes on the tracks' channels, clock): 4-byte USB-MIDI packets */
extern void (*mn_midi_hook)(uint32_t pkt);

/* the render (audio ISR): n stereo frames, 24-bit in int32; gain Q12 (the MASTER pot) */
void mn_render(int32_t *out_lr, uint32_t n, uint32_t gain_q12);

/* state for the screen (UI) */
extern volatile int8_t mn_step;                 /* the step playing, -1 stopped */
extern volatile float mn_step_frac;             /* how far into it, 0..1 */
extern volatile uint8_t mn_playing, mn_ext;     /* running; following MIDI clock */
extern volatile uint32_t mn_loops;              /* times the pattern came round (the queue swaps then) */
extern volatile float mn_level[MN_NT];          /* each track's output peak, 0..~1 */
extern volatile uint8_t mn_trig_count[MN_NT];   /* +1 for each note a track plays */
extern volatile float mn_out_level;             /* the output's peak over the last block (tails too) */
extern volatile uint32_t mn_dropped;            /* commands lost to a full queue */
extern volatile float mn_lfo_val[MN_NT][3];     /* each LFO's output, -1..1 */
