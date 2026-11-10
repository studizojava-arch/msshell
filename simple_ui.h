/*
 * simple_ui.h — "Simple interface": a configuration front-end drawn into the
 * terminal grid, opened by typing $simpleinterface at the mshell prompt.
 *
 * It gives the user:
 *   - a circular dock of app icons along the bottom
 *   - named containers the user can create, rename and delete
 *   - an OS switch (Classic / Win95 vs Aero / Win7)
 *   - a box64 stability selector (Basic / Enhanced / Stability x3)
 *   - a LAUNCH button at the bottom that boots Nadrino OS with those settings
 *
 * The module never touches SDL, a pty or the network: it only paints cells and
 * reports what the user asked for. main.c polls su_take_action() and performs
 * the actual switching, so this file stays testable and display-agnostic.
 */
#ifndef MSHELL_SIMPLE_UI_H
#define MSHELL_SIMPLE_UI_H

#include "term.h"

#define SU_MAX_CONTAINERS  8
#define SU_NAME_LEN        32
#define SU_ICONS_PER_BOX   16
#define SU_DOCK_ICONS      8

/* box64 stability presets. SU_STAB_X3 is the "stability x3" mode. */
typedef enum {
    SU_STAB_BASIC = 0,
    SU_STAB_ENHANCED,
    SU_STAB_X3,
    SU_STAB_COUNT
} su_stab_t;

/* Desktop look. Values match nd_theme_t in os_nadrino.h. */
typedef enum {
    SU_THEME_CLASSIC = 0,   /* Windows 95 style */
    SU_THEME_AERO,          /* Windows 7 style  */
    SU_THEME_COUNT
} su_theme_t;

/* What the LAUNCH bar boots. Passed to the host as the action argument. */
typedef enum {
    SU_OS_NADRINO = 0,      /* Nadrino OS desktop   */
    SU_OS_TERMINAL,         /* plain GL terminal    */
    SU_OS_COUNT
} su_os_t;

/* Rows inside the settings zone. */
#define SU_SETTING_OS     0
#define SU_SETTING_THEME  1
#define SU_SETTING_STAB   2
#define SU_SETTINGS       3

/* Keyboard focus zones. */
typedef enum {
    SU_ZONE_CONTAINERS = 0,
    SU_ZONE_SETTINGS,
    SU_ZONE_LAUNCH,
    SU_ZONE_COUNT
} su_zone_t;

typedef struct {
    char name[SU_NAME_LEN];
    int  icon_ids[SU_ICONS_PER_BOX];
    int  nicons;
    int  theme;             /* su_theme_t — per-container override */
    int  stab;              /* su_stab_t  */
} su_container_t;

typedef struct {
    const char *name;
    char        glyph;
    int         app;        /* nd_app_t value to launch, -1 = panel action */
} su_icon_t;

/* What the host should do after a key/click. Popped by su_take_action(). */
typedef enum {
    SU_ACT_NONE = 0,
    SU_ACT_LAUNCH_OS,       /* open Nadrino OS with the chosen theme/stab */
    SU_ACT_LAUNCH_APP,      /* launch icon `arg` (nd_app_t) inside the OS */
    SU_ACT_EXIT             /* close the simple interface */
} su_action_t;

typedef struct {
    int running;
    int cols, rows;

    su_container_t cont[SU_MAX_CONTAINERS];
    int            ncont;
    int            sel;         /* highlighted container row, -1 = none */
    int            open;        /* container whose contents are shown, -1 = none */
    int            renaming;    /* container being renamed, -1 = none */
    char           rbuf[SU_NAME_LEN];
    int            rlen;

    su_icon_t      icons[SU_DOCK_ICONS];
    int            nicons;
    int            dock_focus;  /* hovered/selected dock slot, -1 = none */

    int            theme;       /* su_theme_t   */
    int            stab;        /* su_stab_t    */
    int            os_sel;      /* su_os_t — what LAUNCH boots        */
    int            setting;     /* SU_SETTING_* — row focused in settings */
    int            zone;        /* su_zone_t — keyboard focus */

    int            tick;
    int            cursor_on;

    su_action_t    action;      /* pending request for the host */
    int            action_arg;
} su_state_t;

/* ---- lifecycle ---- */
void su_init(su_state_t *su, int cols, int rows);
void su_free(su_state_t *su);
int  su_running(const su_state_t *su);
void su_resize(su_state_t *su, int cols, int rows);
void su_tick(su_state_t *su);

/* ---- drawing ---- */
void su_render(const su_state_t *su, term_t *t);

/* ---- input (all coordinates in CELLS) ---- */
void su_key(su_state_t *su, term_key_t key, int mods);
void su_text(su_state_t *su, const char *utf8);
void su_click(su_state_t *su, int cx, int cy);
void su_motion(su_state_t *su, int cx, int cy, int down);

/* ---- host handshake ---- */
/* Pops one pending request; returns SU_ACT_NONE when there is nothing to do. */
su_action_t su_take_action(su_state_t *su, int *arg);
int  su_chosen_theme(const su_state_t *su);   /* su_theme_t */
int  su_chosen_stab(const su_state_t *su);    /* su_stab_t  */
int  su_chosen_os(const su_state_t *su);      /* su_os_t    */

/* ---- geometry shared by render + hit testing ---- */
void su_dock_slot(const su_state_t *su, int i, int *cx, int *cy);
void su_launch_rect(const su_state_t *su, int *x0, int *y0, int *x1, int *y1);

#endif /* MSHELL_SIMPLE_UI_H */
