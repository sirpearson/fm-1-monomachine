/* SPDX-License-Identifier: GPL-3.0-only */
/* MNM-1 UI: the panel and the screen. Part of the unity build (after gfx.c and project.c); the host
 * simulator includes it too. (The frame of it, bands, knobs and autosave, is FiMba-1's / FoMni's.)
 *
 *   SEQ        GRID (the white keys are the 16 steps of the track selected) / KEYS (every key plays it)
 *   GRID       a step key: trig on / off. Held: KNOB 1-4 lock the page's values on that step (several
 *              held: on all of them), PRESETS its note, ALGORITHM its octave.
 *              black keys: 1-6 the tracks; MUTE (tap: the track; held: + a track key), ACC + a step:
 *              accent, SLD + a step: slide, COPY (the track), PASTE (its steps; held: and its sound),
 *              COPY held + PASTE: clear the track
 *   KEYS       27 keys from F, OCT- / OCT+ the octave; the pattern's scale lights its keys (SNAP: a key out
 *              of it plays the nearest note in it). With REC, playing: the notes go on the steps they fall on
 *              (a key pressed while another is held: a slide). With REC, stopped: STEP REC, a note a step and
 *              on to the next; FX rest, SEL slide, ENV accent (for the next note), LFO tie, EDIT back,
 *              ALGORITHM moves the cursor
 *   PLAY       start / stop     REC  record: the keys and MIDI notes, and the knobs as locks on the trigs
 *   EDIT SYN  ENV AMP  SEL FILT  FX FX  LFO LFO1-3  ARP TRK   pressed again: the other four of the page
 *   HOME       the pattern: tempo, swing, length, speed; root, scale, snap; every track's steps
 *   GLO        MIDI, clock, the grid following, the colours
 *   SELECT     the track     ALGORITHM the machine (SYN page)     PRESETS the pattern (32 slots; playing:
 *              it changes at the end of this one)     KNOB 1-4 the four values of the page
 *   SAVE       tap: saves the pattern and its kit. Held + PRESETS: saves to another slot (a copy).
 *              Held long: CLEAR? (SAVE again: the pattern; REC: the kit too)
 *   OCT- / OCT+  GRID: the page of steps (longer patterns); KEYS: the octave */

enum { V_HOME = MN_NPAGE, V_GLO, NVIEWS };
#define NWHITE 16
#define NBLACK 11
static const uint8_t WHITE_K[NWHITE] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const uint8_t BLACK_K[NBLACK] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
enum { BK_MUTE = 6, BK_ACC, BK_SLD, BK_COPY, BK_PASTE };
static const char *const BK_NAME[NBLACK] = {"1", "2", "3", "4", "5", "6", "MUT", "ACC", "SLD", "CPY", "PST"};
#define SAVE_HOLD_MS 1500u
#define ASK_MS 5000u
#define PASTE_HOLD_MS 800u
#define SLOT_COMMIT_MS 700u                /* PRESETS: the slot is loaded when it stops turning */
#define NBUF 4                             /* songs in RAM: playing, queued, two waiting to be saved */

static mn_song_t sbuf[NBUF];
static struct {
    uint8_t slot[NBUF], dirty[NBUF];
} sb;

static struct {
    uint8_t view, row[NVIEWS], lfo;        /* the page; its row (0: values 1-4, 1: 5-8); LFO page 0..2 */
    uint8_t track, keys_mode, spage, rec;
    int8_t octave;
    uint32_t btn, keys, btn_used, key_used;
    uint32_t held;                         /* GRID: the step keys held (bit w) */
    uint32_t enc_t[NE];
    uint32_t act_t;
    char msg[2][24];
    uint32_t msg_until;
    int8_t touched;
    uint32_t touch_until;
    uint32_t frame;
    uint32_t sig[3];
    uint8_t last_note[MN_NT];
    uint8_t key_note[NKEYS];               /* the note each key played (its note off) */
    uint8_t cursor;                        /* step entry */
    uint8_t ask;                           /* 1: CLEAR? asking */
    uint32_t ask_until, save_t, paste_t;
    uint8_t save_turned;                   /* PRESETS turned while SAVE held: a copy */
    int8_t sel_slot;                       /* PRESETS: the slot chosen, -1 none */
    uint32_t sel_t;
    uint8_t queued_slot;
    uint8_t clip_ok;
    uint8_t pdirty;                        /* the global settings changed */
    uint8_t trig_seen[MN_NT];
    uint8_t flash[MN_NT];
    uint8_t arm;                           /* STEP REC: MN_ACC / MN_SLIDE for the next note */
    int8_t pulse_step;                     /* the step that just played (its cell lights up) */
    uint32_t pulse_t;
    int8_t step_seen;
    float meter[MN_NT];                    /* the strip's meters, falling */
} ui;

/* the clipboard: one track */
static struct {
    uint8_t machine, par[MN_NP], trig[MN_NSTEP], note[MN_NSTEP];
    uint8_t nrow, lpar[MN_NLOCK], lval[MN_NLOCK][MN_NSTEP];
} clip;

static void say(const char *a, const char *b)
{
    uint32_t i;
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
}
void ui_say(const char *a, const char *b) { say(a, b); }

/* (no C library on the device: libc.c has only mem*) */
static int32_t s_len(const char *s)
{
    int32_t n = 0;
    while (s[n])
        n++;
    return n;
}
static char *s_cpy(char *d, const char *s)          /* returns the end */
{
    while ((*d = *s++))
        d++;
    return d;
}
static char *itoa_u(uint32_t v, char *b)
{
    char t[12];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        *b++ = t[--n];
    *b = 0;
    return b;
}
static char *slot_name(uint32_t s, char *b)         /* A01 .. B16 */
{
    *b++ = (char)('A' + s / 16u);
    *b++ = (char)('0' + (s % 16u + 1u) / 10u);
    *b++ = (char)('0' + (s % 16u + 1u) % 10u);
    *b = 0;
    return b;
}
static const char *const NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static char *note_name(int n, char *b)              /* MIDI 60: C4 */
{
    b = s_cpy(b, NOTE_NAME[n % 12]);
    if (n < 12) {
        *b++ = '-';
        *b++ = '1';
        *b = 0;
        return b;
    }
    return itoa_u((uint32_t)(n / 12 - 1), b);
}

/* ------------------------------------------------------------- songs --- */
static mn_song_t *song(void) { return mn_song(); }
static mn_kit_t *kit(void) { return &mn_song()->kit; }
static mn_pat_t *pat(void) { return &mn_song()->pat; }
static int buf_of(const mn_song_t *s)
{
    int i;
    for (i = 0; i < NBUF; i++)
        if (&sbuf[i] == s)
            return i;
    return 0;
}
static void mark_dirty(void)
{
    sb.dirty[buf_of(song())] = 1;
}
int ui_dirty(void)
{
    int i, d = ui.pdirty;
    for (i = 0; i < NBUF; i++)
        d |= sb.dirty[i];
    return d;
}

static int save_buf(int i)
{
    int rc = song_save(sb.slot[i], &sbuf[i]);
    if (rc == 0)
        sb.dirty[i] = 0;
    return rc;
}

/* a buffer free for a slot: not playing, not queued, nothing in it to save (saved now if it must) */
static int free_buf(void)
{
    int i;
    for (i = 0; i < NBUF; i++)
        if (&sbuf[i] != mn_song() && &sbuf[i] != mn_queued() && !sb.dirty[i])
            return i;
    for (i = 0; i < NBUF; i++)
        if (&sbuf[i] != mn_song() && &sbuf[i] != mn_queued()) {
            save_buf(i);                           /* (a flash write while it plays: rare) */
            return i;
        }
    return -1;
}

static void load_slot(int slot)
{
    int b;
    char t[8];
    if (slot < 0 || slot >= (int)NSLOT)
        return;
    if (!mn_playing && sb.dirty[buf_of(song())])
        save_buf(buf_of(song()));                   /* stopped: the one we leave is saved first */
    b = free_buf();
    if (b < 0)
        return;
    song_load((uint32_t)slot, &sbuf[b]);
    sb.slot[b] = (uint8_t)slot;
    sb.dirty[b] = 0;
    slot_name((uint32_t)slot, t);
    if (mn_playing) {
        mn_queue(&sbuf[b]);
        ui.queued_slot = (uint8_t)slot;
        say("NEXT", t);
    } else {
        mn_set_song(&sbuf[b]);
        proj.slot = (uint8_t)slot;
        ui.pdirty = 1;
        say("PATTERN", t);
    }
    ui.spage = 0;
}

/* ------------------------------------------------------------ values --- */
static int view_is_track(void) { return ui.view < MN_NPAGE; }
static int page_of_view(void) { return ui.view == PG_LFO1 ? PG_LFO1 + ui.lfo : ui.view; }
/* the value KNOB i turns on a track page */
static int knob_par(int i) { return page_of_view() * 8 + ui.row[ui.view] * 4 + i; }

static uint8_t accel_off;                    /* the host simulator's "spin": exact steps */
static int32_t accel(int role, int32_t s, int range)
{
    uint32_t now = plat_ms(), dt = now - ui.enc_t[role], a = (uint32_t)(s < 0 ? -s : s), m;
    ui.enc_t[role] = now;
    if (range <= 24 || !a || accel_off)
        return s;
    if (a > 1)
        dt /= a;
    m = dt < 15u ? 6u : dt < 30u ? 4u : dt < 60u ? 2u : 1u;
    return s * (int32_t)m;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int step_of(int w) { return ui.spage * 16 + w; }

static void say_par(int t, int p, int v, int locked)
{
    char a[16], b[16];
    char *e = s_cpy(a, locked ? "LOCK " : "");
    s_cpy(e, mn_par_info(kit()->machine[t], p)->name);
    if (p >= P_LFO1 && p < P_ARPM && ((p - P_LFO1) & 7) == L_DEST)
        mn_dest_text(kit()->machine, t, kit()->par[t][p - L_DEST + L_TRK], v, b);
    else
        mn_par_text(kit()->machine[t], p, v, b);
    say(a, b);
}

/* a track value: on the held steps as locks, else the kit's (and, recording while it plays, a lock on
 * the step playing if it has a trig) */
static void track_turn(int i, int32_t e)
{
    int t = ui.track, p = knob_par(i), mx = mn_par_info(kit()->machine[t], p)->max, w, d;
    d = accel(EN_K1 + i, e, mx);
    if (ui.held && !ui.keys_mode) {
        int full = 0, v = 0;
        for (w = 0; w < NWHITE; w++)
            if (ui.held >> w & 1u) {
                int s = step_of(w), cur = mn_lock_get(pat(), t, p, s);
                v = clampi((cur < 0 ? kit()->par[t][p] : cur) + d, 0, mx);
                full |= mn_lock_set(pat(), t, p, s, v);
                ui.key_used |= 1u << WHITE_K[w];
            }
        if (full)
            say("LOCKS FULL", "32 IN USE");
        else
            say_par(t, p, v, 1);
        mark_dirty();
        return;
    }
    kit()->par[t][p] = (uint8_t)clampi(kit()->par[t][p] + d, 0, mx);
    if (ui.rec && mn_playing && mn_step >= 0 && (pat()->trig[t][mn_step] & MN_TRIG))
        mn_lock_set(pat(), t, p, mn_step, kit()->par[t][p]);
    mark_dirty();
}

static const char *const SPEED_NAME[MN_NSPEED] = {"1/2X", "3/4X", "1X", "3/2X", "2X"};
static void home_turn(int i, int32_t e)
{
    mn_pat_t *p = pat();
    int d;
    if (ui.row[V_HOME]) {                            /* the scale */
        if (i == 0)
            p->root = (uint8_t)((p->root + (int)e % 12 + 12) % 12);
        else if (i == 1)
            p->scale = (uint8_t)clampi(p->scale + (int)e, 0, MN_NSCALE - 1);
        else if (i == 2)
            p->snap = e > 0;
        else
            return;
        mark_dirty();
        return;
    }
    switch (i) {
    case 0:
        d = accel(EN_K1, e, 100);
        proj.tempo10 = (uint16_t)clampi(proj.tempo10 + d * 10, 300, 3000);
        mn_tempo10 = proj.tempo10;
        ui.pdirty = 1;
        return;
    case 1: p->swing = (uint8_t)clampi(p->swing + (int)e, 50, 80); break;
    case 2:
        p->len = (uint8_t)clampi(p->len + (int)e, 1, MN_NSTEP);
        if (ui.spage * 16 >= p->len)
            ui.spage = (uint8_t)((p->len - 1) / 16);
        break;
    default: p->speed = (uint8_t)clampi(p->speed + (int)e, 0, MN_NSPEED - 1); break;
    }
    mark_dirty();
}

static void glo_turn(int i, int32_t e)
{
    int s = e > 0 ? 1 : -1;
    switch (ui.row[V_GLO] * 4 + i) {
    case 0: proj.midi_base = (uint8_t)clampi(proj.midi_base + s, 0, 10); break;
    case 1: proj.auto_ch = (uint8_t)clampi(proj.auto_ch + s, 0, 16); break;
    case 2: proj.clock_in = s > 0; break;
    case 3: proj.clock_out = s > 0; break;
    case 4: proj.midi_out = s > 0; break;
    case 5: proj.follow = s > 0; break;
    case 6: proj.theme = (uint8_t)clampi(proj.theme + s, 0, (int)NPALETTES - 1); break;
    default: return;
    }
    project_apply();
    ui.pdirty = 1;
    ui.sig[0] = ui.sig[1] = ui.sig[2] = 0;           /* (a new theme: everything again) */
    lcd_fill(0, 0, 240, 240, C_BLACK);
}

/* ------------------------------------------------------------- tracks --- */
static void set_track(int t)
{
    char a[8], b[16];
    ui.track = (uint8_t)clampi(t, 0, MN_NT - 1);
    a[0] = 'T';
    a[1] = (char)('1' + ui.track);
    a[2] = 0;
    s_cpy(b, mn_machine_long(kit()->machine[ui.track]));
    say(a, b);
}

static void set_machine(int t, int m)
{
    uint8_t *p = kit()->par[t];
    int was = kit()->machine[t];
    mn_machine_set(kit(), t, m);
    if (m == MC_DRUM && was != MC_DRUM && p[P_HOLD] == 127) {   /* drums keep their own envelopes */
        p[P_HOLD] = 0;
        p[P_DEC] = 127;
        p[P_REL] = 127;
    } else if (m != MC_DRUM && was == MC_DRUM && p[P_HOLD] == 0 && p[P_DEC] == 127) {
        p[P_HOLD] = 127;
        p[P_DEC] = 64;
        p[P_REL] = 30;
    }
    mark_dirty();
}

static void toggle_mute(int t)
{
    char a[4] = {'T', (char)('1' + t), 0, 0};
    mn_mute ^= (uint8_t)(1u << t);
    say(a, (mn_mute >> t & 1u) ? "MUTED" : "ON");
}

static void copy_track(void)
{
    int t = ui.track, r, s;
    clip.machine = kit()->machine[t];
    memcpy(clip.par, kit()->par[t], MN_NP);
    memcpy(clip.trig, pat()->trig[t], MN_NSTEP);
    memcpy(clip.note, pat()->note[t], MN_NSTEP);
    clip.nrow = 0;
    for (r = 0; r < MN_NLOCK; r++)
        if (pat()->ltrk[r] == t) {
            clip.lpar[clip.nrow] = pat()->lpar[r];
            for (s = 0; s < MN_NSTEP; s++)
                clip.lval[clip.nrow][s] = pat()->lval[r][s];
            clip.nrow++;
        }
    ui.clip_ok = 1;
    say("COPIED", "TRACK");
}

static void clear_track(int t)
{
    int s;
    mn_lock_clear_track(pat(), t);
    for (s = 0; s < MN_NSTEP; s++) {
        pat()->trig[t][s] = 0;
        pat()->note[t][s] = ui.last_note[t];
    }
    mark_dirty();
}

static void paste_track(int sound)
{
    int t = ui.track, r, s, full = 0;
    if (!ui.clip_ok) {
        say("NOTHING", "COPIED");
        return;
    }
    clear_track(t);
    memcpy(pat()->trig[t], clip.trig, MN_NSTEP);
    memcpy(pat()->note[t], clip.note, MN_NSTEP);
    for (r = 0; r < clip.nrow; r++)
        for (s = 0; s < MN_NSTEP; s++)
            if (clip.lval[r][s] != 0xFF)
                full |= mn_lock_set(pat(), t, clip.lpar[r], s, clip.lval[r][s]);
    if (sound) {
        kit()->machine[t] = clip.machine;
        memcpy(kit()->par[t], clip.par, MN_NP);
    }
    mark_dirty();
    say(full ? "LOCKS FULL" : "PASTED", sound ? "SOUND+STEPS" : "STEPS");
}

/* ---------------------------------------------------------- the keys --- */
static int key_base(void) { return 53 + 12 * ui.octave; }     /* key 0 is F3 (MIDI 53) */
static uint8_t midi_ch_of(int t) { return (uint8_t)((proj.midi_base + t) & 15); }
static void note_out(int t, int on, int n, int vel)
{
    uint32_t st;
    if (!proj.midi_out)
        return;
    st = (on ? 0x90u : 0x80u) | midi_ch_of(t);
    plat_midi_out(st >> 4 | st << 8 | (uint32_t)n << 16 | (uint32_t)(on ? vel : 0) << 24);
}

static int pat_len(void) { return pat()->len ? pat()->len : 16; }
static int step_rec(void) { return ui.keys_mode && ui.rec && !mn_playing; }
static int scaled(int n) { return pat()->snap ? mn_scale_snap(pat()->scale, pat()->root, n) : n; }
static int key_note_of(uint32_t k) { return clampi(scaled(key_base() + (int)k), 0, 127); }

/* recording a note: playing, on the step it falls on (the nearer); stopped (STEP REC), at the cursor, which
 * moves on. legato (a key pressed while another is held): the step slides into the note */
static void cursor_to(int s)
{
    ui.cursor = (uint8_t)((s % pat_len() + pat_len()) % pat_len());
    ui.spage = (uint8_t)(ui.cursor / 16);
}
static void rec_note(int t, int n, int legato)
{
    int s;
    uint8_t f = MN_TRIG;
    if (!ui.rec)
        return;
    if (mn_playing) {
        s = mn_step;
        if (s < 0)
            return;
        if (mn_step_frac > 0.5f)
            s = (s + 1) % pat_len();
    } else {
        s = ui.cursor % pat_len();
        f |= ui.arm;
        ui.arm = 0;
        cursor_to(s + 1);
    }
    if (legato)
        f |= MN_SLIDE;
    if (!mn_playing)
        mn_lock_clear_step(pat(), t, s);            /* (STEP REC writes the step anew) */
    pat()->trig[t][s] = f;
    pat()->note[t][s] = (uint8_t)n;
    mark_dirty();
}

static void play_note(int t, int n, int vel, int legato)
{
    mn_note_on(t, n, vel);
    note_out(t, 1, n, vel);
    ui.last_note[t] = (uint8_t)n;
    rec_note(t, n, legato);
}

/* STEP REC's buttons: a rest, a tie (the note before, held on), back; slide and accent for the next note */
static void step_rec_button(int b)
{
    int t = ui.track, s = ui.cursor % pat_len(), prev = (s + pat_len() - 1) % pat_len();
    char a[8];
    switch (b) {
    case B_FX:                                       /* REST */
        pat()->trig[t][s] = 0;
        mn_lock_clear_step(pat(), t, s);
        cursor_to(s + 1);
        mark_dirty();
        itoa_u((uint32_t)s + 1u, a);
        say("REST", a);
        break;
    case B_LFO:                                      /* TIE: the same note, sliding on from the one before */
        if (!(pat()->trig[t][prev] & MN_TRIG)) {
            say("TIE", "NO NOTE BEFORE");
            break;
        }
        pat()->trig[t][s] = MN_TRIG | MN_SLIDE;
        pat()->note[t][s] = pat()->note[t][prev];
        mn_lock_clear_step(pat(), t, s);
        cursor_to(s + 1);
        mark_dirty();
        note_name(pat()->note[t][s], a);
        say("TIE", a);
        break;
    case B_EDIT:                                     /* BACK */
        cursor_to(s - 1);
        itoa_u((uint32_t)ui.cursor + 1u, a);
        say("BACK TO", a);
        break;
    case B_SEL: ui.arm ^= MN_SLIDE; say("NEXT NOTE", ui.arm & MN_SLIDE ? "SLIDE" : "NO SLIDE"); break;
    case B_ENV: ui.arm ^= MN_ACC; say("NEXT NOTE", ui.arm & MN_ACC ? "ACCENT" : "NO ACCENT"); break;
    default: break;
    }
}

static void step_toggle(int w)
{
    int t = ui.track, s = step_of(w);
    mn_pat_t *p = pat();
    if (s >= pat_len())
        return;
    if (p->trig[t][s] & MN_TRIG) {
        p->trig[t][s] = 0;
        mn_lock_clear_step(p, t, s);
    } else {
        p->trig[t][s] = MN_TRIG;
        p->note[t][s] = ui.last_note[t];
    }
    mark_dirty();
}

static void white_down(int w)
{
    uint32_t k = WHITE_K[w];
    if (ui.keys_mode) {
        ui.key_note[k] = (uint8_t)key_note_of(k);
        play_note(ui.track, ui.key_note[k], 100, (ui.keys & ~(1u << k)) != 0);
        return;
    }
    if (ui.keys >> BLACK_K[BK_ACC] & 1u || ui.keys >> BLACK_K[BK_SLD] & 1u) {   /* ACC / SLD + a step */
        int s = step_of(w), t = ui.track;
        uint8_t f = ui.keys >> BLACK_K[BK_ACC] & 1u ? MN_ACC : MN_SLIDE;
        if (s < pat_len()) {
            if (!(pat()->trig[t][s] & MN_TRIG))
                pat()->note[t][s] = ui.last_note[t];
            pat()->trig[t][s] = (uint8_t)((pat()->trig[t][s] ^ f) | MN_TRIG);
            mark_dirty();
        }
        ui.key_used |= 1u << k;
        ui.key_used |= ui.keys >> BLACK_K[BK_ACC] & 1u ? 1u << BLACK_K[BK_ACC] : 1u << BLACK_K[BK_SLD];
        return;
    }
    ui.held |= 1u << w;
    ui.key_used &= ~(1u << k);
}

static void white_up(int w)
{
    uint32_t k = WHITE_K[w];
    if (ui.keys_mode) {
        mn_note_off(ui.track, ui.key_note[k]);
        note_out(ui.track, 0, ui.key_note[k], 0);
        return;
    }
    if (ui.held >> w & 1u) {
        ui.held &= ~(1u << w);
        if (!(ui.key_used >> k & 1u))
            step_toggle(w);
    }
}

static void black_down(int b)
{
    uint32_t k = BLACK_K[b];
    if (ui.keys_mode) {
        ui.key_note[k] = (uint8_t)key_note_of(k);
        play_note(ui.track, ui.key_note[k], 100, (ui.keys & ~(1u << k)) != 0);
        return;
    }
    ui.key_used &= ~(1u << k);
    if (b < MN_NT) {
        if (ui.keys >> BLACK_K[BK_MUTE] & 1u) {      /* MUTE held + a track */
            toggle_mute(b);
            ui.key_used |= 1u << BLACK_K[BK_MUTE];
        } else {
            set_track(b);
        }
    } else if (b == BK_PASTE) {
        if (ui.keys >> BLACK_K[BK_COPY] & 1u) {      /* COPY held + PASTE: clear */
            char a[4] = {'T', (char)('1' + ui.track), 0, 0};
            clear_track(ui.track);
            ui.key_used |= 1u << BLACK_K[BK_COPY] | 1u << k;
            say(a, "CLEARED");
        } else {
            ui.paste_t = plat_ms();
        }
    }
}

static void black_up(int b)
{
    uint32_t k = BLACK_K[b];
    if (ui.keys_mode) {
        mn_note_off(ui.track, ui.key_note[k]);
        note_out(ui.track, 0, ui.key_note[k], 0);
        return;
    }
    if (ui.key_used >> k & 1u)
        return;
    switch (b) {
    case BK_MUTE: toggle_mute(ui.track); break;
    case BK_COPY: copy_track(); break;
    case BK_PASTE: paste_track(0); break;
    default: break;
    }
}

/* ------------------------------------------------------------ buttons --- */
static void set_view(int v)
{
    ui.view = (uint8_t)v;
    ui.touched = -1;
}

static void page_button(int v)
{
    if (ui.view == v) {                             /* again: the other row (LFO: the next half, the next LFO) */
        if (v == PG_LFO1) {
            int x = ui.lfo * 2 + ui.row[v] + 1;
            ui.lfo = (uint8_t)(x / 2 % 3);
            ui.row[v] = (uint8_t)(x & 1);
        } else {
            ui.row[v] ^= 1u;
        }
    }
    set_view(v);
}

static void ask(int on)
{
    ui.ask = (uint8_t)on;
    ui.ask_until = on ? plat_ms() + ASK_MS : 0u;
    if (on) {
        say("CLEAR?", "SAVE:PATTERN REC:+KIT");
        ui.msg_until = ui.ask_until;
    }
}

static void clear_pattern(int with_kit)
{
    char t[8];
    int i;
    mn_pat_clear(pat());
    for (i = 0; i < MN_NT; i++)
        ui.last_note[i] = 60;
    if (with_kit)
        mn_kit_init(kit());
    mark_dirty();
    slot_name(sb.slot[buf_of(song())], t);
    say(with_kit ? "CLEARED+KIT" : "CLEARED", t);
}

static void do_save(void)
{
    char t[8];
    int b = buf_of(song()), rc;
    rc = save_buf(b);
    if (rc == 0 && ui.pdirty) {
        if (project_save() == 0)
            ui.pdirty = 0;
    }
    slot_name(sb.slot[b], t);
    if (rc == 0)
        say("SAVED", t);
    else
        say("NOT SAVED", "FLASH ERROR");
}

static void save_as(int slot)                      /* SAVE held + PRESETS: the song goes to another slot */
{
    char t[8];
    int b = buf_of(song()), i;
    for (i = 0; i < NBUF; i++)                     /* a copy of that slot waiting elsewhere is stale now */
        if (i != b && sb.slot[i] == slot && &sbuf[i] != mn_queued())
            sb.dirty[i] = 0, sb.slot[i] = 0xFF;
    sb.slot[b] = (uint8_t)slot;
    proj.slot = (uint8_t)slot;
    ui.pdirty = 1;
    slot_name((uint32_t)slot, t);
    if (save_buf(b) == 0) {
        project_save();
        ui.pdirty = 0;
        say("SAVED TO", t);
    } else {
        say("NOT SAVED", "FLASH ERROR");
    }
}

static void button(int b)
{
    if (ui.ask) {                                   /* CLEAR?: SAVE the pattern, REC with the kit, else kept */
        ui.ask = 0;
        if (b == B_SAVE || b == B_REC)
            clear_pattern(b == B_REC);
        else
            say("KEPT", 0);
        return;
    }
    if (step_rec() && (b == B_FX || b == B_SEL || b == B_ENV || b == B_LFO || b == B_EDIT)) {
        step_rec_button(b);
        return;
    }
    switch (b) {
    case B_EDIT: page_button(PG_SYN); break;
    case B_ENV: page_button(PG_AMP); break;
    case B_SEL: page_button(PG_FILT); break;
    case B_FX: page_button(PG_FX); break;
    case B_LFO: page_button(PG_LFO1); break;
    case B_ARP: page_button(PG_TRK); break;
    case B_HOME: page_button(V_HOME); break;
    case B_GLO: page_button(V_GLO); break;
    case B_SEQ:
        ui.keys_mode ^= 1u;
        ui.held = 0;
        mn_all_off();
        say(ui.keys_mode ? "KEYS" : "GRID", ui.keys_mode ? "PLAY THE TRACK" : "STEPS");
        break;
    case B_PLAY: mn_play(!mn_playing); break;
    case B_REC:
        ui.rec ^= 1u;
        ui.arm = 0;
        if (ui.rec && !mn_playing)
            ui.cursor = (uint8_t)(ui.spage * 16);
        say("REC", !ui.rec ? "OFF" : step_rec() ? "STEP: A NOTE A STEP" : ui.keys_mode || mn_playing ? "ON" : "ON (KEYS: STEP REC)");
        break;
    case B_OCTDN: case B_OCTUP: {
        int d = b == B_OCTUP ? 1 : -1;
        if (ui.keys_mode) {
            char t[8] = {0};
            ui.octave = (int8_t)clampi(ui.octave + d, -3, 3);
            if (ui.octave > 0)
                t[0] = '+';
            itoa_u((uint32_t)(ui.octave < 0 ? -ui.octave : ui.octave), t + (ui.octave != 0));
            if (ui.octave < 0)
                t[0] = '-';
            say("OCTAVE", t);
        } else {
            int pages = (pat_len() + 15) / 16;
            char t[8];
            ui.spage = (uint8_t)((ui.spage + pages + d) % pages);
            itoa_u((uint32_t)(ui.spage * 16 + 1), t);
            say("STEPS", t);
        }
        break;
    }
    case B_SAVE: do_save(); break;
    default: break;
    }
}

/* ------------------------------------------------------------- MIDI in --- */
static int midi_track(uint32_t ch)                  /* the track a channel plays, -1 none */
{
    if (proj.auto_ch && ch + 1u == proj.auto_ch)
        return ui.track;
    if (ch >= proj.midi_base && ch < proj.midi_base + (uint32_t)MN_NT)
        return (int)(ch - proj.midi_base);
    return -1;
}

static void midi_in(void)
{
    uint32_t pkt;
    while (plat_midi_in(&pkt)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, type = st & 0xF0u;
        int t;
        if (st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu) {
            mn_clock(st == 0xF8u ? MN_CLK_TICK : st == 0xFAu ? MN_CLK_START : st == 0xFBu ? MN_CLK_CONTINUE : MN_CLK_STOP);
            continue;
        }
        if (st >= 0xF0u)
            continue;
        t = midi_track(st & 0x0Fu);
        if (type == 0xC0u && proj.auto_ch && (st & 0x0Fu) + 1u == proj.auto_ch) {
            if (d1 < NSLOT)
                load_slot((int)d1);                  /* program change: a pattern */
            continue;
        }
        if (t < 0)
            continue;
        if (type == 0x90u && d2) {
            mn_note_on(t, (int)d1, (int)d2);
            ui.last_note[t] = (uint8_t)d1;
            rec_note(t, (int)d1, 0);
        } else if (type == 0x80u || type == 0x90u) {
            mn_note_off(t, (int)d1);
        } else if (type == 0xB0u) {
            if (d1 == 120u || d1 == 123u) {
                mn_all_off();
            } else if (d1 == 7u || d1 == 10u || (d1 >= 16u && d1 < 16u + MN_NP && d1 != 64u)) {
                /* CC 16..79: the track's values, in order (not 64: a sustain pedal); 7 volume, 10 pan */
                int p = d1 == 7u ? P_VOL : d1 == 10u ? P_PAN : (int)d1 - 16, mx = mn_par_info(kit()->machine[t], p)->max;
                kit()->par[t][p] = (uint8_t)((d2 * (uint32_t)mx + 63u) / 127u);
                mark_dirty();
            }
        }
        ui.act_t = plat_ms();
    }
}

/* -------------------------------------------------------------- input --- */
static void encoders(void)
{
    int32_t e;
    uint32_t i;
    if ((e = plat_enc(EN_SELECT)) != 0)
        set_track(ui.track + (int)e);
    if ((e = plat_enc(EN_ALGO)) != 0) {
        if (step_rec()) {                             /* STEP REC: the cursor */
            char a[8];
            cursor_to(ui.cursor + (int)e);
            itoa_u((uint32_t)ui.cursor + 1u, a);
            say("STEP", a);
        } else if (ui.held && !ui.keys_mode) {               /* a step held: its octave */
            int w;
            for (w = 0; w < NWHITE; w++)
                if (ui.held >> w & 1u) {
                    int s = step_of(w);
                    pat()->note[ui.track][s] = (uint8_t)clampi(pat()->note[ui.track][s] + 12 * (int)e, 0, 127);
                    ui.last_note[ui.track] = pat()->note[ui.track][s];
                    ui.key_used |= 1u << WHITE_K[w];
                }
            mark_dirty();
        } else if (ui.view == PG_SYN) {
            int m = ((kit()->machine[ui.track] + (int)e) % MC_N + MC_N) % MC_N;
            char a[8] = {'T', (char)('1' + ui.track), ' ', 0};
            set_machine(ui.track, m);
            s_cpy(a + 3, mn_machine_name(m));
            say(a, mn_machine_long(m));
        } else {
            say("ALGORITHM", "THE MACHINE: SYN PAGE");
        }
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        if (ui.held && !ui.keys_mode) {               /* a step held: its note */
            int w;
            char a[8];
            for (w = 0; w < NWHITE; w++)
                if (ui.held >> w & 1u) {
                    int s = step_of(w);
                    int nn = pat()->scale ? mn_scale_move(pat()->scale, pat()->root, pat()->note[ui.track][s], (int)e)
                                          : pat()->note[ui.track][s] + (int)e;   /* (in the scale, when one is set) */
                    pat()->note[ui.track][s] = (uint8_t)clampi(nn, 0, 127);
                    ui.last_note[ui.track] = pat()->note[ui.track][s];
                    ui.key_used |= 1u << WHITE_K[w];
                }
            note_name(ui.last_note[ui.track], a);
            say("NOTE", a);
            mark_dirty();
        } else {
            int cur = ui.sel_slot >= 0 ? ui.sel_slot : (int)sb.slot[buf_of(song())];
            char a[8];
            ui.sel_slot = (int8_t)(((cur + (int)e) % (int)NSLOT + (int)NSLOT) % (int)NSLOT);
            ui.sel_t = plat_ms();
            slot_name((uint32_t)ui.sel_slot, a);
            say(ui.btn >> B_SAVE & 1u ? "SAVE TO" : "PATTERN", a);
            if (ui.btn >> B_SAVE & 1u) {
                ui.save_turned = 1;
                ui.btn_used |= 1u << B_SAVE;
                ui.msg_until = plat_ms() + 60000u;
            }
        }
    }
    for (i = 0; i < 4; i++)
        if ((e = plat_enc(EN_K1 + (int)i)) != 0) {
            if (view_is_track())
                track_turn((int)i, e);
            else if (ui.view == V_HOME)
                home_turn((int)i, e);
            else
                glo_turn((int)i, e);
            ui.touched = (int8_t)i;
            ui.touch_until = plat_ms() + 1200u;
            ui.act_t = plat_ms();
        }
}

static void input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), ch, i, now = plat_ms();
    ch = btn ^ ui.btn;
    ui.btn = btn;
    for (i = 0; i < NB; i++) {
        uint32_t m = 1u << i;
        if (!(ch & m))
            continue;
        if (btn & m) {
            ui.btn_used &= ~m;
            if (i == B_SAVE) {
                ui.save_t = now;
                ui.save_turned = 0;
            }
            if (i == B_PLAY || i == B_REC || (ui.ask && i != B_SAVE))   /* the playing buttons act on the press */
                button((int)i), ui.btn_used |= m;
        } else if (!(ui.btn_used & m)) {
            button((int)i);
        } else if (i == B_SAVE && ui.save_turned && ui.sel_slot >= 0) {
            save_as(ui.sel_slot);                      /* released after choosing a slot: the copy */
            ui.sel_slot = -1;
            ui.save_turned = 0;
        }
    }
    if ((btn >> B_SAVE & 1u) && !(ui.btn_used >> B_SAVE & 1u) && !ui.ask && now - ui.save_t >= SAVE_HOLD_MS) {
        ask(1);                                      /* SAVE held: CLEAR? (no save on release) */
        ui.btn_used |= 1u << B_SAVE;
    }
    if (ui.ask && (int32_t)(now - ui.ask_until) >= 0) {
        ui.ask = 0;
        say("KEPT", 0);
    }
    ch = keys ^ ui.keys;
    ui.keys = keys;
    for (i = 0; i < NBLACK; i++)
        if (ch >> BLACK_K[i] & 1u) {
            if (keys >> BLACK_K[i] & 1u)
                black_down((int)i);
            else
                black_up((int)i);
        }
    for (i = 0; i < NWHITE; i++)
        if (ch >> WHITE_K[i] & 1u) {
            if (keys >> WHITE_K[i] & 1u)
                white_down((int)i);
            else
                white_up((int)i);
        }
    if (!ui.keys_mode && (keys >> BLACK_K[BK_PASTE] & 1u) && !(ui.key_used >> BLACK_K[BK_PASTE] & 1u) &&
        now - ui.paste_t >= PASTE_HOLD_MS) {
        paste_track(1);                              /* PASTE held: the sound too */
        ui.key_used |= 1u << BLACK_K[BK_PASTE];
    }
    if (btn || keys || ch)
        ui.act_t = now;
    midi_in();
    encoders();
    if (ui.sel_slot >= 0 && !(btn >> B_SAVE & 1u) && now - ui.sel_t >= SLOT_COMMIT_MS) {
        if (ui.sel_slot != (int)sb.slot[buf_of(song())])
            load_slot(ui.sel_slot);
        ui.sel_slot = -1;
    }
    /* the queued pattern took over: it is the slot now */
    if (mn_playing || mn_queued() == 0) {
        int b = buf_of(song());
        if (sb.slot[b] != proj.slot && sb.slot[b] < NSLOT) {
            proj.slot = sb.slot[b];
            ui.pdirty = 1;
        }
    }
    if (proj.follow && mn_playing && !ui.held && mn_step >= 0)
        ui.spage = (uint8_t)(mn_step / 16);
}

/* ------------------------------------------------------------- screen --- */
/* three bands: the header (28 px), the page (144), the steps (68). Colour means something: each track has
 * its hue (its cell in the strip, its steps, its values and graphs), each page its label's; white is what
 * is touched, held or locked; red is recording. Neutral greys (the theme's) carry the structure. */
#define HDR_H 28
#define MAIN_Y 28
#define MAIN_H 144
#define FTR_Y 172
#define FTR_H 68

static const uint16_t TRK_COL[MN_NT] = {RGB(255, 96, 82), RGB(255, 160, 50), RGB(240, 214, 72),
                                        RGB(96, 224, 138), RGB(76, 200, 240), RGB(182, 140, 255)};
static const uint16_t PAGE_COL[NVIEWS] = {RGB(236, 236, 240), RGB(255, 138, 92), RGB(255, 210, 92), RGB(110, 210, 255),
                                          RGB(200, 146, 255), RGB(200, 146, 255), RGB(200, 146, 255), RGB(124, 242, 154),
                                          RGB(255, 255, 255), RGB(160, 168, 176)};
#define C_REC RGB(255, 64, 56)

static uint32_t hash(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

static uint16_t mix(uint16_t a, uint16_t b, int t)   /* t 0..16: a -> b */
{
    int r = ((a >> 11) * (16 - t) + (b >> 11) * t) / 16, g = (((a >> 5) & 63) * (16 - t) + ((b >> 5) & 63) * t) / 16,
        bl = ((a & 31) * (16 - t) + (b & 31) * t) / 16;
    return (uint16_t)(r << 11 | g << 5 | bl);
}
static uint16_t tcol(int t) { return TRK_COL[(unsigned)t % MN_NT]; }
static uint16_t tdim(int t, int k) { return mix(C_BLACK, tcol(t), k); }

static void cv_round(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    static const uint8_t IN[7][6] = {{0}, {1}, {1, 0}, {2, 1, 0}, {2, 1, 0, 0}, {3, 2, 1, 0, 0}, {4, 2, 1, 1, 0, 0}};
    int32_t j;
    if (r > 6)
        r = 6;
    if (r > h / 2)
        r = h / 2;
    cv_rect(x, y + r, w, h - 2 * r, c);
    for (j = 0; j < r; j++) {
        int32_t d = IN[r][j];
        cv_rect(x + d, y + j, w - 2 * d, 1, c);
        cv_rect(x + d, y + h - 1 - j, w - 2 * d, 1, c);
    }
}

static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

static int blink(void) { return (plat_ms() / 250u) & 1u; }
/* 0..16: how much of the touched-knob highlight is left (it fades over its last 400 ms) */
static int touch_fade(void)
{
    uint32_t now = plat_ms();
    if (ui.touched < 0 || (int32_t)(ui.touch_until - now) <= 0)
        return 0;
    return ui.touch_until - now >= 400u ? 16 : (int)((ui.touch_until - now) * 16u / 400u);
}
/* a chip: rounded, filled, its text dark */
static int32_t chip(int32_t x, int32_t y, const char *s, uint16_t bg, uint16_t fg)
{
    int32_t w = text_w(&FONT_XS, s) + 10;
    cv_round(x, y, w, 16, 5, bg);
    cv_text(x + 5, y + 1, &FONT_XS, s, fg);
    return x + w;
}

static void draw_header(void)
{
    char t[24], *e;
    int msg = plat_ms() < ui.msg_until, b = buf_of(song()), beat = mn_playing && (mn_step & 3) == 0;
    uint32_t h = hash(hash(2166136261u, (uint32_t)msg | (uint32_t)mn_playing << 1 | (uint32_t)ui.rec << 2 |
                                            (uint32_t)mn_ext << 3 | (uint32_t)ui.track << 4 | (uint32_t)ui_dirty() << 8 |
                                            (uint32_t)(mn_queued() != 0) << 9 | (uint32_t)blink() << 10 |
                                            (uint32_t)(ui.sel_slot + 1) << 11 | (uint32_t)kit()->machine[ui.track] << 18 |
                                            (uint32_t)beat << 23 | (uint32_t)step_rec() << 24),
                      (uint32_t)proj.tempo10 | (uint32_t)sb.slot[b] << 16 | (uint32_t)ui.queued_slot << 24);
    if (msg)
        h = hash(hash(h, (uint32_t)ui.msg[0][0] << 8 | (uint32_t)ui.msg[0][1] | (uint32_t)ui.msg[1][0] << 16),
                 ui.msg_until ^ (uint32_t)s_len(ui.msg[1]) << 20 ^ (uint32_t)ui.msg[1][1] << 24);
    if (h == ui.sig[0])
        return;
    ui.sig[0] = h;
    cv_begin(240, HDR_H, C_BLACK);
    if (msg) {                                       /* a message: on the track's colour band */
        int32_t x;
        cv_rect(0, 0, 4, HDR_H, tcol(ui.track));
        x = cv_text(10, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], tcol(ui.track));
    } else {
        int32_t x;
        slot_name(sb.slot[b], t);
        x = cv_text(8, 6, &FONT_B, t, C_WHITE);
        if (mn_queued() && blink()) {               /* the next pattern, waiting for the end of this one */
            e = s_cpy(t, ">");
            slot_name(ui.queued_slot, e);
            x = cv_text(x + 2, 6, &FONT_B, t, C_HI);
        }
        if (ui_dirty())
            cv_round(x + 3, 10, 4, 4, 2, C_GRAY);
        x += 12;
        t[0] = 'T';                                  /* the track: a chip in its colour, the machine beside it */
        t[1] = (char)('1' + ui.track);
        t[2] = 0;
        cv_round(x, 5, 22, 18, 5, tcol(ui.track));
        text_c(x + 11, 6, &FONT_B, t, C_BLACK);
        cv_text(x + 27, 6, &FONT_B, mn_machine_name(kit()->machine[ui.track]), tcol(ui.track));
        /* the tempo, the transport */
        e = itoa_u(proj.tempo10 / 10u, t);
        *e++ = '.';
        *e++ = (char)('0' + proj.tempo10 % 10u);
        *e = 0;
        x = 232 - text_w(&FONT_B, t);
        cv_text(x, 6, &FONT_B, t, mn_ext ? C_GRAY : C_WHITE);
        if (mn_ext) {
            x -= text_w(&FONT_XS, "EXT") + 5;
            cv_text(x, 8, &FONT_XS, "EXT", C_HI);
        }
        x -= 16;
        if (mn_playing) {                            /* a triangle: white on the beat */
            int32_t j;
            for (j = 0; j < 6; j++)
                cv_rect(x + j, 8 + j, 1, 12 - 2 * j, beat ? C_WHITE : C_HI);
        } else {
            cv_rect(x, 9, 9, 9, C_DIM);
        }
        if (ui.rec) {
            x -= 14;
            cv_round(x, 8, 10, 10, 5, step_rec() && blink() ? mix(C_REC, C_BLACK, 8) : C_REC);
        }
    }
    cv_blit(0, 0);
}

/* the steps held (GRID): the first, for the page's lock view; -1 none */
static int held_step(void)
{
    int w;
    if (ui.keys_mode)
        return -1;
    for (w = 0; w < NWHITE; w++)
        if (ui.held >> w & 1u)
            return step_of(w);
    return -1;
}

static void draw_tracks(void)                       /* the strip: the six tracks, in their colours */
{
    int t;
    for (t = 0; t < MN_NT; t++) {
        int32_t x = t * 40 + 1, lv;
        int sel = t == ui.track, muted = mn_mute >> t & 1u;
        char s[8];
        uint16_t c = tcol(t), bg, fg;
        bg = sel ? c : mix(tdim(t, 3), c, ui.flash[t] * 2);   /* a note: the cell lights up and fades */
        fg = sel ? C_BLACK : muted ? C_DIM : c;
        if (muted && !sel)
            bg = mix(C_LINE, C_BLACK, 6);
        s[0] = (char)('1' + t);
        s[1] = ' ';
        s_cpy(s + 2, mn_machine_name(kit()->machine[t]));
        cv_round(x, 0, 38, 19, 4, bg);
        text_c(x + 19, 2, &FONT_XS, s, fg);
        if (muted)
            cv_rect(x + 4, 9, 30, 1, sel ? C_BLACK : C_DIM);
        lv = (int)(ui.meter[t] * 60.0f);
        lv = lv > 38 ? 38 : lv;
        cv_rect(x, 21, 38, 3, C_LINE);
        if (lv)
            cv_rect(x, 21, lv, 3, muted ? C_DIM : c);
    }
}

/* ---- the pages' little graphs, in the track's colour, x .. x+w, y .. y+h */
static void graph_env(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *p, uint16_t c)
{
    int32_t a = 2 + p[P_ATK] * (w / 4) / 127, hl, d, r, x0 = x, yb = y + h - 1;
    int gate = p[P_HOLD] >= 127;
    hl = gate ? w / 4 : p[P_HOLD] * (w / 6) / 127;
    d = 3 + p[P_DEC] * (w / 3) / 127;
    r = gate ? 3 + p[P_REL] * (w / 4) / 127 : 0;
    cv_line(x0, yb, x0 + a, y, c);                                   /* attack */
    cv_line(x0 + a, y, x0 + a + hl, y, c);                           /* hold */
    if (gate) {
        cv_line(x0 + a + hl, y, x0 + a + hl + r, yb, c);             /* release */
    } else {
        int32_t i, px = x0 + a + hl, py = y;
        for (i = 1; i <= 8; i++) {                                   /* decay: a curve, in eight pieces */
            int32_t nx = x0 + a + hl + d * i / 8, ny = yb - (int32_t)((float)(h - 1) * fm_exp2f(-(float)i * 0.75f));
            cv_line(px, py, nx, ny, c);
            px = nx;
            py = ny;
        }
    }
    cv_rect(x, yb + 1, w, 1, C_LINE);
}

static void graph_filter(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *p, uint16_t c)
{
    int32_t i, py = 0;
    float hp = (float)p[P_BASE], lp = hp + (float)p[P_WDTH], hq = (float)p[P_HPQ] / 127.0f, lq = (float)p[P_LPQ] / 127.0f;
    for (i = 0; i < w; i++) {
        float f = (float)i * 127.0f / (float)(w - 1), g = 1.0f, d;
        if (f < hp) {                                                /* below the high pass: 12 dB an octave */
            d = (hp - f) / 12.7f;
            g = fm_exp2f(-2.0f * d);
        }
        if (f > lp && lp < 126.0f) {
            d = (f - lp) / 12.7f;
            g *= fm_exp2f(-2.0f * d);
        }
        d = (f - hp) / 6.0f;                                         /* the resonances: bumps at the corners */
        g += hq * 1.2f * fm_expf(-d * d);
        d = (f - lp) / 6.0f;
        if (lp < 126.0f)
            g += lq * 1.4f * fm_expf(-d * d);
        g = g > 2.0f ? 2.0f : g;
        {
            int32_t yy = y + h - 1 - (int32_t)(g * 0.5f * (float)(h - 2));
            if (i)
                cv_line(x + i - 1, py, x + i, yy, c);
            py = yy;
        }
    }
    cv_rect(x, y + h, w, 1, C_LINE);
}

static float scope[3][48];                          /* the LFOs' recent values, for the scope */
static uint8_t scope_w;
static void graph_lfo(int32_t x, int32_t y, int32_t w, int32_t h, int k, uint16_t c)
{
    int32_t i, py = 0, n = w < 48 ? w : 48;
    cv_rect(x, y + h / 2, n, 1, C_LINE);
    for (i = 0; i < n; i++) {
        float v = scope[k][(scope_w + (uint32_t)i + 48u - (uint32_t)n) % 48u];
        int32_t yy = y + h / 2 - (int32_t)(v * (float)(h / 2 - 1));
        if (i)
            cv_line(x + i - 1, py, x + i, yy, i == n - 1 ? C_WHITE : c);
        py = yy;
    }
    cv_round(x + n - 2, py - 2, 5, 5, 2, C_WHITE);
}

static void graph_delay(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *p, uint16_t c)
{
    float lv = (float)p[P_DSND] / 127.0f, fb = (float)p[P_DFB] / 127.0f * 1.1f;
    int32_t gap = 4 + p[P_DTIM] * 14 / 127, xx = x, i;
    cv_rect(x, y, 3, h, C_WHITE);                                    /* the note */
    for (i = 0; i < 8 && lv > 0.03f; i++) {                          /* its echoes */
        int32_t hh = (int32_t)((float)h * (lv > 1.0f ? 1.0f : lv));
        xx += gap;
        if (xx + 3 > x + w)
            break;
        cv_rect(xx, y + h - hh, 3, hh, c);
        lv *= fb;
    }
    cv_rect(x, y + h, w, 1, C_LINE);
}

static void graph_arp(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *p, uint16_t c)
{
    static const int8_t CH[MN_NCHORD][4] = {{0, -1, -1, -1}, {0, 7, -1, -1}, {0, 4, 7, -1}, {0, 3, 7, -1}, {0, 5, 7, -1},
                                            {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 4, 7, 10}, {0, 4, 7, 9}, {0, 3, 6, -1},
                                            {0, 4, 8, -1}, {0, 3, 10, 14}};
    int notes[16], n = 0, o, j, mode = p[P_ARPM], i;
    const int8_t *ch = CH[p[P_ACHD] % MN_NCHORD];
    if (mode == ARP_OFF) {
        cv_text(x, y + 1, &FONT_XS, "ARP OFF", C_DIM);
        return;
    }
    for (o = 0; o <= (p[P_ARNG] & 3); o++)
        for (j = 0; j < 4 && ch[j] >= 0 && n < 16; j++)
            notes[n++] = ch[j] + 12 * o;
    for (i = 0; i < 12; i++) {                                       /* the first twelve notes it plays */
        int idx, top = notes[n - 1] ? notes[n - 1] : 1;
        switch (mode) {
        case ARP_DOWN: idx = n - 1 - i % n; break;
        case ARP_UPDN: { int per = n > 1 ? 2 * n - 2 : 1, q = i % per; idx = q < n ? q : per - q; break; }
        case ARP_RAND: idx = (int)((uint32_t)(i * 7 + 3) % (uint32_t)n); break;
        default: idx = i % n; break;
        }
        cv_rect(x + i * (w / 12), y + h - 3 - notes[idx] * (h - 4) / top, w / 12 - 2, 3, c);
    }
}

static void param_cell(int32_t x, int32_t y, const char *name, const char *val, int frac_x44, int bip, int active,
                       int hot, int locked, uint16_t c)
{
    uint16_t vc = locked ? C_WHITE : active ? c : mix(C_BLACK, C_GRAY, 11);
    int32_t cx = x + 30;
    const char *q = val;
    int lower = 0;
    if (hot) {                                      /* the knob just turned: its cell lights up, then fades */
        cv_round(x + 3, y - 2, 54, 45, 5, mix(tdim(ui.track, 3), tcol(ui.track), hot / 4));
        vc = C_WHITE;
    }
    text_c(cx, y, &FONT_XS, name, active ? (locked ? C_WHITE : C_GRAY) : C_DIM);
    for (; *q; q++)                                 /* (the big face has no lower case) */
        lower |= *q >= 'a' && *q <= 'z';
    if (lower || text_w(&FONT_M, val) > 56)
        text_c(cx, y + 17, &FONT_B, val, vc);
    else
        text_c(cx, y + 13, &FONT_M, val, vc);
    cv_round(cx - 22, y + 38, 44, 4, 2, C_LINE);
    {
        uint16_t bc = locked ? C_WHITE : active ? c : mix(C_BLACK, c, 6);
        if (bip) {
            int32_t m = cx, e2 = cx - 22 + frac_x44;
            cv_round(e2 < m ? e2 : m, y + 38, (e2 < m ? m - e2 : e2 - m) + 2, 4, 2, bc);
            cv_rect(m, y + 36, 1, 8, C_GRAY);
        } else if (frac_x44) {
            cv_round(cx - 22, y + 38, frac_x44 < 4 ? 4 : frac_x44, 4, 2, bc);
        }
    }
    if (locked) {                                   /* a lock: a white diamond */
        cv_rect(x + 6, y + 1, 1, 7, C_WHITE);
        cv_rect(x + 4, y + 3, 5, 3, C_WHITE);
        cv_rect(x + 5, y + 2, 3, 5, C_WHITE);
    }
}

/* the page's title: its label in its colour, what it is about, its graph, which four values the knobs have */
static void page_title(int view, const char *name, const char *sub)
{
    int32_t x = 4, r;
    x = chip(x, 28, name, PAGE_COL[view], C_BLACK);
    if (sub)
        cv_text(x + 6, 28, &FONT_S, sub, C_GRAY);
    for (r = 0; r < 2; r++)                          /* the rows: the one the knobs turn, bright */
        cv_round(232, 30 + r * 7, 6, 5, 2, r == ui.row[ui.view] ? PAGE_COL[view] : C_LINE);
}

static void draw_step_rec(void)                     /* STEP REC: a note a step, on to the next */
{
    int t = ui.track, s = ui.cursor % pat_len(), prev = (s + pat_len() - 1) % pat_len(), i, lo = 127, hi = 0;
    static const char *const SOFT[5] = {"REST", "SLD", "ACC", "TIE", "BACK"};   /* FX SEL ENV LFO EDIT */
    static const uint8_t SOFT_ARM[5] = {0, MN_SLIDE, MN_ACC, 0, 0};
    char a[16], *e;
    uint16_t c = tcol(t);
    int32_t x = chip(4, 28, "STEP REC", C_REC, C_WHITE);
    e = s_cpy(a, "STEP ");
    e = itoa_u((uint32_t)s + 1u, e);
    *e++ = '/';
    itoa_u((uint32_t)pat_len(), e);
    cv_text(x + 8, 28, &FONT_S, a, C_WHITE);
    cv_text(236 - text_w(&FONT_XS, "LEGATO = SLIDE"), 30, &FONT_XS, "LEGATO = SLIDE", C_GRAY);
    /* the note before the cursor, big */
    if (pat()->trig[t][prev] & MN_TRIG) {
        note_name(pat()->note[t][prev], a);
        cv_text(10, 50, &FONT_L, a, c);
        if (pat()->trig[t][prev] & MN_ACC)
            chip(10, 86, "ACC", c, C_BLACK);
        if (pat()->trig[t][prev] & MN_SLIDE)
            chip(50, 86, "SLD", c, C_BLACK);
    } else {
        cv_text(10, 50, &FONT_L, "--", C_DIM);
    }
    /* a piano roll of the track's steps on this page: the cursor white */
    for (i = 0; i < pat_len() && i < MN_NSTEP; i++)
        if (pat()->trig[t][i] & MN_TRIG) {
            lo = pat()->note[t][i] < lo ? pat()->note[t][i] : lo;
            hi = pat()->note[t][i] > hi ? pat()->note[t][i] : hi;
        }
    if (hi - lo < 12) {
        lo = (lo + hi) / 2 - 6;
        hi = lo + 12;
    }
    cv_round(98, 48, 138, 54, 4, mix(C_BLACK, C_LINE, 10));
    for (i = 0; i < 16; i++) {
        int st = ui.spage * 16 + i;
        int32_t xx = 101 + i * 8 + (i / 4);
        if (st >= pat_len())
            continue;
        if (st == s)
            cv_rect(xx - 1, 49, 8, 52, blink() ? C_WHITE : C_GRAY);
        if (pat()->trig[t][st] & MN_TRIG) {
            int32_t yy = 96 - (pat()->note[t][st] - lo) * 44 / (hi - lo);
            cv_rect(xx, yy, 6, 4, (pat()->trig[t][st] & MN_ACC) ? C_WHITE : c);
            if (pat()->trig[t][st] & MN_SLIDE)
                cv_rect(xx - 3, yy + 1, 3, 2, c);
        }
    }
    /* the five buttons above the keys, as they sit on the panel: what they do here */
    for (i = 0; i < 5; i++) {
        static const char *const BTN_NAME[5] = {"FX", "SEL", "ENV", "LFO", "EDIT"};
        int32_t bx = 4 + i * 47;
        int on = SOFT_ARM[i] && (ui.arm & SOFT_ARM[i]);
        cv_round(bx, 108, 43, 20, 5, on ? c : mix(C_BLACK, C_LINE, 12));
        text_c(bx + 21, 110, &FONT_XS, SOFT[i], on ? C_BLACK : C_WHITE);
        text_c(bx + 21, 129, &FONT_XS, BTN_NAME[i], C_DIM);
    }
}

static void draw_track_page(void)
{
    int t = ui.track, m = kit()->machine[t], pg = page_of_view(), r, i, hs = held_step(), hot = touch_fade();
    char a[24], *e;
    uint16_t c = tcol(t);
    const uint8_t *par = kit()->par[t];
    if (hs >= 0) {                                   /* a step held: its note and its locks */
        e = s_cpy(a, "STEP ");
        e = itoa_u((uint32_t)hs + 1u, e);
        chip(4, 28, a, C_WHITE, C_BLACK);
        if (pat()->trig[t][hs] & MN_TRIG) {
            note_name(pat()->note[t][hs], a);
            cv_text(70, 28, &FONT_B, a, c);
        } else {
            cv_text(70, 29, &FONT_XS, "NO TRIG: TRIGLESS LOCK", C_GRAY);
        }
        e = itoa_u((uint32_t)mn_step_locks(pat(), t, hs), a);
        s_cpy(e, mn_step_locks(pat(), t, hs) == 1 ? " LOCK" : " LOCKS");
        cv_text(234 - text_w(&FONT_XS, a), 29, &FONT_XS, a, C_WHITE);
    } else {
        const char *sub = 0;
        char nm[8];
        if (pg == PG_SYN)
            sub = mn_machine_long(m);
        s_cpy(nm, mn_page_name(pg));
        page_title(ui.view, nm, sub);
        switch (pg) {
        case PG_AMP: graph_env(150, 30, 76, 14, par, c); break;
        case PG_FILT: graph_filter(150, 30, 76, 14, par, c); break;
        case PG_FX: graph_delay(150, 30, 76, 14, par, c); break;
        case PG_TRK: graph_arp(150, 30, 76, 14, par, c); break;
        case PG_LFO1: case PG_LFO2: case PG_LFO3: {
            int b = pg * 8;
            e = s_cpy(a, "> ");
            mn_dest_text(kit()->machine, t, par[b + L_TRK], par[b + L_DEST], e);
            cv_text(54, 28, &FONT_S, a, par[b + L_DEST] ? C_GRAY : C_DIM);
            graph_lfo(176, 30, 48, 14, pg - PG_LFO1, c);
            break;
        }
        default: break;
        }
    }
    for (r = 0; r < 2; r++) {
        int active = r == ui.row[ui.view];
        int32_t y = 50 + r * 47;
        if (active)
            cv_round(2, y - 3, 236, 46, 5, tdim(t, 2));
        for (i = 0; i < 4; i++) {
            int p = pg * 8 + r * 4 + i, v = par[p], lk = -1, mx;
            const mn_pinfo_t *in = mn_par_info(m, p);
            char val[16];
            if (hs >= 0)
                lk = mn_lock_get(pat(), t, p, hs);
            if (lk >= 0)
                v = lk;
            mx = in->max ? in->max : 1;
            if (p >= P_LFO1 && p < P_ARPM && ((p - P_LFO1) & 7) == L_DEST)
                mn_dest_text(kit()->machine, t, par[p - L_DEST + L_TRK], v, val);
            else
                mn_par_text(m, p, v, val);
            param_cell(i * 60, y, in->name, val, 44 * v / mx, in->kind == PK_BIP, active,
                       active && ui.touched == i ? hot : 0, lk >= 0, c);
        }
    }
}

static void draw_scale_keys(int32_t x, int32_t y, int32_t w, int32_t h)   /* one octave from the root */
{
    static const uint8_t BLK[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
    int i, nw = 0;
    int32_t kw = w / 7;
    mn_pat_t *p = pat();
    for (i = 0; i < 12; i++) {                       /* white keys first, then the black ones over them */
        int in = mn_in_scale(p->scale, p->root, i), root = i == p->root;
        uint16_t c = root ? tcol(ui.track) : in ? mix(C_WHITE, tcol(ui.track), 5) : mix(C_BLACK, C_GRAY, 8);
        if (BLK[i])
            continue;
        cv_round(x + nw * kw, y, kw - 2, h, 3, c);
        text_c(x + nw * kw + kw / 2 - 1, y + h - 15, &FONT_XS, NOTE_NAME[i], root || in ? C_BLACK : C_GRAY);
        nw++;
    }
    for (i = 0, nw = 0; i < 12; i++) {
        int in = mn_in_scale(p->scale, p->root, i), root = i == p->root;
        uint16_t c = root ? tcol(ui.track) : in ? mix(C_BLACK, tcol(ui.track), 11) : C_LINE;
        if (!BLK[i]) {
            nw++;
            continue;
        }
        cv_round(x + nw * kw - kw / 3 - 1, y, kw * 2 / 3, h * 3 / 5, 3, c);
    }
}

static void draw_home(void)
{
    mn_pat_t *p = pat();
    char a[24], v[4][12], *e;
    int i, t, s, row = ui.row[V_HOME], hot = touch_fade();
    e = s_cpy(a, "PATTERN ");
    slot_name(sb.slot[buf_of(song())], e);
    page_title(V_HOME, a, 0);
    e = itoa_u((uint32_t)(MN_NLOCK - mn_lock_rows_free(p)), a);
    s_cpy(e, "/32 LOCKS");
    cv_text(226 - text_w(&FONT_XS, a), 29, &FONT_XS, a, C_DIM);
    cv_round(2, 47, 236, 46, 5, tdim(ui.track, 2));
    if (!row) {
        static const char *const N[4] = {"TEMPO", "SWING", "LENGTH", "SPEED"};
        int fr[4];
        e = itoa_u(proj.tempo10 / 10u, v[0]);
        *e++ = '.';
        *e++ = (char)('0' + proj.tempo10 % 10u);
        *e = 0;
        e = itoa_u(p->swing, v[1]);
        s_cpy(e, "%");
        itoa_u(p->len, v[2]);
        s_cpy(v[3], SPEED_NAME[p->speed % MN_NSPEED]);
        fr[0] = 44 * (proj.tempo10 - 300) / 2700;
        fr[1] = 44 * (p->swing - 50) / 30;
        fr[2] = 44 * (p->len - 1) / 63;
        fr[3] = 44 * p->speed / (MN_NSPEED - 1);
        for (i = 0; i < 4; i++)
            param_cell(i * 60, 50, N[i], v[i], fr[i], 0, 1, ui.touched == i ? hot : 0, 0, tcol(ui.track));
        /* every track's steps, this page, in their colours */
        for (t = 0; t < MN_NT; t++)
            for (s = 0; s < 16; s++) {
                int st = ui.spage * 16 + s;
                int32_t x = 8 + s * 14 + (s / 4) * 2, y = 98 + t * 7;
                uint16_t c = C_LINE;
                if (st >= p->len)
                    continue;
                if (p->trig[t][st] & MN_TRIG)
                    c = (mn_mute >> t & 1u) ? C_DIM : (p->trig[t][st] & MN_ACC) ? tcol(t) : tdim(t, 10);
                if (st == mn_step)
                    c = (p->trig[t][st] & MN_TRIG) ? C_WHITE : C_GRAY;
                cv_rect(x, y, 12, 5, c);
                if (t == ui.track && s == 0)
                    cv_rect(2, y, 3, 5, tcol(t));
            }
    } else {
        static const char *const N[3] = {"ROOT", "SCALE", "SNAP"};
        s_cpy(v[0], NOTE_NAME[p->root % 12]);
        s_cpy(v[1], mn_scale_name(p->scale));
        s_cpy(v[2], p->snap ? "ON" : "OFF");
        for (i = 0; i < 3; i++)
            param_cell(i * 60, 50, N[i], v[i], i == 0 ? 44 * p->root / 11 : i == 1 ? 44 * p->scale / (MN_NSCALE - 1) : 0,
                       0, 1, ui.touched == i ? hot : 0, 0, tcol(ui.track));
        cv_text(184, 52, &FONT_XS, p->snap ? "KEYS SNAP" : "KEYS: LEDS", C_GRAY);
        cv_text(184, 68, &FONT_XS, "TO THE", C_DIM);
        cv_text(184, 80, &FONT_XS, "SCALE", C_DIM);
        draw_scale_keys(8, 98, 224, 42);
    }
}

static void draw_glo(void)
{
    char v[8][12], *e;
    int i, r, hot = touch_fade();
    static const char *const N[8] = {"TRK CH", "AUTO CH", "CLK IN", "CLK OUT", "MIDI OUT", "FOLLOW", "THEME", "CPU"};
    e = itoa_u(proj.midi_base + 1u, v[0]);
    *e++ = '-';
    itoa_u(proj.midi_base + 6u, e);
    if (proj.auto_ch)
        itoa_u(proj.auto_ch, v[1]);
    else
        s_cpy(v[1], "OFF");
    s_cpy(v[2], proj.clock_in ? "ON" : "OFF");
    s_cpy(v[3], proj.clock_out ? "ON" : "OFF");
    s_cpy(v[4], proj.midi_out ? "ON" : "OFF");
    s_cpy(v[5], proj.follow ? "ON" : "OFF");
    s_cpy(v[6], PALETTES[proj.theme % NPALETTES].name);
    e = itoa_u(plat_cpu_pct(), v[7]);
    s_cpy(e, "%");
    page_title(V_GLO, "GLOBAL", 0);
    cv_text(226 - text_w(&FONT_XS, "MNM-1 " OM_VERSION), 29, &FONT_XS, "MNM-1 " OM_VERSION, C_DIM);
    for (r = 0; r < 2; r++) {
        int active = r == ui.row[V_GLO];
        if (active)
            cv_round(2, 47 + r * 47, 236, 46, 5, mix(C_BLACK, C_LINE, 12));
        for (i = 0; i < 4; i++)
            param_cell(i * 60, 50 + r * 47, N[r * 4 + i], v[r * 4 + i], 0, 0, active,
                       active && ui.touched == i ? hot : 0, 0, C_HI);
    }
}

static void draw_main(void)
{
    uint32_t h = hash(2166136261u, ui.view | (uint32_t)ui.lfo << 4 | (uint32_t)ui.row[ui.view] << 6 |
                                       (uint32_t)ui.track << 7 | (uint32_t)ui.held << 10 | (uint32_t)mn_mute << 26);
    int t, i;
    const uint8_t *par = kit()->par[ui.track];
    for (t = 0; t < MN_NT; t++) {
        uint8_t tc = mn_trig_count[t];
        float lv = mn_level[t];
        ui.flash[t] = tc != ui.trig_seen[t] ? 6 : ui.flash[t] ? ui.flash[t] - 1 : 0;
        ui.trig_seen[t] = tc;
        ui.meter[t] = lv > ui.meter[t] ? lv : ui.meter[t] * 0.82f;   /* meters: up at once, down slowly */
        h = hash(h, (uint32_t)(ui.meter[t] * 60.0f) | (uint32_t)kit()->machine[t] << 8 | (uint32_t)ui.flash[t] << 12);
    }
    for (i = 0; i < 3; i++)                          /* the scopes: one value a frame */
        scope[i][scope_w] = mn_lfo_val[ui.track][i];
    scope_w = (uint8_t)((scope_w + 1u) % 48u);
    for (i = 0; i < MN_NP; i += 4)
        h = hash(h, (uint32_t)par[i] | (uint32_t)par[i + 1] << 8 | (uint32_t)par[i + 2] << 16 | (uint32_t)par[i + 3] << 24);
    h = hash(h, (uint32_t)touch_fade() | (uint32_t)(ui.touched + 1) << 5 | (uint32_t)ui.spage << 8 |
                    (uint32_t)step_rec() << 12 | (uint32_t)ui.arm << 13 | (uint32_t)ui.cursor << 16);
    if (step_rec()) {
        h = hash(h, (uint32_t)blink());
        for (i = 0; i < MN_NSTEP; i++)
            h = hash(h, (uint32_t)pat()->trig[ui.track][i] | (uint32_t)pat()->note[ui.track][i] << 8);
    }
    if (ui.view == V_HOME) {
        h = hash(hash(h, (uint32_t)(mn_step + 1) | (uint32_t)pat()->len << 8 | (uint32_t)pat()->swing << 16 |
                             (uint32_t)pat()->speed << 24), proj.tempo10 | (uint32_t)mn_lock_rows_free(pat()) << 16 |
                                                                (uint32_t)pat()->root << 22 | (uint32_t)pat()->scale << 26 |
                                                                (uint32_t)pat()->snap << 31);
        for (t = 0; t < MN_NT; t++)
            for (i = 0; i < 16; i += 4) {               /* (bytes: the pattern is not word aligned) */
                const uint8_t *g = &pat()->trig[t][ui.spage * 16 + i];
                h = hash(h, (uint32_t)g[0] | (uint32_t)g[1] << 8 | (uint32_t)g[2] << 16 | (uint32_t)g[3] << 24);
            }
    }
    if (ui.view == V_GLO)
        h = hash(h, (uint32_t)proj.midi_base | (uint32_t)proj.auto_ch << 4 | (uint32_t)proj.clock_in << 9 |
                        (uint32_t)proj.clock_out << 10 | (uint32_t)proj.midi_out << 11 | (uint32_t)proj.follow << 12 |
                        (uint32_t)proj.theme << 13 | plat_cpu_pct() << 16);
    if (ui.view >= PG_LFO1 && ui.view <= PG_LFO3)
        h = hash(h, ui.frame);                      /* (the scope moves every frame) */
    if (held_step() >= 0) {
        int r;
        for (r = 0; r < MN_NLOCK; r++)
            h = hash(h, (uint32_t)pat()->ltrk[r] << 8 | pat()->lval[r][held_step()]);
        h = hash(h, (uint32_t)pat()->note[ui.track][held_step()] | (uint32_t)pat()->trig[ui.track][held_step()] << 8);
    }
    if (h == ui.sig[1])
        return;
    ui.sig[1] = h;
    cv_begin(240, MAIN_H, C_BLACK);
    draw_tracks();
    if (view_is_track() && step_rec())
        draw_step_rec();
    else if (view_is_track())
        draw_track_page();
    else if (ui.view == V_HOME)
        draw_home();
    else
        draw_glo();
    cv_blit(0, MAIN_Y);
}

static void draw_steps(void)
{
    mn_pat_t *p = pat();
    int t = ui.track, w, len = pat_len(), pages = (len + 15) / 16, cur_page = mn_step >= 0 ? mn_step / 16 : -1;
    int lo = 127, hi = 0, melodic = kit()->machine[t] != MC_DRUM, pulse;
    uint32_t now = plat_ms(), h;
    uint16_t c = tcol(t);
    char a[24], *e;
    if (mn_step != ui.step_seen) {                   /* a new step: it lights up for a moment */
        ui.step_seen = mn_step;
        ui.pulse_step = mn_step;
        ui.pulse_t = now;
    }
    pulse = mn_step >= 0 && now - ui.pulse_t < 120u;
    h = hash(2166136261u, (uint32_t)(mn_step + 1) | (uint32_t)ui.spage << 8 | (uint32_t)len << 12 | (uint32_t)t << 20 |
                              (uint32_t)ui.keys_mode << 24 | (uint32_t)ui.rec << 25 | (uint32_t)(ui.octave + 4) << 26 |
                              (uint32_t)pulse << 29 | (uint32_t)step_rec() << 30);
    h = hash(h, ui.held | (uint32_t)ui.cursor << 20 | (uint32_t)(step_rec() && blink()) << 28 | (uint32_t)p->scale << 16);
    h = hash(h, (uint32_t)p->root | (uint32_t)p->snap << 4 | ui.keys << 5);
    for (w = 0; w < len; w++) {
        if (p->trig[t][w] & MN_TRIG) {
            lo = p->note[t][w] < lo ? p->note[t][w] : lo;
            hi = p->note[t][w] > hi ? p->note[t][w] : hi;
        }
    }
    for (w = 0; w < 16; w++) {
        int s = ui.spage * 16 + w;
        h = hash(h, (uint32_t)p->trig[t][s] | (uint32_t)mn_step_locks(p, t, s) << 4 | (uint32_t)p->note[t][s] << 12);
    }
    if (h == ui.sig[2])
        return;
    ui.sig[2] = h;
    if (hi - lo < 12) {
        lo = (lo + hi) / 2 - 6;
        hi = lo + 12;
    }
    cv_begin(240, FTR_H, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    for (w = 0; w < 16; w++) {
        int s = ui.spage * 16 + w;
        int32_t x = 6 + w * 14 + (w / 4) * 2;
        uint8_t f = p->trig[t][s];
        uint16_t bg = (w / 4) & 1 ? mix(C_LINE, C_BLACK, 4) : C_LINE;   /* (beats: every other four a shade darker) */
        if (s >= len) {
            cv_rect(x, 26, 12, 2, C_LINE);
            continue;
        }
        if (f & MN_TRIG)
            bg = (f & MN_ACC) ? c : tdim(t, 10);
        if (s == mn_step && pulse && (f & MN_TRIG))
            bg = mix(bg, C_WHITE, 10);                /* the note playing: a flash */
        if (ui.held >> w & 1u && !ui.keys_mode)
            bg = C_WHITE;
        cv_round(x, 6, 12, 22, 3, bg);
        if ((f & MN_TRIG) && melodic) {              /* the note's height: a piano roll in the steps */
            int32_t yy = 24 - (p->note[t][s] - lo) * 16 / (hi - lo);
            cv_rect(x + 2, yy, 8, 2, (f & MN_ACC) || (ui.held >> w & 1u) ? C_BLACK : mix(C_BLACK, c, 4));
        }
        if (f & MN_SLIDE)
            cv_rect(x - 4, 14, 6, 3, C_WHITE);       /* a slide: a bridge from the step before */
        if (mn_step_locks(p, t, s))
            cv_round(x + 4, 8, 4, 4, 2, (f & MN_TRIG) ? C_BLACK : C_WHITE);
        if (s == mn_step)                            /* the step playing: a bar under it */
            cv_rect(x, 31, 12, 3, C_WHITE);
        if (step_rec() && s == ui.cursor) {          /* STEP REC: the cursor */
            uint16_t cc = blink() ? C_WHITE : C_REC;
            cv_rect(x - 1, 4, 14, 1, cc);
            cv_rect(x - 1, 29, 14, 1, cc);
            cv_rect(x - 1, 4, 1, 26, cc);
            cv_rect(x + 12, 4, 1, 26, cc);
        }
    }
    /* the line below: the mode, the pages of steps, the scale or the black keys' tools */
    {
        int32_t x = 6;
        const char *mode = step_rec() ? "STEP REC" : ui.keys_mode ? "KEYS" : "GRID";
        x = chip(x, 42, mode, step_rec() ? C_REC : ui.keys_mode ? c : C_LINE, step_rec() ? C_WHITE : ui.keys_mode ? C_BLACK : C_AMB);
        x += 8;
        for (w = 0; w < pages; w++) {
            uint16_t pc = w == ui.spage ? c : w == cur_page ? C_GRAY : C_LINE;
            cv_rect(x + w * 12, 47, 9, 6, pc);
        }
        x += pages * 12 + 6;
        e = itoa_u((uint32_t)ui.spage * 16u + 1u, a);
        *e++ = '-';
        e = itoa_u((uint32_t)(ui.spage * 16 + 16 > len ? len : ui.spage * 16 + 16), e);
        *e++ = '/';
        itoa_u((uint32_t)len, e);
        cv_text(x, 43, &FONT_XS, a, C_DIM);
        if (ui.keys_mode) {
            e = s_cpy(a, NOTE_NAME[p->root % 12]);
            *e++ = ' ';
            e = s_cpy(e, mn_scale_name(p->scale));
            if (p->snap)
                e = s_cpy(e, " SNAP");
            e = s_cpy(e, "  OCT ");
            if (ui.octave > 0)
                *e++ = '+';
            if (ui.octave < 0)
                *e++ = '-';
            itoa_u((uint32_t)(ui.octave < 0 ? -ui.octave : ui.octave), e);
            cv_text(234 - text_w(&FONT_XS, a), 43, &FONT_XS, a, c);
        } else {
            int b;
            int32_t bx = 234;
            for (b = NBLACK - 1; b >= MN_NT; b--) {   /* the black keys' tools, as a reminder */
                int on = ui.keys >> BLACK_K[b] & 1u;
                bx -= text_w(&FONT_XS, BK_NAME[b]) + 4;
                cv_text(bx, 43, &FONT_XS, BK_NAME[b], on ? C_WHITE : C_DIM);
            }
        }
    }
    cv_blit(0, FTR_Y);
}

static void leds(void)
{
    static const uint8_t VIEW_BTN[NVIEWS] = {B_EDIT, B_ENV, B_SEL, B_FX, B_LFO, B_LFO, B_LFO, B_ARP, B_HOME, B_GLO};
    uint32_t b = step_rec() ? 0u : 1u << VIEW_BTN[ui.view], k = 0, d = 0;
    int w, t;
    if (mn_playing && (mn_step & 3) != 0)            /* PLAY: lit, dark on each beat */
        b |= 1u << B_PLAY;
    if (ui.rec && (!step_rec() || blink()))
        b |= 1u << B_REC;
    if (step_rec()) {                                /* STEP REC: its five buttons lit; slide / accent armed blink */
        b |= 1u << B_FX | 1u << B_LFO | 1u << B_EDIT;
        if (!(ui.arm & MN_SLIDE) || blink())
            b |= 1u << B_SEL;
        if (!(ui.arm & MN_ACC) || blink())
            b |= 1u << B_ENV;
    }
    if (!ui.keys_mode)
        b |= 1u << B_SEQ;
    if (ui.keys_mode ? ui.octave != 0 : ui.spage != 0)
        b |= ui.keys_mode && ui.octave < 0 ? 1u << B_OCTDN : 1u << B_OCTUP;
    if (ui.ask && blink())
        b |= 1u << B_SAVE;
    if (ui.keys_mode) {                              /* the keys held lit; the scale's keys dim, its roots lit */
        mn_pat_t *p = pat();
        k = ui.keys;
        for (w = 0; w < NKEYS; w++) {
            int n = key_base() + w;
            if ((n - p->root) % 12 == 0 || (n - p->root) % 12 == -12)
                k |= 1u << w;
            else if (p->scale && mn_in_scale(p->scale, p->root, n))
                d |= 1u << w;
        }
    } else {
        for (w = 0; w < NWHITE; w++) {
            int s = step_of(w), on = s < pat_len() && (pat()->trig[ui.track][s] & MN_TRIG);
            if (s == mn_step)
                on = !on;                            /* the step playing: the other way round */
            if (on)
                k |= 1u << WHITE_K[w];
            else if (s < pat_len() && mn_step_locks(pat(), ui.track, s))
                d |= 1u << WHITE_K[w];
        }
        for (t = 0; t < MN_NT; t++) {                /* the track selected lit, the others dim (muted: dark) */
            if (t == ui.track)
                k |= 1u << BLACK_K[t];
            else if (!(mn_mute >> t & 1u))
                d |= 1u << BLACK_K[t];
        }
        k |= ui.keys & (1u << BLACK_K[BK_MUTE] | 1u << BLACK_K[BK_ACC] | 1u << BLACK_K[BK_SLD] |
                        1u << BLACK_K[BK_COPY] | 1u << BLACK_K[BK_PASTE]);
        if (mn_mute)
            d |= 1u << BLACK_K[BK_MUTE];
    }
    plat_leds(b, k, d);
}

/* AUTOSAVE: a few seconds after a change, stopped and quiet (a flash erase silences the audio for a
 * moment; while it plays, the changes wait in RAM) */
#define AUTOSAVE_QUIET 4000u
static void autosave(void)
{
    uint32_t now = plat_ms();
    int i;
    if (!ui_dirty() || mn_playing || ui.btn || ui.keys || now - ui.act_t < AUTOSAVE_QUIET || mn_out_level > 0.0005f)
        return;
    for (i = 0; i < NBUF; i++)
        if (sb.dirty[i] && sb.slot[i] < NSLOT)
            save_buf(i);
    if (ui.pdirty && project_save() == 0)
        ui.pdirty = 0;
}

static void midi_hook(uint32_t pkt) { plat_midi_out(pkt); }

void ui_init(void)
{
    int i;
    memset(&ui, 0, sizeof ui);
    ui.touched = -1;
    ui.sel_slot = -1;
    ui.step_seen = -1;
    for (i = 0; i < MN_NT; i++)
        ui.last_note[i] = 60;
    for (i = 0; i < NBUF; i++) {
        sb.slot[i] = 0xFF;
        sb.dirty[i] = 0;
    }
    if (mn_song() == 0) {                           /* the slot last playing */
        song_load(proj.slot, &sbuf[0]);
        mn_set_song(&sbuf[0]);
    }
    sb.slot[buf_of(mn_song())] = proj.slot;
    mn_midi_hook = midi_hook;
    lcd_fill(0, 0, 240, 240, C_BLACK);
}

void ui_frame(void)
{
    input();
    autosave();
    draw_header();
    draw_main();
    draw_steps();
    leds();
    ui.frame++;
}

void ui_input_only(void) { input(); }
