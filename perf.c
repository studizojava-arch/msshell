/*
 * perf.c — capability probe + preset table. See perf.h.
 *
 * Everything is read from /proc and /sys, which Termux can see without root.
 * Any probe that fails simply scores 0 and the decision falls through to the
 * values we did get, so a device with no /proc/meminfo still ends up with a
 * sane profile instead of a crash.
 */
#include "perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>

/* ------------------------------------------------------------- presets */

typedef struct {
    int frame_ms;
    int swap_interval;
    int scrollback;
    int read_chunk;
    int max_fps;
} perf_preset_t;

static const perf_preset_t kPreset[PERF_COUNT] = {
    /* LOW  */ { 33, 1,  200, 4096, 30 },
    /* MID  */ { 16, 1,  500, 8192, 60 },
    /* HIGH */ { 16, 1, 1000, 8192, 60 },
};

const char *perf_level_name(perf_level_t lvl)
{
    switch (lvl) {
    case PERF_LOW:  return "low";
    case PERF_MID:  return "mid";
    case PERF_HIGH: return "high";
    default:        return "mid";
    }
}

int perf_parse_level(const char *s)
{
    if (!s) return -1;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!strcasecmp(s, "low")  || !strcasecmp(s, "lowend") ||
        !strcasecmp(s, "budget"))   return PERF_LOW;
    if (!strcasecmp(s, "mid")  || !strcasecmp(s, "medium") ||
        !strcasecmp(s, "balanced")) return PERF_MID;
    if (!strcasecmp(s, "high") || !strcasecmp(s, "flagship"))
        return PERF_HIGH;
    return -1;
}

/* --------------------------------------------------------------- probes */

static int probe_cores(void)
{
    int n = (int)sysconf(_SC_NPROCESSORS_ONLN);

    if (n < 1) {
        /* Fall back to counting the per-CPU sysfs directories. */
        char path[64];
        int i;
        n = 0;
        for (i = 0; i < 64; i++) {
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", i);
            if (access(path, F_OK) == 0) n++;
        }
    }
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    return n;
}

static long probe_ram_mb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    long kb = 0;

    if (!f) {
        long pages = sysconf(_SC_PHYS_PAGES);
        long psz = sysconf(_SC_PAGESIZE);
        if (pages > 0 && psz > 0) return (pages / 1024) * (psz / 1024);
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemTotal:", 7)) {
            kb = strtol(line + 7, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb / 1024;
}

static int probe_sdk(void)
{
    char buf[64];
    FILE *f;
    int i, v = 0;

    f = fopen("/system/build.prop", "r");
    if (!f) return 0;
    while (fgets(buf, sizeof(buf), f)) {
        if (!strncmp(buf, "ro.build.version.sdk=", 21)) {
            v = (int)strtol(buf + 21, NULL, 10);
            break;
        }
    }
    fclose(f);
    if (v > 0) return v;

    /* Some devices hide it behind a getprop-only path. */
    f = popen("getprop ro.build.version.sdk 2>/dev/null", "r");
    if (f) {
        if (fgets(buf, sizeof(buf), f)) {
            for (i = 0; buf[i]; i++)
                if (isdigit((unsigned char)buf[i])) v = v * 10 + (buf[i] - '0');
        }
        pclose(f);
    }
    return v;
}

static perf_level_t classify(int cores, long ram_mb, int sdk)
{
    /* Anything with real memory is not low-end, whatever the core count says:
     * big.LITTLE kernels sometimes report only the online cluster. */
    if (ram_mb >= 6000 && cores >= 8) return PERF_HIGH;
    if (ram_mb && ram_mb < 3000)       return PERF_LOW;
    if (cores <= 2)                    return PERF_LOW;
    if (ram_mb && ram_mb < 4500)       return PERF_MID;
    if (sdk && sdk <= 28)              return PERF_MID;
    return PERF_MID;
}

void perf_detect(perf_t *p)
{
    const perf_preset_t *pre;

    memset(p, 0, sizeof(*p));
    p->cores = probe_cores();
    p->ram_mb = probe_ram_mb();
    p->sdk = probe_sdk();
    p->level = classify(p->cores, p->ram_mb, p->sdk);
    p->auto_detected = 1;

    pre = &kPreset[p->level];
    p->frame_ms = pre->frame_ms;
    p->swap_interval = pre->swap_interval;
    p->scrollback = pre->scrollback;
    p->read_chunk = pre->read_chunk;
    p->max_fps = pre->max_fps;
}

/* ------------------------------------------------------------ config file */

const char *perf_conf_path(void)
{
    static char path[512];
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/data/data/com.termux/files/home";
    snprintf(path, sizeof(path), "%s/.mshell.conf", home);
    return path;
}

static void apply_level(perf_t *p, perf_level_t lvl, int explicit_choice)
{
    const perf_preset_t *pre;

    if (lvl >= PERF_COUNT) lvl = PERF_MID;
    p->level = lvl;
    p->auto_detected = !explicit_choice;
    pre = &kPreset[lvl];
    p->frame_ms = pre->frame_ms;
    p->swap_interval = pre->swap_interval;
    p->scrollback = pre->scrollback;
    p->read_chunk = pre->read_chunk;
    p->max_fps = pre->max_fps;
}

int perf_load(perf_t *p)
{
    FILE *f;
    char line[256];
    int chosen = -1;
    const char *env = getenv("MSHELL_PERF");

    if (env && *env) {
        int c = perf_parse_level(env);
        if (c >= 0) { apply_level(p, (perf_level_t)c, 1); return 1; }
    }

    f = fopen(perf_conf_path(), "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *eq, *val, *key = line;
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        while (*key && isspace((unsigned char)*key)) key++;
        if (*key == '#' || *key == ';' || !*key) continue;
        eq = strchr(key, '=');
        if (!eq) continue;
        *eq = '\0';
        val = eq + 1;
        while (*val && isspace((unsigned char)*val)) val++;
        if (strcmp(key, "perf") == 0 || strcmp(key, "level") == 0) {
            int c = perf_parse_level(val);
            if (c >= 0) chosen = c;
        }
    }
    fclose(f);

    if (chosen < 0) return 0;
    apply_level(p, (perf_level_t)chosen, 1);
    return 1;
}

void perf_force(perf_t *p, perf_level_t lvl)
{
    apply_level(p, lvl, 1);
}

int perf_save(const perf_t *p)
{
    FILE *f = fopen(perf_conf_path(), "w");
    if (!f) return -1;
    fprintf(f, "# mshell performance profile — written by !setconf\n");
    fprintf(f, "# values: low | mid | high\n");
    fprintf(f, "perf=%s\n", perf_level_name(p->level));
    fclose(f);
    return 0;
}

void perf_describe(const perf_t *p, char *out, int outsz)
{
    if (outsz <= 0) return;
    out[0] = '\0';

    if (p->ram_mb >= 1024)
        snprintf(out, (size_t)outsz, "%d cores, %.1f GB", p->cores,
                 (double)p->ram_mb / 1024.0);
    else if (p->ram_mb > 0)
        snprintf(out, (size_t)outsz, "%d cores, %ld MB", p->cores, p->ram_mb);
    else
        snprintf(out, (size_t)outsz, "%d cores", p->cores);

    if (p->sdk > 0) {
        int used = (int)strlen(out);
        if (used < outsz - 1)
            snprintf(out + used, (size_t)(outsz - used), ", API %d", p->sdk);
    }
}
