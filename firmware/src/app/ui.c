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
 *   KEYS       27 keys from F, OCT- / OCT+ the octave. With REC: playing, the notes go on the steps they
 *              fall on; stopped, one step after another (step entry)
 *   PLAY       start / stop     REC  record: the keys and MIDI notes, and the knobs as locks on the trigs
 *   EDIT SYN  ENV AMP  SEL FILT  FX FX  LFO LFO1-3  ARP TRK   pressed again: the other four of the page
 *   HOME       the pattern: tempo, swing, length, speed, and every track's steps
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
    if (ui.row[V_HOME])
        return;
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

/* recording a note: playing, on the step it falls on (the nearer); stopped, at the cursor, which moves on */
static void rec_note(int t, int n)
{
    int s;
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
        ui.cursor = (uint8_t)((s + 1) % pat_len());
        ui.spage = (uint8_t)(ui.cursor / 16);
    }
    pat()->trig[t][s] = MN_TRIG;
    pat()->note[t][s] = (uint8_t)n;
    mark_dirty();
}

static void play_note(int t, int n, int vel)
{
    mn_note_on(t, n, vel);
    note_out(t, 1, n, vel);
    ui.last_note[t] = (uint8_t)n;
    rec_note(t, n);
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
        ui.key_note[k] = (uint8_t)clampi(key_base() + (int)k, 0, 127);
        play_note(ui.track, ui.key_note[k], 100);
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
        ui.key_note[k] = (uint8_t)clampi(key_base() + (int)k, 0, 127);
        play_note(ui.track, ui.key_note[k], 100);
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
        } else if (v != V_HOME) {
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
        if (ui.rec && !mn_playing)
            ui.cursor = (uint8_t)(ui.spage * 16);
        say("REC", ui.rec ? "ON" : "OFF");
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
            rec_note(t, (int)d1);
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
        if (ui.held && !ui.keys_mode) {               /* a step held: its octave */
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
                    pat()->note[ui.track][s] = (uint8_t)clampi(pat()->note[ui.track][s] + (int)e, 0, 127);
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
/* three bands: the header (28 px), the page (144), the steps (68) */
#define HDR_H 28
#define MAIN_Y 28
#define MAIN_H 144
#define FTR_Y 172
#define FTR_H 68

static uint32_t hash(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

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

static void draw_header(void)
{
    char t[24], *e;
    int msg = plat_ms() < ui.msg_until, b = buf_of(song());
    uint32_t h = hash(hash(2166136261u, (uint32_t)msg | (uint32_t)mn_playing << 1 | (uint32_t)ui.rec << 2 |
                                            (uint32_t)mn_ext << 3 | (uint32_t)ui.track << 4 | (uint32_t)ui_dirty() << 8 |
                                            (uint32_t)(mn_queued() != 0) << 9 | (uint32_t)blink() << 10 |
                                            (uint32_t)(ui.sel_slot + 1) << 11 | (uint32_t)kit()->machine[ui.track] << 18),
                      (uint32_t)proj.tempo10 | (uint32_t)sb.slot[b] << 16 | (uint32_t)ui.queued_slot << 24);
    if (msg)
        h = hash(hash(h, (uint32_t)ui.msg[0][0] << 8 | (uint32_t)ui.msg[0][1] | (uint32_t)ui.msg[1][0] << 16),
                 ui.msg_until ^ (uint32_t)s_len(ui.msg[1]) << 20 ^ (uint32_t)ui.msg[1][1] << 24);
    if (h == ui.sig[0])
        return;
    ui.sig[0] = h;
    cv_begin(240, HDR_H, C_BLACK);
    if (msg) {
        int32_t x = cv_text(8, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        int32_t x;
        slot_name(sb.slot[b], t);
        x = cv_text(8, 6, &FONT_B, t, C_HI);
        if (mn_queued() && blink()) {               /* the next pattern, waiting for the end of this one */
            e = s_cpy(t, ">");
            slot_name(ui.queued_slot, e);
            x = cv_text(x + 2, 6, &FONT_B, t, C_WHITE);
        }
        if (ui_dirty())
            cv_round(x + 3, 10, 4, 4, 2, C_DIM);
        e = s_cpy(t, "T");
        *e++ = (char)('1' + ui.track);
        *e++ = ' ';
        s_cpy(e, mn_machine_name(kit()->machine[ui.track]));
        cv_text(x + 14, 6, &FONT_B, t, C_AMB);
        /* the tempo, the transport */
        e = itoa_u(proj.tempo10 / 10u, t);
        *e++ = '.';
        *e++ = (char)('0' + proj.tempo10 % 10u);
        *e = 0;
        x = 232 - text_w(&FONT_B, t);
        cv_text(x, 6, &FONT_B, t, mn_ext ? C_DIM : C_HI);
        if (mn_ext) {
            x -= text_w(&FONT_XS, "EXT") + 5;
            cv_text(x, 8, &FONT_XS, "EXT", C_HI);
        }
        x -= 16;
        if (mn_playing) {                            /* a triangle */
            int32_t j;
            for (j = 0; j < 6; j++)
                cv_rect(x + j, 8 + j, 1, 12 - 2 * j, C_HI);
        } else {
            cv_rect(x, 9, 9, 9, C_DIM);
        }
        if (ui.rec) {
            x -= 14;
            cv_round(x, 8, 10, 10, 5, RGB(240, 60, 50));
        }
    }
    cv_blit(0, 0);
}

static uint16_t mix(uint16_t a, uint16_t b, int t)   /* t 0..16: a -> b */
{
    int r = ((a >> 11) * (16 - t) + (b >> 11) * t) / 16, g = (((a >> 5) & 63) * (16 - t) + ((b >> 5) & 63) * t) / 16,
        bl = ((a & 31) * (16 - t) + (b & 31) * t) / 16;
    return (uint16_t)(r << 11 | g << 5 | bl);
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

static void draw_tracks(void)                       /* the strip: the six tracks */
{
    int t;
    for (t = 0; t < MN_NT; t++) {
        int32_t x = t * 40 + 1;
        int sel = t == ui.track, muted = mn_mute >> t & 1u, lv;
        char s[8];
        uint16_t bg = sel ? C_HI : ui.flash[t] ? C_DIM : C_LINE, fg = sel ? C_BLACK : muted ? C_DIM : C_AMB;
        s[0] = (char)('1' + t);
        s[1] = ' ';
        s_cpy(s + 2, mn_machine_name(kit()->machine[t]));
        cv_round(x, 0, 38, 19, 4, bg);
        text_c(x + 19, 2, &FONT_XS, s, fg);
        if (muted)
            cv_rect(x + 4, 9, 30, 1, sel ? C_BLACK : C_DIM);
        lv = (int)(mn_level[t] * 60.0f);
        lv = lv > 38 ? 38 : lv;
        cv_rect(x, 21, 38, 3, C_LINE);
        if (lv)
            cv_rect(x, 21, lv, 3, muted ? C_DIM : C_HI);
    }
}

static void param_cell(int32_t x, int32_t y, const char *name, const char *val, int frac_x44, int bip, int active,
                       int hot, int locked)
{
    uint16_t vc = hot ? C_WHITE : locked ? C_WHITE : active ? C_HI : C_DIM;
    int32_t cx = x + 30;
    text_c(cx, y, &FONT_XS, name, active ? (locked ? C_HI : C_GRAY) : C_DIM);
    const char *q = val;
    int lower = 0;
    for (; *q; q++)                                 /* (the big face has no lower case) */
        lower |= *q >= 'a' && *q <= 'z';
    if (lower || text_w(&FONT_M, val) > 56)
        text_c(cx, y + 17, &FONT_B, val, vc);
    else
        text_c(cx, y + 13, &FONT_M, val, vc);
    cv_rect(cx - 22, y + 39, 44, 3, C_LINE);
    if (bip) {
        int32_t m = cx, e2 = cx - 22 + frac_x44;
        cv_rect(e2 < m ? e2 : m, y + 39, (e2 < m ? m - e2 : e2 - m) + 1, 3, active ? (locked ? C_WHITE : C_HI) : C_DIM);
    } else if (frac_x44) {
        cv_rect(cx - 22, y + 39, frac_x44, 3, active ? (locked ? C_WHITE : C_HI) : C_DIM);
    }
    if (locked)
        cv_round(x + 2, y + 1, 5, 5, 2, C_WHITE);
}

static void draw_track_page(void)
{
    int t = ui.track, m = kit()->machine[t], pg = page_of_view(), r, i, hs = held_step();
    char a[24], *e;
    /* the title: the page, the machine (SYN), the LFO's target; a step held: its note and locks */
    if (hs >= 0) {
        e = s_cpy(a, "STEP ");
        e = itoa_u((uint32_t)hs + 1u, e);
        if (pat()->trig[t][hs] & MN_TRIG) {
            *e++ = ' ';
            *e++ = ' ';
            e = note_name(pat()->note[t][hs], e);
        }
        cv_text(6, 28, &FONT_B, a, C_WHITE);
        e = itoa_u((uint32_t)mn_step_locks(pat(), t, hs), a);
        s_cpy(e, mn_step_locks(pat(), t, hs) == 1 ? " LOCK" : " LOCKS");
        cv_text(234 - text_w(&FONT_XS, a), 30, &FONT_XS, a, C_HI);
    } else {
        int32_t x = cv_text(6, 28, &FONT_B, mn_page_name(pg), C_HI);
        if (pg == PG_SYN)
            cv_text(x + 8, 28, &FONT_S, mn_machine_long(m), C_AMB);
        else if (pg >= PG_LFO1 && pg <= PG_LFO3) {
            int b = pg * 8;
            float lv = mn_lfo_val[t][pg - PG_LFO1];
            e = s_cpy(a, "> ");
            mn_dest_text(kit()->machine, t, kit()->par[t][b + L_TRK], kit()->par[t][b + L_DEST], e);
            cv_text(x + 8, 28, &FONT_S, a, C_AMB);
            cv_rect(150, 34, 40, 3, C_LINE);       /* the LFO moving */
            cv_rect(170 + (int32_t)(lv * 19.0f) - 1, 32, 3, 7, C_HI);
        }
        a[0] = ui.row[ui.view] ? '5' : '1';
        a[1] = '-';
        a[2] = ui.row[ui.view] ? '8' : '4';
        a[3] = 0;
        cv_text(234 - text_w(&FONT_XS, a), 30, &FONT_XS, a, C_DIM);
    }
    for (r = 0; r < 2; r++) {
        int active = r == ui.row[ui.view];
        int32_t y = 50 + r * 47;
        if (active)
            cv_round(2, y - 3, 236, 46, 5, C_LINE);
        for (i = 0; i < 4; i++) {
            int p = pg * 8 + r * 4 + i, v = kit()->par[t][p], lk = -1, mx;
            const mn_pinfo_t *in = mn_par_info(m, p);
            char val[16];
            if (hs >= 0)
                lk = mn_lock_get(pat(), t, p, hs);
            if (lk >= 0)
                v = lk;
            mx = in->max ? in->max : 1;
            if (p >= P_LFO1 && p < P_ARPM && ((p - P_LFO1) & 7) == L_DEST)
                mn_dest_text(kit()->machine, t, kit()->par[t][p - L_DEST + L_TRK], v, val);
            else
                mn_par_text(m, p, v, val);
            param_cell(i * 60, y, in->name, val, 44 * v / mx, in->kind == PK_BIP, active,
                       active && plat_ms() < ui.touch_until && ui.touched == i, lk >= 0);
        }
    }
}

static void draw_home(void)
{
    mn_pat_t *p = pat();
    char a[24], v[4][12], *e;
    int i, t, s;
    e = s_cpy(a, "PATTERN ");
    slot_name(sb.slot[buf_of(song())], e);
    cv_text(6, 28, &FONT_B, a, C_HI);
    e = itoa_u((uint32_t)(MN_NLOCK - mn_lock_rows_free(p)), a);
    s_cpy(e, "/32 LOCKS");
    cv_text(234 - text_w(&FONT_XS, a), 30, &FONT_XS, a, C_DIM);
    e = itoa_u(proj.tempo10 / 10u, v[0]);
    *e++ = '.';
    *e++ = (char)('0' + proj.tempo10 % 10u);
    *e = 0;
    e = itoa_u(p->swing, v[1]);
    s_cpy(e, "%");
    itoa_u(p->len, v[2]);
    s_cpy(v[3], SPEED_NAME[p->speed % MN_NSPEED]);
    cv_round(2, 47, 236, 46, 5, C_LINE);
    {
        static const char *const N[4] = {"TEMPO", "SWING", "LENGTH", "SPEED"};
        static const int MX[4] = {2700, 30, 63, MN_NSPEED - 1};
        int fr[4];
        fr[0] = 44 * (proj.tempo10 - 300) / MX[0];
        fr[1] = 44 * (p->swing - 50) / MX[1];
        fr[2] = 44 * (p->len - 1) / MX[2];
        fr[3] = 44 * p->speed / MX[3];
        for (i = 0; i < 4; i++)
            param_cell(i * 60, 50, N[i], v[i], fr[i], 0, 1, plat_ms() < ui.touch_until && ui.touched == i, 0);
    }
    /* every track's steps, this page */
    for (t = 0; t < MN_NT; t++)
        for (s = 0; s < 16; s++) {
            int st = ui.spage * 16 + s;
            int32_t x = 8 + s * 14 + (s / 4) * 2, y = 98 + t * 7;
            uint16_t c = C_LINE;
            if (st >= p->len)
                continue;
            if (p->trig[t][st] & MN_TRIG)
                c = (mn_mute >> t & 1u) ? C_DIM : (p->trig[t][st] & MN_ACC) ? C_HI : C_AMB;
            if (st == mn_step)
                c = (p->trig[t][st] & MN_TRIG) ? C_WHITE : C_GRAY;
            cv_rect(x, y, 12, 5, c);
            if (t == ui.track && s == 0)
                cv_rect(2, y, 3, 5, C_HI);
        }
}

static void draw_glo(void)
{
    char v[8][12], *e;
    int i, r;
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
    cv_text(6, 28, &FONT_B, "GLOBAL", C_HI);
    cv_text(234 - text_w(&FONT_XS, "MNM-1 " OM_VERSION), 30, &FONT_XS, "MNM-1 " OM_VERSION, C_DIM);
    for (r = 0; r < 2; r++) {
        int active = r == ui.row[V_GLO];
        if (active)
            cv_round(2, 47 + r * 47, 236, 46, 5, C_LINE);
        for (i = 0; i < 4; i++)
            param_cell(i * 60, 50 + r * 47, N[r * 4 + i], v[r * 4 + i], 0, 0, active,
                       active && plat_ms() < ui.touch_until && ui.touched == i, 0);
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
        ui.flash[t] = tc != ui.trig_seen[t] ? 4 : ui.flash[t] ? ui.flash[t] - 1 : 0;
        ui.trig_seen[t] = tc;
        h = hash(h, (uint32_t)(mn_level[t] * 60.0f) | (uint32_t)kit()->machine[t] << 8 | (uint32_t)(ui.flash[t] != 0) << 12);
    }
    for (i = 0; i < MN_NP; i += 4)
        h = hash(h, (uint32_t)par[i] | (uint32_t)par[i + 1] << 8 | (uint32_t)par[i + 2] << 16 | (uint32_t)par[i + 3] << 24);
    h = hash(h, (uint32_t)(plat_ms() < ui.touch_until) | (uint32_t)(ui.touched + 1) << 1 | (uint32_t)ui.spage << 4);
    if (ui.view == V_HOME)
        h = hash(hash(h, (uint32_t)(mn_step + 1) | (uint32_t)pat()->len << 8 | (uint32_t)pat()->swing << 16 |
                             (uint32_t)pat()->speed << 24), proj.tempo10 | (uint32_t)mn_lock_rows_free(pat()) << 16);
    if (ui.view == V_HOME)
        for (t = 0; t < MN_NT; t++)
            for (i = 0; i < 16; i += 4) {               /* (bytes: the pattern is not word aligned) */
                const uint8_t *g = &pat()->trig[t][ui.spage * 16 + i];
                h = hash(h, (uint32_t)g[0] | (uint32_t)g[1] << 8 | (uint32_t)g[2] << 16 | (uint32_t)g[3] << 24);
            }
    if (ui.view == V_GLO)
        h = hash(h, (uint32_t)proj.midi_base | (uint32_t)proj.auto_ch << 4 | (uint32_t)proj.clock_in << 9 |
                        (uint32_t)proj.clock_out << 10 | (uint32_t)proj.midi_out << 11 | (uint32_t)proj.follow << 12 |
                        (uint32_t)proj.theme << 13 | plat_cpu_pct() << 16);
    if (ui.view >= PG_LFO1 && ui.view <= PG_LFO3)
        h = hash(h, (uint32_t)(int32_t)(mn_lfo_val[ui.track][ui.lfo] * 19.0f));
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
    if (view_is_track())
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
    uint32_t h = hash(2166136261u, (uint32_t)(mn_step + 1) | (uint32_t)ui.spage << 8 | (uint32_t)len << 12 |
                                       (uint32_t)t << 20 | (uint32_t)ui.keys_mode << 24 | (uint32_t)ui.rec << 25 |
                                       (uint32_t)(ui.octave + 4) << 26);
    char a[24], *e;
    h = hash(h, ui.held | (uint32_t)ui.cursor << 20);
    for (w = 0; w < 16; w++) {
        int s = ui.spage * 16 + w;
        h = hash(h, (uint32_t)p->trig[t][s] | (uint32_t)mn_step_locks(p, t, s) << 4);
    }
    if (h == ui.sig[2])
        return;
    ui.sig[2] = h;
    cv_begin(240, FTR_H, C_BLACK);
    cv_rect(0, 0, 240, 1, C_LINE);
    for (w = 0; w < 16; w++) {
        int s = ui.spage * 16 + w;
        int32_t x = 6 + w * 14 + (w / 4) * 2;
        uint8_t f = p->trig[t][s];
        uint16_t c = C_LINE;
        if (s >= len) {
            cv_rect(x, 26, 12, 2, C_LINE);
            continue;
        }
        if (f & MN_TRIG)
            c = (f & MN_ACC) ? C_HI : C_AMB;
        if (ui.held >> w & 1u && !ui.keys_mode)
            c = C_WHITE;
        cv_round(x, 6, 12, 22, 3, c);
        if (s == mn_step) {                          /* the step playing: a bar under it */
            cv_rect(x, 31, 12, 3, C_WHITE);
        }
        if (f & MN_SLIDE)
            cv_rect(x + 6, 23, 14, 3, C_WHITE);
        if (mn_step_locks(p, t, s))
            cv_round(x + 4, 9, 4, 4, 2, (f & MN_TRIG) ? C_BLACK : C_HI);
        if (ui.rec && !mn_playing && ui.keys_mode && s == ui.cursor)
            cv_rect(x, 3, 12, 2, C_HI);
    }
    /* the line below: the mode, the pages of steps, the octave */
    {
        int32_t x = 6;
        const char *mode = ui.keys_mode ? "KEYS" : "GRID";
        int32_t mw = text_w(&FONT_XS, mode) + 10;
        cv_round(x, 42, mw, 18, 5, ui.keys_mode ? C_HI : C_LINE);
        cv_text(x + 5, 44, &FONT_XS, mode, ui.keys_mode ? C_BLACK : C_AMB);
        x += mw + 8;
        for (w = 0; w < pages; w++) {
            uint16_t c = w == ui.spage ? C_HI : w == cur_page ? C_GRAY : C_LINE;
            cv_rect(x + w * 14, 49, 11, 5, c);
        }
        x += pages * 14 + 6;
        e = itoa_u((uint32_t)ui.spage * 16u + 1u, a);
        *e++ = '-';
        e = itoa_u((uint32_t)(ui.spage * 16 + 16 > len ? len : ui.spage * 16 + 16), e);
        *e++ = '/';
        itoa_u((uint32_t)len, e);
        cv_text(x, 44, &FONT_XS, a, C_DIM);
        if (ui.keys_mode) {
            e = s_cpy(a, "OCT ");
            if (ui.octave > 0)
                *e++ = '+';
            if (ui.octave < 0)
                *e++ = '-';
            itoa_u((uint32_t)(ui.octave < 0 ? -ui.octave : ui.octave), e);
            cv_text(234 - text_w(&FONT_XS, a), 44, &FONT_XS, a, C_AMB);
        } else {
            int b;
            int32_t bx = 234;
            for (b = NBLACK - 1; b >= MN_NT; b--) {   /* the black keys' tools, as a reminder */
                int on = ui.keys >> BLACK_K[b] & 1u;
                bx -= text_w(&FONT_XS, BK_NAME[b]) + 4;
                cv_text(bx, 44, &FONT_XS, BK_NAME[b], on ? C_WHITE : C_DIM);
            }
        }
    }
    cv_blit(0, FTR_Y);
}

static void leds(void)
{
    static const uint8_t VIEW_BTN[NVIEWS] = {B_EDIT, B_ENV, B_SEL, B_FX, B_LFO, B_LFO, B_LFO, B_ARP, B_HOME, B_GLO};
    uint32_t b = 1u << VIEW_BTN[ui.view], k = 0, d = 0;
    int w, t;
    if (mn_playing && (mn_step & 3) != 0)            /* PLAY: lit, dark on each beat */
        b |= 1u << B_PLAY;
    if (ui.rec)
        b |= 1u << B_REC;
    if (!ui.keys_mode)
        b |= 1u << B_SEQ;
    if (ui.keys_mode ? ui.octave != 0 : ui.spage != 0)
        b |= ui.keys_mode && ui.octave < 0 ? 1u << B_OCTDN : 1u << B_OCTUP;
    if (ui.ask && blink())
        b |= 1u << B_SAVE;
    if (ui.keys_mode) {
        k = ui.keys;
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
