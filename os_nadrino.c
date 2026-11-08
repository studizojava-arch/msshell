/*
 * os_nadrino.c — Nadrino OS desktop rendered into the terminal grid.
 *
 * This is a self-contained desktop environment that paints into a term_t grid.
 * It receives key/text/click events from the host (main.c). No pty, no SDL,
 * no network. All rendering is via term_cell() writes.
 *
 * Layout:
 *   - Full-screen teal desktop
 *   - Desktop icons (top-left)
 *   - Floating windows with title bars, borders, content
 *   - 1-row taskbar at bottom with Start, window buttons, clock
 *
 * Apps:
 *   Terminal  — scrollback + input line, echo text
 *   Calculator — digit entry, + - * / = C
 *   Text Editor — multi-line text, cursor navigation
 *   File Manager — list of entries, cursor selection
 *   About  — static info page
 */
#include "os_nadrino.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ------------------------------------------------------------------ palette */

#define CLR_WINDOW   RGBA(0xC0, 0xC0, 0xC0, 0xFF)
#define CLR_TITLE    RGBA(0x00, 0x00, 0x80, 0xFF)
#define CLR_TITLE_IN RGBA(0x80, 0x80, 0x80, 0xFF)
#define CLR_DESKTOP  RGBA(0x00, 0x80, 0x80, 0xFF)
#define CLR_TEXT     RGBA(0x00, 0x00, 0x00, 0xFF)
#define CLR_TEXT_WIN RGBA(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_TASKBAR  RGBA(0xC0, 0xC0, 0xC0, 0xFF)
#define CLR_BORDER   RGBA(0x80, 0x80, 0x80, 0xFF)
#define CLR_HIGHLIGHT RGBA(0x00, 0x00, 0xFF, 0xFF)
#define CLR_EDIT     RGBA(0xFF, 0xFF, 0xFF, 0xFF)
#define CLR_ICON_SEL RGBA(0x00, 0x00, 0x80, 0x80)

/* ------------------------------------------------------------------ helpers */

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void cell_set(term_t *t, int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    if (x < 0 || y < 0 || x >= t->cols || y >= t->rows) return;
    const term_cell_t *c = term_cell(t, x, y, 0);
    if (!c) return;
    term_cell_t *mut = (term_cell_t *)c;
    mut->cp   = (uint32_t)(unsigned char)ch;
    mut->fg   = fg;
    mut->bg   = bg;
    mut->style = 0;
}

static void rect_fill(term_t *t, int x0, int y0, int x1, int y1,
                      char ch, uint32_t fg, uint32_t bg)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            cell_set(t, x, y, ch, fg, bg);
}

static int idx_of(os_state_t *os, int id)
{
    for (int i = 0; i < os->nwin; i++)
        if (os->win[i].id == id) return i;
    return -1;
}

static int top_z(const os_state_t *os, int cx, int cy)
{
    for (int i = os->nwin - 1; i >= 0; i--) {
        const nd_window_t *w = &os->win[i];
        if (w->state == ND_WIN_MINIMIZED) continue;
        if (cx >= w->x && cx < w->x + w->w && cy >= w->y && cy < w->y + w->h)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ init */

void os_nadrino_init(os_state_t *os, int cols, int rows)
{
    memset(os, 0, sizeof(*os));
    os->running = 1;
    os->focus = -1;
    os->icon_sel = -1;
    os->start_open = 0;
    os->cols = cols;
    os->rows = rows;
    os->tick = 0;
}

void os_nadrino_free(os_state_t *os)
{
    (void)os;
}

int os_nadrino_running(const os_state_t *os)
{
    return os->running;
}

void os_nadrino_resize(os_state_t *os, int cols, int rows)
{
    os->cols = cols;
    os->rows = rows;
}

void os_nadrino_tick(os_state_t *os)
{
    os->tick++;
}

/* ------------------------------------------------------------------ window ops */

static nd_window_t *add_window(os_state_t *os, nd_app_t app, int x, int y, int w, int h)
{
    if (os->nwin >= ND_MAX_WINDOWS) return NULL;
    int i = os->nwin++;
    nd_window_t *w2 = &os->win[i];
    memset(w2, 0, sizeof(*w2));
    w2->id = ++os->next_id;
    w2->app = app;
    w2->state = ND_WIN_NORMAL;
    w2->x = x;
    w2->y = y;
    w2->w = w;
    w2->h = h;
    w2->ox = x; w2->oy = y; w2->ow = w; w2->oh = h;

    /* init app state */
    switch (app) {
    case ND_APP_TERMINAL:
        w2->u.term.nlines = 0;
        w2->u.term.ilen = 0;
        break;
    case ND_APP_CALCULATOR:
        strncpy(w2->u.calc.display, "0", sizeof(w2->u.calc.display));
        w2->u.calc.display[sizeof(w2->u.calc.display)-1] = '\0';
        w2->u.calc.acc = 0; w2->u.calc.op = 0; w2->u.calc.fresh = 1;
        break;
    case ND_APP_EDITOR:
        w2->u.edit.nl = 0; w2->u.edit.cx = 0; w2->u.edit.cy = 0;
        break;
    case ND_APP_FILES:
        w2->u.files.count = 0; w2->u.files.sel = 0; w2->u.files.top = 0;
        strncpy(w2->u.files.dir, "/home", sizeof(w2->u.files.dir) - 1);
        w2->u.files.dir[sizeof(w2->u.files.dir)-1] = '\0';
        break;
    case ND_APP_ABOUT:
        break;
    default:
        break;
    }
    return w2;
}

static void bring_top(os_state_t *os, int idx)
{
    if (idx < 0 || idx >= os->nwin) return;
    nd_window_t tmp = os->win[idx];
    for (int i = idx; i < os->nwin - 1; i++)
        os->win[i] = os->win[i + 1];
    os->win[os->nwin - 1] = tmp;
    os->focus = os->nwin - 1;
}

static void close_window(os_state_t *os, int idx)
{
    if (idx < 0 || idx >= os->nwin) return;
    for (int i = idx; i < os->nwin - 1; i++)
        os->win[i] = os->win[i + 1];
    os->nwin--;
    if (os->focus >= os->nwin) os->focus = os->nwin > 0 ? os->nwin - 1 : -1;
}

static void minimize(os_state_t *os, int idx)
{
    if (idx < 0 || idx >= os->nwin) return;
    os->win[idx].state = ND_WIN_MINIMIZED;
    if (os->focus == idx) os->focus = -1;
}

static void toggle_max(os_state_t *os, int idx)
{
    if (idx < 0 || idx >= os->nwin) return;
    nd_window_t *w = &os->win[idx];
    if (w->state == ND_WIN_MAXIMIZED) {
        w->x = w->ox; w->y = w->oy; w->w = w->ow; w->h = w->oh;
        w->state = ND_WIN_NORMAL;
    } else {
        w->ox = w->x; w->oy = w->y; w->ow = w->w; w->oh = w->h;
        w->x = 0; w->y = 0;
        w->w = os->cols;
        w->h = os->rows - ND_TASKBAR_H;
        w->state = ND_WIN_MAXIMIZED;
    }
}

/* ------------------------------------------------------------------ render */

static const char *app_name(nd_app_t app)
{
    static const char *names[] = {
        "Terminal", "Calculator", "Text Editor", "File Manager", "About"
    };
    return names[app];
}

/* Title bar chars: ┌─┐│└─┘ */
#define TTITLE_CH '='
#define TBORD_CH '|'
#define TBR_CH   '-'
#define TBL_CH   '+'

static void render_title_bar(const os_state_t *os, const nd_window_t *w, term_t *t)
{
    int y = w->y + 1;
    int x0 = w->x + 1, x1 = w->x + w->w - 1;
    if (y < 0 || y >= t->rows) return;
    int bg = w->state == ND_WIN_MINIMIZED ? CLR_TITLE_IN :
             (os->focus >= 0 && os->focus < os->nwin &&
              os->win[os->focus].id == w->id) ? CLR_TITLE : CLR_TITLE_IN;
    rect_fill(t, x0, y, x1, y + 1, ' ', CLR_TEXT_WIN, (uint32_t)bg);
    /* Title text */
    char buf[128];
    snprintf(buf, sizeof(buf), " %s - %s ", app_name(w->app),
             w->state == ND_WIN_MAXIMIZED ? "Maximized" : "");
    int len = (int)strlen(buf);
    for (int i = 0, j = x0; i < len && j < x1; i++, j++)
        cell_set(t, j, y, buf[i], CLR_TEXT_WIN, (uint32_t)bg);
    /* Buttons: minimize, maximize, close */
    if (x1 - 8 >= x0) cell_set(t, x1 - 8, y, '_', CLR_TEXT_WIN, (uint32_t)bg);
    if (x1 - 5 >= x0) cell_set(t, x1 - 5, y, 'o', CLR_TEXT_WIN, (uint32_t)bg);
    if (x1 - 2 >= x0) cell_set(t, x1 - 2, y, 'x', CLR_TEXT_WIN, (uint32_t)bg);
}

static void render_window_frame(const nd_window_t *w, term_t *t)
{
    /* border rows */
    int x0 = w->x, x1 = w->x + w->w - 1;
    int y0 = w->y, y1 = w->y + w->h - 1;
    /* top */
    if (y0 >= 0 && y0 < t->rows) {
        cell_set(t, x0, y0, '+', CLR_BORDER, CLR_WINDOW);
        for (int x = x0 + 1; x < x1 && x < t->cols; x++)
            cell_set(t, x, y0, TTITLE_CH, CLR_BORDER, CLR_WINDOW);
        if (x1 >= 0 && x1 < t->cols)
            cell_set(t, x1, y0, '+', CLR_BORDER, CLR_WINDOW);
    }
    /* bottom */
    if (y1 >= 0 && y1 < t->rows) {
        cell_set(t, x0, y1, '+', CLR_BORDER, CLR_WINDOW);
        for (int x = x0 + 1; x < x1 && x < t->cols; x++)
            cell_set(t, x, y1, TBR_CH, CLR_BORDER, CLR_WINDOW);
        if (x1 >= 0 && x1 < t->cols)
            cell_set(t, x1, y1, '+', CLR_BORDER, CLR_WINDOW);
    }
    /* sides */
    for (int y = y0 + 1; y < y1; y++) {
        if (y < 0 || y >= t->rows) continue;
        if (x0 >= 0 && x0 < t->cols) cell_set(t, x0, y, TBORD_CH, CLR_BORDER, CLR_WINDOW);
        if (x1 >= 0 && x1 < t->cols) cell_set(t, x1, y, TBORD_CH, CLR_BORDER, CLR_WINDOW);
    }
}

/* Draw a single line of text at (x, y) with a bg colour */
static void draw_line(term_t *t, int x, int y, int max_w,
                      const char *s, uint32_t fg, uint32_t bg)
{
    int len = (int)strlen(s);
    for (int i = 0; i < max_w && i < len; i++)
        cell_set(t, x + i, y, s[i], fg, bg);
    /* clear rest */
    for (int i = len; i < max_w; i++)
        cell_set(t, x + i, y, ' ', CLR_TEXT, bg);
}

static void render_app_terminal(const nd_window_t *w, term_t *t)
{
    int x0 = w->x + 1, y0 = w->y + 2;
    int cw = w->w - 2, ch = w->h - 3;
    if (cw <= 0 || ch <= 0) return;
    const nd_terminal_t *tm = &w->u.term;
    int start = tm->nlines > ch ? tm->nlines - ch : 0;
    for (int i = 0; i < ch && i < tm->nlines - start; i++)
        draw_line(t, x0, y0 + i, cw, tm->lines[start + i], CLR_TEXT, CLR_EDIT);
    /* input line */
    int yinp = y0 + ch - 1;
    if (yinp >= 0 && yinp < t->rows) {
        char inp[ND_TWIDTH + 4];
        snprintf(inp, sizeof(inp), "$ %s_", tm->input);
        draw_line(t, x0, yinp, cw, inp, CLR_TEXT, CLR_HIGHLIGHT);
    }
}

static void render_app_calc(const nd_window_t *w, term_t *t)
{
    int x0 = w->x + 1, y0 = w->y + 2;
    int cw = w->w - 2, ch = w->h - 3;
    if (cw <= 0 || ch <= 0) return;
    const nd_calc_t *c = &w->u.calc;
    char buf[64];
    snprintf(buf, sizeof(buf), "  %s  ", c->display);
    draw_line(t, x0, y0, cw, buf, CLR_TEXT, CLR_EDIT);
    if (ch > 1)
        draw_line(t, x0, y0 + 1, cw, "  + - * / = C", CLR_TEXT, CLR_WINDOW);
}

static void render_app_editor(const nd_window_t *w, term_t *t)
{
    int x0 = w->x + 1, y0 = w->y + 2;
    int cw = w->w - 2, ch = w->h - 3;
    if (cw <= 0 || ch <= 0) return;
    const nd_editor_t *e = &w->u.edit;
    int shown = ch < e->nl ? ch : e->nl;
    for (int i = 0; i < shown; i++)
        draw_line(t, x0, y0 + i, cw, e->lines[i], CLR_TEXT, CLR_EDIT);
    /* cursor line */
    if (e->cy < ch) {
        char buf[ND_EWIDTH + 4];
        int off = 0;
        for (int j = 0; j < e->cx && j < (int)strlen(e->lines[e->cy]); j++)
            buf[off++] = e->lines[e->cy][j];
        buf[off++] = '_';
        for (; off < (int)strlen(e->lines[e->cy]) && off < cw - 1; off++)
            buf[off] = e->lines[e->cy][off];
        buf[off] = '\0';
        draw_line(t, x0, y0 + e->cy, cw, buf, CLR_TEXT, CLR_HIGHLIGHT);
    }
}

static void render_app_files(const nd_window_t *w, term_t *t)
{
    int x0 = w->x + 1, y0 = w->y + 2;
    int cw = w->w - 2, ch = w->h - 3;
    if (cw <= 0 || ch <= 0) return;
    const nd_files_t *f = &w->u.files;
    char dirbuf[128];
    snprintf(dirbuf, sizeof(dirbuf), "  %s", f->dir);
    draw_line(t, x0, y0, cw, dirbuf, CLR_TEXT, CLR_WINDOW);
    for (int i = 0; i < f->count && i < ch - 1; i++) {
        int idx = i + f->top;
        if (idx >= f->count) break;
        char buf[128];
        snprintf(buf, sizeof(buf), "  %s%s", f->entries[idx], "");
        if (idx == f->sel)
            draw_line(t, x0, y0 + 1 + i, cw, buf, CLR_TEXT_WIN, CLR_HIGHLIGHT);
        else
            draw_line(t, x0, y0 + 1 + i, cw, buf, CLR_TEXT, CLR_EDIT);
    }
}

static void render_app_about(const nd_window_t *w, term_t *t)
{
    int x0 = w->x + 1, y0 = w->y + 2;
    int cw = w->w - 2, ch = w->h - 3;
    if (cw <= 0 || ch <= 0) return;
    const char *lines[] = {
        "  === Nadrino OS ===",
        "  Version 0.1",
        "",
        "  A classic desktop",
        "  environment for",
        "  Termux — no root!",
        "",
        "  Press ESC to close",
    };
    for (int i = 0; i < 8 && i < ch; i++)
        draw_line(t, x0, y0 + i, cw, lines[i], CLR_TEXT_WIN, CLR_TITLE);
}

/* ---- render_app dispatch ---- */

static void render_app(const nd_window_t *w, term_t *t)
{
    if (w->state == ND_WIN_MINIMIZED) return;
    switch (w->app) {
    case ND_APP_TERMINAL: render_app_terminal(w, t); break;
    case ND_APP_CALCULATOR: render_app_calc(w, t); break;
    case ND_APP_EDITOR:     render_app_editor(w, t); break;
    case ND_APP_FILES:      render_app_files(w, t); break;
    case ND_APP_ABOUT:      render_app_about(w, t); break;
    default: break;
    }
}

/* ---- desktop icons ---- */

static void render_desktop_icons(const os_state_t *os, term_t *t)
{
    struct { char ch; const char *label; nd_app_t app; } icons[] = {
        { 'T', "Terminal",  ND_APP_TERMINAL },
        { 'C', "Calculator",ND_APP_CALCULATOR },
        { 'E', "Text Editor",ND_APP_EDITOR },
        { 'F', "Files",     ND_APP_FILES },
        { '?', "About",     ND_APP_ABOUT },
    };
    int cols = os->cols / 14;
    if (cols < 1) cols = 1;
    for (int i = 0; i < ND_ICON_COUNT; i++) {
        int col = i % cols, row = i / cols;
        int ix = col * 14 + 1, iy = row * 18 + 1;
        if (ix + 12 >= t->cols || iy + 16 >= t->rows) continue;
        if (top_z(os, ix, iy) >= 0) continue;
        /* selected highlight */
        if (os->icon_sel == i)
            rect_fill(t, ix, iy, ix + 12, iy + 14, ' ', CLR_ICON_SEL, CLR_ICON_SEL);
        /* icon char */
        cell_set(t, ix + 5, iy + 5, icons[i].ch, CLR_TEXT_WIN, CLR_ICON_SEL);
        /* label */
        const char *lbl = icons[i].label;
        for (int j = 0; j < (int)strlen(lbl) && ix + 1 + j < t->cols; j++)
            cell_set(t, ix + 1 + j, iy + 10, lbl[j], CLR_TEXT_WIN, CLR_DESKTOP);
    }
}

/* ---- taskbar ---- */

static void render_taskbar(const os_state_t *os, term_t *t)
{
    int y = os->rows - ND_TASKBAR_H;
    if (y < 0) return;
    rect_fill(t, 0, y, os->cols, os->rows, ' ', CLR_TEXT, CLR_TASKBAR);

    /* Start button */
    uint32_t bg = os->start_open ? CLR_TITLE : CLR_TASKBAR;
    rect_fill(t, 0, y, 1, os->rows, ' ', CLR_TEXT_WIN, (uint32_t)bg);
    if (y < t->rows) cell_set(t, 0, y, ' ', CLR_TEXT_WIN, (uint32_t)bg);

    /* Window buttons */
    int bx = 2;
    for (int i = 0; i < os->nwin; i++) {
        const nd_window_t *w = &os->win[i];
        if (w->state == ND_WIN_MINIMIZED) continue;
        char buf[32];
        int bl = snprintf(buf, sizeof(buf), " %s ", app_name(w->app));
        int bw = bl + 1;
        if (bx + bw >= os->cols) break;
        uint32_t tbg = (os->focus == i) ? CLR_TITLE : CLR_TASKBAR;
        rect_fill(t, bx, y, bx + bw, os->rows, ' ', CLR_TEXT_WIN, (uint32_t)tbg);
        for (int j = 0; j < bl && bx + j < os->cols; j++)
            cell_set(t, bx + j, y, buf[j], CLR_TEXT_WIN, (uint32_t)tbg);
        bx += bw + 1;
    }

    /* Clock */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char cs[9];
    strftime(cs, sizeof(cs), "%H:%M", tm);
    int cx = os->cols - (int)strlen(cs) - 1;
    if (cx > bx) {
        for (int i = 0; i < (int)strlen(cs); i++)
            cell_set(t, cx + i, y, cs[i], CLR_TEXT, CLR_TASKBAR);
    }
}

/* ---- main render ---- */

void os_nadrino_render(const os_state_t *os, term_t *t)
{
    /* clear to desktop */
    rect_fill(t, 0, 0, t->cols, t->rows, ' ', CLR_TEXT, CLR_DESKTOP);

    /* draw windows back-to-front */
    for (int i = 0; i < os->nwin; i++) {
        render_window_frame(&os->win[i], t);
        render_title_bar(os, &os->win[i], t);
        render_app(&os->win[i], t);
    }

    /* desktop icons */
    render_desktop_icons(os, t);

    /* taskbar */
    render_taskbar(os, t);
}

/* ------------------------------------------------------------------ input */

static void open_app(os_state_t *os, nd_app_t app, int cols, int rows)
{
    int w = clamp(cols * 3 / 4, 30, cols - 2);
    int h = clamp(rows * 2 / 3, 10, rows - ND_TASKBAR_H - 2);
    int x = (cols - w) / 2;
    int y = (rows - ND_TASKBAR_H - h) / 2;
    nd_window_t *nw = add_window(os, app, x, y, w, h);
    if (nw) {
        os->focus = idx_of(os, nw->id);
        os->icon_sel = -1;
        os->start_open = 0;
    }
}

/* Handle app-specific key input */
static void handle_app_key(os_state_t *os, nd_window_t *w, term_key_t key, int mods)
{
    (void)mods;
    if (os->focus < 0 || os->focus >= os->nwin) return;

    switch (w->app) {
    case ND_APP_TERMINAL: {
        nd_terminal_t *tm = &w->u.term;
        if (key == K_ENTER) {
            /* Execute — just echo */
            if (tm->ilen > 0) {
                tm->input[tm->ilen] = '\0';
                char buf[ND_TWIDTH + 64];
                int bl = snprintf(buf, sizeof(buf), "$ %s\nCommand not found: %s\n",
                                  tm->input, tm->input);
                if (bl > 0 && tm->nlines < ND_TLINES) {
                    strncpy(tm->lines[tm->nlines], buf, ND_TWIDTH - 1);
                    tm->lines[tm->nlines][ND_TWIDTH - 1] = '\0';
                    tm->nlines++;
                }
                tm->ilen = 0;
            }
        } else if (key == K_BS || key == K_DEL) {
            if (tm->ilen > 0) tm->ilen--;
        } else if (key == K_HOME) {
            tm->ilen = 0;
        } else if (key == K_END) {
            tm->ilen = (int)strlen(tm->input);
        } else if (key == K_LEFT) {
            if (tm->ilen > 0) tm->ilen--;
        } else if (key == K_RIGHT) {
            if ((int)strlen(tm->input) > tm->ilen) tm->ilen++;
        }
        break;
    }
    case ND_APP_CALCULATOR: {
        nd_calc_t *c = &w->u.calc;
        char ch = (char)key;
        if (ch >= '0' && ch <= '9') {
            if (c->fresh) { c->display[0] = ch; c->display[1] = '\0'; c->fresh = 0; }
            else {
                int l = (int)strlen(c->display);
                if (l < 29) { c->display[l] = ch; c->display[l+1] = '\0'; }
            }
        } else if (ch == '+' || ch == '-' || ch == '*' || ch == '/') {
            c->acc = atof(c->display); c->op = ch; c->fresh = 1;
        } else if (ch == '=') {
            double v = atof(c->display);
            switch (c->op) {
            case '+': c->acc += v; break;
            case '-': c->acc -= v; break;
            case '*': c->acc *= v; break;
            case '/': if (v) c->acc /= v; break;
            }
            snprintf(c->display, sizeof(c->display), "%.2f", c->acc);
            c->fresh = 1;
        } else if (ch == 'C') {
            c->display[0] = '0'; c->display[1] = '\0';
            c->acc = 0; c->op = 0; c->fresh = 1;
        }
        break;
    }
    case ND_APP_EDITOR: {
        nd_editor_t *e = &w->u.edit;
        char ch = (char)key;
        if (key == K_ENTER && e->cy < ND_ELINES - 1) {
            /* Split line at cursor */
            char *s = e->lines[e->cy];
            int cur = (int)strlen(s);
            if (e->cx < cur) {
                char rest[ND_EWIDTH];
                strncpy(rest, s + e->cx, ND_EWIDTH - 1);
                rest[ND_EWIDTH - 1] = '\0';
                s[e->cx] = '\0';
                /* shift lines down */
                for (int i = e->nl; i > e->cy + 1; i--)
                    strncpy(e->lines[i], e->lines[i-1], ND_EWIDTH);
                e->nl++;
                strncpy(e->lines[e->cy + 1], rest, ND_EWIDTH - 1);
            } else {
                if (e->nl < ND_ELINES) {
                    e->lines[e->nl][0] = '\0';
                    e->nl++;
                }
            }
            e->cx = 0; e->cy++;
        } else if (key == K_BS) {
            if (e->cx > 0) {
                char *s = e->lines[e->cy];
                s[e->cx - 1] = '\0';
                e->cx--;
            } else if (e->cy > 0) {
                e->cx = (int)strlen(e->lines[e->cy - 1]);
                strncat(e->lines[e->cy - 1], e->lines[e->cy], ND_EWIDTH - e->cx - 1);
                for (int i = e->cy; i < e->nl - 1; i++)
                    strncpy(e->lines[i], e->lines[i+1], ND_EWIDTH);
                e->nl--;
                e->lines[e->nl][0] = '\0';
                e->cy--;
            }
        } else if (key == K_LEFT) {
            if (e->cx > 0) e->cx--;
            else if (e->cy > 0) { e->cy--; e->cx = (int)strlen(e->lines[e->cy]); }
        } else if (key == K_RIGHT) {
            int l = (int)strlen(e->lines[e->cy]);
            if (e->cx < l) e->cx++;
            else if (e->cy < e->nl - 1) { e->cy++; e->cx = 0; }
        } else if (key == K_HOME) {
            e->cx = 0;
        } else if (key == K_END) {
            e->cx = (int)strlen(e->lines[e->cy]);
        } else if (ch >= 32 && ch <= 126) {
            char *s = e->lines[e->cy];
            int l = (int)strlen(s);
            if (l < ND_EWIDTH - 1) {
                memmove(s + e->cx + 1, s + e->cx, l - e->cx + 1);
                s[e->cx] = ch;
                e->cx++;
            }
        }
        break;
    }
    case ND_APP_FILES: {
        nd_files_t *f = &w->u.files;
        if (key == K_UP) {
            if (f->sel > 0) f->sel--;
            if (f->sel < f->top) f->top = f->sel;
        } else if (key == K_DOWN) {
            if (f->sel < f->count - 1) f->sel++;
            int shown = w->h - 4;
            if (f->sel >= f->top + shown) f->top = f->sel - shown + 1;
        }
        break;
    }
    case ND_APP_ABOUT:
        break;
    default:
        break;
    }
}

void os_nadrino_key(os_state_t *os, term_key_t key, int mods)
{
    (void)mods;

    if (os->start_open) {
        /* Start menu open: ESC closes it */
        if (key == K_ESC) {
            os->start_open = 0;
        } else if (key >= K_F1 && key <= K_F4 && os->icon_sel >= 0) {
            /* F1-F4 in start menu = launch icons 0-3 */
            nd_app_t apps[] = { ND_APP_TERMINAL, ND_APP_CALCULATOR,
                               ND_APP_EDITOR, ND_APP_FILES };
            if (os->icon_sel < 4)
                open_app(os, apps[os->icon_sel], os->cols, os->rows);
            os->start_open = 0;
        }
        return;
    }

    /* Global shortcuts */
    if (key == K_F10) {
        os->start_open = 1;
        os->icon_sel = 0;
        return;
    }

    if (key == K_ESC) {
        /* Close focused window */
        if (os->focus >= 0 && os->focus < os->nwin)
            close_window(os, os->focus);
        return;
    }

    if (key == K_F4 && mods & K_ALT) {
        if (os->focus >= 0 && os->focus < os->nwin)
            close_window(os, os->focus);
        return;
    }

    if (key == K_UP) {
        /* Cycle focus upward */
        if (os->nwin == 0) return;
        if (os->focus < 0) os->focus = 0;
        else {
            os->focus--;
            if (os->focus < 0) os->focus = os->nwin - 1;
        }
        /* skip minimized */
        int tries = os->nwin;
        while (tries-- > 0 && os->win[os->focus].state == ND_WIN_MINIMIZED) {
            os->focus--;
            if (os->focus < 0) os->focus = os->nwin - 1;
        }
    } else if (key == K_DOWN) {
        if (os->nwin == 0) return;
        if (os->focus < 0) os->focus = 0;
        else {
            os->focus++;
            if (os->focus >= os->nwin) os->focus = 0;
        }
        int tries = os->nwin;
        while (tries-- > 0 && os->win[os->focus].state == ND_WIN_MINIMIZED) {
            os->focus++;
            if (os->focus >= os->nwin) os->focus = 0;
        }
    } else {
        /* Send to focused app */
        if (os->focus >= 0 && os->focus < os->nwin)
            handle_app_key(os, &os->win[os->focus], key, mods);
    }
}

void os_nadrino_text(os_state_t *os, const char *utf8)
{
    if (os->start_open) return;
    if (os->focus < 0 || os->focus >= os->nwin) return;
    nd_window_t *w = &os->win[os->focus];
    /* Only terminal accepts text */
    if (w->app != ND_APP_TERMINAL) return;
    nd_terminal_t *tm = &w->u.term;
    int len = (int)strlen(utf8);
    for (int i = 0; i < len && tm->ilen < ND_TWIDTH - 1; i++) {
        tm->input[tm->ilen++] = utf8[i];
    }
}

void os_nadrino_click(os_state_t *os, int cx, int cy)
{
    os->start_open = 0;
    os->icon_sel = -1;

    /* taskbar click */
    if (cy >= os->rows - ND_TASKBAR_H) {
        /* Start button */
        if (cx < 2) {
            os->start_open = !os->start_open;
            if (os->start_open) os->icon_sel = 0;
            return;
        }
        /* Window button */
        int bx = 2;
        for (int i = 0; i < os->nwin; i++) {
            nd_window_t *w = &os->win[i];
            if (w->state == ND_WIN_MINIMIZED) continue;
            char buf[32];
            int bl = snprintf(buf, sizeof(buf), " %s ", app_name(w->app));
            int bw = bl + 1;
            if (cx >= bx && cx < bx + bw) {
                if (w->state == ND_WIN_MINIMIZED) {
                    w->state = ND_WIN_NORMAL;
                    os->focus = i;
                } else if (os->focus == i) {
                    minimize(os, i);
                } else {
                    bring_top(os, i);
                }
                return;
            }
            bx += bw + 1;
        }
        return;
    }

    /* check if clicking on a window */
    int zi = top_z(os, cx, cy);
    if (zi >= 0) {
        bring_top(os, zi);
        nd_window_t *w = &os->win[os->nwin - 1];   /* bring_top moved it to top */
        if (cy == w->y + 1) {
            int rx = w->x + w->w - 1;              /* right border column */
            if (cx == rx - 2) { close_window(os, os->nwin - 1); return; }
            if (cx == rx - 5) { toggle_max(os, os->nwin - 1); return; }
            if (cx == rx - 8) { minimize(os, os->nwin - 1); return; }
            /* grab the title bar for dragging */
            w->dragging = 1;
            w->drag_dx = cx - w->x;
            w->drag_dy = cy - w->y;
        }
    } else {
        /* desktop click — check icons */
        struct { int col, row; nd_app_t app; } icon_pos[5];
        int cols = os->cols / 14;
        if (cols < 1) cols = 1;
        for (int i = 0; i < ND_ICON_COUNT; i++) {
            icon_pos[i].col = i % cols;
            icon_pos[i].row = i / cols;
            icon_pos[i].app = (nd_app_t)i;
            int ix = icon_pos[i].col * 14 + 1;
            int iy = icon_pos[i].row * 18 + 1;
            if (cx >= ix && cx < ix + 12 && cy >= iy && cy < iy + 14) {
                open_app(os, icon_pos[i].app, os->cols, os->rows);
                return;
            }
        }
        os->focus = -1;
    }
}

void os_nadrino_motion(os_state_t *os, int cx, int cy, int down)
{
    if (!down) {
        for (int i = 0; i < os->nwin; i++)
            os->win[i].dragging = 0;
        return;
    }
    for (int i = 0; i < os->nwin; i++) {
        if (os->win[i].dragging) {
            nd_window_t *w = &os->win[i];
            w->x = clamp(cx - w->drag_dx, 0, os->cols - w->w);
            w->y = clamp(cy - w->drag_dy, 0, os->rows - ND_TASKBAR_H - w->h);
        }
    }
}
