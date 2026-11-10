/*
 * os_nadrino.h — Nadrino OS: a classic desktop environment rendered into the
 * terminal grid (and from there to the GL window).
 *
 * The OS is self-contained: it only writes cells into a term_t grid and
 * receives key/text/click events from the host (main.c). No pty, no SDL,
 * no network. All rendering is via term_cell() writes.
 *
 * Look & feel: Windows 95 (default) or Aero (Windows 7) — theme switchable.
 * Teal desktop, navy title bars, gray taskbar with a Start menu, desktop
 * icons, and five built-in apps: Terminal, Calculator, Text Editor, File
 * Manager, About.
 */
#ifndef MSHELL_OS_NADRINO_H
#define MSHELL_OS_NADRINO_H

#include "term.h"

#define ND_MAX_WINDOWS   8
#define ND_TASKBAR_H     1
#define ND_ICON_COUNT    5

typedef enum {
    ND_APP_TERMINAL = 0,
    ND_APP_CALCULATOR,
    ND_APP_EDITOR,
    ND_APP_FILES,
    ND_APP_ABOUT,
    ND_APP_COUNT
} nd_app_t;

typedef enum {
    ND_WIN_NORMAL = 0,
    ND_WIN_MINIMIZED,
    ND_WIN_MAXIMIZED
} nd_win_state_t;

typedef enum {
    ND_THEME_WIN95 = 0,
    ND_THEME_AERO,
    ND_THEME_COUNT
} nd_theme_t;

/* ---- app state (embedded, no malloc needed) ---- */

#define ND_TLINES   64      /* terminal app scrollback lines */
#define ND_TWIDTH   160
#define ND_ELINES   48      /* editor lines */
#define ND_EWIDTH   160
#define ND_FENTRIES 64

typedef struct {
    char lines[ND_TLINES][ND_TWIDTH];
    int  nlines;
    char input[ND_TWIDTH];
    int  ilen;
    int  dirty;
} nd_terminal_t;

typedef struct {
    char display[32];
    double acc;
    char op;
    int fresh;
} nd_calc_t;

typedef struct {
    char lines[ND_ELINES][ND_EWIDTH];
    int  nl;            /* number of used lines */
    int  cx, cy;        /* cursor */
    int  dirty;
} nd_editor_t;

typedef struct {
    char entries[ND_FENTRIES][72];
    int  count;
    int  sel, top;
    char dir[256];
} nd_files_t;

typedef struct {
    int  id;
    int  x, y, w, h;
    int  ox, oy, ow, oh;          /* saved geometry for restore */
    nd_win_state_t state;
    nd_app_t app;
    int  dragging;                /* title bar held: move with motion events */
    int  drag_dx, drag_dy;        /* click offset inside the window */
    union {
        nd_terminal_t term;
        nd_calc_t     calc;
        nd_editor_t   edit;
        nd_files_t    files;
    } u;
} nd_window_t;

typedef struct {
    nd_window_t win[ND_MAX_WINDOWS];
    int nwin;
    int next_id;
    int focus;            /* index into win[], -1 = desktop */
    int icon_sel;         /* selected desktop icon */
    int start_open;       /* Start menu visible */
    int menu_sel;         /* highlighted row in the Start menu */
    int running;
    int cols, rows;
    int tick;             /* incremented by os_nadrino_tick() */
    int theme;            /* nd_theme_t: classic Win95 or Aero Win7 */
    int stab;             /* 0 basic, 1 enhanced, 2 stability x3 (from config) */
} os_state_t;

/* ---- lifecycle ---- */

void os_nadrino_init(os_state_t *os, int cols, int rows);
void os_nadrino_free(os_state_t *os);
int  os_nadrino_running(const os_state_t *os);
void os_nadrino_resize(os_state_t *os, int cols, int rows);
void os_nadrino_tick(os_state_t *os);

/* Look and box64 preset, normally taken from the $simpleinterface screen.
 * Values are nd_theme_t / 0..2; out-of-range values fall back to Win95/basic. */
void os_nadrino_set_theme(os_state_t *os, int theme);
void os_nadrino_set_stab(os_state_t *os, int stab);

/* Opens an app window. Used by the dock icons in the simple interface.
 * Returns 0 on success, -1 if the app is unknown or no window slot is free. */
int  os_nadrino_launch(os_state_t *os, nd_app_t app);

/* ---- drawing: paints the whole desktop into the grid ---- */

void os_nadrino_render(const os_state_t *os, term_t *t);

/* ---- input: all coordinates are in CELLS ---- */

void os_nadrino_key(os_state_t *os, term_key_t key, int mods);
void os_nadrino_text(os_state_t *os, const char *utf8);
void os_nadrino_click(os_state_t *os, int cx, int cy);
/* Motion with left button held (down=1) drags grabbed windows; down=0 ends it. */
void os_nadrino_motion(os_state_t *os, int cx, int cy, int down);

#endif /* MSHELL_OS_NADRINO_H */
