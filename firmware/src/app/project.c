/* SPDX-License-Identifier: GPL-3.0-only */
/* MNM-1 storage: the global settings (OBJ_PROJ) and the 16 pattern slots (a kit and a pattern each,
 * OBJ_PAT0 + slot). A slot of another format, or none, gives a new song: the demo in slot 0, the default
 * kit and an empty pattern elsewhere. Part of the unity build, after app.h. (FiMba-1's project.c.) */
project_t proj;

void project_defaults(void)
{
    memset(&proj, 0, sizeof proj);
    proj.magic = PROJ_MAGIC;
    proj.format = PROJ_FORMAT;
    proj.tempo10 = 1200;
    proj.slot = 0;
    proj.midi_base = 1;                  /* tracks on channels 2..7 */
    proj.auto_ch = 1;                    /* channel 1 plays the track selected */
    proj.clock_in = 1;
    proj.clock_out = 0;
    proj.midi_out = 0;
    proj.theme = 0;
    proj.follow = 1;
}

static int project_valid(int bytes)
{
    if (bytes < 16 || proj.magic != PROJ_MAGIC || proj.format < 1u || proj.format > PROJ_FORMAT)
        return 0;
    proj.format = PROJ_FORMAT;
    if (proj.tempo10 < 300 || proj.tempo10 > 3000)
        proj.tempo10 = 1200;
    if (proj.slot >= NSLOT)
        proj.slot = 0;
    if (proj.midi_base > 10)
        proj.midi_base = 1;
    if (proj.auto_ch > 16)
        proj.auto_ch = 1;
    proj.clock_in = proj.clock_in != 0;
    proj.clock_out = proj.clock_out != 0;
    proj.midi_out = proj.midi_out != 0;
    proj.follow = proj.follow != 0;
    if (proj.theme >= NPALETTES)
        proj.theme = 0;
    return 1;
}

int project_load(void)
{
    int n;
    memset(&proj, 0, sizeof proj);
    n = plat_store_load(OBJ_PROJ, &proj, sizeof proj);
    if (project_valid(n))
        return 0;
    project_defaults();
    return -1;
}

int project_save(void) { return plat_store_save(OBJ_PROJ, &proj, sizeof proj); }

void project_apply(void)
{
    mn_tempo10 = proj.tempo10;
    mn_midi_base = proj.midi_base;
    mn_clock_in = proj.clock_in;
    mn_clock_out = proj.clock_out;
    mn_midi_out_on = proj.midi_out;
    palette_set(proj.theme);
}

int song_load(uint32_t slot, mn_song_t *s)
{
    int n = plat_store_load(OBJ_PAT0 + slot, s, sizeof *s);
    if (mn_song_load_size(s, n))                    /* (a song of 0.1, without the scale, too) */
        return 0;
    if (slot == 0)
        mn_song_demo(s);
    else
        mn_song_init(s);
    return -1;
}

int song_save(uint32_t slot, const mn_song_t *s) { return plat_store_save(OBJ_PAT0 + slot, s, sizeof *s); }
