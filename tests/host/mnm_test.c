/* SPDX-License-Identifier: GPL-3.0-only */
/* MNM-1 engine tests, on the host, linked without libm as on the device: every machine sounds (and stays
 * finite and below clipping), the sequencer plays the steps at the tempo, swing moves the odd steps,
 * locks and trigless locks change the sound, slides do not restart the envelope, mutes silence a track,
 * probability thins the trigs, the queue swaps patterns at the end, MIDI clock leads, the arpeggiator
 * plays its chord, every LFO wave moves its target, the delay echoes at its time, the song checks.
 * WAVs of every machine and of the demo pattern land in OUTDIR.
 *   mnm_test [OUTDIR] */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mnm.h"

static int fails;
static void ok(int c, const char *what)
{
    printf("%s %s\n", c ? "  ok  " : "  FAIL", what);
    if (!c)
        fails++;
}

static float absf(float x) { return x < 0.0f ? -x : x; }

static int32_t buf[2 * 256];
static FILE *wav;
static uint32_t wav_frames;
static void wav_open(const char *dir, const char *name)
{
    char path[512];
    static const uint8_t h[44] = {0};
    if (!dir) {
        wav = 0;
        return;
    }
    snprintf(path, sizeof path, "%s/%s", dir, name);
    wav = fopen(path, "wb");
    if (wav)
        fwrite(h, 1, 44, wav);
    wav_frames = 0;
}
static void wav_close(void)
{
    uint32_t bytes = wav_frames * 4, v;
    uint16_t fmt[2] = {1, 2}, al[2] = {4, 16};
    uint32_t r[2] = {44100, 44100 * 4};
    if (!wav)
        return;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav);
    v = 36 + bytes;
    fwrite(&v, 4, 1, wav);
    fwrite("WAVEfmt ", 1, 8, wav);
    v = 16;
    fwrite(&v, 4, 1, wav);
    fwrite(fmt, 2, 2, wav);
    fwrite(r, 4, 2, wav);
    fwrite(al, 2, 2, wav);
    fwrite("data", 1, 4, wav);
    fwrite(&bytes, 4, 1, wav);
    fclose(wav);
    wav = 0;
}

/* render ms of audio; returns the peak (0..1); bad = a NaN-like jump or a stuck rail */
static float run(uint32_t ms, double *energy)
{
    uint32_t blocks = (uint32_t)((double)ms * 44.1 / 256.0 + 0.5), b, k;
    float pk = 0.0f;
    double en = 0.0;
    for (b = 0; b < blocks; b++) {
        mn_render(buf, 256, 4096);
        for (k = 0; k < 512; k++) {
            float x = (float)buf[k] / 8388608.0f;
            if (absf(x) > pk)
                pk = absf(x);
            en += (double)(x * x);
        }
        if (wav) {
            int16_t s[512];
            for (k = 0; k < 512; k++)
                s[k] = (int16_t)(buf[k] / 256);
            fwrite(s, 2, 512, wav);
            wav_frames += 256;
        }
    }
    if (energy)
        *energy = en / (double)(blocks ? blocks * 512u : 1u);
    return pk;
}

static mn_song_t song, song2;

/* how many times track t has played a note since the last call */
static uint8_t last_count[MN_NT];
static int notes(int t)
{
    int n = (uint8_t)(mn_trig_count[t] - last_count[t]);
    last_count[t] = mn_trig_count[t];
    return n;
}

static void fresh(void)
{
    int t;
    mn_init();
    mn_song_init(&song);
    mn_pat_clear(&song.pat);
    mn_set_song(&song);
    mn_tempo10 = 1200;
    mn_mute = 0;
    for (t = 0; t < MN_NT; t++) {
        song.kit.par[t][P_VOL] = 100;
        last_count[t] = mn_trig_count[t];
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : 0;
    int m, t, s;
    float pk;
    double en;

    /* ---- every machine, alone on track 1: a few notes up the keyboard */
    for (m = 0; m < MC_N; m++) {
        char name[64], what[96];
        float pmax = 0.0f;
        int bad = 0;
        fresh();
        mn_track_init(&song.kit, 0, m);
        if (m == MC_DRUM)
            song.kit.par[0][1] = 0;
        snprintf(name, sizeof name, "machine_%s.wav", mn_machine_name(m));
        wav_open(dir, name);
        for (s = 0; s < 6; s++) {
            mn_note_on(0, 36 + s * 7, 110);
            pk = run(220, &en);
            if (pk > pmax)
                pmax = pk;
            if (!(pk == pk) || en < 1e-7) {
                printf("       (note %d: peak %f energy %g)\n", s, (double)pk, en);
                bad = 1;
            }
            mn_note_off(0, 36 + s * 7);
            run(80, 0);
        }
        pk = run(1500, &en);
        wav_close();
        snprintf(what, sizeof what, "%-5s sounds, peak %.2f, no NaN, below clipping", mn_machine_name(m), (double)pmax);
        ok(!bad && pmax > 0.02f && pmax < 0.99f, what);
        snprintf(what, sizeof what, "%-5s dies away after the notes (%.4f)", mn_machine_name(m), (double)pk);
        ok(pk < 0.02f, what);
    }

    /* ---- the drum box: every type */
    for (s = 0; s < 8; s++) {
        char what[64];
        fresh();
        mn_track_init(&song.kit, 0, MC_DRUM);
        song.kit.par[0][1] = (uint8_t)s;
        mn_note_on(0, 60, 120);
        pk = run(300, &en);
        mn_note_off(0, 60);
        snprintf(what, sizeof what, "drum type %d sounds (peak %.2f)", s, (double)pk);
        ok(pk > 0.02f && pk < 0.99f, what);
    }

    /* ---- the sequencer: 16 steps at 120 BPM take 2 s, every trig plays */
    fresh();
    for (s = 0; s < 16; s++)
        song.pat.trig[0][s] = MN_TRIG;
    mn_play(1);
    run(1990, 0);
    ok(notes(0) == 16, "16 trigs in 2 s at 120 BPM");
    ok(mn_loops == 0, "the pattern has not come round yet");
    run(40, 0);
    ok(mn_loops == 1 && notes(0) == 1, "...and comes round at 2 s");
    mn_play(0);
    run(20, 0);
    ok(mn_step == -1 && !mn_playing, "stopped");

    /* speed: 2x plays 32 steps in 2 s */
    song.pat.speed = 4;
    notes(0);
    mn_play(1);
    run(1990, 0);
    ok(notes(0) == 32, "speed 2x: 32 steps in 2 s");
    mn_play(0);
    song.pat.speed = 2;

    /* length: 12 steps come round after 1.5 s */
    song.pat.len = 12;
    run(500, 0);
    {
        uint32_t l0 = mn_loops;
        mn_play(1);
        run(1480, 0);
        ok(mn_loops - l0 == 0, "12 steps: not round before 1.5 s");
        run(40, 0);
        ok(mn_loops - l0 == 1, "...round just after");
    }
    mn_play(0);
    song.pat.len = 16;

    /* swing: step 2 (odd, the second) comes later than at 50 % */
    fresh();
    song.pat.trig[0][1] = MN_TRIG;
    song.pat.swing = 75;
    mn_play(1);
    run(150, 0);
    ok(notes(0) == 0, "swing 75 %: the odd step is not at 125 ms");
    run(50, 0);
    ok(notes(0) == 1, "...but by 200 ms (187.5)");
    mn_play(0);

    /* mute */
    fresh();
    for (s = 0; s < 16; s++)
        song.pat.trig[0][s] = MN_TRIG;
    mn_mute = 1;
    mn_play(1);
    pk = run(500, 0);
    ok(notes(0) == 0 && pk < 0.001f, "a muted track plays nothing");
    mn_mute = 0;
    run(500, 0);
    ok(notes(0) == 4, "unmuted: it plays");
    mn_play(0);

    /* probability */
    fresh();
    for (s = 0; s < 16; s++)
        song.pat.trig[0][s] = MN_TRIG;
    song.kit.par[0][P_PROB] = 64;
    mn_play(1);
    run(8000, 0);
    m = notes(0);
    ok(m > 20 && m < 44, "probability 50 %: about half the trigs");
    mn_play(0);

    /* locks: the step with a lock plays a different sound; a trigless lock changes the next notes */
    fresh();
    mn_track_init(&song.kit, 0, MC_SINE);
    song.kit.par[0][P_HOLD] = 127;
    song.pat.trig[0][0] = song.pat.trig[0][4] = MN_TRIG;
    song.kit.par[0][P_VOL] = 100;
    ok(mn_lock_set(&song.pat, 0, P_VOL, 4, 20) == 0, "a lock is set");
    ok(mn_lock_get(&song.pat, 0, P_VOL, 4) == 20 && mn_lock_get(&song.pat, 0, P_VOL, 3) == -1, "...and read back");
    ok(mn_step_locks(&song.pat, 0, 4) == 1, "...one on the step");
    mn_play(1);
    {
        float a, b;
        a = run(400, 0);
        run(150, 0);
        b = run(300, 0);
        ok(b < a * 0.3f, "the locked step (VOL 20) is quieter than the plain one");
    }
    mn_play(0);
    run(500, 0);
    mn_lock_clear_step(&song.pat, 0, 4);
    ok(mn_lock_get(&song.pat, 0, P_VOL, 4) == -1 && mn_lock_rows_free(&song.pat) == MN_NLOCK, "cleared: the row is free");
    {
        int full = 0, r;
        for (r = 0; r < MN_NLOCK; r++)
            full |= mn_lock_set(&song.pat, r % MN_NT, 8 + r, 0, 1);
        ok(!full && mn_lock_set(&song.pat, 0, 63, 0, 1) == -1, "32 rows, then full");
        for (t = 0; t < MN_NT; t++)
            mn_lock_clear_track(&song.pat, t);
        ok(mn_lock_rows_free(&song.pat) == MN_NLOCK, "every track cleared");
    }
    /* trigless */
    fresh();
    mn_track_init(&song.kit, 0, MC_SINE);
    song.kit.par[0][P_HOLD] = 127;
    song.kit.par[0][P_GATE] = 127;                 /* the note holds for 8 steps */
    song.pat.trig[0][0] = song.pat.trig[0][8] = MN_TRIG;
    mn_lock_set(&song.pat, 0, P_VOL, 4, 10);       /* no trig on step 4 */
    mn_play(1);
    {
        float a, b, c;
        a = run(400, 0);
        run(150, 0);
        b = run(300, 0);
        run(200, 0);
        c = run(300, 0);
        ok(notes(0) == 2, "a trigless lock plays no note");
        ok(b < a * 0.5f, "...but the note playing gets quieter from its step on");
        ok(c > b * 2.0f, "...until the next trig");
    }
    mn_play(0);

    /* slide: no new attack (the envelope keeps its level) */
    fresh();
    mn_track_init(&song.kit, 0, MC_SAW);
    song.kit.par[0][P_HOLD] = 127;
    song.kit.par[0][P_ATK] = 100;                 /* a slow attack shows a restart */
    song.kit.par[0][P_GATE] = 40;
    song.pat.trig[0][0] = MN_TRIG;
    song.pat.trig[0][2] = MN_TRIG | MN_SLIDE;
    song.pat.note[0][2] = 67;
    mn_play(1);
    {
        float a, b;
        run(240, 0);
        a = run(20, 0);
        b = run(40, 0);
        ok(b > a * 0.8f, "a slide does not restart the envelope");
    }
    mn_play(0);

    /* the queue: the next song takes over at the end, not before */
    fresh();
    mn_song_init(&song2);
    for (s = 0; s < 16; s++)
        song2.pat.trig[1][s] = MN_TRIG;
    song.pat.trig[0][0] = MN_TRIG;
    mn_play(1);
    run(500, 0);
    mn_queue(&song2);
    run(1000, 0);
    ok(mn_song() == &song && mn_queued() == &song2 && notes(1) == 0, "queued: the first song still plays");
    run(600, 0);
    ok(mn_song() == &song2 && !mn_queued() && notes(1) > 0, "...and the second takes over at the end");
    mn_play(0);
    run(10, 0);
    mn_queue(&song);
    run(10, 0);
    ok(mn_song() == &song, "stopped: a queued song is at once");

    /* MIDI clock: 6 ticks a step */
    fresh();
    for (s = 0; s < 16; s++)
        song.pat.trig[0][s] = MN_TRIG;
    mn_clock(MN_CLK_START);
    for (s = 0; s < 24 * 2; s++) {                 /* two beats, at 100 BPM */
        mn_clock(MN_CLK_TICK);
        run(25, 0);
    }
    ok(mn_ext && notes(0) == 8, "MIDI clock: 48 ticks, 8 steps");
    mn_clock(MN_CLK_STOP);
    run(10, 0);
    ok(!mn_playing, "MIDI stop");
    run(700, 0);
    ok(!mn_ext, "no ticks for a while: back to the internal clock");

    /* the arpeggiator: one trig, a minor chord up two octaves at 1/32 */
    fresh();
    mn_track_init(&song.kit, 0, MC_PULSE);
    song.kit.par[0][P_ARPM] = ARP_UP;
    song.kit.par[0][P_ASPD] = 1;
    song.kit.par[0][P_ARNG] = 1;
    song.kit.par[0][P_ACHD] = 3;
    song.pat.trig[0][0] = MN_TRIG;
    mn_play(1);
    run(990, 0);
    ok(notes(0) == 16, "arpeggio: 16 notes in 8 steps at 1/32");
    mn_play(0);
    /* live: two keys held arpeggiate */
    fresh();
    song.kit.par[0][P_ARPM] = ARP_UPDN;
    song.kit.par[0][P_ASPD] = 3;
    mn_note_on(0, 60, 100);
    mn_note_on(0, 64, 100);
    run(1000, 0);
    m = notes(0);
    ok(m >= 7 && m <= 10, "live arpeggio: the keys held, a note a step");
    mn_note_off(0, 60);
    mn_note_off(0, 64);
    run(500, 0);
    ok(notes(0) == 0, "...and stops when they are let go");

    /* LFOs: every wave moves a filter, the output changes over a cycle */
    for (m = 0; m < LW_N; m++) {
        char what[64];
        float lo = 1.0f, hi = 0.0f;
        int k;
        fresh();
        mn_track_init(&song.kit, 0, MC_SAW);
        song.kit.par[0][P_HOLD] = 127;
        song.kit.par[0][P_LFO1 + L_DEST] = P_VOL + 1;
        song.kit.par[0][P_LFO1 + L_WAVE] = (uint8_t)m;
        song.kit.par[0][P_LFO1 + L_SPD] = 64;
        song.kit.par[0][P_LFO1 + L_MULT] = 3;      /* 64 x 8 / 2048: a cycle in 4 steps (0.5 s) */
        song.kit.par[0][P_LFO1 + L_DPTH] = 0;
        mn_note_on(0, 48, 100);
        for (k = 0; k < 20; k++) {
            float p = run(50, 0);
            if (p < lo)
                lo = p;
            if (p > hi)
                hi = p;
        }
        mn_note_off(0, 48);
        snprintf(what, sizeof what, "LFO %d moves the volume (%.2f .. %.2f)", m, (double)lo, (double)hi);
        ok(hi > lo * 1.5f + 0.01f, what);
    }
    /* an LFO aimed at another track */
    fresh();
    mn_track_init(&song.kit, 1, MC_SINE);
    song.kit.par[1][P_HOLD] = 127;
    song.kit.par[0][P_LFO1 + L_TRK] = 2;
    song.kit.par[0][P_LFO1 + L_DEST] = P_VOL + 1;
    song.kit.par[0][P_LFO1 + L_WAVE] = LW_SQR;
    song.kit.par[0][P_LFO1 + L_SPD] = 64;
    song.kit.par[0][P_LFO1 + L_MULT] = 3;
    song.kit.par[0][P_LFO1 + L_DPTH] = 0;
    mn_note_on(1, 60, 100);
    {
        float a = run(200, 0), b = run(250, 0);
        ok(absf(a - b) > 0.1f, "an LFO on track 1 moves track 2");
    }

    /* the delay: one click, its echo at the time (3 steps at 120 BPM = 375 ms) */
    fresh();
    mn_track_init(&song.kit, 0, MC_DRUM);
    song.kit.par[0][1] = 6;                      /* rim: short */
    song.kit.par[0][P_DSND] = 127;
    song.kit.par[0][P_DTIM] = 23;
    song.kit.par[0][P_DFB] = 0;
    mn_note_on(0, 60, 120);
    mn_note_off(0, 60);
    {
        float a, b, c;
        a = run(60, 0);
        b = run(300, 0);
        c = run(40, 0);
        printf("       (delay: %f %f %f)\n", (double)a, (double)b, (double)c);
        ok(a > 0.05f && b < a * 0.2f && c > a * 0.2f, "the delay echoes at 375 ms");
    }

    /* the song */
    mn_song_demo(&song);
    ok(mn_song_valid(&song), "the demo song is valid");
    song.kit.par[0][P_VOL] = 200;
    song.pat.len = 0;
    song.kit.machine[2] = 99;
    ok(mn_song_valid(&song) && song.kit.par[0][P_VOL] <= 127 && song.pat.len == 16 && song.kit.machine[2] < MC_N,
       "values out of range are fixed");
    song.magic = 0;
    ok(!mn_song_valid(&song), "another magic is not a song");
    ok(sizeof(mn_song_t) <= 3840u, "a song fits a flash object");

    /* the demo, 4 bars, to listen to */
    fresh();
    mn_song_demo(&song);
    mn_set_song(&song);
    wav_open(dir, "demo.wav");
    mn_play(1);
    pk = run(8000, &en);
    wav_close();
    {
        char what[64];
        snprintf(what, sizeof what, "the demo plays: peak %.2f", (double)pk);
        ok(pk > 0.2f && pk < 0.99f, what);
    }
    mn_play(0);
    ok(mn_dropped == 0, "no commands dropped");
    printf(fails ? "FAILED: %d\n" : "all engine tests passed\n", fails);
    return fails ? 1 : 0;
}
