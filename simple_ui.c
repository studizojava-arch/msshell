/*
 * simple_ui.c — the $simpleinterface configuration screen.
 *
 * Everything is painted into a term_t grid and all user intent is reported back
 * to the host through su_take_action(), so this file needs no display, no pty
 * and no allocation. The font atlas only covers 0x20..0x7E, so every glyph here
 * is plain ASCII.
 *
 * Screen layout
 *   row 0            title
 *   row 1            summary of the current choices
 *   row 2            key hints
 *   row 3 .. dock-1  containers (left) | settings (right)
 *   dock rows        circular dock of app icons
 *   last row         LAUNCH bar — boots what the user configured
 */
#include "simple_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846

#define UI_TOP      3       /* rows consumed by title + summary + hints */
#define UI_DOCK_H   4       /* rows reserved for the circular dock      */
#define UI_LAUNCH_H 1       /* rows for the LAUNCH bar                  */

/* ------------------------------------------------------------------ palette */

typedef struct {
    uint32_t bg, panel, border, title, text, dim, sel, selfg, field,
             dock, dockfg, launch, launchfg;
} pal_t;

static const pal_t pal_classic = {
    RGBA(0x00,0x80,0x80,0xFF), RGBA(0xC0,0xC0,0xC0,0xFF),
    RGBA(0x80,0x80,0x80,0xFF), RGBA(0x00,0x00,0x80,0xFF),
    RGBA(0x00,0x00,0x00,0xFF), RGBA(0x40,0x40,0x40,0xFF),
    RGBA(0x00,0x00,0xFF,0xFF), RGBA(0xFF,0xFF,0xFF,0xFF),
    RGBA(0xFF,0xFF,0xFF,0xFF), RGBA(0xC0,0xC0,0xC0,0xFF),
    RGBA(0x00,0x00,0x00,0xFF), RGBA(0x00,0x80,0x00,0xFF),
    RGBA(0xFF,0xFF,0xFF,0xFF)
};

static const pal_t pal_aero = {
    RGBA(0x0B,0x1E,0x3C,0xFF), RGBA(0x1E,0x3A,0x5F,0xFF),
    RGBA(0x3F,0x6E,0x9C,0xFF), RGBA(0x1F,0x4F,0x7F,0xFF),
    RGBA(0xEA,0xF2,0xFA,0xFF), RGBA(0x9F,0xB8,0xD0,0xFF),
    RGBA(0x2E,0x7B,0xD6,0xFF), RGBA(0xFF,0xFF,0xFF,0xFF),
    RGBA(0xD8,0xEA,0xFA,0xFF), RGBA(0x14,0x2A,0x45,0xFF),
    RGBA(0xCF,0xE4,0xF7,0xFF), RGBA(0x2E,0x7B,0xD6,0xFF),
    RGBA(0xFF,0xFF,0xFF,0xFF)
};

static const char *const stab_name[SU_STAB_COUNT] = {
    "Basic", "Enhanced", "Stability x3"
};
static const char *const stab_note[SU_STAB_COUNT] = {
    "fastest, fewest guards",
    "guards + automatic retries",
    "x3 safety: retries, cache flush, strict dynarec"
};
static const char *const theme_name[SU_THEME_COUNT] = {
    "Classic 95", "Aero Win7"
};
static const char *const os_name[SU_OS_COUNT] = {
    "Nadrino OS", "GL terminal"
};

static pal_t pal_of(const su_state_t *su)
{
    return su->theme == SU_THEME_AERO ? pal_aero : pal_classic;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ------------------------------------------------------------------ cells */

static void cell(term_t *t, int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    const term_cell_t *c;
    term_cell_t *m;

    if (x < 0 || y < 0 || x >= t->cols || y >= t->rows) return;
    c = term_cell(t, x, y, 0);
    if (!c) return;
    m = (term_cell_t *)c;
    m->cp    = (uint32_t)(unsigned char)ch;
    m->fg    = fg;
    m->bg    = bg;
    m->style = 0;
}

static void fill(term_t *t, int x0, int y0, int x1, int y1,
                 char ch, uint32_t fg, uint32_t bg)
{
    int x, y;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            cell(t, x, y, ch, fg, bg);
}

static void text_at(term_t *t, int x, int y, int w,
                    const char *s, uint32_t fg, uint32_t bg)
{
    int len = (int)strlen(s);
    int i;
    for (i = 0; i < w; i++)
        cell(t, x + i, y, i < len ? s[i] : ' ', fg, bg);
}

static void text_mid(term_t *t, int x0, int y, int w,
                     const char *s, uint32_t fg, uint32_t bg)
{
    int len = (int)strlen(s);
    int start = x0 + (w - len) / 2;
    int i;
    fill(t, x0, y, x0 + w, y + 1, ' ', fg, bg);
    for (i = 0; i < len; i++)
        cell(t, start + i, y, s[i], fg, bg);
}

/* ------------------------------------------------------------------ geometry */

static int dock_top(const su_state_t *su)
{
    return su->rows - UI_DOCK_H - UI_LAUNCH_H;
}

void su_launch_rect(const su_state_t *su, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = su->cols / 5;
    *y0 = su->rows - UI_LAUNCH_H;
    *x1 = su->cols - su->cols / 5;
    *y1 = su->rows;
}

/* Centre of the circle the dock icons are laid around. */
static void dock_circle(const su_state_t *su, int *cx, int *cy, int *rx, int *ry)
{
    int n = su->nicons > 0 ? su->nicons : 1;

    *cx = su->cols / 2;
    *cy = su->rows - UI_LAUNCH_H + 2;   /* arc bows upward from below the bar */
    *rx = clampi(n * 4, 8, su->cols / 2 - 4);
    *ry = 3;
}

void su_dock_slot(const su_state_t *su, int i, int *ox, int *oy)
{
    int cx, cy, rx, ry;
    int n = su->nicons > 0 ? su->nicons : 1;
    double a;

    dock_circle(su, &cx, &cy, &rx, &ry);
    a = PI - PI * (double)(i + 1) / (double)(n + 1);
    *ox = cx + (int)floor(rx * cos(a) + 0.5);
    *oy = cy - (int)floor(ry * sin(a) + 0.5);
    *ox = clampi(*ox, 2, su->cols - 3);
    *oy = clampi(*oy, dock_top(su), su->rows - UI_LAUNCH_H - 1);
}

/* ------------------------------------------------------------------ lifecycle */

void su_init(su_state_t *su, int cols, int rows)
{
    static const struct { const char *n; char g; int app; } dflt[] = {
        { "Term",  'T',  0 }, { "Calc", 'C', 1 },
        { "Edit",  'E',  2 }, { "Files", 'F', 3 },
        { "About", '?',  4 }, { "Hide",  'H', -1 }
    };
    static const char *cn[] = { "Work", "Games", "Dev" };
    int i;

    memset(su, 0, sizeof(*su));
    su->running      = 1;
    su->cols         = cols;
    su->rows         = rows;
    su->theme        = SU_THEME_AERO;
    su->stab         = SU_STAB_X3;
    su->os_sel       = SU_OS_NADRINO;
    su->zone         = SU_ZONE_CONTAINERS;
    su->setting      = SU_SETTING_OS;
    su->sel          = 0;
    su->open         = -1;
    su->renaming     = -1;
    su->dock_focus   = -1;

    su->nicons = (int)(sizeof(dflt) / sizeof(dflt[0]));
    for (i = 0; i < su->nicons; i++) {
        su->icons[i].name  = dflt[i].n;
        su->icons[i].glyph = dflt[i].g;
        su->icons[i].app   = dflt[i].app;
    }

    su->ncont = (int)(sizeof(cn) / sizeof(cn[0]));
    for (i = 0; i < su->ncont; i++) {
        strncpy(su->cont[i].name, cn[i], SU_NAME_LEN - 1);
        su->cont[i].name[SU_NAME_LEN - 1] = '\0';
        su->cont[i].theme = su->theme;
        su->cont[i].stab  = su->stab;
    }
}

void su_free(su_state_t *su) { (void)su; }

int su_running(const su_state_t *su) { return su->running; }

void su_resize(su_state_t *su, int cols, int rows)
{
    su->cols = cols;
    su->rows = rows;
}

void su_tick(su_state_t *su)
{
    su->tick++;
    su->cursor_on = ((su->tick / 15) & 1) != 0;
}

/* ------------------------------------------------------------------ render */

static void draw_head(const su_state_t *su, term_t *t, const pal_t *p)
{
    char buf[256];

    text_at(t, 0, 0, su->cols, " Simple Interface", p->text, p->title);

    snprintf(buf, sizeof(buf), " Boot: %s   Look: %s   box64: %s   Boxes: %d",
             os_name[su->os_sel], theme_name[su->theme],
             stab_name[su->stab], su->ncont);
    text_at(t, 0, 1, su->cols, buf, p->text, p->panel);

    text_at(t, 0, 2, su->cols,
            " Tab zone  Up/Down pick  Left/Right change  Enter open  "
            "N new  R rename  D delete  L launch  Esc exit",
            p->dim, p->panel);
}

static void draw_boxes(const su_state_t *su, term_t *t, const pal_t *p)
{
    int x0 = 1, y0 = UI_TOP + 1;
    int w  = clampi(su->cols / 3, 12, su->cols - 6);
    int y1 = clampi(dock_top(su) - 1, y0 + 1, su->rows);
    int rows_avail = y1 - y0 - 1;
    int i;
    char buf[80];

    if (w < 8 || rows_avail < 1) return;

    fill(t, x0, y0, x0 + w, y1, ' ', p->text, p->panel);
    snprintf(buf, sizeof(buf), " Containers %d/%d", su->ncont, SU_MAX_CONTAINERS);
    text_at(t, x0 + 1, y0, w - 1, buf, p->text, p->border);

    for (i = 0; i < su->ncont && i < rows_avail; i++) {
        int y   = y0 + 1 + i;
        int on  = (su->zone == SU_ZONE_CONTAINERS && i == su->sel);
        char nm[SU_NAME_LEN + 4];

        if (su->renaming == i)
            snprintf(nm, sizeof(nm), "%s%c", su->rbuf, su->cursor_on ? '|' : ' ');
        else
            snprintf(nm, sizeof(nm), "%s", su->cont[i].name);

        snprintf(buf, sizeof(buf), " %c %-22s",
                 i == su->open ? '>' : (on ? '*' : ' '), nm);
        text_at(t, x0 + 1, y, w - 1, buf,
                on ? p->selfg : p->text, on ? p->sel : p->panel);
    }

    if (su->ncont < SU_MAX_CONTAINERS && su->ncont < rows_avail) {
        snprintf(buf, sizeof(buf), "   (N) new container");
        text_at(t, x0 + 1, y0 + 1 + su->ncont, w - 1, buf, p->dim, p->panel);
    }
}

static void draw_field(const su_state_t *su, term_t *t, const pal_t *p,
                       int x0, int w, int y, int row,
                       const char *label, const char *value)
{
    char buf[128];
    int on = (su->zone == SU_ZONE_SETTINGS && su->setting == row);

    text_at(t, x0 + 1, y, w - 1, label, p->text, p->bg);
    snprintf(buf, sizeof(buf), "   [ %s ]%s", value, on ? "  <-/->" : "");
    text_at(t, x0 + 1, y + 1, w - 1, buf,
            on ? p->selfg : p->text, on ? p->sel : p->field);
}

static void draw_settings(const su_state_t *su, term_t *t, const pal_t *p)
{
    int x0 = clampi(su->cols / 3, 12, su->cols - 6) + 2;
    int w  = su->cols - x0 - 1;
    int y  = UI_TOP + 1;
    int y1 = clampi(dock_top(su) - 1, y + 1, su->rows);
    char buf[128];

    if (w < 14 || y1 - y < 6) return;

    fill(t, x0, y, x0 + w, y1, ' ', p->text, p->bg);
    snprintf(buf, sizeof(buf), " Settings");
    text_at(t, x0 + 1, y, w - 1, buf, p->text, p->border);
    y += 2;

    draw_field(su, t, p, x0, w, y, SU_SETTING_OS,
               " Operating system", os_name[su->os_sel]);
    y += 2;
    draw_field(su, t, p, x0, w, y, SU_SETTING_THEME,
               " Interface style", theme_name[su->theme]);
    y += 2;
    draw_field(su, t, p, x0, w, y, SU_SETTING_STAB,
               " box64 stability", stab_name[su->stab]);
    y += 2;

    snprintf(buf, sizeof(buf), "   %s", stab_note[su->stab]);
    text_at(t, x0 + 1, y, w - 1, buf, p->dim, p->bg);
    y += 2;

    if (su->open >= 0 && su->open < su->ncont) {
        snprintf(buf, sizeof(buf), " Open box: %s", su->cont[su->open].name);
        text_at(t, x0 + 1, y, w - 1, buf, p->text, p->bg);
    }
}

static void draw_dock(const su_state_t *su, term_t *t, const pal_t *p)
{
    int i;
    char buf[40];

    for (i = 0; i < su->nicons; i++) {
        int ox, oy;
        int hot = (su->dock_focus == i);

        su_dock_slot(su, i, &ox, &oy);

        /* a four-cell ring reads as a round icon on a coarse grid */
        cell(t, ox,     oy - 1, '_', p->dockfg, p->dock);
        cell(t, ox - 1, oy,     '(', p->dockfg, p->dock);
        cell(t, ox + 1, oy,     ')', p->dockfg, p->dock);
        cell(t, ox,     oy + 1, '-', p->dockfg, p->dock);
        cell(t, ox,     oy,     su->icons[i].glyph,
             hot ? p->dock : p->dockfg, hot ? p->dockfg : p->dock);

        if (hot) {
            snprintf(buf, sizeof(buf), "[%s]", su->icons[i].name);
            text_mid(t, ox - 5, oy + 2, 11, buf, p->text, p->panel);
        }
    }
}

static void draw_launch(const su_state_t *su, term_t *t, const pal_t *p)
{
    int x0, y0, x1, y1;
    char buf[128];

    su_launch_rect(su, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y0 >= su->rows) return;

    snprintf(buf, sizeof(buf), " LAUNCH  >  %s  /  %s  /  box64 %s ",
             os_name[su->os_sel], theme_name[su->theme], stab_name[su->stab]);

    if (su->zone == SU_ZONE_LAUNCH)
        text_mid(t, x0, y0, x1 - x0, buf, p->selfg, p->sel);
    else
        text_mid(t, x0, y0, x1 - x0, buf, p->launchfg, p->launch);
}

void su_render(const su_state_t *su, term_t *t)
{
    const pal_t *p;
    int i;
    pal_t local;

    if (su->cols <= 0 || su->rows <= 0) return;
    local = pal_of(su);
    p = &local;

    fill(t, 0, 0, su->cols, su->rows, ' ', p->text, p->bg);

    draw_head(su, t, p);
    draw_boxes(su, t, p);
    draw_settings(su, t, p);
    draw_dock(su, t, p);
    draw_launch(su, t, p);

    for (i = 0; i < su->cols; i++)
        cell(t, i, dock_top(su) - 1, '=', p->border, p->border);
}

/* ------------------------------------------------------------------ edits */

static void rename_begin(su_state_t *su, int i)
{
    if (i < 0 || i >= su->ncont) return;
    su->renaming = i;
    strncpy(su->rbuf, su->cont[i].name, SU_NAME_LEN - 1);
    su->rbuf[SU_NAME_LEN - 1] = '\0';
    su->rlen = (int)strlen(su->rbuf);
}

static void rename_commit(su_state_t *su)
{
    if (su->renaming < 0 || su->renaming >= su->ncont) { su->renaming = -1; return; }
    su->rbuf[su->rlen] = '\0';
    if (su->rlen > 0) {
        strncpy(su->cont[su->renaming].name, su->rbuf, SU_NAME_LEN - 1);
        su->cont[su->renaming].name[SU_NAME_LEN - 1] = '\0';
        /* the box keeps whatever the user had chosen while naming it */
        su->cont[su->renaming].theme = su->theme;
        su->cont[su->renaming].stab  = su->stab;
    }
    su->renaming = -1;
    su->rlen = 0;
}

static void box_add(su_state_t *su)
{
    if (su->ncont >= SU_MAX_CONTAINERS) return;
    snprintf(su->cont[su->ncont].name, SU_NAME_LEN, "Box %d", su->ncont + 1);
    su->cont[su->ncont].nicons = 0;
    su->cont[su->ncont].theme  = su->theme;
    su->cont[su->ncont].stab   = su->stab;
    su->sel = su->ncont;
    su->ncont++;
    rename_begin(su, su->sel);
}

static void box_del(su_state_t *su, int i)
{
    int k;
    if (i < 0 || i >= su->ncont) return;
    for (k = i; k < su->ncont - 1; k++) su->cont[k] = su->cont[k + 1];
    su->ncont--;
    memset(&su->cont[su->ncont], 0, sizeof(su->cont[0]));
    su->renaming = -1;
    su->rlen = 0;
    if (su->sel >= su->ncont) su->sel = su->ncont - 1;
    if (su->sel < 0) su->sel = 0;
    if (su->open >= su->ncont) su->open = -1;
}

static void setting_cycle(su_state_t *su, int dir)
{
    switch (su->setting) {
    case SU_SETTING_OS:
        su->os_sel = (su->os_sel + dir + SU_OS_COUNT) % SU_OS_COUNT;
        break;
    case SU_SETTING_THEME:
        su->theme = (su->theme + dir + SU_THEME_COUNT) % SU_THEME_COUNT;
        break;
    case SU_SETTING_STAB:
        su->stab = (su->stab + dir + SU_STAB_COUNT) % SU_STAB_COUNT;
        break;
    default:
        break;
    }
    if (su->open >= 0 && su->open < su->ncont) {
        su->cont[su->open].theme = su->theme;
        su->cont[su->open].stab  = su->stab;
    }
}

/* ------------------------------------------------------------------ input */

void su_key(su_state_t *su, term_key_t key, int mods)
{
    (void)mods;

    if (su->renaming >= 0) {
        if (key == K_ENTER)      rename_commit(su);
        else if (key == K_ESC)   { su->renaming = -1; su->rlen = 0; }
        else if (key == K_BS)    { if (su->rlen > 0) su->rlen--; }
        else if (key == K_DEL)   { su->rlen = 0; }
        return;
    }

    if (key == K_ESC) { su->action = SU_ACT_EXIT; return; }

    switch (su->zone) {
    case SU_ZONE_CONTAINERS:
        if (key == K_UP   && su->sel > 0) su->sel--;
        if (key == K_DOWN && su->sel < su->ncont - 1) su->sel++;
        if (key == K_RIGHT) su->zone = SU_ZONE_SETTINGS;
        if (key == K_ENTER) {
            su->open = (su->open == su->sel) ? -1 : su->sel;
        }
        break;

    case SU_ZONE_SETTINGS:
        if (key == K_UP   && su->setting > 0) su->setting--;
        if (key == K_DOWN && su->setting < SU_SETTINGS - 1) su->setting++;
        if (key == K_LEFT)  setting_cycle(su, -1);
        if (key == K_RIGHT) setting_cycle(su,  1);
        if (key == K_ENTER) setting_cycle(su,  1);
        if (key == K_DOWN && su->setting == SU_SETTINGS - 1)
            su->zone = SU_ZONE_LAUNCH;
        if (key == K_RIGHT && su->setting == SU_SETTINGS - 1)
            su->zone = SU_ZONE_LAUNCH;
        break;

    case SU_ZONE_LAUNCH:
        if (key == K_ENTER) su->action = SU_ACT_LAUNCH_OS;
        if (key == K_LEFT)  su->zone = SU_ZONE_SETTINGS;
        break;

    default:
        break;
    }

    if (key == K_TAB) {
        su->zone = (su->zone + 1) % SU_ZONE_COUNT;
    }
}

void su_text(su_state_t *su, const char *utf8)
{
    size_t len = strlen(utf8);
    size_t i;

    if (su->renaming >= 0) {
        for (i = 0; i < len && su->rlen < SU_NAME_LEN - 1; i++) {
            char c = utf8[i];
            if (c >= 32 && c < 127) su->rbuf[su->rlen++] = c;
        }
        su->rbuf[su->rlen] = '\0';
        return;
    }

    for (i = 0; i < len; i++) {
        char c = utf8[i];
        if (c == 'n' || c == 'N') { box_add(su); }
        else if (c == 'r' || c == 'R') {
            int t = (su->zone == SU_ZONE_CONTAINERS) ? su->sel : su->open;
            rename_begin(su, t >= 0 ? t : su->sel);
        }
        else if (c == 'd' || c == 'D') box_del(su, su->sel);
        else if (c == 'l' || c == 'L' || c == ' ') su->action = SU_ACT_LAUNCH_OS;
        else if (c == 't' || c == 'T') { su->setting = SU_SETTING_THEME; setting_cycle(su, 1); }
        else if (c == 's' || c == 'S') { su->setting = SU_SETTING_STAB;  setting_cycle(su, 1); }
    }
}

void su_click(su_state_t *su, int cx, int cy)
{
    int x0, y0, x1, y1;

    if (cx < 0 || cy < 0 || cx >= su->cols || cy >= su->rows) return;

    /* LAUNCH bar first: it is the primary control. */
    su_launch_rect(su, &x0, &y0, &x1, &y1);
    if (cy >= y0 && cy < y1 && cx >= x0 && cx < x1) {
        su->zone = SU_ZONE_LAUNCH;
        su->action = SU_ACT_LAUNCH_OS;
        return;
    }

    /* dock icons */
    if (cy >= dock_top(su) && cy < y0) {
        int i;
        for (i = 0; i < su->nicons; i++) {
            int ox, oy;
            su_dock_slot(su, i, &ox, &oy);
            if (cx >= ox - 1 && cx <= ox + 1 && cy >= oy - 1 && cy <= oy + 2) {
                su->dock_focus = i;
                if (su->icons[i].app >= 0) {
                    su->action = SU_ACT_LAUNCH_APP;
                    su->action_arg = su->icons[i].app;
                } else {
                    su->zone = SU_ZONE_SETTINGS;
                }
                return;
            }
        }
        return;
    }

    if (cy < UI_TOP) return;

    /* container rows */
    {
        int left_w = clampi(su->cols / 3, 12, su->cols - 6);
        int row = cy - (UI_TOP + 2);
        if (cx <= left_w + 1) {
            if (row >= 0 && row < su->ncont) {
                if (su->renaming >= 0) rename_commit(su);
                su->zone = SU_ZONE_CONTAINERS;
                su->sel  = row;
                su->open = row;
            }
            return;
        }
        /* settings fields: three two-row blocks starting after the header */
        if (cx > left_w + 2) {
            int rel = cy - (UI_TOP + 3);
            if (rel >= 0 && rel < SU_SETTINGS * 2) {
                su->zone    = SU_ZONE_SETTINGS;
                su->setting = rel / 2;
                if (su->setting >= SU_SETTINGS) su->setting = SU_SETTINGS - 1;
            }
            return;
        }
    }
}

void su_motion(su_state_t *su, int cx, int cy, int down)
{
    int i;

    if (!down) return;
    if (cy < dock_top(su) || cy >= su->rows - UI_LAUNCH_H) return;

    for (i = 0; i < su->nicons; i++) {
        int ox, oy;
        su_dock_slot(su, i, &ox, &oy);
        if (cx >= ox - 1 && cx <= ox + 1 && cy >= oy - 1 && cy <= oy + 2) {
            su->dock_focus = i;
            return;
        }
    }
}

/* ------------------------------------------------------------------ handshake */

su_action_t su_take_action(su_state_t *su, int *arg)
{
    su_action_t a = su->action;
    if (arg) *arg = su->action_arg;
    su->action = SU_ACT_NONE;
    su->action_arg = 0;
    if (a == SU_ACT_EXIT) su->running = 0;
    return a;
}

int su_chosen_theme(const su_state_t *su) { return su->theme; }
int su_chosen_stab(const su_state_t *su)  { return su->stab; }
int su_chosen_os(const su_state_t *su)
{
    return su->os_sel < SU_OS_COUNT ? su->os_sel : SU_OS_NADRINO;
}
