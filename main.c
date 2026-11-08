/*
 * main.c — mshell: no-root Termux terminal with on-demand GL window.
 *
 * Two modes, one binary:
 *
 *   mshell              launcher. Runs a shell whose pty we sit on, mirroring
 *                       everything to the real terminal.  When the user types
 *                       !experiment we fork a copy with --gl (GL terminal).
 *                       When the user types !adaptivenadrino we fork a copy
 *                       with --os (Nadrino OS desktop).
 *
 *   mshell --gl         OpenGL ES 2.0 terminal: SDL2 window + GLES2 + pty'd shell
 *   mshell --os         OpenGL ES 2.0 desktop: SDL2 window + GLES2 + Nadrino OS
 *
 * The launcher never needs a display. Only --gl and --os need SDL's video driver.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>

#include <pty.h>

#ifdef MSHELL_USE_SDL
#include <SDL2/SDL.h>
#include "gl.h"
#include "os_nadrino.h"
#endif

#include "term.h"

#define READ_CHUNK    8192
#define FRAME_MS      16          /* ~60 fps */
#define CURSOR_PERIOD 530         /* ms per blink half-cycle */

/* ===== command triggers ===== */

#define BANG_GL       "!experiment"
#define BANG_OS       "!adaptivenadrino"

#define FIRE_NONE     0
#define FIRE_GL       1
#define FIRE_OS       2

/* ===== bang_scan: detects trigger words in echoed shell output ===== */

typedef struct {
    char tok[32];
    int  len;
    int  active;
    int  cmd_ok;
    int  quote;
} bang_t;

static void bang_init(bang_t *b) { memset(b, 0, sizeof(*b)); b->cmd_ok = 1; }

static int bang_match(const char *tok)
{
    if (!strcmp(tok, BANG_GL)) return FIRE_GL;
    if (!strcmp(tok, BANG_OS)) return FIRE_OS;
    return FIRE_NONE;
}

static int bang_scan(bang_t *b, const uint8_t *d, int n)
{
    int fired = FIRE_NONE, i;
    for (i = 0; i < n; i++) {
        char c = (char)d[i];
        /* boundary ends token */
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t' ||
            c == ';' || c == '|' || c == '&') {
            if (b->active && !b->quote) fired = bang_match(b->tok);
            b->active = 0; b->len = 0; b->tok[0] = '\0';
            if (c == '\n' || c == '\r') b->quote = 0;
            b->cmd_ok = 1;
            continue;
        }
        if (c == '\'' || c == '"' || c == '\\') {
            b->quote = 1; b->active = 0; b->len = 0; b->tok[0] = '\0';
            b->cmd_ok = 0; continue;
        }
        if (!b->active && !b->cmd_ok &&
            (c == '$' || c == '#' || c == '>' || c == '%'))
            continue;
        if (!b->active) {
            if (!b->cmd_ok) continue;
            b->active = 1; b->len = 0; b->tok[0] = '\0';
        }
        if (b->len < (int)sizeof(b->tok) - 1) b->tok[b->len++] = c;
        b->tok[b->len] = '\0';
        b->cmd_ok = 0;
    }
    return fired;
}

/* ===== helpers ===== */

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void pty_set_size(int fd, int cols, int rows)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    if (fd >= 0 && cols > 0 && rows > 0) ioctl(fd, TIOCSWINSZ, &ws);
}

static void set_shell_defaults(void)
{
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("LANG", "en_US.UTF-8", 0);
}

static const char *self_path(const char *argv0)
{
    static char buf[512];
    if (argv0 && *argv0 && strchr(argv0, '/')) return argv0;
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return buf; }
    return (argv0 && *argv0) ? argv0 : "mshell";
}

static int write_all(int fd, const char *s, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w > 0) { s += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* ===== pty spawn ===== */

static int spawn_shell(pid_t *out_pid, int cols, int rows)
{
    const char *env;
    char **argv;
    struct winsize ws;
    int master = -1, n = 0, i = 0;
    pid_t pid;

    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    set_shell_defaults();

    env = getenv("MSHELL_SHELL");
    if (!env || !*env) env = getenv("SHELL");

    /* candidates */
    const char *candidates[] = {
        "/data/data/com.termux/files/usr/bin/bash",
        "/data/data/com.termux/files/usr/bin/sh",
        "/data/data/com.termux/files/usr/bin/zsh",
        "/bin/bash", "/bin/sh", "/system/bin/sh", NULL
    };
    while (candidates[n]) n++;
    argv = calloc((size_t)n + 2, sizeof(char *));
    if (!argv) return -1;
    if (env && *env) argv[i++] = (char *)env;
    for (n = 0; candidates[n]; n++) argv[i + n] = (char *)candidates[n];

    pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) { free(argv); return -1; }

    if (pid == 0) {
        for (int k = 0; argv[k]; k++) {
            execlp(argv[k], argv[k], (char *)NULL);
            if (errno != ENOENT && errno != ENOEXEC && errno != EACCES) break;
        }
        fprintf(stderr, "mshell: cannot start a shell: %s\r\n", strerror(errno));
        _exit(127);
    }
    free(argv);
    *out_pid = pid;
    set_nonblock(master);
    return master;
}

/* ===== raw tty ===== */

static struct termios g_saved_tio;
static int g_raw_on = 0;

static void tty_raw(void)
{
    struct termios t;
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &g_saved_tio) != 0) return;
    t = g_saved_tio;
    t.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= (tcflag_t)~OPOST;
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == 0) g_raw_on = 1;
}

static void tty_restore(void)
{
    if (g_raw_on) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tio); g_raw_on = 0; }
}

/* ===== launcher ===== */

static void launcher_banner(void)
{
    const char *msg =
        "\x1b[1;36mmshell\x1b[0m \x1b[2m(no-root launcher)\x1b[0m\r\n"
        "  \x1b[33m!experiment\x1b[0m      open OpenGL ES terminal window\r\n"
        "  \x1b[33m!adaptivenadrino\x1b[0m   open Nadrino OS desktop\r\n"
        "  \x1b[32mready\x1b[0m\r\n\r\n";
    write_all(STDOUT_FILENO, msg, strlen(msg));
}

static int run_launcher(const char *self)
{
    bang_t bang;
    uint8_t buf[READ_CHUNK];
    pid_t shell = -1, child_pid = -1, child_type = 0;
    int master;
    int running = 1;
    struct winsize ws;

    master = spawn_shell(&shell, 80, 24);
    if (master < 0) {
        fprintf(stderr, "mshell: forkpty failed: %s\n", strerror(errno));
        return 1;
    }

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
        pty_set_size(master, ws.ws_col, ws.ws_row);

    bang_init(&bang);
    tty_raw();
    launcher_banner();

    while (running) {
        fd_set rfds;
        struct timeval tv;
        int ready;

        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(master, &rfds);
        tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
        ready = select(master + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) { if (errno == EINTR) continue; break; }

        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
            pty_set_size(master, ws.ws_col, ws.ws_row);

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r > 0) {
                if (write_all(master, (const char *)buf, (size_t)r) != 0) break;
            } else if (r == 0) break;
            else if (errno != EAGAIN && errno != EINTR) break;
        }

        if (FD_ISSET(master, &rfds)) {
            ssize_t r = read(master, buf, sizeof(buf));
            if (r > 0) {
                write_all(STDOUT_FILENO, (const char *)buf, (size_t)r);
                if (!child_pid && child_type == 0) {
                    int fire = bang_scan(&bang, buf, (int)r);
                    if (fire == FIRE_GL || fire == FIRE_OS) {
                        const char *mode = fire == FIRE_GL ? "--gl" : "--os";
                        pid_t pid = fork();
                        if (pid == 0) {
                            execl(self, self, mode, (char *)NULL);
                            _exit(127);
                        }
                        if (pid > 0) {
                            child_pid = pid;
                            child_type = fire;
                        }
                    }
                }
            } else if (r == 0 || errno == EIO) {
                running = 0;
            } else if (errno != EAGAIN && errno != EINTR) {
                running = 0;
            }
        }

        /* Reap finished child */
        if (child_pid > 0) {
            pid_t done = waitpid(child_pid, NULL, WNOHANG);
            if (done == child_pid) {
                child_pid = 0;
                child_type = 0;
                bang_init(&bang);
            }
        }
    }

    if (child_pid > 0) kill(child_pid, SIGHUP);
    kill(shell, SIGHUP);
    waitpid(shell, NULL, WNOHANG);
    close(master);
    tty_restore();
    return 0;
}

/* =========================================================================== */
/*                          OpenGL ES 2.0 terminal window                      */
/* =========================================================================== */

#ifdef MSHELL_USE_SDL

static void boot_splash(term_t *t, const char *shell_path)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "\x1b[2J\x1b[H"
        "\x1b[1;36m  mshell\x1b[0m \x1b[2mOpenGL ES terminal\x1b[0m\r\n"
        "\x1b[2m  %s\x1b[0m\r\n\r\n"
        "\x1b[33mPgUp/PgDn\x1b[0m scrollback    "
        "\x1b[33mCtrl-C\x1b[0m interrupt    "
        "\x1b[33mCtrl-D\x1b[0m exit\r\n\r\n",
        (shell_path && *shell_path) ? shell_path : "shell");
    if (n > 0) term_feed(t, (const uint8_t *)buf, n);
}

static int mods_from_sdl(SDL_Keymod km)
{
    int m = 0;
    if (km & (KMOD_LSHIFT | KMOD_RSHIFT)) m |= K_SHIFT;
    if (km & (KMOD_LCTRL  | KMOD_RCTRL))  m |= K_CTRL;
    if (km & (KMOD_LALT   | KMOD_RALT))   m |= K_ALT;
    return m;
}

static term_key_t key_from_sdl(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_UP:         return K_UP;
    case SDLK_DOWN:       return K_DOWN;
    case SDLK_RIGHT:      return K_RIGHT;
    case SDLK_LEFT:       return K_LEFT;
    case SDLK_HOME:       return K_HOME;
    case SDLK_END:        return K_END;
    case SDLK_PAGEUP:     return K_PGUP;
    case SDLK_PAGEDOWN:   return K_PGDN;
    case SDLK_INSERT:     return K_INS;
    case SDLK_DELETE:     return K_DEL;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:   return K_ENTER;
    case SDLK_TAB:        return K_TAB;
    case SDLK_BACKSPACE:  return K_BS;
    case SDLK_ESCAPE:     return K_ESC;
    case SDLK_F1:         return K_F1;
    case SDLK_F2:         return K_F2;
    case SDLK_F3:         return K_F3;
    case SDLK_F4:         return K_F4;
    case SDLK_F5:         return K_F5;
    case SDLK_F6:         return K_F6;
    case SDLK_F7:         return K_F7;
    case SDLK_F8:         return K_F8;
    case SDLK_F9:         return K_F9;
    case SDLK_F10:        return K_F10;
    case SDLK_F11:        return K_F11;
    case SDLK_F12:        return K_F12;
    default:              return K_NONE;
    }
}

static int ctrl_byte(SDL_Keycode sym, int *out)
{
    if (sym >= SDLK_a && sym <= SDLK_z) { *out = (int)(sym - SDLK_a) + 1; return 1; }
    if (sym >= SDLK_A && sym <= SDLK_Z) { *out = (int)(sym - SDLK_A) + 1; return 1; }
    switch (sym) {
    case SDLK_LEFTBRACKET:  *out = 27; return 1;
    case SDLK_RIGHTBRACKET: *out = 28; return 1;
    case SDLK_BACKSLASH:    *out = 28; return 1;
    case SDLK_6:            *out = 30; return 1;
    case SDLK_MINUS:        *out = 31; return 1;
    case SDLK_SLASH:        *out = 31; return 1;
    default: break;
    }
    return 0;
}

static int drain_pty(term_t *t, uint8_t *scratch, int cap)
{
    int total = 0;
    for (;;) {
        ssize_t r = read(t->fd, scratch, (size_t)cap);
        if (r > 0) { term_feed(t, scratch, (int)r); total += (int)r; if ((int)r < cap) break; continue; }
        if (r == 0) return -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        if (errno == EIO) return -1;
        break;
    }
    return total;
}

/* --- GL terminal (pty shell) --- */

static int run_gl_terminal(void)
{
    SDL_Window *win = NULL;
    SDL_GLContext glctx = NULL;
    gl_renderer_t gl;
    term_t term;
    uint8_t *scratch = NULL;
    pid_t child = -1;
    int master = -1;
    int cols = 80, rows = 24, dw = 960, dh = 640;
    int scroll = 0;
    int running = 1, alive = 1, force_redraw = 1;
    Uint32 blink_t0 = 0, cursor_on = 1, dead_at = 0;
    char win_title[192] = "mshell";

    signal(SIGPIPE, SIG_IGN);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n  No display? Install Termux:X11 and start it.\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    win = SDL_CreateWindow("mshell", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           dw, dh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { SDL_Quit(); return 1; }

    glctx = SDL_GL_CreateContext(win);
    if (!glctx) { SDL_DestroyWindow(win); SDL_Quit(); return 1; }
    SDL_GL_SetSwapInterval(1);
    SDL_GL_GetDrawableSize(win, &dw, &dh);

    if (gl_init(&gl, dw, dh) != 0) goto cleanup_ctx;

    cols = dw / CELL_W; rows = dh / CELL_H;
    if (cols < 20) cols = 20; if (rows < 6) rows = 6;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;

    if (term_init(&term, cols, rows) != 0) { gl_destroy(&gl); goto cleanup_ctx; }

    scratch = malloc(READ_CHUNK);
    if (!scratch) { term_free(&term); gl_destroy(&gl); goto cleanup_ctx; }

    master = spawn_shell(&child, cols, rows);
    if (master < 0) {
        boot_splash(&term, "no shell available");
        alive = 0;
    } else {
        term.fd = master;
        pty_set_size(master, cols, rows);
        boot_splash(&term, getenv("SHELL"));
    }

    blink_t0 = (Uint32)SDL_GetTicks();

    while (running) {
        SDL_Event ev;
        int activity = force_redraw, ready = 0;

        if (term.fd >= 0) {
            fd_set rfds; struct timeval tv;
            FD_ZERO(&rfds); FD_SET(term.fd, &rfds);
            tv.tv_sec = 0; tv.tv_usec = FRAME_MS * 1000;
            ready = select(term.fd + 1, &rfds, NULL, NULL, &tv);
            if (ready < 0 && errno != EINTR) ready = 0;
        } else { SDL_Delay(FRAME_MS); }

        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    SDL_GL_GetDrawableSize(win, &dw, &dh);
                    if (dw < CELL_W * 4) dw = CELL_W * 4;
                    if (dh < CELL_H * 4) dh = CELL_H * 4;
                    gl_resize(&gl, dw, dh);
                    cols = dw / CELL_W; rows = dh / CELL_H;
                    if (cols < 20) cols = 20; if (rows < 6) rows = 6;
                    if (cols != term.cols || rows != term.rows) {
                        term_resize(&term, cols, rows);
                        pty_set_size(term.fd, cols, rows);
                        scroll = 0;
                    }
                    activity = 1;
                }
                break;
            case SDL_TEXTINPUT:
                scroll = 0; term_send_text(&term, ev.text.text, (int)strlen(ev.text.text));
                activity = 1; break;
            case SDL_KEYDOWN: {
                int m = mods_from_sdl(ev.key.keysym.mod);
                term_key_t k = key_from_sdl(ev.key.keysym.sym);
                int cb;
                if (k == K_PGUP) {
                    int maxs = term_max_scroll(&term);
                    scroll += rows > 1 ? rows / 2 : 1;
                    if (scroll > maxs) scroll = maxs;
                    blink_t0 = (Uint32)SDL_GetTicks(); cursor_on = 0; activity = 1;
                    break;
                }
                if (k == K_PGDN && scroll > 0) {
                    scroll -= rows > 1 ? rows / 2 : 1;
                    if (scroll < 0) scroll = 0;
                    blink_t0 = (Uint32)SDL_GetTicks(); activity = 1;
                    break;
                }
                if ((m & K_CTRL) && ctrl_byte(ev.key.keysym.sym, &cb)) {
                    char b = (char)cb; scroll = 0;
                    term_send_text(&term, &b, 1); activity = 1; break;
                }
                if (k != K_NONE) {
                    if (k == K_ENTER || k == K_BS || k == K_TAB || k == K_ESC) scroll = 0;
                    term_send_key(&term, k, m);
                    blink_t0 = (Uint32)SDL_GetTicks(); cursor_on = 0; activity = 1;
                }
                break;
            }
            default: break;
            }
        }

        if (term.fd >= 0 && ready > 0) {
            if (drain_pty(&term, scratch, READ_CHUNK) < 0) {
                char msg[64];
                int l = snprintf(msg, sizeof(msg), "\r\n\x1b[2m[process exited]\x1b[0m\r\n");
                if (l > 0) term_feed(&term, (const uint8_t *)msg, l);
                close(term.fd); term.fd = -1;
            }
            activity = 1;
        }

        if (term_bell_consume(&term)) { SDL_Beep(); activity = 1; }

        if (term.title[0] && strcmp(term.title, win_title) != 0) {
            snprintf(win_title, sizeof(win_title), "mshell — %s", term.title);
            SDL_SetWindowTitle(win, win_title);
        }

        if (term.cursor_visible && scroll == 0 && term.fd >= 0) {
            Uint32 on = (((Uint32)SDL_GetTicks() - blink_t0) / CURSOR_PERIOD) & 1u;
            if (on != cursor_on) { cursor_on = on; activity = 1; }
        } else cursor_on = 0;

        if (activity) {
            gl_render(&gl, &term, scroll, (int)cursor_on);
            SDL_GL_SwapWindow(win);
            force_redraw = 0;
        }

        if (!alive && term.fd < 0) {
            Uint32 now = (Uint32)SDL_GetTicks();
            if (dead_at == 0) dead_at = now;
            else if (now - dead_at > 1500) running = 0;
        }
    }

    if (child > 0) { kill(child, SIGHUP); waitpid(child, NULL, WNOHANG); }
    if (term.fd >= 0) close(term.fd);
    free(scratch); term_free(&term); gl_destroy(&gl);
    SDL_GL_DeleteContext(glctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;

cleanup_ctx:
    SDL_GL_DeleteContext(glctx); SDL_DestroyWindow(win); SDL_Quit();
    return 1;
}

/* --- Nadrino OS desktop --- */

static int run_os_desktop(void)
{
    SDL_Window *win = NULL;
    SDL_GLContext glctx = NULL;
    gl_renderer_t gl;
    os_state_t os;
    int cols = 80, rows = 24, dw = 960, dh = 640;
    int running = 1, force_redraw = 1;
    Uint32 last_tick = 0;

    signal(SIGPIPE, SIG_IGN);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n  Install Termux:X11 and start it.\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    win = SDL_CreateWindow("Nadrino OS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           dw, dh, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { SDL_Quit(); return 1; }

    glctx = SDL_GL_CreateContext(win);
    if (!glctx) { SDL_DestroyWindow(win); SDL_Quit(); return 1; }
    SDL_GL_SetSwapInterval(1);
    SDL_GL_GetDrawableSize(win, &dw, &dh);

    if (gl_init(&gl, dw, dh) != 0) goto cleanup_ctx;

    cols = dw / CELL_W; rows = dh / CELL_H;
    if (cols < 30) cols = 30; if (rows < 15) rows = 15;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;

    os_nadrino_init(&os, cols, rows);
    force_redraw = 1;

    while (running && os_nadrino_running(&os)) {
        SDL_Event ev;
        int activity = force_redraw;
        Uint32 now = SDL_GetTicks();

        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0; break;

            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    SDL_GL_GetDrawableSize(win, &dw, &dh);
                    if (dw < CELL_W * 4) dw = CELL_W * 4;
                    if (dh < CELL_H * 4) dh = CELL_H * 4;
                    gl_resize(&gl, dw, dh);
                    cols = dw / CELL_W; rows = dh / CELL_H;
                    if (cols < 30) cols = 30; if (rows < 15) rows = 15;
                    os_nadrino_resize(&os, cols, rows);
                    activity = 1;
                }
                break;

            case SDL_TEXTINPUT:
                os_nadrino_text(&os, ev.text.text);
                activity = 1; break;

            case SDL_KEYDOWN: {
                int m = mods_from_sdl(ev.key.keysym.mod);
                term_key_t k = key_from_sdl(ev.key.keysym.sym);
                if (k != K_NONE) {
                    os_nadrino_key(&os, k, m);
                    activity = 1;
                }
                break;
            }

            case SDL_MOUSEBUTTONDOWN:
                os_nadrino_click(&os, ev.button.x / CELL_W, ev.button.y / CELL_H);
                activity = 1; break;

            case SDL_MOUSEMOTION:
                os_nadrino_motion(&os, ev.motion.x / CELL_W, ev.motion.y / CELL_H,
                                  ev.motion.state & SDL_BUTTON(1));
                activity = 1; break;

            default: break;
            }
        }

        /* Tick clock for animations (cursor blink, etc.) */
        if (now - last_tick >= 500) {
            os_nadrino_tick(&os);
            last_tick = now;
        }

        if (activity) {
            /* Render OS into the term_t grid */
            term_t tmp;
            term_init(&tmp, cols, rows);
            os_nadrino_render(&os, &tmp);
            gl_render(&gl, &tmp, 0, 0);
            term_free(&tmp);
            SDL_GL_SwapWindow(win);
            force_redraw = 0;
        }
    }

    os_nadrino_free(&os);
    gl_destroy(&gl);
    SDL_GL_DeleteContext(glctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;

cleanup_ctx:
    SDL_GL_DeleteContext(glctx); SDL_DestroyWindow(win); SDL_Quit();
    return 1;
}

#endif /* MSHELL_USE_SDL */

/* =========================================================================== */
/*                          Usage / main entry point                           */
/* =========================================================================== */

static void usage(void)
{
    printf("usage: mshell [options]\n"
           "\n"
           "  (no options)   run a shell in the terminal; type:\n"
           "                 !experiment      — OpenGL ES terminal window\n"
           "                 !adaptivenadrino — Nadrino OS desktop\n"
           "  --gl           start with the OpenGL terminal straight away\n"
           "  --os           start with the Nadrino OS desktop straight away\n"
           "  -h, --help     this message\n"
           "\n"
           "  MSHELL_SHELL=/path/to/shell mshell   choose the shell\n");
}

int main(int argc, char **argv)
{
    const char *self;
    int gl_first = 0, os_first = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--gl"))  gl_first = 1;
        else if (!strcmp(argv[i], "--os"))  os_first = 1;
    }

    self = self_path(argv[0]);
    signal(SIGPIPE, SIG_IGN);

#ifdef MSHELL_USE_SDL
    if (gl_first) {
        /* Detach from real terminal */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO); if (devnull > 2) close(devnull); }
        return run_gl_terminal();
    }
    if (os_first) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO); if (devnull > 2) close(devnull); }
        return run_os_desktop();
    }
#else
    if (gl_first) {
        fprintf(stderr, "mshell: built without OpenGL support\n");
        return 1;
    }
    if (os_first) {
        fprintf(stderr, "mshell: built without OpenGL support\n");
        return 1;
    }
#endif

    return run_launcher(self);
}
