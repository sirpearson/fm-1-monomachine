/* SPDX-License-Identifier: GPL-3.0-only */
/* MNM-1's engine (mnm.h): six mono tracks, their machines and strips, the LFOs, the sequencer and the
 * arpeggiators. One compilation unit, -O2. The command queue and the float-only maths follow FiMba-1 /
 * FoMni / X0X. */
#include "mnm.h"
#include "fastmath.h"

#ifdef OM_HOST
#define MN_POOL
#else
#define MN_POOL __attribute__((section(".pool")))
#endif
#define BARRIER() __asm__ volatile("" ::: "memory")

#define BLK 32u                                   /* control rate: LFOs, envelopes' targets, the sequencer */
#define DLY_N 16384u                              /* a track's delay line, at half rate: 743 ms */
#define PLK_N 2048u                               /* the plucked string's line: down to 22 Hz */
#define WT_N 256u                                 /* a wavetable's single cycle */
#define WT_K 8                                    /* tables */

/* ================================================================ values === */
static const char *const PAGE_NAME[MN_NPAGE] = {"SYN", "AMP", "FILT", "FX", "LFO1", "LFO2", "LFO3", "TRK"};
const char *mn_page_name(int pg) { return PAGE_NAME[(unsigned)pg % MN_NPAGE]; }

/* every page but SYN: the same on every track */
static const mn_pinfo_t COMMON[MN_NP - 8] = {
    {"ATK", 127, 0, PK_SPECIAL}, {"HOLD", 127, 127, PK_SPECIAL}, {"DEC", 127, 64, PK_SPECIAL}, {"REL", 127, 30, PK_SPECIAL},
    {"DIST", 127, 0, PK_NUM}, {"VOL", 127, 100, PK_NUM}, {"PAN", 127, 64, PK_BIP}, {"PORT", 127, 0, PK_NUM},
    {"BASE", 127, 0, PK_NUM}, {"WDTH", 127, 127, PK_NUM}, {"HPQ", 127, 0, PK_NUM}, {"LPQ", 127, 0, PK_NUM},
    {"FATK", 127, 0, PK_NUM}, {"FDEC", 127, 64, PK_NUM}, {"BOFS", 127, 64, PK_BIP}, {"WOFS", 127, 64, PK_BIP},
    {"EQF", 127, 64, PK_NUM}, {"EQG", 127, 64, PK_BIP}, {"SRR", 127, 0, PK_NUM}, {"DTIM", 127, 23, PK_SPECIAL},
    {"DSND", 127, 0, PK_NUM}, {"DFB", 127, 50, PK_NUM}, {"DHP", 127, 0, PK_NUM}, {"DLP", 127, 100, PK_NUM},
#define LFO_INFO {"TRK", 6, 0, PK_SPECIAL}, {"DEST", MN_NMOD, 0, PK_SPECIAL}, {"WAVE", LW_N - 1, 0, PK_ENUM}, \
                 {"TRIG", LT_N - 1, 0, PK_ENUM}, {"MULT", 7, 2, PK_SPECIAL}, {"SPD", 127, 32, PK_NUM},       \
                 {"PHAS", 127, 0, PK_NUM}, {"DPTH", 127, 64, PK_BIP}
    LFO_INFO, LFO_INFO, LFO_INFO,
    {"ARP", ARP_N - 1, 0, PK_ENUM}, {"ASPD", 7, 3, PK_SPECIAL}, {"ARNG", 3, 0, PK_SPECIAL}, {"ACHD", MN_NCHORD - 1, 2, PK_ENUM},
    {"GATE", 127, 14, PK_SPECIAL}, {"ACNT", 127, 64, PK_NUM}, {"PROB", 127, 127, PK_SPECIAL}, {"TRNS", 127, 64, PK_BIP},
};

typedef struct {
    const char *name, *longname;
    uint8_t fine;                                 /* SYN 2 is FINE (else TUNE is all the pitch it has) */
    mn_pinfo_t p[8];
} machine_t;
#define TUNE {"TUNE", 127, 64, PK_BIP}
#define FINE {"FINE", 127, 64, PK_BIP}
static const machine_t MACH[MC_N] = {
    {"SAW", "Supersaw", 1, {TUNE, FINE, {"DET", 127, 40, PK_NUM}, {"VOIC", 6, 4, PK_SPECIAL},
                             {"SUB", 127, 0, PK_NUM}, {"NOIS", 127, 0, PK_NUM}, {"DRFT", 127, 12, PK_NUM}, {"BRIT", 127, 127, PK_NUM}}},
    {"PULS", "Pulse", 1, {TUNE, FINE, {"PW", 127, 0, PK_NUM}, {"PWM", 127, 30, PK_NUM},
                           {"PWMS", 127, 40, PK_NUM}, {"SUB", 127, 0, PK_NUM}, {"DET", 127, 0, PK_NUM}, {"NOIS", 127, 0, PK_NUM}}},
    {"ENS", "Ensemble", 1, {TUNE, FINE, {"CHRD", MN_NCHORD - 1, 3, PK_ENUM}, {"WAVE", 127, 0, PK_NUM},
                             {"DET", 127, 30, PK_NUM}, {"SPRD", 127, 0, PK_NUM}, {"BRIT", 127, 110, PK_NUM}, {"SUB", 127, 0, PK_NUM}}},
    {"SID", "SID chip", 1, {TUNE, FINE, {"WAVE", 7, 2, PK_ENUM}, {"PW", 127, 64, PK_NUM},
                             {"PWM", 127, 0, PK_NUM}, {"O2TN", 127, 64, PK_BIP}, {"RING", 127, 0, PK_NUM}, {"BITS", 127, 0, PK_NUM}}},
    {"FM", "FM static", 1, {TUNE, FINE, {"RAT1", 23, 6, PK_ENUM}, {"IDX1", 127, 40, PK_NUM},
                             {"RAT2", 23, 3, PK_ENUM}, {"IDX2", 127, 0, PK_NUM}, {"FB", 127, 0, PK_NUM}, {"ALGO", 3, 0, PK_ENUM}}},
    {"FMDY", "FM dynamic", 1, {TUNE, FINE, {"RAT", 23, 3, PK_ENUM}, {"IDX", 127, 20, PK_NUM},
                                {"IATK", 127, 0, PK_NUM}, {"IDEC", 127, 50, PK_NUM}, {"IENV", 127, 100, PK_BIP}, {"FB", 127, 0, PK_NUM}}},
    {"WAVE", "Wavetable", 1, {TUNE, FINE, {"WAV", 127, 16, PK_NUM}, {"WMOD", 127, 64, PK_BIP},
                               {"WDEC", 127, 60, PK_NUM}, {"FOLD", 127, 0, PK_NUM}, {"BITS", 127, 0, PK_NUM}, {"SYNC", 127, 0, PK_NUM}}},
    {"DRUM", "Drum box", 0, {TUNE, {"TYPE", 7, 0, PK_ENUM}, {"PDEC", 127, 40, PK_NUM}, {"PAMT", 127, 60, PK_NUM},
                              {"NOIS", 127, 40, PK_NUM}, {"SNAP", 127, 40, PK_NUM}, {"TONE", 127, 64, PK_NUM}, {"DCAY", 127, 64, PK_NUM}}},
    {"NOIS", "Noise", 0, {TUNE, {"COLR", 127, 64, PK_BIP}, {"SH", 127, 0, PK_NUM}, {"BITS", 127, 0, PK_NUM},
                          {"DENS", 127, 127, PK_NUM}, {"RING", 127, 0, PK_NUM}, {"SINE", 127, 0, PK_NUM}, {"RESO", 127, 0, PK_NUM}}},
    {"PLUK", "Plucked", 1, {TUNE, FINE, {"DCAY", 127, 80, PK_NUM}, {"TONE", 127, 80, PK_NUM},
                             {"PICK", 127, 30, PK_NUM}, {"EXCT", 127, 0, PK_NUM}, {"BRST", 127, 100, PK_NUM}, {"STIF", 127, 0, PK_NUM}}},
    {"SINE", "Sine fold", 1, {TUNE, FINE, {"FOLD", 127, 0, PK_NUM}, {"DRIV", 127, 0, PK_NUM},
                               {"SUB", 127, 0, PK_NUM}, {"OCT2", 127, 0, PK_NUM}, {"PDEC", 127, 30, PK_NUM}, {"PAMT", 127, 0, PK_NUM}}},
};

const char *mn_machine_name(int m) { return MACH[(unsigned)m < MC_N ? m : 0].name; }
const char *mn_machine_long(int m) { return MACH[(unsigned)m < MC_N ? m : 0].longname; }
const mn_pinfo_t *mn_par_info(int machine, int p)
{
    if ((unsigned)p >= MN_NP)
        p = 0;
    if (p < 8)
        return &MACH[(unsigned)machine < MC_N ? machine : 0].p[p];
    return &COMMON[p - 8];
}

static const char *const LW_NAME[LW_N] = {"TRI", "SIN", "SQR", "SAW", "RAMP", "EXP", "RND"};
static const char *const LT_NAME[LT_N] = {"FREE", "TRIG", "HOLD", "ONE", "HALF"};
static const char *const ARP_NAME[ARP_N] = {"OFF", "UP", "DOWN", "U-D", "RND"};
static const char *const CHORD_NAME[MN_NCHORD] = {"OCT", "5TH", "MAJ", "MIN", "SUS4", "MAJ7", "MIN7", "DOM7",
                                                  "MAJ6", "DIM", "AUG", "MIN9"};
static const int8_t CHORD[MN_NCHORD][4] = {   /* intervals; -1: no more */
    {0, -1, -1, -1}, {0, 7, -1, -1}, {0, 4, 7, -1}, {0, 3, 7, -1}, {0, 5, 7, -1}, {0, 4, 7, 11},
    {0, 3, 7, 10}, {0, 4, 7, 10}, {0, 4, 7, 9}, {0, 3, 6, -1}, {0, 4, 8, -1}, {0, 3, 10, 14}};
static const char *const SIDW_NAME[8] = {"TRI", "SAW", "PULS", "NOIS", "T&S", "P&S", "P&T", "PTS"};
static const char *const ALGO_NAME[4] = {"3>2>1", "2+3>1", "3>1+2", "1+2+3"};
static const char *const DRUM_NAME[8] = {"BD", "SD", "HH CL", "HH OP", "CLAP", "TOM", "RIM", "BELL"};
static const float RATIO[24] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 4.5f,
                                5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f};
static const char *const RATIO_NAME[24] = {"x.25", "x.5", "x.75", "x1", "x1.25", "x1.5", "x2", "x2.5", "x3", "x3.5",
                                           "x4", "x4.5", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                                           "x14", "x15", "x16"};
static const float ASPD_STEPS[8] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
static const char *const ASPD_NAME[8] = {"1/64", "1/32", "3/64", "1/16", "3/32", "1/8", "3/16", "1/4"};

static void s_cpy(char *d, const char *s)
{
    while ((*d++ = *s++))
        ;
}
static char *itoa_s(int v, char *b)                /* returns the end */
{
    char t[12];
    int n = 0;
    if (v < 0) {
        *b++ = '-';
        v = -v;
    }
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *b++ = t[--n];
    *b = 0;
    return b;
}

/* the times of the envelopes: 1.5 ms .. 4.3 s, a doubling every 11 steps */
static float env_time(float v) { return 0.0015f * fm_exp2f(v * (11.5f / 127.0f)); }

void mn_par_text(int machine, int p, int v, char *b)
{
    const mn_pinfo_t *in = mn_par_info(machine, p);
    if (in->kind == PK_BIP) {
        if (v > 64)
            *b++ = '+';
        itoa_s(v - 64, b);
        return;
    }
    if (in->kind == PK_ENUM) {
        const char *s = "?";
        if (p < 8) {
            switch (machine) {
            case MC_ENS: s = CHORD_NAME[v % MN_NCHORD]; break;
            case MC_SID: s = SIDW_NAME[v & 7]; break;
            case MC_FM: s = p == 7 ? ALGO_NAME[v & 3] : RATIO_NAME[v % 24]; break;
            case MC_FMDYN: s = RATIO_NAME[v % 24]; break;
            case MC_DRUM: s = DRUM_NAME[v & 7]; break;
            default: break;
            }
        } else if (p >= P_LFO1 && p < P_ARPM) {
            int l = (p - P_LFO1) & 7;
            s = l == L_WAVE ? LW_NAME[v % LW_N] : LT_NAME[v % LT_N];
        } else if (p == P_ARPM) {
            s = ARP_NAME[v % ARP_N];
        } else if (p == P_ACHD) {
            s = CHORD_NAME[v % MN_NCHORD];
        }
        s_cpy(b, s);
        return;
    }
    if (in->kind == PK_SPECIAL) {
        if (p == P_ATK || p == P_HOLD || p == P_DEC || p == P_REL) {
            int ms;
            if (p == P_HOLD && v == 127) {
                s_cpy(b, "GATE");
                return;
            }
            ms = (int)(env_time((float)v) * 1000.0f + 0.5f);
            if (ms >= 1000) {
                b = itoa_s(ms / 1000, b);
                *b++ = '.';
                *b++ = (char)('0' + (ms / 100) % 10);
                s_cpy(b, "s");
            } else {
                b = itoa_s(ms, b);
                s_cpy(b, "ms");
            }
            return;
        }
        if (p == 3 && machine == MC_SAW) {
            itoa_s(v + 1, b);
            return;
        }
        if (p == P_DTIM) {                                 /* eighths of a step */
            b = itoa_s((v + 1) / 8, b);
            if ((v + 1) % 8) {
                *b++ = '.';
                b = itoa_s((v + 1) % 8 * 125, b);
            }
            return;
        }
        if (p == P_GATE) {                                 /* sixteenths of a step */
            int s = v + 1;
            b = itoa_s(s / 16, b);
            if (s % 16) {
                *b++ = '.';
                b = itoa_s(s % 16 * 100 / 16, b);
            }
            return;
        }
        if (p == P_PROB) {
            b = itoa_s((v * 100 + 63) / 127, b);
            s_cpy(b, "%");
            return;
        }
        if (p == P_ASPD) {
            s_cpy(b, ASPD_NAME[v & 7]);
            return;
        }
        if (p == P_ARNG) {
            b = itoa_s(v + 1, b);
            s_cpy(b, " OCT");
            return;
        }
        if (p >= P_LFO1 && p < P_ARPM) {
            int l = (p - P_LFO1) & 7;
            if (l == L_TRK) {
                if (!v) {
                    s_cpy(b, "SELF");
                } else {
                    *b++ = 'T';
                    itoa_s(v, b);
                }
                return;
            }
            if (l == L_MULT) {
                *b++ = 'x';
                itoa_s(1 << (v & 7), b);
                return;
            }
            if (l == L_DEST) {                         /* without the kit: the page and place */
                if (!v) {
                    s_cpy(b, "OFF");
                    return;
                }
                s_cpy(b, PAGE_NAME[(v - 1) / 8]);
                while (*b)
                    b++;
                itoa_s((v - 1) % 8 + 1, b);
                return;
            }
        }
    }
    itoa_s(v, b);
}

void mn_dest_text(const uint8_t *machines, int self, int trk, int dest, char *b)
{
    int t = trk ? trk - 1 : self;
    if (!dest) {
        s_cpy(b, "OFF");
        return;
    }
    s_cpy(b, mn_par_info(machines[t % MN_NT], dest - 1)->name);
}

/* ================================================================== songs === */
static void track_defaults(mn_kit_t *k, int t)
{
    int p;
    for (p = 8; p < MN_NP; p++)
        k->par[t][p] = COMMON[p - 8].def;
}

void mn_machine_set(mn_kit_t *k, int t, int m)
{
    int p;
    m = (unsigned)m < MC_N ? m : 0;
    k->machine[t] = (uint8_t)m;
    for (p = 0; p < 8; p++)
        k->par[t][p] = MACH[m].p[p].def;
}

void mn_track_init(mn_kit_t *k, int t, int m)
{
    track_defaults(k, t);
    mn_machine_set(k, t, m);
    if (m == MC_DRUM) {                                /* the drum keeps its own envelopes: the amp stays open */
        k->par[t][P_HOLD] = 0;
        k->par[t][P_DEC] = 127;
        k->par[t][P_REL] = 127;
    }
}

/* the kit every new slot starts with: three drums, a bass, an FM voice and a pad */
void mn_kit_init(mn_kit_t *k)
{
    uint8_t *p;
    mn_track_init(k, 0, MC_DRUM);
    p = k->par[0];
    p[1] = 0, p[2] = 34, p[3] = 72, p[4] = 10, p[5] = 50, p[6] = 40, p[7] = 62, p[P_VOL] = 116;
    mn_track_init(k, 1, MC_DRUM);
    p = k->par[1];
    p[1] = 1, p[2] = 28, p[3] = 40, p[4] = 92, p[5] = 50, p[6] = 76, p[7] = 52, p[P_VOL] = 96;
    p[P_DSND] = 20, p[P_DTIM] = 23, p[P_DFB] = 40;
    mn_track_init(k, 2, MC_DRUM);
    p = k->par[2];
    p[1] = 2, p[4] = 50, p[6] = 84, p[7] = 40, p[P_VOL] = 78, p[P_PAN] = 76;
    mn_track_init(k, 3, MC_SAW);
    p = k->par[3];
    p[2] = 22, p[3] = 2, p[4] = 70, p[P_REL] = 20, p[P_WDTH] = 52, p[P_LPQ] = 64, p[P_FDEC] = 46, p[P_WOFS] = 100;
    p[P_VOL] = 84, p[P_GATE] = 10, p[P_ACNT] = 90;
    mn_track_init(k, 4, MC_FM);
    p = k->par[4];
    p[2] = 8, p[3] = 52, p[4] = 6, p[5] = 20, p[P_HOLD] = 0, p[P_DEC] = 70, p[P_REL] = 50;
    p[P_DSND] = 60, p[P_DTIM] = 23, p[P_DFB] = 64, p[P_PAN] = 44, p[P_VOL] = 80;
    mn_track_init(k, 5, MC_ENS);
    p = k->par[5];
    p[2] = 6, p[4] = 40, p[5] = 40, p[P_ATK] = 70, p[P_REL] = 84, p[P_WDTH] = 92, p[P_VOL] = 70, p[P_PAN] = 84;
    p[P_LFO1 + L_DEST] = P_WDTH + 1, p[P_LFO1 + L_SPD] = 16, p[P_LFO1 + L_DPTH] = 84;
    p[P_DSND] = 30, p[P_DTIM] = 31, p[P_DFB] = 60, p[P_GATE] = 127;
}

void mn_pat_clear(mn_pat_t *p)
{
    int t, s;
    for (t = 0; t < MN_NT; t++)
        for (s = 0; s < MN_NSTEP; s++) {
            p->trig[t][s] = 0;
            p->note[t][s] = 60;
        }
    for (s = 0; s < MN_NLOCK; s++) {
        int i;
        p->ltrk[s] = 0xFF;
        p->lpar[s] = 0;
        for (i = 0; i < MN_NSTEP; i++)
            p->lval[s][i] = 0xFF;
    }
    p->len = 16;
    p->speed = 2;
    p->swing = 50;
    p->rsv = 0;
}

void mn_song_init(mn_song_t *s)
{
    s->magic = MN_SONG_MAGIC;
    s->format = MN_SONG_FORMAT;
    mn_kit_init(&s->kit);
    mn_pat_clear(&s->pat);
}

void mn_song_demo(mn_song_t *s)
{
    static const uint8_t BASS[16] = {36, 0, 0, 36, 0, 0, 48, 0, 36, 0, 39, 41, 0, 0, 43, 0};
    mn_pat_t *p = &s->pat;
    int i;
    mn_song_init(s);
    for (i = 0; i < 16; i += 4)
        p->trig[0][i] = MN_TRIG;
    p->trig[1][4] = p->trig[1][12] = MN_TRIG;
    p->trig[1][15] = MN_TRIG;
    mn_lock_set(p, 1, P_VOL, 15, 50);                 /* a ghost note: a lock */
    for (i = 2; i < 16; i += 4)
        p->trig[2][i] = MN_TRIG | MN_ACC;
    p->trig[2][7] = p->trig[2][13] = MN_TRIG;
    for (i = 0; i < 16; i++)
        if (BASS[i]) {
            p->trig[3][i] = MN_TRIG | (i == 0 || i == 8 ? MN_ACC : 0u) | (i == 11 ? MN_SLIDE : 0u);
            p->note[3][i] = BASS[i];
        }
    p->trig[4][3] = p->trig[4][11] = MN_TRIG;
    p->note[4][3] = 72;
    p->note[4][11] = 75;
    mn_lock_set(p, 4, 2, 11, 10);                      /* RAT1 x3: another colour, one step */
    p->trig[5][0] = p->trig[5][8] = MN_TRIG;
    p->note[5][0] = 60;
    p->note[5][8] = 56;
    p->swing = 54;
}

int mn_song_valid(mn_song_t *s)
{
    int t, p, i, r;
    if (s->magic != MN_SONG_MAGIC || s->format < 1u || s->format > MN_SONG_FORMAT)
        return 0;
    for (t = 0; t < MN_NT; t++) {
        if (s->kit.machine[t] >= MC_N)
            mn_track_init(&s->kit, t, MC_SAW);
        for (p = 0; p < MN_NP; p++)
            if (s->kit.par[t][p] > mn_par_info(s->kit.machine[t], p)->max)
                s->kit.par[t][p] = mn_par_info(s->kit.machine[t], p)->def;
        for (i = 0; i < MN_NSTEP; i++) {
            s->pat.trig[t][i] &= MN_TRIG | MN_ACC | MN_SLIDE;
            if (s->pat.note[t][i] > 127)
                s->pat.note[t][i] = 60;
        }
    }
    for (r = 0; r < MN_NLOCK; r++)
        if (s->pat.ltrk[r] != 0xFF && (s->pat.ltrk[r] >= MN_NT || s->pat.lpar[r] >= MN_NP))
            s->pat.ltrk[r] = 0xFF;
    if (s->pat.len < 1 || s->pat.len > MN_NSTEP)
        s->pat.len = 16;
    if (s->pat.speed >= MN_NSPEED)
        s->pat.speed = 2;
    if (s->pat.swing < 50 || s->pat.swing > 80)
        s->pat.swing = 50;
    return 1;
}

/* ---- locks */
static int lock_row(const mn_pat_t *p, int t, int par)
{
    int r;
    for (r = 0; r < MN_NLOCK; r++)
        if (p->ltrk[r] == t && p->lpar[r] == par)
            return r;
    return -1;
}

int mn_lock_get(const mn_pat_t *p, int t, int par, int step)
{
    int r = lock_row(p, t, par);
    return r < 0 || p->lval[r][step & (MN_NSTEP - 1)] == 0xFF ? -1 : p->lval[r][step & (MN_NSTEP - 1)];
}

static void row_free_if_empty(mn_pat_t *p, int r)
{
    int i;
    for (i = 0; i < MN_NSTEP; i++)
        if (p->lval[r][i] != 0xFF)
            return;
    p->ltrk[r] = 0xFF;
}

int mn_lock_set(mn_pat_t *p, int t, int par, int step, int v)
{
    int r = lock_row(p, t, par), i;
    if (r < 0) {
        for (r = 0; r < MN_NLOCK && p->ltrk[r] != 0xFF; r++)
            ;
        if (r == MN_NLOCK)
            return -1;
        for (i = 0; i < MN_NSTEP; i++)
            p->lval[r][i] = 0xFF;
        p->lpar[r] = (uint8_t)par;
        BARRIER();                                    /* (the row's values are clear before it is in use) */
        p->ltrk[r] = (uint8_t)t;
    }
    p->lval[r][step & (MN_NSTEP - 1)] = (uint8_t)v;
    return 0;
}

void mn_lock_clear_step(mn_pat_t *p, int t, int step)
{
    int r;
    for (r = 0; r < MN_NLOCK; r++)
        if (p->ltrk[r] == t) {
            p->lval[r][step & (MN_NSTEP - 1)] = 0xFF;
            row_free_if_empty(p, r);
        }
}

void mn_lock_clear_track(mn_pat_t *p, int t)
{
    int r;
    for (r = 0; r < MN_NLOCK; r++)
        if (p->ltrk[r] == t)
            p->ltrk[r] = 0xFF;
}

int mn_step_locks(const mn_pat_t *p, int t, int step)
{
    int r, n = 0;
    for (r = 0; r < MN_NLOCK; r++)
        n += p->ltrk[r] == t && p->lval[r][step & (MN_NSTEP - 1)] != 0xFF;
    return n;
}

int mn_lock_rows_free(const mn_pat_t *p)
{
    int r, n = 0;
    for (r = 0; r < MN_NLOCK; r++)
        n += p->ltrk[r] == 0xFF;
    return n;
}

/* =============================================================== commands === */
enum { C_PLAY, C_NOTEON, C_NOTEOFF, C_ALLOFF, C_PANIC, C_CLOCK, C_SONG, C_QUEUE };
typedef struct {
    uint8_t c, t;
    int16_t a, b;
    void *p;
} cmd_t;
#define QN 256u
static cmd_t q[QN];
static volatile uint32_t q_w, q_r;
volatile uint32_t mn_dropped;
static void post(int c, int t, int a, int b, void *p)
{
    uint32_t w = q_w;
    cmd_t *e;
    if (w - q_r >= QN) {
        mn_dropped++;
        return;
    }
    e = &q[w % QN];
    e->c = (uint8_t)c;
    e->t = (uint8_t)t;
    e->a = (int16_t)a;
    e->b = (int16_t)b;
    e->p = p;
    BARRIER();
    q_w = w + 1;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
void mn_play(int on) { post(C_PLAY, 0, on, 0, 0); }
void mn_note_on(int t, int note, int vel) { post(C_NOTEON, clampi(t, 0, MN_NT - 1), clampi(note, 0, 127), clampi(vel, 1, 127), 0); }
void mn_note_off(int t, int note) { post(C_NOTEOFF, clampi(t, 0, MN_NT - 1), clampi(note, 0, 127), 0, 0); }
void mn_all_off(void) { post(C_ALLOFF, 0, 0, 0, 0); }
void mn_panic(void) { post(C_PANIC, 0, 0, 0, 0); }
void mn_clock(int msg) { post(C_CLOCK, 0, msg, 0, 0); }

/* the song pointers: written here (and by the commands, for the render's own view) */
static mn_song_t *volatile cur, *volatile queued;
void mn_set_song(mn_song_t *s)
{
    cur = s;                                          /* (a pointer store: the render sees one or the other) */
    queued = 0;
}
void mn_queue(mn_song_t *s) { post(C_QUEUE, 0, 0, 0, s); }   /* (the render alone writes the queue) */
mn_song_t *mn_song(void) { return cur; }
mn_song_t *mn_queued(void) { return queued; }

/* ================================================================== state === */
volatile uint16_t mn_tempo10 = 1200;
volatile uint8_t mn_mute, mn_clock_in = 1, mn_clock_out, mn_midi_out_on, mn_midi_base;
void (*mn_midi_hook)(uint32_t pkt);
volatile int8_t mn_step = -1;
volatile float mn_step_frac;
volatile uint8_t mn_playing, mn_ext;
volatile uint32_t mn_loops;
volatile float mn_level[MN_NT];
volatile uint8_t mn_trig_count[MN_NT];
volatile float mn_out_level;
volatile float mn_lfo_val[MN_NT][3];

static uint32_t rng = 0x2545F491u;
static inline uint32_t rnd(void)
{
    uint32_t x = rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng = x;
}
static inline float rnds(void) { return (float)(int32_t)rnd() * (1.0f / 2147483648.0f); }   /* -1..1 */
static inline float rndu(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }      /* 0..1 */

static float wt[WT_K][WT_N + 1] MN_POOL;               /* (+1: the first sample again, for interpolation) */
static int16_t dly_buf[MN_NT][DLY_N] MN_POOL;
static float plk_buf[MN_NT][PLK_N] MN_POOL;

typedef struct {
    float ph, val, hold, rv;
    uint8_t run;
} lfo_t;

typedef struct {
    /* pitch */
    float pitch, target;                          /* semitones (MIDI note numbers) */
    float glide;                                  /* seconds; 0: jump */
    float vel;
    uint8_t gate, inf_gate;                       /* gate open; open until a note off (live) */
    uint32_t gate_left;                           /* samples */
    int8_t live[8];                               /* live notes held, last on top */
    uint8_t nlive;
    /* amp envelope */
    uint8_t stage;
    float env, a_inc, d_k, r_k;
    uint32_t hold_left;
    /* filter envelope */
    uint8_t fstage;
    float fenv;
    /* filters */
    float h1, h2, l1, l2;
    float hg_a1, hg_a2, hg_a3, hk, lg_a1, lg_a2, lg_a3, lk;
    uint8_t hp_on;
    /* EQ */
    float eb0, eb1, eb2, ea1, ea2, ex1, ex2, ey1, ey2, eqf_last, eqg_last;
    uint8_t eq_on;
    /* SRR, distortion, out */
    float srr_ph, srr_inc, srr_hold, drive, dnorm, gl, gr, gain;
    /* delay (half rate) */
    uint32_t dw;
    float dacc, dold, dnew, dt_s, dsend, dfb, dhp_k, dlp_k, dlp, dhp, dlast;
    uint32_t dquiet;
    /* the machine */
    uint8_t machine;
    float ph[8], drift[8], aux[8];
    float m_lp, m_bp, m_env1, m_env2, m_env3, m_pe;
    uint32_t acc, acc2, lfsr, m_cnt;
    float plk_len, plk_s, plk_ap_x, plk_ap_y;
    uint32_t plk_w;
    /* LFOs and locks */
    lfo_t lfo[3];
    int16_t lock[MN_NP];                          /* -1: the kit's value */
    float mod[MN_NMOD];                           /* the LFOs' sum on each value (this chunk) */
    float e[MN_NMOD];                             /* the effective values (this chunk) */
    /* arpeggiator */
    uint8_t arp_on, arp_n, arp_i, arp_dir;
    int8_t arp_note[16];
    float arp_left, arp_len;
    float arp_vel;
    /* MIDI out */
    int8_t midi_note;
} voice_t;
static voice_t vo[MN_NT];

/* ================================================================ helpers === */
static int machine_of(int t) { return cur ? cur->kit.machine[t] : 0; }
static float base_val(int t, int p)
{
    int l = vo[t].lock[p];
    return (float)(l >= 0 ? l : cur ? cur->kit.par[t][p] : 0);
}
static float step_samples(void)                    /* one step at 1x and the tempo */
{
    return MN_SR * 60.0f / ((float)mn_tempo10 * 0.1f * 4.0f);
}
static const float SPEED_MUL[MN_NSPEED] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
static float pat_step_samples(void)
{
    return step_samples() / SPEED_MUL[cur ? cur->pat.speed % MN_NSPEED : 2];
}
static inline float k_from_time(float t, float n) { return 1.0f - fm_expf(-n / (t * MN_SR)); }

static void midi_send(uint32_t st, uint32_t d1, uint32_t d2)
{
    void (*h)(uint32_t) = mn_midi_hook;
    if (h)
        h(((st >> 4) & 0x0Fu) | st << 8 | d1 << 16 | d2 << 24);
}
static void midi_rt(uint32_t st)
{
    void (*h)(uint32_t) = mn_midi_hook;
    if (h)
        h(0x0Fu | st << 8);
}
static void midi_off(int t)
{
    if (vo[t].midi_note >= 0) {
        midi_send(0x80u | ((mn_midi_base + (uint32_t)t) & 15u), (uint32_t)vo[t].midi_note, 0);
        vo[t].midi_note = -1;
    }
}

/* =============================================================== wavetables === */
static void wt_init(void)
{
    int k, i, h;
    for (k = 0; k < WT_K; k++)
        for (i = 0; i < (int)WT_N; i++) {
            float ph = (float)i / (float)WT_N, s = 0.0f;
            switch (k) {
            case 0: s = fm_sin_turns(ph); break;
            case 1:                                       /* triangle: odd harmonics, alternating, 1/h^2 */
                for (h = 1; h < 32; h += 2)
                    s += ((h >> 1) & 1 ? -1.0f : 1.0f) * fm_sin_turns(ph * (float)h) / (float)(h * h);
                s *= 0.81f;
                break;
            case 2:                                       /* saw, 32 harmonics */
                for (h = 1; h <= 32; h++)
                    s += fm_sin_turns(ph * (float)h) / (float)h;
                s *= 0.55f;
                break;
            case 3:                                       /* square */
                for (h = 1; h < 32; h += 2)
                    s += fm_sin_turns(ph * (float)h) / (float)h;
                s *= 0.9f;
                break;
            case 4:                                       /* pulse, 25 % */
                for (h = 1; h <= 32; h++)
                    s += fm_sin_turns((float)h * 0.125f) * fm_cosf(FM_TWO_PI * ph * (float)h - FM_PI * 0.25f * (float)h) / (float)h;
                s *= 0.75f;
                break;
            case 5: {                                     /* organ: drawbars 8', 4', 2 2/3', 2', 1 3/5', 1' */
                static const float H[6] = {1, 2, 3, 4, 5, 8}, A[6] = {1.0f, 0.7f, 0.5f, 0.45f, 0.25f, 0.3f};
                for (h = 0; h < 6; h++)
                    s += A[h] * fm_sin_turns(ph * H[h]);
                s *= 0.36f;
                break;
            }
            case 6:                                       /* vowel: harmonics under two formants */
                for (h = 1; h <= 24; h++) {
                    float f = (float)h, a = fm_expf(-(f - 3.0f) * (f - 3.0f) * 0.35f) + 0.6f * fm_expf(-(f - 9.0f) * (f - 9.0f) * 0.15f);
                    s += a * fm_sin_turns(ph * f);
                }
                s *= 0.55f;
                break;
            default:                                      /* metal: sparse upper harmonics */
                for (h = 1; h <= 31; h += 3)
                    s += (h % 2 ? 1.0f : -0.8f) * fm_sin_turns(ph * (float)h) / fm_sqrtf((float)h);
                s *= 0.35f;
                break;
            }
            wt[k][i] = s;
        }
    for (k = 0; k < WT_K; k++)
        wt[k][WT_N] = wt[k][0];
}

/* ================================================================ machines === */
static inline float blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}
static inline float saw_blep(float ph, float dt) { return 2.0f * ph - 1.0f - blep(ph, dt); }
static inline float wrap1(float x) { return x >= 1.0f ? x - 1.0f : x; }
static inline float pulse_blep(float ph, float dt, float pw)
{
    float p2 = ph + 1.0f - pw;
    p2 = wrap1(p2);
    return saw_blep(ph, dt) - saw_blep(p2, dt) + 2.0f * pw - 1.0f;
}
static inline float crush(float x, float bits)      /* bits 0..127: off .. 3 bits */
{
    float lv;
    if (bits < 1.0f)
        return x;
    lv = fm_exp2f(12.0f - bits * (9.0f / 127.0f));
    return fm_floorf(x * lv + 0.5f) / lv;
}
static inline float onepole_k(float hz)
{
    float k = 1.0f - fm_expf(-FM_TWO_PI * hz / MN_SR);
    return k > 1.0f ? 1.0f : k;
}

/* a note starts (not on a slide): what the machines restart */
static void m_note_on(voice_t *v, int machine)
{
    int i;
    v->m_env1 = 1.0f;
    v->m_env2 = 0.0f;
    v->m_env3 = 1.0f;
    v->m_pe = 1.0f;
    v->m_cnt = 0;
    switch (machine) {
    case MC_DRUM:
        for (i = 0; i < 8; i++)
            v->ph[i] = 0.0f;
        v->m_lp = v->m_bp = 0.0f;
        break;
    case MC_SINE:
        v->ph[0] = v->ph[1] = v->ph[2] = 0.0f;
        break;
    case MC_FMDYN:
    case MC_FM:
        v->ph[0] = v->ph[1] = v->ph[2] = 0.0f;
        v->aux[0] = v->aux[1] = 0.0f;
        break;
    case MC_PLUCK: {
        /* fill the string: a burst (noise, or a sharp pluck), combed at the pick, through the tone */
        float f = 440.0f * fm_exp2f((v->pitch + (v->e[0] - 64.0f) + (v->e[1] - 64.0f) / 64.0f - 69.0f) * (1.0f / 12.0f));
        float len, burst, exc, pick, k, s = 0.0f, prev[64];
        int n, pk, j;
        if (f < 22.0f)
            f = 22.0f;
        len = MN_SR / f;
        if (len > (float)(PLK_N - 4))
            len = (float)(PLK_N - 4);
        n = (int)len;
        burst = 0.08f + 0.92f * v->e[6] * (1.0f / 127.0f);
        exc = v->e[5] * (1.0f / 127.0f);
        pick = v->e[4] * (1.0f / 127.0f) * 0.5f;
        pk = (int)(pick * len);
        if (pk > 63)
            pk = 63;
        k = 0.15f + 0.85f * v->e[3] * (1.0f / 127.0f);
        for (j = 0; j < 64; j++)
            prev[j] = 0.0f;
        for (j = 0; j < n; j++) {
            float pos = (float)j / len, x, w;
            x = (1.0f - exc) * rnds() + exc * (1.0f - 2.0f * pos);
            w = pos < burst ? 1.0f : 0.0f;
            x *= w;
            s += (x - s) * k;
            prev[j & 63] = s;
            x = pk ? s - prev[(j - pk) & 63] * 0.9f : s;
            plk_buf[v - vo][(v->plk_w + PLK_N - (uint32_t)n + (uint32_t)j) % PLK_N] += x * 0.7f;   /* what is read next */
        }
        break;
    }
    default:
        break;
    }
}

/* one chunk of the machine's oscillator into out[n]; f0 in Hz */
static void m_render(voice_t *v, int machine, float f0, float *out, uint32_t n)
{
    const float *e = v->e;
    uint32_t i;
    float nyq = MN_SR * 0.45f;
    if (f0 > nyq)
        f0 = nyq;
    switch (machine) {
    case MC_SAW: {
        int nv = 1 + (int)(e[3] + 0.5f), j;
        float dt[7], det = e[2] * (1.0f / 127.0f), sub = e[4] * (1.0f / 127.0f), noi = e[5] * (1.0f / 127.0f) * 0.6f;
        float drf = e[6] * (1.0f / 127.0f), norm = fm_rsqrtf((float)nv), bk = onepole_k(150.0f * fm_exp2f(e[7] * (7.0f / 127.0f)));
        float dsub = f0 * 0.5f / MN_SR;
        if (nv > 7)
            nv = 7;
        for (j = 0; j < nv; j++) {
            float off = nv > 1 ? (float)(2 * j - (nv - 1)) / (float)(nv - 1) : 0.0f, cents;
            v->drift[j] += rnds() * 0.02f;
            v->drift[j] *= 0.995f;
            cents = off * det * det * 70.0f + v->drift[j] * drf * 40.0f;
            dt[j] = f0 * fm_exp2f(cents * (1.0f / 1200.0f)) / MN_SR;
            if (dt[j] > 0.45f)
                dt[j] = 0.45f;
        }
        for (i = 0; i < n; i++) {
            float s = 0.0f;
            for (j = 0; j < nv; j++) {
                float p = v->ph[j] + dt[j];
                p = wrap1(p);
                v->ph[j] = p;
                s += saw_blep(p, dt[j]);
            }
            s *= norm;
            if (sub > 0.0f) {
                float p = wrap1(v->ph[7] + dsub);
                v->ph[7] = p;
                s += sub * pulse_blep(p, dsub, 0.5f);
            }
            if (noi > 0.0f)
                s += noi * rnds();
            v->m_lp += (s - v->m_lp) * bk;
            out[i] = v->m_lp;
        }
        break;
    }
    case MC_PULSE: {
        float rate = 0.05f * fm_exp2f(e[4] * (8.0f / 127.0f)), pw, dt = f0 / MN_SR;
        float sub = e[5] * (1.0f / 127.0f), noi = e[7] * (1.0f / 127.0f) * 0.6f, det = e[6] * (1.0f / 127.0f);
        float dt2 = dt * fm_exp2f(det * det * 60.0f / 1200.0f), dsub = dt * 0.5f;
        v->aux[0] = wrap1(v->aux[0] + rate * (float)n / MN_SR);
        pw = 0.5f + e[2] * (0.45f / 127.0f) + fm_sin_turns(v->aux[0]) * e[3] * (0.4f / 127.0f);
        pw = fm_clampf(pw, 0.04f, 0.96f);
        if (dt > 0.45f)
            dt = 0.45f;
        if (dt2 > 0.45f)
            dt2 = 0.45f;
        for (i = 0; i < n; i++) {
            float p = wrap1(v->ph[0] + dt), s;
            v->ph[0] = p;
            s = pulse_blep(p, dt, pw);
            if (det > 0.0f) {
                float p2 = wrap1(v->ph[1] + dt2);
                v->ph[1] = p2;
                s = (s + pulse_blep(p2, dt2, 1.0f - pw)) * 0.7f;
            }
            if (sub > 0.0f) {
                float p3 = wrap1(v->ph[7] + dsub);
                v->ph[7] = p3;
                s += sub * pulse_blep(p3, dsub, 0.5f);
            }
            if (noi > 0.0f)
                s += noi * rnds();
            out[i] = s * 0.8f;
        }
        break;
    }
    case MC_ENS: {
        const int8_t *ch = CHORD[(int)(e[2] + 0.5f) % MN_NCHORD];
        float dt[4], w = e[3] * (1.0f / 127.0f), det = e[4] * (1.0f / 127.0f), spr = e[5] * (1.0f / 127.0f);
        float bk = onepole_k(150.0f * fm_exp2f(e[6] * (7.0f / 127.0f))), sub = e[7] * (1.0f / 127.0f);
        float dsub = f0 * 0.5f / MN_SR;
        int nv = 0, j;
        for (j = 0; j < 4 && ch[j] >= 0; j++) {
            float semi = (float)ch[j] + 12.0f * fm_floorf(spr * 2.0f * (float)j / 3.0f + 0.001f);
            float cents = (j & 1 ? 1.0f : -1.0f) * det * (6.0f + 5.0f * (float)j) + v->drift[j] * 6.0f;
            v->drift[j] += rnds() * 0.02f;
            v->drift[j] *= 0.995f;
            dt[j] = f0 * fm_exp2f(semi * (1.0f / 12.0f) + cents * (1.0f / 1200.0f)) / MN_SR;
            if (dt[j] > 0.45f)
                dt[j] = 0.45f;
            nv++;
        }
        for (i = 0; i < n; i++) {
            float s = 0.0f;
            for (j = 0; j < nv; j++) {
                float p = wrap1(v->ph[j] + dt[j]), x = saw_blep(p, dt[j]);
                v->ph[j] = p;
                if (w > 0.0f)
                    x += w * (pulse_blep(p, dt[j], 0.5f) - x);
                s += x;
            }
            s *= 0.5f;
            if (sub > 0.0f) {
                float p = wrap1(v->ph[7] + dsub);
                v->ph[7] = p;
                s += sub * fm_sin_turns(p);
            }
            v->m_lp += (s - v->m_lp) * bk;
            out[i] = v->m_lp;
        }
        break;
    }
    case MC_SID: {
        int wave = (int)(e[2] + 0.5f) & 7;
        float o2 = (e[5] - 64.0f) * (24.0f / 64.0f), ring = e[6] * (1.0f / 127.0f), bits = e[7];
        uint32_t inc = (uint32_t)(f0 / MN_SR * 4294967296.0f), inc2 = (uint32_t)(f0 * fm_exp2f(o2 * (1.0f / 12.0f)) / MN_SR * 4294967296.0f);
        uint32_t pw12;
        float pw;
        v->aux[0] = wrap1(v->aux[0] + 0.7f * (float)n / MN_SR);
        pw = e[3] * (1.0f / 127.0f) + fm_sin_turns(v->aux[0]) * e[4] * (0.45f / 127.0f);
        pw = fm_clampf(pw, 0.02f, 0.98f);
        pw12 = (uint32_t)(pw * 4095.0f);
        for (i = 0; i < n; i++) {
            uint32_t a0 = v->acc, a = a0 + inc, saw, tri, pul, noi, w12;
            float s;
            v->acc = a;
            v->acc2 += inc2;
            if ((a ^ a0) & 0x00080000u & a) {            /* bit 19 rises: the noise shifts */
                uint32_t l = v->lfsr ? v->lfsr : 0x7FFFF8u, b = ((l >> 22) ^ (l >> 17)) & 1u;
                v->lfsr = ((l << 1) | b) & 0x7FFFFFu;
            }
            saw = a >> 20;
            tri = (((a & 0x80000000u) ? ~a : a) >> 19) & 0xFFFu;
            pul = saw >= pw12 ? 0xFFFu : 0u;
            noi = ((v->lfsr >> 11) & 0xFFFu);
            switch (wave) {
            case 0: w12 = tri; break;
            case 1: w12 = saw; break;
            case 2: w12 = pul; break;
            case 3: w12 = noi; break;
            case 4: w12 = tri & saw; break;
            case 5: w12 = pul & saw; break;
            case 6: w12 = pul & tri; break;
            default: w12 = pul & tri & saw; break;
            }
            s = (float)w12 * (1.0f / 2048.0f) - 1.0f;
            if (ring > 0.0f) {
                float r = (v->acc2 & 0x80000000u) ? -s : s;   /* the SID's ring: osc 2 flips osc 1 */
                s += ring * (r - s);
            }
            out[i] = crush(s, bits) * 0.8f;
        }
        break;
    }
    case MC_FM: {
        float r2 = RATIO[(int)(e[2] + 0.5f) % 24], r3 = RATIO[(int)(e[4] + 0.5f) % 24];
        float i1 = e[3] * e[3] * (1.6f / (127.0f * 127.0f)), i2 = e[5] * e[5] * (1.6f / (127.0f * 127.0f));
        float fb = e[6] * (0.25f / 127.0f), d1 = f0 / MN_SR, d2 = d1 * r2, d3 = d1 * r3;
        int algo = (int)(e[7] + 0.5f) & 3;
        for (i = 0; i < n; i++) {
            float p1 = v->ph[0] + d1, p2 = v->ph[1] + d2, p3 = v->ph[2] + d3, m3, m2, s;
            p1 -= fm_floorf(p1);
            p2 -= fm_floorf(p2);
            p3 -= fm_floorf(p3);
            v->ph[0] = p1;
            v->ph[1] = p2;
            v->ph[2] = p3;
            m3 = fm_sin_turns(p3 + fb * (v->aux[0] + v->aux[1]));
            v->aux[1] = v->aux[0];
            v->aux[0] = m3;
            switch (algo) {
            case 0: m2 = fm_sin_turns(p2 + i2 * m3); s = fm_sin_turns(p1 + i1 * m2); break;
            case 1: m2 = fm_sin_turns(p2); s = fm_sin_turns(p1 + i1 * m2 + i2 * m3); break;
            case 2: s = 0.6f * (fm_sin_turns(p1 + i2 * m3) + i1 * 0.6f * fm_sin_turns(p2 + i2 * m3)); break;
            default: s = 0.5f * (fm_sin_turns(p1) + i1 * 0.6f * fm_sin_turns(p2) + i2 * 0.6f * m3); break;
            }
            out[i] = s * 0.8f;
        }
        break;
    }
    case MC_FMDYN: {
        float r = RATIO[(int)(e[2] + 0.5f) % 24], base = e[3] * e[3] * (1.3f / (127.0f * 127.0f));
        float amt = (e[6] - 64.0f) * (1.3f / 64.0f), fb = e[7] * (0.25f / 127.0f), d1 = f0 / MN_SR, d2 = d1 * r;
        float ak = 1.0f / (env_time(e[4]) * MN_SR), dk = fm_expf(-6.9f / (env_time(e[5]) * MN_SR) * (float)n);
        float idx;
        /* the index envelope: attack, then decay (per chunk) */
        if (v->m_env1 > 0.5f) {                           /* attacking */
            v->m_env2 += ak * (float)n;
            if (v->m_env2 >= 1.0f) {
                v->m_env2 = 1.0f;
                v->m_env1 = 0.0f;
            }
        } else {
            v->m_env2 *= dk;
        }
        idx = base + amt * v->m_env2;
        if (idx < 0.0f)
            idx = 0.0f;
        for (i = 0; i < n; i++) {
            float p1 = v->ph[0] + d1, p2 = v->ph[1] + d2, m;
            p1 -= fm_floorf(p1);
            p2 -= fm_floorf(p2);
            v->ph[0] = p1;
            v->ph[1] = p2;
            m = fm_sin_turns(p2 + fb * v->aux[0]);
            v->aux[0] = m;
            out[i] = fm_sin_turns(p1 + idx * m) * 0.8f;
        }
        break;
    }
    case MC_WAVE: {
        float dk = fm_expf(-6.9f / (env_time(e[4]) * MN_SR) * (float)n), pos, fr, fold = e[5] * (5.0f / 127.0f);
        float sync = 1.0f + e[7] * (7.0f / 127.0f), dt = f0 / MN_SR, bits = e[6];
        int k0;
        pos = e[2] * (7.0f / 127.0f) + v->m_env3 * (e[3] - 64.0f) * (7.0f / 64.0f);
        v->m_env3 *= dk;
        pos = fm_clampf(pos, 0.0f, 6.999f);
        k0 = (int)pos;
        fr = pos - (float)k0;
        for (i = 0; i < n; i++) {
            float p = wrap1(v->ph[0] + dt), sp, x, a, b;
            uint32_t ix;
            v->ph[0] = p;
            sp = p * sync;
            sp -= fm_floorf(sp);
            x = sp * (float)WT_N;
            ix = (uint32_t)x;
            x -= (float)ix;
            ix &= WT_N - 1u;
            a = wt[k0][ix] + (wt[k0][ix + 1] - wt[k0][ix]) * x;
            b = wt[k0 + 1][ix] + (wt[k0 + 1][ix + 1] - wt[k0 + 1][ix]) * x;
            a += (b - a) * fr;
            if (fold > 0.0f)
                a = fm_sinf(a * (1.0f + fold) * (FM_PI * 0.5f));
            out[i] = crush(a, bits) * 0.85f;
        }
        break;
    }
    case MC_DRUM: {
        int type = (int)(e[1] + 0.5f) & 7;
        float tr = f0 * (1.0f / 261.6256f);            /* the note (and TUNE) over C4 */
        float pdk = fm_expf(-6.9f / ((0.004f + e[2] * e[2] * (0.25f / 16129.0f)) * MN_SR));
        float pamt = e[3] * (1.0f / 127.0f), noi = e[4] * (1.0f / 127.0f), snap = e[5] * (1.0f / 127.0f);
        float tone = e[6] * (1.0f / 127.0f), dcy = e[7] * (1.0f / 127.0f), dk, nk;
        float t_dec;
        switch (type) {
        case 2: t_dec = 0.02f + dcy * dcy * 0.25f; break;              /* closed hat */
        case 3: t_dec = 0.12f + dcy * dcy * 1.6f; break;               /* open hat */
        case 4: t_dec = 0.05f + dcy * dcy * 0.6f; break;               /* clap */
        case 6: t_dec = 0.01f + dcy * dcy * 0.12f; break;              /* rim */
        default: t_dec = 0.05f + dcy * dcy * 1.8f; break;
        }
        dk = fm_expf(-6.9f / (t_dec * MN_SR));
        nk = fm_expf(-6.9f / ((t_dec * 0.6f + 0.01f) * MN_SR));
        {
            /* the noise filter (a state-variable band/high pass): its frequency and damping per type */
            float fc, q = 0.6f;
            switch (type) {
            case 1: fc = 1500.0f + tone * 7000.0f; break;
            case 2: case 3: fc = 6000.0f + tone * 7000.0f; break;
            case 4: fc = 900.0f + tone * 1600.0f; q = 0.25f; break;
            case 7: fc = 1800.0f + tone * 1200.0f; q = 0.3f; break;
            default: fc = 300.0f + tone * 3000.0f; break;
            }
            fc *= fm_sqrtf(tr);
            if (fc > MN_SR * 0.22f)
                fc = MN_SR * 0.22f;
            v->aux[6] = 2.0f * fm_sinf(FM_PI * fc / MN_SR);
            v->aux[7] = q;
        }
        for (i = 0; i < n; i++) {
            float s = 0.0f, nz = rnds(), f, hp, pe = v->m_pe, a = v->m_env1, ne = v->m_env3;
            float fk = v->aux[6], q = v->aux[7];
            uint32_t c = v->m_cnt++;
            /* the noise through the state-variable filter (Chamberlin) */
            hp = nz - v->m_lp - q * v->m_bp;
            v->m_bp += fk * hp;
            v->m_lp += fk * v->m_bp;
            switch (type) {
            case 0:                                       /* bass drum: a sine falling from up high */
                f = 48.0f * tr * (1.0f + pamt * 7.0f * pe);
                v->ph[0] = wrap1(v->ph[0] + f / MN_SR);
                s = fm_sin_turns(v->ph[0]) * a;
                if (tone > 0.0f)
                    s = fm_tanhf(s * (1.0f + tone * 5.0f)) * (1.0f - tone * 0.35f);
                s += noi * 0.4f * v->m_lp * ne;
                break;
            case 1:                                       /* snare: two tones and the wires */
                f = 180.0f * tr * (1.0f + pamt * 1.5f * pe);
                v->ph[0] = wrap1(v->ph[0] + f / MN_SR);
                v->ph[1] = wrap1(v->ph[1] + f * 1.78f / MN_SR);
                s = (fm_sin_turns(v->ph[0]) + 0.6f * fm_sin_turns(v->ph[1])) * 0.75f * a * (1.0f - noi * 0.5f);
                s += noi * 1.9f * (nz - v->m_lp) * ne;
                break;
            case 2: case 3: {                             /* hats: six squares, metal, through the high pass */
                static const float HF[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
                float m = 0.0f;
                int j;
                for (j = 0; j < 6; j++) {
                    v->ph[j] = wrap1(v->ph[j] + HF[j] * 1.7f * tr / MN_SR);
                    m += v->ph[j] < 0.5f ? 1.0f : -1.0f;
                }
                m *= 0.16f;
                m = m * (1.0f - noi) + nz * noi;
                v->aux[0] += (m - v->aux[0]) * 0.55f;
                s = (m - v->aux[0] - 0.4f * v->aux[1]) * a * 3.2f;
                v->aux[1] = v->aux[1] * 0.7f + s * 0.3f;
                break;
            }
            case 4: {                                     /* clap: three quick bursts, then the tail */
                float burst = c < 1300u ? (c % 440u < 220u ? 1.0f : 0.25f) : 1.0f;
                s = v->m_bp * 2.2f * ne * burst + v->m_bp * 0.8f * a * (c >= 1300u ? 1.0f : 0.0f);
                break;
            }
            case 5:                                       /* tom */
                f = 110.0f * tr * (1.0f + pamt * 2.0f * pe);
                v->ph[0] = wrap1(v->ph[0] + f / MN_SR);
                s = fm_sin_turns(v->ph[0]) * a + noi * 0.3f * v->m_lp * ne;
                break;
            case 6:                                       /* rim */
                f = 1700.0f * tr * (1.0f + pamt * 0.5f * pe);
                v->ph[0] = wrap1(v->ph[0] + f / MN_SR);
                v->ph[1] = wrap1(v->ph[1] + f * 0.29f / MN_SR);
                s = (fm_sin_turns(v->ph[0]) * 0.6f + fm_sin_turns(v->ph[1]) * 0.6f) * a;
                break;
            default: {                                    /* bell: two squares, as the cowbell */
                float x;
                v->ph[0] = wrap1(v->ph[0] + 587.0f * tr / MN_SR);
                v->ph[1] = wrap1(v->ph[1] + 845.0f * tr / MN_SR);
                x = (v->ph[0] < 0.5f ? 0.5f : -0.5f) + (v->ph[1] < 0.5f ? 0.5f : -0.5f);
                v->aux[0] += (x - v->aux[0]) * 0.3f;
                s = (v->aux[0] - v->aux[1]) * a * 1.5f;
                v->aux[1] += (v->aux[0] - v->aux[1]) * 0.05f;
                s += noi * 0.2f * v->m_bp * ne;
                break;
            }
            }
            if (c < 160u && snap > 0.0f)                  /* the click */
                s += snap * (c < 40u ? 0.9f : 0.3f) * nz * (1.0f - (float)c * (1.0f / 160.0f));
            v->m_pe = pe * pdk;
            v->m_env1 = a * dk;
            v->m_env3 = ne * nk;
            out[i] = s;
        }
        break;
    }
    case MC_NOISE: {
        float colr = (e[1] - 64.0f) * (1.0f / 64.0f), sh = e[2], bits = e[3], dens = e[4] * (1.0f / 127.0f);
        float ring = e[5] * (1.0f / 127.0f), sine = e[6] * (1.0f / 127.0f), reso = e[7] * (1.0f / 127.0f);
        float shr = sh > 0.5f ? f0 * fm_exp2f(4.0f - sh * (8.0f / 127.0f)) / MN_SR : 0.0f;
        float ck = colr < 0.0f ? onepole_k(18000.0f * fm_exp2f(colr * 9.0f)) : onepole_k(30.0f * fm_exp2f(colr * 9.0f));
        float dt = f0 / MN_SR, fk = 2.0f * fm_sinf(FM_PI * fm_minf(f0, MN_SR * 0.2f) / MN_SR), q = 1.0f - reso * 0.97f;
        float dp = dens * dens * dens;
        for (i = 0; i < n; i++) {
            float x;
            if (dens >= 0.999f)
                x = rnds();
            else
                x = rndu() < dp * 0.5f + 0.0005f ? rnds() * 1.5f : 0.0f;   /* dust */
            if (shr > 0.0f) {                              /* sample and hold at a rate that follows the pitch */
                v->aux[0] += shr;
                if (v->aux[0] >= 1.0f) {
                    v->aux[0] -= fm_floorf(v->aux[0]);
                    v->aux[1] = x;
                }
                x = v->aux[1];
            }
            v->m_lp += (x - v->m_lp) * ck;
            x = colr < 0.0f ? v->m_lp : (colr > 0.0f ? x - v->m_lp : x);
            v->ph[0] = wrap1(v->ph[0] + dt);
            if (ring > 0.0f || sine > 0.0f) {
                float sn = fm_sin_turns(v->ph[0]);
                x += ring * (x * sn * 1.5f - x);
                x += sine * sn;
            }
            if (reso > 0.0f) {                             /* through a resonant band pass at the pitch */
                float hp = x - v->aux[3] - q * v->aux[2];
                v->aux[2] += fk * hp;
                v->aux[3] += fk * v->aux[2];
                x += reso * (v->aux[2] * (0.5f + reso * 1.5f) - x);
            }
            out[i] = crush(x, bits) * 0.7f;
        }
        break;
    }
    case MC_PLUCK: {
        float len = MN_SR / (f0 < 22.0f ? 22.0f : f0), t60 = 0.15f * fm_exp2f(e[2] * (6.5f / 127.0f));
        float g = fm_exp2f(-9.97f / (t60 * (MN_SR / len))), k = 0.2f + 0.8f * e[3] * (1.0f / 127.0f);
        float c = e[7] * (0.6f / 127.0f), *buf = plk_buf[v - vo];
        if (len > (float)(PLK_N - 4))
            len = (float)(PLK_N - 4);
        len -= 0.5f + (1.0f - k) * 0.6f;                   /* the loop filter's delay, roughly */
        if (len < 2.0f)
            len = 2.0f;
        for (i = 0; i < n; i++) {
            float rp = (float)v->plk_w - len, fr, y;
            uint32_t ri;
            rp += (float)PLK_N;
            ri = (uint32_t)rp;
            fr = rp - (float)ri;
            y = buf[ri % PLK_N] + (buf[(ri + 1u) % PLK_N] - buf[ri % PLK_N]) * fr;
            v->plk_s += (y - v->plk_s) * k;
            y = v->plk_s;
            if (c > 0.0f) {                                /* stiffness: an all-pass in the loop */
                float ap = -c * y + v->plk_ap_x + c * v->plk_ap_y;
                v->plk_ap_x = y;
                v->plk_ap_y = ap;
                y = ap;
            }
            y = fm_flush(y * g);
            buf[v->plk_w % PLK_N] = y;
            v->plk_w++;
            out[i] = y;
        }
        v->plk_w %= PLK_N;
        break;
    }
    default: {                                            /* MC_SINE */
        float fold = e[2] * (5.0f / 127.0f), drv = e[3] * (10.0f / 127.0f), sub = e[4] * (1.0f / 127.0f);
        float oc2 = e[5] * (1.0f / 127.0f), pdk = fm_expf(-6.9f / ((0.004f + e[6] * e[6] * (0.4f / 16129.0f)) * MN_SR) * (float)n);
        float pamt = e[7] * (48.0f / 127.0f), f = f0 * fm_exp2f(v->m_pe * pamt * (1.0f / 12.0f)), dt, dn;
        v->m_pe *= pdk;
        if (f > nyq)
            f = nyq;
        dt = f / MN_SR;
        dn = drv > 0.0f ? 1.0f / fm_tanhf(1.0f + drv) : 1.0f;
        for (i = 0; i < n; i++) {
            float p = wrap1(v->ph[0] + dt), s;
            v->ph[0] = p;
            s = fm_sin_turns(p);
            if (sub > 0.0f) {
                v->ph[1] = wrap1(v->ph[1] + dt * 0.5f);
                s += sub * fm_sin_turns(v->ph[1]);
            }
            if (oc2 > 0.0f)
                s += oc2 * fm_sin_turns(p * 2.0f);
            if (fold > 0.0f)
                s = fm_sinf(s * (1.0f + fold) * (FM_PI * 0.5f));
            if (drv > 0.0f)
                s = fm_tanhf(s * (1.0f + drv)) * dn;
            out[i] = s * 0.8f;
        }
        break;
    }
    }
}

/* ================================================================= voices === */
static void track_eval(int t);
static void env_times(voice_t *v)
{
    const float *e = v->e;
    v->a_inc = 1.0f / (env_time(e[P_ATK]) * MN_SR);
    v->d_k = fm_expf(-6.9f / (env_time(e[P_DEC]) * MN_SR));
    v->r_k = fm_expf(-6.9f / (env_time(e[P_REL]) * MN_SR));
}

static void lfo_trig(voice_t *v)
{
    int k;
    for (k = 0; k < 3; k++) {
        lfo_t *l = &v->lfo[k];
        int mode = (int)base_val((int)(v - vo), P_LFO1 + 8 * k + L_TRIG);
        float ph0 = base_val((int)(v - vo), P_LFO1 + 8 * k + L_PHAS) * (1.0f / 128.0f);
        if (mode == LT_TRIG || mode == LT_ONE || mode == LT_HALF) {
            l->ph = ph0;
            l->run = 1;
            l->rv = rnds();
        } else if (mode == LT_HOLD) {
            l->hold = l->val;
        }
    }
}

/* a note: slide = legato (no envelope restart), gate in samples (0: until a note off) */
static void v_trigger(int t, float note, float vel, int slide, uint32_t gate)
{
    voice_t *v = &vo[t];
    int m = machine_of(t);
    int sounding = v->stage != 0;
    track_eval(t);                                 /* (the step's locks, a machine just chosen) */
    v->target = note;
    if (slide && sounding) {
        v->glide = v->e[P_PORT] > 0.5f ? env_time(v->e[P_PORT]) * 0.5f : 0.06f;
    } else {
        v->glide = v->e[P_PORT] > 0.5f ? env_time(v->e[P_PORT]) * 0.5f : 0.0f;
        if (v->glide <= 0.0f || !sounding)
            v->pitch = note;
        v->vel = vel;
        v->stage = 1;                                  /* attack, from where it is */
        v->fstage = 1;
        v->fenv = 0.0f;
        v->hold_left = 0;
        m_note_on(v, m);
        lfo_trig(v);
    }
    v->gate = 1;
    v->inf_gate = gate == 0u;
    v->gate_left = gate;
    mn_trig_count[t]++;
}

/* the gate closes: with HOLD at GATE the release starts; otherwise the envelope runs its course
 * (attack, hold time, decay), however short the note */
static void v_gate_off(voice_t *v)
{
    v->gate = 0;
    v->inf_gate = 0;
    if (v->stage && v->stage < 4 && v->e[P_HOLD] >= 126.5f)
        v->stage = 4;
}

/* ============================================================= arpeggiator === */
static void arp_start(int t, int root, float vel, const int8_t *held, int nheld)
{
    voice_t *v = &vo[t];
    int oct = (int)base_val(t, P_ARNG) + 1, o, j, n = 0;
    int8_t base[8];
    int nb = 0;
    if (nheld > 1) {
        for (j = 0; j < nheld && nb < 8; j++)          /* the keys held, low to high */
            base[nb++] = held[j];
        for (j = 1; j < nb; j++) {
            int8_t x = base[j];
            int k = j;
            while (k > 0 && base[k - 1] > x) {
                base[k] = base[k - 1];
                k--;
            }
            base[k] = x;
        }
    } else {
        const int8_t *ch = CHORD[(int)base_val(t, P_ACHD) % MN_NCHORD];
        for (j = 0; j < 4 && ch[j] >= 0; j++)
            base[nb++] = (int8_t)clampi(root + ch[j], 0, 127);
    }
    for (o = 0; o < oct; o++)
        for (j = 0; j < nb && n < 16; j++)
            v->arp_note[n++] = (int8_t)clampi(base[j] + 12 * o, 0, 127);
    v->arp_n = (uint8_t)n;
    v->arp_on = n > 0;
    v->arp_i = 0;
    v->arp_dir = 0;
    v->arp_left = 0.0f;
    v->arp_vel = vel;
}

static void arp_next(int t)
{
    voice_t *v = &vo[t];
    int mode = (int)base_val(t, P_ARPM), n = v->arp_n, idx;
    float len = pat_step_samples() * ASPD_STEPS[(int)base_val(t, P_ASPD) & 7];
    if (!n || mode == ARP_OFF) {
        v->arp_on = 0;
        return;
    }
    switch (mode) {
    case ARP_DOWN: idx = n - 1 - v->arp_i % n; v->arp_i++; break;
    case ARP_UPDN: {
        int per = n > 1 ? 2 * n - 2 : 1, k = v->arp_i % per;
        idx = k < n ? k : per - k;
        v->arp_i++;
        break;
    }
    case ARP_RAND: idx = (int)(rnd() % (uint32_t)n); break;
    default: idx = v->arp_i % n; v->arp_i++; break;
    }
    v_trigger(t, (float)v->arp_note[idx], v->arp_vel, 0, (uint32_t)(len * 0.6f));
    v->arp_len = len;
    v->arp_left += len;
}

/* ================================================================== the LFOs === */
static float lfo_wave(lfo_t *l, int wave, float ph)
{
    switch (wave) {
    case LW_TRI: return ph < 0.25f ? 4.0f * ph : ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f;
    case LW_SIN: return fm_sin_turns(ph);
    case LW_SQR: return ph < 0.5f ? 1.0f : -1.0f;
    case LW_SAW: return 1.0f - 2.0f * ph;
    case LW_RAMP: return 2.0f * ph - 1.0f;
    case LW_EXP: return 2.0f * fm_exp2f(-7.0f * ph) - 1.0f;
    default: return l->rv;
    }
}

/* every track's LFOs, this chunk: their values, and their sums on their targets */
static void lfo_chunk(uint32_t n)
{
    int t, k, p;
    float step = step_samples();
    for (t = 0; t < MN_NT; t++)
        for (p = 0; p < MN_NMOD; p++)
            vo[t].mod[p] = 0.0f;
    for (t = 0; t < MN_NT; t++) {
        voice_t *v = &vo[t];
        for (k = 0; k < 3; k++) {
            lfo_t *l = &v->lfo[k];
            int b = P_LFO1 + 8 * k, dest, trk, wave, mode;
            float spd, mult, cyc, x, depth, out;
            dest = (int)base_val(t, b + L_DEST);
            depth = base_val(t, b + L_DPTH) + v->mod[b + L_DPTH];
            spd = fm_clampf(base_val(t, b + L_SPD) + v->mod[b + L_SPD], 0.0f, 127.0f);
            mult = (float)(1 << ((int)base_val(t, b + L_MULT) & 7));
            wave = (int)base_val(t, b + L_WAVE) % LW_N;
            mode = (int)base_val(t, b + L_TRIG) % LT_N;
            cyc = spd * mult * (1.0f / 2048.0f) / step * (float)n;   /* cycles this chunk */
            if (mode != LT_ONE && mode != LT_HALF)
                l->run = 1;
            if (l->run) {
                float end = mode == LT_HALF ? 0.5f : 1.0f, ph0 = base_val(t, b + L_PHAS) * (1.0f / 128.0f);
                l->ph += cyc;
                if (mode == LT_ONE || mode == LT_HALF) {
                    float done = l->ph - ph0;
                    if (done < 0.0f)
                        done += 1.0f;
                    if (done >= end) {
                        l->run = 0;
                        l->ph = ph0 + end - 0.0001f;
                    }
                }
                if (l->ph >= 1.0f) {
                    l->ph -= fm_floorf(l->ph);
                    l->rv = rnds();
                }
            }
            x = lfo_wave(l, wave, l->ph);
            l->val = x;
            out = mode == LT_HOLD ? l->hold : x;
            mn_lfo_val[t][k] = out;
            if (!dest || dest > MN_NMOD)
                continue;
            trk = (int)base_val(t, b + L_TRK);
            trk = trk ? trk - 1 : t;
            dest -= 1;
            vo[trk].mod[dest] += out * (depth - 64.0f) * (1.0f / 64.0f) * (float)mn_par_info(machine_of(trk), dest)->max;
        }
    }
}

/* ================================================================ the strip === */
static void svf_coef(float fc, float q01, float *a1, float *a2, float *a3, float *k)
{
    float g = fm_tanf(FM_PI * fc / MN_SR);
    *k = 2.0f - 1.94f * q01;
    *a1 = 1.0f / (1.0f + g * (g + *k));
    *a2 = g * *a1;
    *a3 = g * *a2;
}

/* a track's values now (the kit, its locks, the LFOs); a new machine starts from scratch */
static void track_eval(int t)
{
    voice_t *v = &vo[t];
    int m = machine_of(t), p;
    for (p = 0; p < MN_NMOD; p++) {
        float x = base_val(t, p) + v->mod[p], mx = (float)mn_par_info(m, p)->max;
        v->e[p] = x < 0.0f ? 0.0f : x > mx ? mx : x;
    }
    if (v->machine != m) {
        int i;
        v->machine = (uint8_t)m;
        for (i = 0; i < 8; i++)
            v->ph[i] = v->drift[i] = v->aux[i] = 0.0f;
        v->m_lp = v->m_bp = 0.0f;
        for (i = 0; i < (int)PLK_N; i++)
            plk_buf[t][i] = 0.0f;
    }
}

/* this chunk's values on a track and what follows from them */
static float track_derive(int t, uint32_t n)
{
    voice_t *v = &vo[t];
    int m;
    float *e = v->e, hp, lp, bofs, wofs, f0, semis;
    track_eval(t);
    m = v->machine;
    env_times(v);
    /* pitch: the glide, TUNE (semitones), FINE (a semitone), TRNS on the track */
    if (v->glide > 0.0f && v->pitch != v->target) {
        v->pitch += (v->target - v->pitch) * k_from_time(v->glide, (float)n);
        if (fm_fabsf(v->target - v->pitch) < 0.002f)
            v->pitch = v->target;
    } else {
        v->pitch = v->target;
    }
    semis = v->pitch + (e[0] - 64.0f) + (MACH[m].fine ? (e[1] - 64.0f) * (1.0f / 64.0f) : 0.0f);
    f0 = 440.0f * fm_exp2f((semis - 69.0f) * (1.0f / 12.0f));
    /* the filter envelope, per chunk */
    if (v->fstage == 1) {
        v->fenv += (float)n / (env_time(e[P_FATK]) * MN_SR);
        if (v->fenv >= 1.0f) {
            v->fenv = 1.0f;
            v->fstage = 2;
        }
    } else if (v->fstage == 2) {
        v->fenv *= fm_expf(-6.9f * (float)n / (env_time(e[P_FDEC]) * MN_SR));
        if (v->fenv < 1e-4f) {
            v->fenv = 0.0f;
            v->fstage = 0;
        }
    }
    bofs = (e[P_BOFS] - 64.0f) * (127.0f / 64.0f) * v->fenv;
    wofs = (e[P_WOFS] - 64.0f) * (127.0f / 64.0f) * v->fenv;
    hp = fm_clampf(e[P_BASE] + bofs, 0.0f, 127.0f);
    lp = fm_clampf(hp + fm_clampf(e[P_WDTH] + wofs, 0.0f, 127.0f), 0.0f, 127.0f);
    v->hp_on = hp > 0.5f || e[P_HPQ] > 0.5f;
    if (v->hp_on)
        svf_coef(fm_minf(20.0f * fm_exp2f(hp * (10.0f / 127.0f)), MN_SR * 0.45f), e[P_HPQ] * (1.0f / 127.0f),
                 &v->hg_a1, &v->hg_a2, &v->hg_a3, &v->hk);
    svf_coef(fm_minf(20.0f * fm_exp2f(lp * (10.0f / 127.0f)), MN_SR * 0.45f), e[P_LPQ] * (1.0f / 127.0f),
             &v->lg_a1, &v->lg_a2, &v->lg_a3, &v->lk);
    /* distortion */
    v->drive = 1.0f + e[P_DIST] * (14.0f / 127.0f);
    v->dnorm = e[P_DIST] > 0.5f ? 1.0f / fm_tanhf(fm_minf(v->drive, 3.0f)) : 1.0f;
    /* EQ: a peak, its gain +-18 dB */
    v->eq_on = fm_fabsf(e[P_EQG] - 64.0f) > 0.5f;
    if (v->eq_on && (e[P_EQF] != v->eqf_last || e[P_EQG] != v->eqg_last)) {
        float f = 40.0f * fm_exp2f(e[P_EQF] * (8.6f / 127.0f)), A = fm_db2lin((e[P_EQG] - 64.0f) * (18.0f / 64.0f) * 0.5f);
        float w = FM_TWO_PI * fm_minf(f, MN_SR * 0.45f) / MN_SR, al = fm_sinf(w) * 0.5f / 0.9f, cs = fm_cosf(w), a0;
        a0 = 1.0f + al / A;
        v->eb0 = (1.0f + al * A) / a0;
        v->eb1 = -2.0f * cs / a0;
        v->eb2 = (1.0f - al * A) / a0;
        v->ea1 = -2.0f * cs / a0;
        v->ea2 = (1.0f - al / A) / a0;
        v->eqf_last = e[P_EQF];
        v->eqg_last = e[P_EQG];
    }
    /* sample-rate reduction */
    v->srr_inc = 1.0f / (1.0f + e[P_SRR] * e[P_SRR] * (40.0f / 16129.0f));
    /* volume and pan (equal power) */
    {
        float a = e[P_PAN] * (1.0f / 127.0f) * (FM_PI * 0.5f), vol = e[P_VOL] * (1.0f / 127.0f);
        v->gain = vol * vol * 0.55f;
        v->gl = fm_cosf(a) * 1.41421f;
        v->gr = fm_sinf(a) * 1.41421f;
    }
    /* delay: eighths of a step, as long as the line allows */
    {
        float ds = (e[P_DTIM] + 1.0f) * 0.125f * pat_step_samples() * 0.5f;   /* in half-rate samples */
        if (ds > (float)(DLY_N - 4))
            ds = (float)(DLY_N - 4);
        if (ds < 2.0f)
            ds = 2.0f;
        v->dt_s += (ds - v->dt_s) * (v->dt_s < 1.0f ? 1.0f : 0.05f);
        v->dsend = e[P_DSND] * (1.0f / 127.0f);
        v->dsend *= v->dsend;
        v->dfb = e[P_DFB] * (1.1f / 127.0f);
        v->dhp_k = onepole_k(20.0f * fm_exp2f(e[P_DHP] * (7.0f / 127.0f)) * 2.0f);
        v->dlp_k = onepole_k(400.0f * fm_exp2f(e[P_DLP] * (5.5f / 127.0f)) * 2.0f);
    }
    return f0;
}

/* one track, one chunk: added into l / r */
static void track_chunk(int t, float *l, float *r, uint32_t n)
{
    voice_t *v = &vo[t];
    float osc[BLK], f0, pk = 0.0f;
    uint32_t i;
    int synth = v->stage != 0, dly = v->dsend > 0.0f || v->dquiet < 4000u;
    if (!synth && !dly) {                          /* silent, its echoes gone: nothing to work out */
        mn_level[t] = 0.0f;
        return;
    }
    f0 = track_derive(t, n);
    dly = v->dsend > 0.0f || v->dquiet < 4000u;
    if (synth)
        m_render(v, v->machine, f0, osc, n);
    for (i = 0; i < n; i++) {
        float x = 0.0f, d;
        if (synth) {
            x = osc[i];
            if (v->hp_on) {                             /* the high pass ... */
                float v3 = x - v->h2, v1 = v->hg_a1 * v->h1 + v->hg_a2 * v3, v2 = v->h2 + v->hg_a2 * v->h1 + v->hg_a3 * v3;
                v->h1 = 2.0f * v1 - v->h1;
                v->h2 = 2.0f * v2 - v->h2;
                x = x - v->hk * v1 - v2;
            }
            {                                           /* ... then the low pass */
                float v3 = x - v->l2, v1 = v->lg_a1 * v->l1 + v->lg_a2 * v3, v2 = v->l2 + v->lg_a2 * v->l1 + v->lg_a3 * v3;
                v->l1 = fm_flush(2.0f * v1 - v->l1);
                v->l2 = fm_flush(2.0f * v2 - v->l2);
                x = v2;
            }
            switch (v->stage) {                         /* the amp envelope */
            case 1:
                v->env += v->a_inc;
                if (v->env >= 1.0f) {
                    v->env = 1.0f;
                    v->stage = 2;
                    v->hold_left = (uint32_t)(env_time(v->e[P_HOLD]) * MN_SR);
                }
                break;
            case 2:
                if (v->e[P_HOLD] < 126.5f) {
                    if (v->hold_left)
                        v->hold_left--;
                    else
                        v->stage = 3;
                }
                break;
            case 3: v->env *= v->d_k; break;
            case 4: v->env *= v->r_k; break;
            default: break;
            }
            if (v->stage >= 3 && v->env < 1e-4f) {
                v->env = 0.0f;
                v->stage = 0;
            }
            x *= v->env * v->vel;
            if (v->drive > 1.01f)
                x = fm_tanhf(x * v->drive) * v->dnorm;
            if (v->eq_on) {
                float y = v->eb0 * x + v->eb1 * v->ex1 + v->eb2 * v->ex2 - v->ea1 * v->ey1 - v->ea2 * v->ey2;
                v->ex2 = v->ex1;
                v->ex1 = x;
                v->ey2 = v->ey1;
                v->ey1 = fm_flush(y);
                x = y;
            }
            if (v->srr_inc < 0.999f) {                  /* sample-rate reduction: hold */
                v->srr_ph += v->srr_inc;
                if (v->srr_ph >= 1.0f) {
                    v->srr_ph -= 1.0f;
                    v->srr_hold = x;
                }
                x = v->srr_hold;
            }
            x *= v->gain;
        }
        /* the delay, at half rate: fed every second sample, read in between by interpolation */
        d = 0.0f;
        if (dly) {
            d = v->dold + (v->dnew - v->dold) * ((v->dw & 1u) ? 1.0f : 0.5f);
            v->dacc += x * v->dsend;
            if (v->dw & 1u) {
                float rp = (float)(v->dw >> 1) - v->dt_s + (float)DLY_N, fr, y, in;
                uint32_t ri = (uint32_t)rp;
                int16_t *buf = dly_buf[t];
                fr = rp - (float)ri;
                y = ((float)buf[ri % DLY_N] + ((float)buf[(ri + 1u) % DLY_N] - (float)buf[ri % DLY_N]) * fr) * (1.0f / 16384.0f);
                v->dlp += (y - v->dlp) * v->dlp_k;
                v->dhp += (v->dlp - v->dhp) * v->dhp_k;
                y = v->dlp - v->dhp;
                in = v->dacc * 0.5f + y * v->dfb;
                in = in > 1.0f || in < -1.0f ? fm_tanhf(in) : in;
                buf[(v->dw >> 1) % DLY_N] = (int16_t)(in * 16384.0f);
                v->dacc = 0.0f;
                v->dold = v->dnew;
                v->dnew = y;
                v->dquiet = fm_fabsf(y) + fm_fabsf(in) < 1e-4f ? v->dquiet + 1u : 0u;
            }
            v->dw = (v->dw + 1u) % (2u * DLY_N);
        }
        x += d;
        l[i] += x * v->gl;
        r[i] += x * v->gr;
        x = fm_fabsf(x);
        pk = x > pk ? x : pk;
    }
    mn_level[t] = pk;
}

/* ================================================================ sequencer === */
static struct {
    int8_t step;
    float left;                                    /* samples until the next step (internal clock) */
    uint32_t count;                                /* steps since the start (swing: even and odd) */
    float clk_left;                                /* samples until the next MIDI clock out */
    uint32_t ext_ticks, ext_age;
    float ext_delay;                               /* a swung step waiting (external clock), samples */
    uint8_t ext_pending;
    float tick_samples;                            /* measured, between MIDI clock ticks */
    uint32_t since_tick;
} sq;

static void step_fire(int s)
{
    mn_song_t *sg = cur;
    int t, r;
    if (!sg)
        return;
    for (t = 0; t < MN_NT; t++) {
        voice_t *v = &vo[t];
        uint8_t fl = sg->pat.trig[t][s];
        int nl = 0;
        if (mn_mute >> t & 1u)
            continue;
        for (r = 0; r < MN_NLOCK; r++)
            nl += sg->pat.ltrk[r] == t && sg->pat.lval[r][s] != 0xFF;
        if (fl & MN_TRIG) {
            int note, prob, gate_v, p;
            float vel;
            uint32_t gate;
            for (p = 0; p < MN_NP; p++)
                v->lock[p] = -1;
            for (r = 0; nl && r < MN_NLOCK; r++)
                if (sg->pat.ltrk[r] == t && sg->pat.lval[r][s] != 0xFF && sg->pat.lpar[r] < MN_NP)
                    v->lock[sg->pat.lpar[r]] = sg->pat.lval[r][s];
            prob = (int)base_val(t, P_PROB);
            if (prob < 127 && (int)(rnd() % 127u) >= prob)
                continue;
            note = clampi((int)sg->pat.note[t][s] + (int)base_val(t, P_TRNS) - 64, 0, 127);
            vel = 0.75f;
            if (fl & MN_ACC)
                vel *= 1.0f + base_val(t, P_ACNT) * (0.7f / 127.0f);
            gate_v = (int)base_val(t, P_GATE);
            gate = (uint32_t)(pat_step_samples() * (float)(gate_v + 1) * (1.0f / 16.0f));
            if (gate < 64u)
                gate = 64u;
            if ((int)base_val(t, P_ARPM) != ARP_OFF) {
                arp_start(t, note, vel, 0, 0);
            } else {
                v->arp_on = 0;
                v_trigger(t, (float)note, vel, (fl & MN_SLIDE) != 0, gate);
            }
            if (mn_midi_out_on) {
                midi_off(t);
                midi_send(0x90u | ((mn_midi_base + (uint32_t)t) & 15u), (uint32_t)note, (fl & MN_ACC) ? 120u : 96u);
                v->midi_note = (int8_t)note;
            }
        } else if (nl) {                               /* a trigless lock: the sound changes, no note */
            for (r = 0; r < MN_NLOCK; r++)
                if (sg->pat.ltrk[r] == t && sg->pat.lval[r][s] != 0xFF && sg->pat.lpar[r] < MN_NP)
                    v->lock[sg->pat.lpar[r]] = sg->pat.lval[r][s];
        }
    }
}

static void seq_advance(void)
{
    int len = cur ? cur->pat.len : 16;
    int s = sq.step + 1;
    if (s >= len || s < 0) {
        s = 0;
        if (sq.step >= 0) {
            mn_loops++;
            if (queued) {                              /* the next pattern takes over, on its first step */
                cur = queued;
                queued = 0;
            }
        }
    }
    sq.step = (int8_t)s;
    mn_step = (int8_t)s;
    step_fire(s);
    sq.count++;
}

static void seq_start(void)
{
    sq.step = -1;
    sq.left = 0.0f;
    sq.count = 0;
    sq.clk_left = 0.0f;
    sq.ext_ticks = 0;
    sq.ext_pending = 0;
    mn_playing = 1;
}

static void seq_stop(void)
{
    int t;
    mn_playing = 0;
    mn_step = -1;
    sq.step = -1;
    for (t = 0; t < MN_NT; t++) {
        vo[t].arp_on = 0;
        if (!vo[t].inf_gate)
            v_gate_off(&vo[t]);
        midi_off(t);
    }
    if (queued) {
        cur = queued;
        queued = 0;
    }
}

/* how long until the next step: a pair of steps is split by the swing */
static float step_len(uint32_t count)
{
    float L = pat_step_samples(), sw = (float)(cur ? cur->pat.swing : 50) * 0.01f;
    return (count & 1u) ? 2.0f * L * sw : 2.0f * L * (1.0f - sw);
}

static void seq_chunk(uint32_t n)
{
    if (!mn_playing)
        return;
    if (mn_ext) {                                      /* a swung step waits for its moment */
        if (sq.ext_pending) {
            sq.ext_delay -= (float)n;
            if (sq.ext_delay <= 0.0f) {
                sq.ext_pending = 0;
                seq_advance();
            }
        }
    } else {
        sq.left -= (float)n;
        while (sq.left <= 0.0f) {
            seq_advance();
            sq.left += step_len(sq.count);
        }
        if (mn_clock_out) {                            /* 24 a beat, straight */
            sq.clk_left -= (float)n;
            while (sq.clk_left <= 0.0f) {
                midi_rt(0xF8u);
                sq.clk_left += step_samples() * (1.0f / 6.0f);
            }
        }
    }
    {
        float L = pat_step_samples(), f;
        f = mn_ext ? 0.0f : 1.0f - sq.left / L;
        mn_step_frac = fm_clampf(f, 0.0f, 1.0f);
    }
}

/* gates closing, arpeggios going on: per chunk */
static void gates_chunk(uint32_t n)
{
    int t;
    for (t = 0; t < MN_NT; t++) {
        voice_t *v = &vo[t];
        if (v->arp_on) {
            v->arp_left -= (float)n;
            if (v->arp_left <= 0.0f)
                arp_next(t);
        }
        if (v->gate && !v->inf_gate) {
            if (v->gate_left > n) {
                v->gate_left -= n;
            } else {
                v_gate_off(v);
                if (!v->arp_on)
                    midi_off(t);
            }
        }
    }
}

/* ================================================================= commands === */
static void live_note_on(int t, int note, int vel)
{
    voice_t *v = &vo[t];
    int i, p;
    for (i = 0; i < v->nlive; i++)                     /* already held: to the top */
        if (v->live[i] == note)
            break;
    if (i < v->nlive) {
        for (; i + 1 < v->nlive; i++)
            v->live[i] = v->live[i + 1];
        v->nlive--;
    }
    if (v->nlive == 8) {
        for (i = 0; i < 7; i++)
            v->live[i] = v->live[i + 1];
        v->nlive--;
    }
    v->live[v->nlive++] = (int8_t)note;
    for (p = 0; p < MN_NP; p++)                         /* a key plays the kit's sound */
        v->lock[p] = -1;
    if ((int)base_val(t, P_ARPM) != ARP_OFF) {
        int keep = v->arp_on ? 1 : 0;
        float left = v->arp_left;
        arp_start(t, note, (float)vel * (0.9f / 127.0f), v->live, v->nlive);
        if (keep)
            v->arp_left = left;                        /* a new key joins the arpeggio in time */
        return;
    }
    v_trigger(t, (float)note, (float)vel * (0.9f / 127.0f), v->nlive > 1 && v->gate, 0);
}

static void live_note_off(int t, int note)
{
    voice_t *v = &vo[t];
    int i, top;
    for (i = 0; i < v->nlive && v->live[i] != note; i++)
        ;
    if (i == v->nlive)
        return;
    top = i == v->nlive - 1;
    for (; i + 1 < v->nlive; i++)
        v->live[i] = v->live[i + 1];
    v->nlive--;
    if ((int)base_val(t, P_ARPM) != ARP_OFF && v->arp_on) {
        if (!v->nlive) {
            v->arp_on = 0;
        } else {
            float left = v->arp_left;
            arp_start(t, v->live[0], v->arp_vel, v->live, v->nlive);
            v->arp_left = left;
        }
        return;
    }
    if (!top || !v->inf_gate)
        return;
    if (v->nlive)                                      /* back to the key still held: legato */
        v_trigger(t, (float)v->live[v->nlive - 1], v->vel, 1, 0);
    else
        v_gate_off(v);
}

static float dc_l, dc_r, dcx_l, dcx_r;
static void quiet(void)
{
    int t, i;
    dc_l = dc_r = dcx_l = dcx_r = 0.0f;
    for (t = 0; t < MN_NT; t++) {
        voice_t *v = &vo[t];
        v->stage = 0;
        v->env = 0.0f;
        v->gate = v->inf_gate = 0;
        v->nlive = 0;
        v->arp_on = 0;
        v->h1 = v->h2 = v->l1 = v->l2 = 0.0f;
        v->ex1 = v->ex2 = v->ey1 = v->ey2 = 0.0f;
        v->dacc = v->dold = v->dnew = v->dlp = v->dhp = 0.0f;
        v->dquiet = 100000u;
        for (i = 0; i < (int)DLY_N; i++)
            dly_buf[t][i] = 0;
        for (i = 0; i < (int)PLK_N; i++)
            plk_buf[t][i] = 0.0f;
        midi_off(t);
    }
}

static void drain(void)
{
    while (q_r != q_w) {
        cmd_t c = q[q_r % QN];
        BARRIER();
        q_r++;
        switch (c.c) {
        case C_PLAY:
            if (c.a && !mn_playing) {
                seq_start();
                if (mn_clock_out && !mn_ext)
                    midi_rt(0xFAu);
            } else if (!c.a && mn_playing) {
                seq_stop();
                if (mn_clock_out && !mn_ext)
                    midi_rt(0xFCu);
            }
            break;
        case C_NOTEON: live_note_on(c.t, c.a, c.b); break;
        case C_NOTEOFF: live_note_off(c.t, c.a); break;
        case C_ALLOFF: {
            int t;
            for (t = 0; t < MN_NT; t++) {
                vo[t].nlive = 0;
                vo[t].arp_on = 0;
                v_gate_off(&vo[t]);
                midi_off(t);
            }
            break;
        }
        case C_PANIC: quiet(); break;
        case C_QUEUE:
            if (!mn_playing && c.p) {                   /* stopped: at once */
                cur = (mn_song_t *)c.p;
                queued = 0;
            } else {
                queued = (mn_song_t *)c.p;
            }
            break;
        case C_CLOCK:
            if (!mn_clock_in)
                break;
            sq.ext_age = 0;
            if (c.a == MN_CLK_TICK) {
                static const uint8_t TPS[MN_NSPEED] = {12, 8, 6, 4, 3};
                uint32_t tps = TPS[cur ? cur->pat.speed % MN_NSPEED : 2];
                if (sq.since_tick > 0u && sq.since_tick < 8820u)   /* (a gap of 200 ms is not a tempo) */
                    sq.tick_samples += ((float)sq.since_tick - sq.tick_samples) * 0.2f;
                sq.since_tick = 0;
                mn_ext = 1;
                if (mn_playing) {
                    if (sq.ext_ticks % tps == 0u) {
                        if (sq.count & 1u) {             /* an odd step: later by the swing */
                            float sw = (float)(cur ? cur->pat.swing : 50) * 0.01f - 0.5f;
                            sq.ext_delay = sw * 2.0f * sq.tick_samples * (float)tps;
                            sq.ext_pending = 1;
                            if (sq.ext_delay < 1.0f) {
                                sq.ext_pending = 0;
                                seq_advance();
                            }
                        } else {
                            if (sq.ext_pending) {         /* (never two steps at once) */
                                sq.ext_pending = 0;
                                seq_advance();
                            }
                            seq_advance();
                        }
                    }
                    sq.ext_ticks++;
                }
            } else if (c.a == MN_CLK_START) {
                seq_start();
                mn_ext = 1;
                sq.since_tick = 0xFFFFFFu;                 /* the next tick is not measured against a gap */
            } else if (c.a == MN_CLK_CONTINUE) {
                mn_playing = 1;
                mn_ext = 1;
            } else {
                seq_stop();
            }
            break;
        default: break;
        }
    }
}

/* ================================================================== render === */
void mn_init(void)
{
    int t, p, k;
    q_w = q_r = 0;
    for (t = 0; t < MN_NT; t++) {
        voice_t *v = &vo[t];
        uint8_t *z = (uint8_t *)v;
        uint32_t i;
        for (i = 0; i < sizeof *v; i++)
            z[i] = 0;
        for (p = 0; p < MN_NP; p++)
            v->lock[p] = -1;
        v->midi_note = -1;
        v->machine = 0xFF;
        v->pitch = v->target = 60.0f;
        v->eqf_last = -1.0f;
        v->dquiet = 100000u;
        v->lfsr = 0x7FFFF8u;
        for (k = 0; k < 3; k++)
            v->lfo[k].run = 1;
    }
    mn_playing = 0;
    mn_ext = 0;
    mn_step = -1;
    sq.step = -1;
    sq.tick_samples = MN_SR * 60.0f / (120.0f * 24.0f);
    wt_init();
    quiet();
}

void mn_render(int32_t *out, uint32_t n, uint32_t gain_q12)
{
    const float gain = (float)gain_q12 * (1.0f / 4096.0f) * 1.6f;
    int32_t *const out0 = out, pk = 0;
    const uint32_t n0 = n;
    uint32_t j;
    drain();
    if (mn_ext && ++sq.ext_age > 86u) {                /* ~0.5 s without a tick: back to its own tempo */
        mn_ext = 0;
        sq.left = 0.0f;
    }
    while (n) {
        uint32_t k = n > BLK ? BLK : n, i;
        float l[BLK], r[BLK];
        int t;
        sq.since_tick += k;
        if (cur) {
            seq_chunk(k);
            gates_chunk(k);
            lfo_chunk(k);
        }
        for (i = 0; i < k; i++)
            l[i] = r[i] = 0.0f;
        if (cur)
            for (t = 0; t < MN_NT; t++)
                track_chunk(t, l, r, k);
        for (i = 0; i < k; i++) {                       /* a DC blocker, the gain, a soft clip */
            float a = l[i], b = r[i], ya = a - dcx_l + 0.9995f * dc_l, yb = b - dcx_r + 0.9995f * dc_r;
            dcx_l = a;
            dcx_r = b;
            dc_l = fm_flush(ya);
            dc_r = fm_flush(yb);
            out[2 * i] = (int32_t)(fm_tanhf(ya * gain) * 8300000.0f);
            out[2 * i + 1] = (int32_t)(fm_tanhf(yb * gain) * 8300000.0f);
        }
        out += 2 * k;
        n -= k;
    }
    for (j = 0; j < 2u * n0; j++) {
        int32_t a = out0[j] < 0 ? -out0[j] : out0[j];
        pk = a > pk ? a : pk;
    }
    mn_out_level = (float)pk * (1.0f / 8388608.0f);
}
