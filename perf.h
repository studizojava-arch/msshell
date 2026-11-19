/*
 * perf.h — device capability probing and the tunables it selects.
 *
 * The GL windows are the only part of mshell that can really strain a phone,
 * and a 2017 budget handset does not want the same frame budget as a 2024
 * flagship. perf_detect() looks at CPU cores, RAM and the Android API level
 * and picks one of three presets; !setconf writes the answer to
 * ~/.mshell.conf so every later window inherits it.
 *
 * No GL, no SDL, no allocation. Safe to call before any window exists.
 */
#ifndef MSHELL_PERF_H
#define MSHELL_PERF_H

typedef enum {
    PERF_LOW = 0,     /* <=4 cores or < 3 GB: 30 fps, short scrollback */
    PERF_MID,         /* the common case: 60 fps, moderate scrollback   */
    PERF_HIGH,        /* 8 cores and >= 6 GB: everything on             */
    PERF_COUNT
} perf_level_t;

typedef struct {
    perf_level_t level;
    int auto_detected;    /* 1 = we guessed, 0 = user/env said so */

    int frame_ms;         /* minimum time between rendered frames    */
    int swap_interval;    /* argument for SDL_GL_SetSwapInterval     */
    int scrollback;       /* rows kept above the screen              */
    int read_chunk;       /* pty read size, in bytes                 */
    int max_fps;          /* informational, for the printed summary  */

    /* What we measured, so the summary can explain itself. */
    int cores;
    long ram_mb;
    int sdk;              /* Android API level, 0 when unknown */
} perf_t;

/* Preset name: "low" / "mid" / "high". Never NULL. */
const char *perf_level_name(perf_level_t lvl);

/* Parse "low"/"mid"/"high" (also "auto"). -1 when unrecognised. */
int perf_parse_level(const char *s);

/* Measure the machine and fill in a preset. Always succeeds. */
void perf_detect(perf_t *p);

/* Read ~/.mshell.conf if present and apply it on top of a detected profile.
 * MSHELL_PERF wins over the file. Returns 1 if an explicit choice was found. */
int perf_load(perf_t *p);

/* Force a level, marking the profile as explicitly chosen (!setconf <lvl>). */
void perf_force(perf_t *p, perf_level_t lvl);

/* Write the profile's level to ~/.mshell.conf. 0 on success, -1 on error. */
int perf_save(const perf_t *p);

/* Path of the config file, for the printed summary. */
const char *perf_conf_path(void);

/* One-line human summary of the probe, e.g. "4 cores, 3.8 GB, API 30". */
void perf_describe(const perf_t *p, char *out, int outsz);

#endif /* MSHELL_PERF_H */
