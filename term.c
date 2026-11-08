/*
 * term.c — grid model, ANSI parser, scrollback ring.
 *
 * See term.h for the public surface. The parser is a small state machine fed
 * one byte at a time, which is what a pty read loop naturally gives us.
 */
#include "term.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define COUNTOF(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ------------------------------------------------------------------ palette */

/* The classic 8 colours, then brightened, then the cube and grey ramp. */
static const uint32_t kBase[8] = {
    RGBA(0x00, 0x00, 0x00, 0xFF), RGBA(0xC7, 0x3C, 0x3C, 0xFF),
    RGBA(0x5E, 0xB4, 0x5E, 0xFF), RGBA(0xE7, 0xDE, 0x6E, 0xFF),
    RGBA(0x4D, 0x7F, 0xD8, 0xFF), RGBA(0xA5, 0x5B, 0xD5, 0xFF),
    RGBA(0x5E, 0xB4, 0x9F, 0xFF), RGBA(0xD0, 0xD0, 0xEA, 0xFF),
};
static const uint32_t kBright[8] = {
    RGBA(0x6B, 0x6B, 0x80, 0xFF), RGBA(0xFF, 0x6B, 0x6B, 0xFF),
    RGBA(0x77, 0xE8, 0x77, 0xFF), RGBA(0xF7, 0xF3, 0x8F, 0xFF),
    RGBA(0x7E, 0xAA, 0xFF, 0xFF), RGBA(0xD5, 0x8F, 0xFF, 0xFF),
    RGBA(0x7F, 0xE8, 0xD0, 0xFF), RGBA(0xFF, 0xFF, 0xFF, 0xFF),
};

void term_build_palette(uint32_t *p)
{
    static const int kCube[6] = { 0, 95, 135, 175, 215, 255 };
    int i;

    for (i = 0; i < 8; i++)  p[i] = kBase[i];
    for (i = 0; i < 8; i++)  p[8 + i] = kBright[i];

    for (i = 0; i < 216; i++) {
        int r = kCube[i / 36];
        int g = kCube[(i / 6) % 6];
        int b = kCube[i % 6];
        p[16 + i] = RGBA((uint32_t)r, (uint32_t)g, (uint32_t)b, 0xFF);
    }
    for (i = 0; i < 24; i++) {
        uint32_t v = (uint32_t)(8 + i * 10);
        p[232 + i] = RGBA(v, v, v, 0xFF);
    }
}

/* ------------------------------------------------------------------- cells */

static void cell_blank(term_cell_t *c, uint32_t fg, uint32_t bg)
{
    c->cp = ' ';
    c->fg = fg;
    c->bg = bg;
    c->style = 0;
}

static term_cell_t *row_of(term_t *t, int y)
{
    return t->grid + (size_t)y * (size_t)t->cols;
}

/* Copy a row that is leaving the visible grid into the ring. */
static void sb_push(term_t *t, const term_cell_t *src)
{
    if (t->sb_cap <= 0) return;
    memcpy(t->sb + (size_t)t->sb_head * (size_t)t->cols, src,
           sizeof(term_cell_t) * (size_t)t->cols);
    t->sb_head = (t->sb_head + 1) % t->sb_cap;
    if (t->sb_count < t->sb_cap) t->sb_count++;
}

/* ---------------------------------------------------------------- scrolling */

static void scroll_up(term_t *t, int n)
{
    int top = t->scrollTop, bot = t->scrollBot;
    int h, i, x, y;

    if (top < 0) top = 0;
    if (bot >= t->rows) bot = t->rows - 1;
    h = bot - top + 1;
    if (h <= 0 || n <= 0) return;
    if (n > h) n = h;

    /* Only the full-height region feeds scrollback; a sub-region scroll is
     * just a rotation of that region. */
    if (top == 0 && bot == t->rows - 1)
        for (i = 0; i < n; i++) sb_push(t, row_of(t, i));

    for (y = top; y <= bot - n; y++)
        memcpy(row_of(t, y), row_of(t, y + n),
               sizeof(term_cell_t) * (size_t)t->cols);
    for (y = bot - n + 1; y <= bot; y++)
        for (x = 0; x < t->cols; x++)
            cell_blank(&row_of(t, y)[x], t->pen_fg, t->pen_bg);
}

static void scroll_down(term_t *t, int n)
{
    int top = t->scrollTop, bot = t->scrollBot;
    int x, y;

    if (top < 0) top = 0;
    if (bot >= t->rows) bot = t->rows - 1;
    if (bot - top + 1 <= 0 || n <= 0) return;
    if (n > bot - top + 1) n = bot - top + 1;

    for (y = bot; y >= top + n; y--)
        memcpy(row_of(t, y), row_of(t, y - n),
               sizeof(term_cell_t) * (size_t)t->cols);
    for (y = top; y < top + n; y++)
        for (x = 0; x < t->cols; x++)
            cell_blank(&row_of(t, y)[x], t->pen_fg, t->pen_bg);
}

/* ------------------------------------------------------------------- output */

static void put_cp(term_t *t, uint32_t cp)
{
    int x, y;

    if (t->wrap_next) {
        t->wrap_next = 0;
        t->cx = 0;
        if (++t->cy > t->scrollBot) { t->cy = t->rows - 1; scroll_up(t, 1); }
    }
    x = t->cx;
    y = t->cy;
    if (x < 0) x = 0;
    if (x >= t->cols) x = t->cols - 1;
    if (y < 0) y = 0;
    if (y >= t->rows) y = t->rows - 1;

    {
        term_cell_t *c = &row_of(t, y)[x];
        c->cp = cp;
        c->fg = t->pen_fg;
        c->bg = t->pen_bg;
        c->style = t->pen;
    }

    if (t->cx + 1 >= t->cols) { t->cx = t->cols - 1; t->wrap_next = 1; }
    else t->cx++;
}

static void crlf(term_t *t)
{
    t->wrap_next = 0;
    t->cx = 0;
}

static void linefeed(term_t *t)
{
    t->wrap_next = 0;
    if (t->cy >= t->scrollBot) scroll_up(t, 1);
    else t->cy++;
}

static void clamp_cursor(term_t *t)
{
    t->wrap_next = 0;
    if (t->cx < 0) t->cx = 0;
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy < 0) t->cy = 0;
    if (t->cy >= t->rows) t->cy = t->rows - 1;
}

/* ---------------------------------------------------------------------- SGR */

static int pal_idx(term_t *t, int n)
{
    (void)t;
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    return n;
}

static void apply_sgr(term_t *t, int code, int *i)
{
    int n = t->p_npar;

    switch (code) {
    case 0:  t->pen = 0; t->pen_fg = t->def_fg; t->pen_bg = t->def_bg; break;
    case 1:  t->pen |= S_BOLD; break;
    case 2:  t->pen |= S_DIM; break;
    case 3:  t->pen |= S_ITALIC; break;
    case 4:  t->pen |= S_UNDERLINE; break;
    case 5:
    case 6:  t->pen |= S_BLINK; break;
    case 7:  t->pen |= S_INVERSE; break;
    case 8:  t->pen |= S_HIDDEN; break;
    case 22: t->pen &= (uint8_t)~(S_BOLD | S_DIM); break;
    case 23: t->pen &= (uint8_t)~S_ITALIC; break;
    case 24: t->pen &= (uint8_t)~S_UNDERLINE; break;
    case 25: t->pen &= (uint8_t)~S_BLINK; break;
    case 27: t->pen &= (uint8_t)~S_INVERSE; break;
    case 28: t->pen &= (uint8_t)~S_HIDDEN; break;

    case 39: t->pen_fg = t->def_fg; break;
    case 49: t->pen_bg = t->def_bg; break;

    default:
        if (code >= 30 && code <= 37) {
            t->pen_fg = t->palette[pal_idx(t, code - 30)];
        } else if (code >= 40 && code <= 47) {
            t->pen_bg = t->palette[pal_idx(t, code - 40)];
        } else if (code >= 90 && code <= 97) {
            t->pen_fg = t->palette[pal_idx(t, code - 90 + 8)];
        } else if (code >= 100 && code <= 107) {
            t->pen_bg = t->palette[pal_idx(t, code - 100 + 8)];
        } else if (code == 38 || code == 48) {
            int store = code;
            int mode = (*i + 1 < n) ? t->p_par[*i + 1] : -1;
            if (mode == 5 && *i + 2 < n) {
                uint32_t c = t->palette[pal_idx(t, t->p_par[*i + 2])];
                if (store == 38) t->pen_fg = c; else t->pen_bg = c;
                *i += 2;
            } else if (mode == 2 && *i + 4 < n) {
                uint32_t c = RGBA((uint32_t)t->p_par[*i + 2],
                                  (uint32_t)t->p_par[*i + 3],
                                  (uint32_t)t->p_par[*i + 4], 0xFF);
                if (store == 38) t->pen_fg = c; else t->pen_bg = c;
                *i += 4;
            }
        }
        break;
    }
}

static void do_sgr(term_t *t)
{
    int i, n = t->p_npar;
    if (n == 0) { apply_sgr(t, 0, &i); return; }
    for (i = 0; i < n; i++) apply_sgr(t, t->p_par[i], &i);
}

/* ------------------------------------------------------------- CSI dispatch */

static int pget(term_t *t, int i, int dflt)
{
    if (i < 0 || i >= t->p_npar) return dflt;
    if (t->p_par[i] == 0 && dflt != 0 && i < t->p_npar && t->p_par[i] == 0)
        return dflt;
    return t->p_par[i];
}

static void erase_line(term_t *t, int mode)
{
    int x, y = t->cy;
    if (y < 0 || y >= t->rows) return;
    if (mode == 0) {
        for (x = t->cx; x < t->cols; x++) cell_blank(&row_of(t, y)[x], t->pen_fg, t->pen_bg);
    } else if (mode == 1) {
        for (x = 0; x <= t->cx && x < t->cols; x++) cell_blank(&row_of(t, y)[x], t->pen_fg, t->pen_bg);
    } else {
        for (x = 0; x < t->cols; x++) cell_blank(&row_of(t, y)[x], t->pen_fg, t->pen_bg);
    }
}

static void erase_display(term_t *t, int mode)
{
    int x, y;
    if (mode == 3 || mode == 2) {
        if (t->sb_cap > 0 && mode == 2)
            for (y = 0; y < t->rows; y++) sb_push(t, row_of(t, y));
        for (y = 0; y < t->rows; y++)
            for (x = 0; x < t->cols; x++)
                cell_blank(&row_of(t, y)[x], t->pen_fg, t->pen_bg);
        if (mode == 3) t->sb_count = 0, t->sb_head = 0;
    } else if (mode == 0) {
        for (x = t->cx; x < t->cols; x++) cell_blank(&row_of(t, t->cy)[x], t->pen_fg, t->pen_bg);
        for (y = t->cy + 1; y < t->rows; y++)
            for (x = 0; x < t->cols; x++) cell_blank(&row_of(t, y)[x], t->pen_fg, t->pen_bg);
    } else {
        for (y = 0; y < t->cy; y++)
            for (x = 0; x < t->cols; x++) cell_blank(&row_of(t, y)[x], t->pen_fg, t->pen_bg);
        for (x = 0; x <= t->cx && x < t->cols; x++) cell_blank(&row_of(t, t->cy)[x], t->pen_fg, t->pen_bg);
    }
}

static void set_priv_mode(term_t *t, int mode, int on)
{
    switch (mode) {
    case 1:  t->app_cursor = on; break;
    case 7:  t->autowrap = on; break;
    case 25: t->cursor_visible = on; break;
    case 1049:
    case 47:
    case 1047: break; /* alt-screen toggle — ignored */
    default: break;
    }
}

static void csi_dispatch(term_t *t, unsigned char fin)
{
    int n;

    if (t->p_priv) {
        if (fin == 'h' || fin == 'l') {
            int i;
            for (i = 0; i < t->p_npar; i++) set_priv_mode(t, t->p_par[i], fin == 'h');
            return;
        }
        if (fin == 'r') { t->scrollTop = 0; t->scrollBot = t->rows - 1; return; }
        /* Unknown private mode — ignore. */
        return;
    }

    switch (fin) {
    case 'm': do_sgr(t); break;
    case 'A': t->cy -= pget(t, 0, 1); clamp_cursor(t); break;
    case 'B': t->cy += pget(t, 0, 1); clamp_cursor(t); break;
    case 'C': t->cx += pget(t, 0, 1); clamp_cursor(t); break;
    case 'D': t->cx -= pget(t, 0, 1); clamp_cursor(t); break;
    case 'E': t->cx = 0; t->cy += pget(t, 0, 1); clamp_cursor(t); break;
    case 'F': t->cx = 0; t->cy -= pget(t, 0, 1); clamp_cursor(t); break;
    case 'd': t->cy = pget(t, 0, 1) - 1; clamp_cursor(t); break;
    case 'G': case 'f': case 'H':
        t->cy = pget(t, 0, 1) - 1;
        t->cx = pget(t, 1, 1) - 1;
        clamp_cursor(t);
        break;
    case 'J': erase_display(t, pget(t, 0, 0)); break;
    case 'K': erase_line(t, pget(t, 0, 0)); break;
    case 'L': n = pget(t, 0, 1); while (n-- > 0) scroll_down(t, 1); break;
    case 'M': n = pget(t, 0, 1); while (n-- > 0) scroll_up(t, 1); break;
    case 'P': { /* DCH: drop cells from the cursor column */
        int k = pget(t, 0, 1), x;
        term_cell_t *r = row_of(t, t->cy);
        if (t->cx + k > t->cols) k = t->cols - t->cx;
        for (x = t->cx; x + k < t->cols; x++) r[x] = r[x + k];
        for (x = t->cols - k; x < t->cols; x++) cell_blank(&r[x], t->pen_fg, t->pen_bg);
        break;
    }
    case '@': { /* ICH: insert blanks at the cursor column */
        int k = pget(t, 0, 1), x;
        term_cell_t *r = row_of(t, t->cy);
        if (t->cx + k > t->cols) k = t->cols - t->cx;
        for (x = t->cols - 1; x >= t->cx + k; x--) r[x] = r[x - k];
        for (x = t->cx; x < t->cx + k && x < t->cols; x++) cell_blank(&r[x], t->pen_fg, t->pen_bg);
        break;
    }
    case 'S': scroll_up(t, pget(t, 0, 1)); break;
    case 'T': scroll_down(t, pget(t, 0, 1)); break;
    case 'r':
        t->scrollTop = pget(t, 0, 1) - 1;
        t->scrollBot = pget(t, 1, t->rows) - 1;
        if (t->scrollTop < 0) t->scrollTop = 0;
        if (t->scrollBot >= t->rows) t->scrollBot = t->rows - 1;
        if (t->scrollBot <= t->scrollTop) { t->scrollTop = 0; t->scrollBot = t->rows - 1; }
        t->cx = 0; t->cy = t->scrollTop;
        break;
    case 'h': case 'l': /* non-private modes: nothing we must act on */
        break;
    case 'c': /* device attributes — reply so the child knows we exist */
        { char rep[16]; int l = snprintf(rep, sizeof(rep), "\x1b[?1;2c");
          if (t->fd >= 0 && l > 0) (void)!write(t->fd, rep, (size_t)l); }
        break;
    case 'n':
        if (pget(t, 0, 0) == 6) {
            char rep[32];
            int l = snprintf(rep, sizeof(rep), "\x1b[%d;%dR", t->cy + 1, t->cx + 1);
            if (t->fd >= 0 && l > 0) (void)!write(t->fd, rep, (size_t)l);
        }
        break;
    case 't': break; /* xterm window ops — ignore */
    default: break;
    }
}

/* -------------------------------------------------------------- ESC dispatch */

static void esc_dispatch(term_t *t, unsigned char ch)
{
    switch (ch) {
    case '7': t->save_cx = t->cx; t->save_cy = t->cy; t->save_pen = t->pen;
              t->save_fg = t->pen_fg; t->save_bg = t->pen_bg; break;
    case '8': t->cx = t->save_cx; t->cy = t->save_cy; t->pen = t->save_pen;
              t->pen_fg = t->save_fg; t->pen_bg = t->save_bg; clamp_cursor(t); break;
    case 'D': linefeed(t); break;
    case 'E': crlf(t); linefeed(t); break;
    case 'M': if (t->cy > t->scrollTop) t->cy--; else scroll_down(t, 1); clamp_cursor(t); break;
    case 'c': term_clear(t); break;
    case 'H': t->tabstop[t->cx] = 1; break;
    default: break;
    }
}

static void osc_dispatch(term_t *t)
{
    /* OSC 0/2 set the window title; we only keep the last one. */
    if (t->p_npar > 0 && (t->p_par[0] == 0 || t->p_par[0] == 2)) {
        size_t n = (size_t)t->p_len;
        if (n >= sizeof(t->title)) n = sizeof(t->title) - 1;
        memcpy(t->title, t->esc_buf, n);
        t->title[n] = '\0';
    }
}

/* -------------------------------------------------------------------- feed */

static void par_finish(term_t *t, int had)
{
    if (t->p_npar < TERM_MAX_PARAMS)
        t->p_par[t->p_npar++] = had ? t->p_cur : 0;
    t->p_cur = 0;
}

void term_feed(term_t *t, const uint8_t *d, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        uint8_t b = d[i];

        switch (t->state) {
        case ST_GROUND:
            if (b == 0x1b) { t->state = ST_ESCAPE; break; }
            if (b == '\n' || b == 0x0b || b == 0x0c) { linefeed(t); break; }
            if (b == '\r') { crlf(t); break; }
            if (b == '\b') { if (t->cx > 0) { t->cx--; t->wrap_next = 0; } break; }
            if (b == '\t') {
                int x = t->cx + 1;
                while (x < t->cols && !t->tabstop[x]) x++;
                t->cx = (x < t->cols) ? x : t->cols - 1;
                t->wrap_next = 0;
                break;
            }
            if (b == 0x07) { t->bell = 1; break; }
            if (b < 0x20) break;                     /* other C0: ignore */

            /* UTF-8 decode, incremental so a split sequence is fine. */
            if (t->u_len > 0) {
                if ((b & 0xC0) == 0x80) {
                    t->u_acc = (t->u_acc << 6) | (uint32_t)(b & 0x3F);
                    if (++t->u_len == t->u_need) {
                        uint32_t cp = t->u_acc;
                        t->u_len = 0; t->u_need = 0;
                        put_cp(t, cp);
                    }
                    break;
                }
                t->u_len = 0; t->u_need = 0;         /* fall through, retry */
            }
            if (b < 0x80) { put_cp(t, b); break; }
            if ((b & 0xE0) == 0xC0) { t->u_need = 2; t->u_acc = b & 0x1F; }
            else if ((b & 0xF0) == 0xE0) { t->u_need = 3; t->u_acc = b & 0x0F; }
            else if ((b & 0xF8) == 0xF0) { t->u_need = 4; t->u_acc = b & 0x07; }
            else break;
            t->u_len = 1;
            break;

        case ST_ESCAPE:
            if (b == '[') {
                t->state = ST_CSI;
                t->p_npar = 0; t->p_cur = 0; t->p_had = 0;
                t->p_priv = 0; t->p_inter = 0;
            } else if (b == ']') {
                t->state = ST_OSC; t->p_npar = 0; t->p_cur = 0; t->p_had = 0;
                t->p_len = 0;
            } else if (b == '(' || b == ')' || b == '*' || b == '+') {
                t->state = ST_CHARSET;
            } else if (b == 'P' || b == '_' || b == '^') {
                t->state = ST_OSC; t->p_len = 0;      /* treat as string osc */
            } else {
                esc_dispatch(t, b);
                t->state = ST_GROUND;
            }
            break;

        case ST_CHARSET:
            t->state = ST_GROUND;                      /* consume the designator */
            break;

        case ST_CSI:
            if (b >= '0' && b <= '9') { t->p_cur = t->p_cur * 10 + (b - '0'); t->p_had = 1; break; }
            if (b == ':') break;             /* sub-parameter separator */
            if (b == ';') { par_finish(t, t->p_had); t->p_had = 0; break; }
            if (b == '?' || b == '>' || b == '=') { t->p_priv = 1; break; }
            if (b == ' ' || b == '!') { t->p_inter = b; break; }
            if (b >= 0x40 && b <= 0x7E) {
                if (t->p_had || t->p_npar > 0) par_finish(t, t->p_had);
                csi_dispatch(t, b);
                t->state = ST_GROUND;
                break;
            }
            if (b == 0x1b) { t->state = ST_ESCAPE; break; }
            if (b < 0x20) { t->state = ST_GROUND; break; }
            break;

        case ST_OSC:
            if (b == 0x07) { osc_dispatch(t); t->state = ST_GROUND; break; }
            if (b == 0x1b) { t->state = ST_OSC_ESC; break; }
            if (b >= '0' && b <= '9' && t->p_len == 0) { t->p_cur = t->p_cur * 10 + (b - '0'); t->p_had = 1; break; }
            if (b == ';' && t->p_len == 0) {
                if (t->p_had && t->p_npar < TERM_MAX_PARAMS) t->p_par[t->p_npar++] = t->p_cur;
                t->p_cur = 0; t->p_had = 0; t->p_len = 1; break;
            }
            if (t->p_len < TERM_ESC_MAX - 1) t->esc_buf[t->p_len++] = (char)b;
            break;

        case ST_OSC_ESC:
            if (b == '\\') { osc_dispatch(t); t->state = ST_GROUND; }
            else t->state = ST_OSC;
            break;

        default:
            t->state = ST_GROUND;
            break;
        }
    }
}

/* ------------------------------------------------------------------ public */

int term_init(term_t *t, int cols, int rows)
{
    int x, y;

    memset(t, 0, sizeof(*t));
    t->fd = -1;
    t->sb_cap = TERM_SCROLLBACK;

    if (cols < 1) cols = 1;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows < 1) rows = 1;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;

    t->cols = cols;
    t->rows = rows;
    t->grid = calloc((size_t)cols * (size_t)rows, sizeof(term_cell_t));
    t->alt  = calloc((size_t)cols * (size_t)rows, sizeof(term_cell_t));
    t->sb   = calloc((size_t)cols * (size_t)t->sb_cap, sizeof(term_cell_t));
    if (!t->grid || !t->alt || !t->sb) { term_free(t); return -1; }

    term_build_palette(t->palette);

    t->def_fg = RGBA(0xD8, 0xDE, 0xE9, 0xFF);
    t->def_bg = RGBA(0x10, 0x12, 0x16, 0xFF);
    t->pen_fg = t->def_fg;
    t->pen_bg = t->def_bg;

    for (x = 0; x < TERM_MAX_COLS; x++) t->tabstop[x] = (x % 8 == 0);
    for (y = 0; y < rows; y++)
        for (x = 0; x < cols; x++)
            cell_blank(&row_of(t, y)[x], t->pen_fg, t->pen_bg);

    t->scrollTop = 0;
    t->scrollBot = rows - 1;
    t->autowrap = 1;
    t->cursor_visible = 1;
    return 0;
}

void term_free(term_t *t)
{
    free(t->grid); t->grid = NULL;
    free(t->alt);  t->alt = NULL;
    free(t->sb);   t->sb = NULL;
}

void term_clear(term_t *t)
{
    int x, y;
    for (y = 0; y < t->rows; y++)
        for (x = 0; x < t->cols; x++)
            cell_blank(&row_of(t, y)[x], t->pen_fg, t->pen_bg);
    t->cx = 0;
    t->cy = 0;
    t->wrap_next = 0;
    t->scrollTop = 0;
    t->scrollBot = t->rows - 1;
}

void term_resize(term_t *t, int cols, int rows)
{
    term_cell_t *ng, *nsb;
    int x, y;

    if (cols < 1) cols = 1;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows < 1) rows = 1;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;
    if (cols == t->cols && rows == t->rows) return;

    ng = calloc((size_t)cols * (size_t)rows, sizeof(term_cell_t));
    if (!ng) return;

    for (y = 0; y < rows; y++) {
        for (x = 0; x < cols; x++) {
            term_cell_t *dst = &ng[(size_t)y * cols + x];
            if (y < t->rows && x < t->cols) *dst = row_of(t, y)[x];
            else cell_blank(dst, t->pen_fg, t->pen_bg);
        }
    }

    free(t->grid);
    t->grid = ng;

    nsb = calloc((size_t)cols * (size_t)t->sb_cap, sizeof(term_cell_t));
    if (nsb) {
        int keep = t->sb_count;
        int start = (t->sb_head - keep + t->sb_cap) % t->sb_cap;
        for (y = 0; y < keep; y++) {
            int s = (start + y) % t->sb_cap;
            int w = (t->cols < cols) ? t->cols : cols;
            for (x = 0; x < w; x++)
                nsb[(size_t)y * cols + x] = t->sb[(size_t)s * t->cols + x];
            for (x = w; x < cols; x++) cell_blank(&nsb[(size_t)y * cols + x], t->pen_fg, t->pen_bg);
        }
        free(t->sb);
        t->sb = nsb;
        t->sb_head = keep % t->sb_cap;
    }

    t->cols = cols;
    t->rows = rows;
    t->scrollTop = 0;
    t->scrollBot = rows - 1;
    clamp_cursor(t);
}

const term_cell_t *term_cell(const term_t *t, int x, int y, int off)
{
    if (x < 0 || y < 0 || x >= t->cols || y >= t->rows) return NULL;

    if (off > 0) {
        int idx;
        if (off > t->sb_count) off = t->sb_count;
        if (y < off) {
            /* Rows above the live screen come from the ring, oldest first. */
            idx = (t->sb_head - off + y + t->sb_cap) % t->sb_cap;
            return t->sb + (size_t)idx * (size_t)t->cols + (size_t)x;
        }
        y -= off;
        if (y >= t->rows) y = t->rows - 1;
    }
    return t->grid + (size_t)y * (size_t)t->cols + (size_t)x;
}

int term_max_scroll(const term_t *t)
{
    return t->sb_count;
}

/* ------------------------------------------------------------------- input */

int term_encode_key(const term_t *t, term_key_t key, int mods, char *out, int outsz)
{
    int n = 0;
    char c;

    if (mods & K_CTRL) {
        switch (key) {
        case K_UP: case K_DOWN: case K_RIGHT: case K_LEFT:
        case K_HOME: case K_END: case K_PGUP: case K_PGDN:
            break;                       /* ctrl+arrow: plain sequence */
        default:
            if (key >= K_F1 && key <= K_F4) break;
            /* Ctrl+letter maps to the control byte. */
            if (key >= 1 && key <= 26) { out[n++] = (char)('a' + key - 1); return n; }
            break;
        }
    }

    switch (key) {
    case K_UP:    n = snprintf(out, outsz, "\x1b%s", t->app_cursor ? "OA" : "[A"); break;
    case K_DOWN:  n = snprintf(out, outsz, "\x1b%s", t->app_cursor ? "OB" : "[B"); break;
    case K_RIGHT: n = snprintf(out, outsz, "\x1b%s", t->app_cursor ? "OC" : "[C"); break;
    case K_LEFT:  n = snprintf(out, outsz, "\x1b%s", t->app_cursor ? "OD" : "[D"); break;
    case K_HOME:  n = snprintf(out, outsz, "\x1b[H"); break;
    case K_END:   n = snprintf(out, outsz, "\x1b[F"); break;
    case K_PGUP:  n = snprintf(out, outsz, "\x1b[5~"); break;
    case K_PGDN:  n = snprintf(out, outsz, "\x1b[6~"); break;
    case K_INS:   n = snprintf(out, outsz, "\x1b[2~"); break;
    case K_DEL:   n = snprintf(out, outsz, "\x1b[3~"); break;
    case K_F1:    n = snprintf(out, outsz, "\x1bOP"); break;
    case K_F2:    n = snprintf(out, outsz, "\x1bOQ"); break;
    case K_F3:    n = snprintf(out, outsz, "\x1bOR"); break;
    case K_F4:    n = snprintf(out, outsz, "\x1bOS"); break;
    case K_ESC:   out[n++] = 0x1b; break;
    case K_ENTER: out[n++] = '\r'; break;
    case K_TAB:   out[n++] = '\t'; break;
    case K_BS:    out[n++] = (mods & K_SHIFT) ? 0x7f : 0x08; break;
    default:      return 0;
    }
    if (n < 0) return 0;

    if ((mods & K_ALT) && n > 0 && out[0] != 0x1b && n + 1 < outsz) {
        memmove(out + 1, out, (size_t)n);
        out[0] = 0x1b;
        n++;
    }
    (void)c;
    return n;
}

void term_send_key(term_t *t, term_key_t key, int mods)
{
    char buf[16];
    int n = term_encode_key(t, key, mods, buf, (int)sizeof(buf));
    if (n > 0 && t->fd >= 0) (void)!write(t->fd, buf, (size_t)n);
}

void term_send_text(term_t *t, const char *s, int len)
{
    if (t->fd < 0) return;
    while (len > 0) {
        ssize_t w = write(t->fd, s, (size_t)len);
        if (w <= 0) return;
        s += w;
        len -= (int)w;
    }
}

int term_bell_consume(term_t *t)
{
    int b = t->bell;
    t->bell = 0;
    return b;
}
