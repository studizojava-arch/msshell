/*
 * term.h — terminal grid model + ANSI/VT parser. No GL, no I/O assumptions.
 *
 * The screen is a rows*cols grid of cells. Each cell carries a codepoint plus
 * resolved colours and style bits, so the renderer never parses escapes.
 *
 * Colours are packed 0xAABBGGRR so they upload to GL as UNSIGNED_BYTE directly.
 * Rows scrolled off the top are kept in a heap ring buffer (scrollback).
 */
#ifndef MSHELL_TERM_H
#define MSHELL_TERM_H

#include <stdint.h>

#define TERM_MAX_COLS   220
#define TERM_MAX_ROWS   80
#define TERM_MAX_PARAMS 32
#define TERM_SCROLLBACK 500
#define TERM_ESC_MAX    256

/* Style bits stored per cell. */
#define S_BOLD      0x01
#define S_UNDERLINE 0x02
#define S_BLINK     0x04
#define S_INVERSE   0x08
#define S_DIM       0x10
#define S_ITALIC    0x20
#define S_HIDDEN    0x40

#define RGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                          ((uint32_t)(g) << 8)  | (uint32_t)(r))
#define RGB_R(c) ((uint8_t)((c)        & 0xFF))
#define RGB_G(c) ((uint8_t)(((c) >> 8) & 0xFF))
#define RGB_B(c) ((uint8_t)(((c) >> 16)& 0xFF))
#define RGB_A(c) ((uint8_t)(((c) >> 24)& 0xFF))

typedef struct {
    uint32_t cp;      /* Unicode codepoint; 0 or ' ' renders blank */
    uint32_t fg;
    uint32_t bg;
    uint8_t  style;
} term_cell_t;

/* Parser states. */
enum {
    ST_GROUND = 0,
    ST_ESCAPE,     /* saw ESC, waiting for the command byte */
    ST_CHARSET,    /* ESC ( ... : consume one designator byte */
    ST_CSI,        /* collecting parameters */
    ST_OSC,        /* OSC payload until BEL or ST */
    ST_OSC_ESC     /* saw ESC inside OSC, waiting for '\' */
};

typedef struct {
    /* Heap grids, all rows*cols (alt) or sb_cap*cols (sb). */
    term_cell_t *grid;
    term_cell_t *alt;      /* spare buffer, kept simple: unused swap target */
    term_cell_t *sb;

    int cols, rows;
    int sb_cap, sb_head, sb_count;

    int cx, cy;
    int wrap_next;         /* deferred wrap: cursor parked on last column */
    int scrollTop, scrollBot;

    int      pen;          /* style bits of the current pen */
    uint32_t pen_fg, pen_bg;
    uint32_t def_fg, def_bg;

    int      save_cx, save_cy, save_pen;
    uint32_t save_fg, save_bg;

    int      tabstop[TERM_MAX_COLS];
    uint32_t palette[256];

    /* Incremental UTF-8 decoding across reads. */
    uint32_t u_acc;
    int      u_need, u_len;

    /* Parser. */
    int  state;
    int  p_par[TERM_MAX_PARAMS], p_npar, p_cur, p_had;
    int  p_priv;           /* CSI had '?' '>' '=' */
    int  p_inter;          /* CSI intermediate byte */
    char esc_buf[TERM_ESC_MAX];
    int  p_len;

    /* Modes. */
    int app_cursor;        /* DECCKM: arrows send SS3 A-D */
    int cursor_visible;
    int autowrap;

    int bell;
    char title[128];

    int fd;                /* pty master, used to answer DSR/DA queries */
} term_t;

void term_build_palette(uint32_t *pal);

/* Allocates the grids. Returns 0 on success, -1 on allocation failure. */
int  term_init(term_t *t, int cols, int rows);
void term_free(term_t *t);
void term_clear(term_t *t);
void term_resize(term_t *t, int cols, int rows);

/* Feed output bytes read from the pty. */
void term_feed(term_t *t, const uint8_t *data, int len);

/*
 * Cell for screen column x and screen row y, with `off` lines of scrollback
 * pulled in from the top. off == 0 shows the live screen.
 */
const term_cell_t *term_cell(const term_t *t, int x, int y, int off);

/* Largest valid `off` for term_cell. */
int term_max_scroll(const term_t *t);

/* ---- input ---- */
#define K_SHIFT 0x01
#define K_CTRL  0x02
#define K_ALT   0x04

typedef enum {
    K_NONE = 0,
    K_UP, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_PGUP, K_PGDN,
    K_INS, K_DEL, K_ENTER, K_TAB, K_BS, K_ESC,
    K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12
} term_key_t;

int  term_encode_key(const term_t *t, term_key_t key, int mods,
                     char *out, int outsz);
void term_send_key(term_t *t, term_key_t key, int mods);
void term_send_text(term_t *t, const char *s, int len);
int  term_bell_consume(term_t *t);

#endif /* MSHELL_TERM_H */
