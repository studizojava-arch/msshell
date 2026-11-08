/*
 * main.c — mshell: a no-root Termux terminal with an on-demand GL window.
 *
 * Two modes, one binary:
 *
 *   mshell                launcher. Runs a shell whose pty we sit on, mirroring
 *                         everything to the real terminal so the session behaves
 *                         like a normal shell (no root, no display server).
 *                         When the user types  !experiment  we fork a second
 *                         copy of ourselves with --gl and keep going after it
 *                         closes.
 *
 *   mshell --gl           the OpenGL ES 2.0 renderer: SDL2 window + GLES2
 *                         context + a pty'd shell, glued to term.c/gl.c.
 *
 * The launcher never needs a display, so it works in bare Termux. Only the
 * !experiment window needs SDL's video driver (Termux:X11 or, with root, KMS).
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

#include <pty.h>          /* forkpty(): Termux libutil / glibc util */

#ifdef MSHELL_USE_SDL
#include <SDL2/SDL.h>
#include "gl.h"
#endif

#include "term.h"

#define FRAME_MS      16          /* ~60 fps */
#define CURSOR_PERIOD 530         /* ms per blink half-cycle */
#define READ_CHUNK    8192
#define DEAD_HOLD_MS  1500        /* keep the final screen up after exit */

#define BANG_CMD      "!experiment"

static const char *kShellCandidates[] = {
    "./myshell",
    "./mshell",
    "/data/data/com.termux/files/usr/bin/bash",
    "/data/data/com.termux/files/usr/bin/sh",
    "/data/data/com.termux/files/usr/bin/zsh",
    "/bin/bash",
    "/bin/sh",
    "/system/bin/sh",
    NULL
};

/* ------------------------------------------------------------------- helpers */

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
    setenv("MSHELL", "1", 1);
}

/* Resolve our own argv[0] so the launcher can re-exec itself. */
static const char *self_path(const char *argv0)
{
    static char buf[512];
    ssize_t n;

    if (argv0 && *argv0 && strchr(argv0, '/')) return argv0;

    n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
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

/* ------------------------------------------------------- shell child (pty) */

/*
 * Fork the shell behind a pty. Returns the master fd and stores the child pid,
 * or -1 on failure. The child never returns.
 */
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

    /* $MSHELL_SHELL wins, then $SHELL, then the built-in candidate list. */
    env = getenv("MSHELL_SHELL");
    if (!env || !*env) env = getenv("SHELL");

    while (kShellCandidates[n]) n++;
    argv = calloc((size_t)n + 2, sizeof(char *));
    if (!argv) return -1;
    if (env && *env) argv[i++] = (char *)env;
    for (n = 0; kShellCandidates[n]; n++) argv[i + n] = (char *)kShellCandidates[n];

    /* forkpty(master, name, termios, winsize) — winsize is the 4th argument. */
    pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        free(argv);
        return -1;
    }

    if (pid == 0) {
        for (int k = 0; argv[k]; k++) {
            execlp(argv[k], argv[k], (char *)NULL);
            if (errno != ENOENT && errno != ENOEXEC && errno != EACCES)
                break;
        }
        fprintf(stderr, "mshell: cannot start a shell: %s\r\n", strerror(errno));
        _exit(127);
    }

    free(argv);
    *out_pid = pid;
    set_nonblock(master);
    return master;
}

/* ------------------------------------------------- "!experiment" detection */

/*
 * The launcher sees the shell's echo of every keystroke, so the trigger can be
 * detected without touching the shell itself. We only fire when the word starts
 * a command (right after the prompt, a newline, or a ; | & separator) and when
 * the line has no quotes or backslashes — that keeps `echo "!experiment"` and
 * `grep !experiment file` from opening a window.
 */
typedef struct {
    int  match;       /* bytes of BANG_CMD matched; <0 = complete, awaiting EOL */
    int  cmd_ok;      /* cursor sits where a command may start */
    int  quote;       /* line contains a quote/backslash — never trigger */
} bang_t;

static void bang_init(bang_t *b)
{
    b->match = 0;
    b->cmd_ok = 1;
    b->quote = 0;
}

static int bang_scan(bang_t *b, const uint8_t *d, int n)
{
    static const char word[] = BANG_CMD;
    const int wlen = (int)sizeof(word) - 1;
    int fired = 0, i;

    for (i = 0; i < n; i++) {
        char c = (char)d[i];

        if (b->match < 0) {
            /* Whole word matched; c is the byte right after it. */
            int boundary = (c == '\r' || c == '\n' || c == ';' || c == '|' ||
                            c == '&' || c == ' ' || c == '\t');
            if (boundary && b->cmd_ok && !b->quote) fired = 1;
            b->match = 0;
        } else if (b->match > 0) {
            if (c == word[b->match]) {
                b->match++;
                if (b->match == wlen) b->match = -wlen;
                continue;
            }
            b->match = 0;                 /* mismatch: re-test c below */
        }

        if (c == '!') {
            if (b->cmd_ok && !b->quote) b->match = 1;
            else b->cmd_ok = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '\\') { b->quote = 1; b->cmd_ok = 0; continue; }
        if (c == '\n' || c == '\r') { b->quote = 0; b->cmd_ok = 1; continue; }
        if (c == ' ' || c == '\t') continue;
        if (c == '$' || c == '#' || c == '>') continue;   /* prompt punctuation */
        if (c == ';' || c == '|' || c == '&') { b->cmd_ok = 1; continue; }
        b->cmd_ok = 0;
    }
    return fired;
}

/* ------------------------------------------------------------------ raw tty */

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
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == 0) g_raw_on = 1;
}

static void tty_restore(void)
{
    if (g_raw_on) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tio);
        g_raw_on = 0;
    }
}

/* ------------------------------------------------------------- GL launcher */

static void gl_banner(int mirror)
{
    const char *msg =
        "\x1b[1;36mmshell\x1b[0m \x1b[2m(no-root launcher)\x1b[0m\r\n"
        "  \x1b[33m!experiment\x1b[0m  open the OpenGL ES terminal window\r\n"
        "  \x1b[32mready\x1b[0m\r\n\r\n";
    const char *off = "mshell: mirroring off, output goes to the GL window only\n";

    (void)write_all(STDOUT_FILENO, msg, strlen(msg));
    if (!mirror) (void)write_all(STDERR_FILENO, off, strlen(off));
}

/*
 * Run a shell and mirror its pty to the real terminal. Returns when the shell
 * exits. Each  !experiment  spawns a GL window that we wait on, then continue.
 */
static int run_launcher(const char *self, int mirror)
{
    bang_t bang;
    uint8_t buf[READ_CHUNK];
    pid_t shell = -1, glpid = -1;
    int master;
    int running = 1;
    struct winsize ws;

    master = spawn_shell(&shell, 80, 24);
    if (master < 0) {
        fprintf(stderr, "mshell: forkpty failed: %s\n", strerror(errno));
        return 1;
    }

    /* Match the window to the real terminal, then keep it in sync. */
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
        pty_set_size(master, ws.ws_col, ws.ws_row);

    bang_init(&bang);
    tty_raw();
    gl_banner(mirror);

    while (running) {
        fd_set rfds;
        struct timeval tv;
        int ready;

        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(master, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;

        ready = select(master + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Keep the shell's idea of the window in sync with the real terminal. */
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
            pty_set_size(master, ws.ws_col, ws.ws_row);

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r > 0) {
                if (write_all(master, (const char *)buf, (size_t)r) != 0) break;
            } else if (r == 0) {
                break;                          /* Ctrl-D at the real tty */
            } else if (errno != EAGAIN && errno != EINTR) {
                break;
            }
        }

        if (FD_ISSET(master, &rfds)) {
            ssize_t r = read(master, buf, sizeof(buf));
            if (r > 0) {
                if (mirror) write_all(STDOUT_FILENO, (const char *)buf, (size_t)r);
                if (glpid < 0 && bang_scan(&bang, buf, (int)r)) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        execl(self, self, "--gl", (char *)NULL);
                        _exit(127);
                    }
                    if (pid > 0) glpid = pid;
                    else fprintf(stderr, "mshell: fork failed: %s\n", strerror(errno));
                }
            } else if (r == 0 || errno == EIO) {
                running = 0;
            } else if (errno != EAGAIN && errno != EINTR) {
                running = 0;
            }
        }

        /* Reap a finished GL window so the shell session continues. */
        if (glpid > 0) {
            pid_t done = waitpid(glpid, NULL, WNOHANG);
            if (done == glpid) {
                glpid = -1;
                bang_init(&bang);
                if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0)
                    pty_set_size(master, ws.ws_col, ws.ws_row);
            }
        }
    }

    if (glpid > 0) kill(glpid, SIGHUP);
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

/*
 * Painted into the grid before the shell prints anything, so the first frame is
 * a real UI instead of a blank rectangle.
 */
static void boot_splash(term_t *t, const char *shell_path)
{
    char buf[512];
    int n;

    n = snprintf(buf, sizeof(buf),
        "\x1b[2J\x1b[H"
        "\x1b[1;36m  mshell\x1b[0m \x1b[2mOpenGL ES terminal\x1b[0m\r\n"
        "\x1b[2m  %s\x1b[0m\r\n\r\n"
        "\x1b[33mPgUp/PgDn\x1b[0m scrollback    "
        "\x1b[33mCtrl-C\x1b[0m interrupt    "
        "\x1b[33mCtrl-D\x1b[0m exit\r\n\r\n",
        (shell_path && *shell_path) ? shell_path : "shell");
    if (n > 0) term_feed(t, (const uint8_t *)buf, n);
}

/* ---------------------------------------------------------------------- input */

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
    default:              return K_NONE;
    }
}

/*
 * Control combinations SDL delivers without a matching TEXTINPUT event.
 * Returns 1 and stores the byte to send, or 0 if there is none.
 */
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

/* ------------------------------------------------------------------ pty drain */

/*
 * Read whatever the shell produced since the last frame. Returns the byte
 * count, or -1 when the child is gone (EOF, or EIO which is how the pty master
 * reports a dead child on Android).
 */
static int drain_pty(term_t *t, uint8_t *scratch, int cap)
{
    int total = 0;

    for (;;) {
        ssize_t r = read(t->fd, scratch, (size_t)cap);
        if (r > 0) {
            term_feed(t, scratch, (int)r);
            total += (int)r;
            if ((int)r < cap) break;
            continue;
        }
        if (r == 0) return -1;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        if (errno == EIO) return -1;
        break;
    }
    return total;
}

/* ------------------------------------------------------------- GL main loop */

static int run_gl(const char *argv0)
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
    int running = 1;
    int alive = 1;
    int force_redraw = 1;
    Uint32 blink_t0, cursor_on = 1, dead_at = 0;
    char win_title[192] = "mshell";

    (void)argv0;
    signal(SIGPIPE, SIG_IGN);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n"
                "  No display? In Termux install Termux:X11 (no root needed),\n"
                "  start its X server, then run with SDL_VIDEODRIVER=x11.\n",
                SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    win = SDL_CreateWindow("mshell",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           dw, dh,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                           SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    glctx = SDL_GL_CreateContext(win);
    if (!glctx) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    SDL_GL_GetDrawableSize(win, &dw, &dh);

    if (gl_init(&gl, dw, dh) != 0) {
        fprintf(stderr, "gl_init: could not build shaders/atlas\n");
        goto cleanup_ctx;
    }

    cols = dw / CELL_W;
    rows = dh / CELL_H;
    if (cols < 20) cols = 20;
    if (rows < 6)  rows = 6;
    if (cols > TERM_MAX_COLS) cols = TERM_MAX_COLS;
    if (rows > TERM_MAX_ROWS) rows = TERM_MAX_ROWS;

    if (term_init(&term, cols, rows) != 0) {
        fprintf(stderr, "term_init: out of memory\n");
        goto cleanup_gl;
    }

    scratch = malloc(READ_CHUNK);
    if (!scratch) {
        fprintf(stderr, "out of memory\n");
        goto cleanup_term;
    }

    master = spawn_shell(&child, cols, rows);
    if (master < 0) {
        fprintf(stderr, "forkpty: %s\n", strerror(errno));
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
        int activity = force_redraw;
        int ready = 0;

        if (term.fd >= 0) {
            fd_set rfds;
            struct timeval tv;
            FD_ZERO(&rfds);
            FD_SET(term.fd, &rfds);
            tv.tv_sec = 0;
            tv.tv_usec = FRAME_MS * 1000;
            ready = select(term.fd + 1, &rfds, NULL, NULL, &tv);
            if (ready < 0 && errno != EINTR) ready = 0;
        } else {
            SDL_Delay(FRAME_MS);
        }

        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;

            case SDL_WINDOWEVENT:
                if (ev.window.event != SDL_WINDOWEVENT_RESIZED &&
                    ev.window.event != SDL_WINDOWEVENT_SIZE_CHANGED &&
                    ev.window.event != SDL_WINDOWEVENT_EXPOSED)
                    break;
                SDL_GL_GetDrawableSize(win, &dw, &dh);
                if (dw < CELL_W * 4) dw = CELL_W * 4;
                if (dh < CELL_H * 4) dh = CELL_H * 4;
                gl_resize(&gl, dw, dh);
                cols = dw / CELL_W;
                rows = dh / CELL_H;
                if (cols < 20) cols = 20;
                if (rows < 6)  rows = 6;
                if (cols != term.cols || rows != term.rows) {
                    term_resize(&term, cols, rows);
                    pty_set_size(term.fd, cols, rows);
                    scroll = 0;
                }
                activity = 1;
                break;

            case SDL_TEXTINPUT:
                scroll = 0;
                term_send_text(&term, ev.text.text, (int)strlen(ev.text.text));
                activity = 1;
                break;

            case SDL_KEYDOWN: {
                int m = mods_from_sdl(ev.key.keysym.mod);
                term_key_t k = key_from_sdl(ev.key.keysym.sym);
                int cb;

                /* Scrolling is our own gesture, never sent to the shell. */
                if (k == K_PGUP) {
                    int maxs = term_max_scroll(&term);
                    scroll += rows > 1 ? rows / 2 : 1;
                    if (scroll > maxs) scroll = maxs;
                    blink_t0 = (Uint32)SDL_GetTicks();
                    cursor_on = 0;
                    activity = 1;
                    break;
                }
                if (k == K_PGDN && scroll > 0) {
                    scroll -= rows > 1 ? rows / 2 : 1;
                    if (scroll < 0) scroll = 0;
                    blink_t0 = (Uint32)SDL_GetTicks();
                    activity = 1;
                    break;
                }

                if ((m & K_CTRL) && ctrl_byte(ev.key.keysym.sym, &cb)) {
                    char b = (char)cb;
                    scroll = 0;
                    term_send_text(&term, &b, 1);
                    activity = 1;
                    break;
                }

                if (k != K_NONE) {
                    if (k == K_ENTER || k == K_BS || k == K_TAB || k == K_ESC)
                        scroll = 0;
                    term_send_key(&term, k, m);
                    blink_t0 = (Uint32)SDL_GetTicks();
                    cursor_on = 0;
                    activity = 1;
                }
                break;
            }

            default:
                break;
            }
        }

        if (term.fd >= 0 && ready > 0) {
            if (drain_pty(&term, scratch, READ_CHUNK) < 0) {
                char msg[64];
                int l;
                close(term.fd);
                term.fd = -1;
                l = snprintf(msg, sizeof(msg),
                             "\r\n\x1b[2m[process exited]\x1b[0m\r\n");
                if (l > 0) term_feed(&term, (const uint8_t *)msg, l);
            }
            activity = 1;
        }

        if (term_bell_consume(&term)) {
            SDL_Beep();
            activity = 1;
        }

        /* OSC 0/2 may have set a new title this frame. */
        if (term.title[0] && strcmp(term.title, win_title) != 0) {
            snprintf(win_title, sizeof(win_title), "mshell — %s", term.title);
            SDL_SetWindowTitle(win, win_title);
        }

        if (term.cursor_visible && scroll == 0 && term.fd >= 0) {
            Uint32 on = (((Uint32)SDL_GetTicks() - blink_t0) / CURSOR_PERIOD) & 1u;
            if (on != cursor_on) { cursor_on = on; activity = 1; }
        } else {
            cursor_on = 0;
        }

        if (activity) {
            gl_render(&gl, &term, scroll, (int)cursor_on);
            SDL_GL_SwapWindow(win);
            force_redraw = 0;
        }

        if (!alive && term.fd < 0) {
            Uint32 now = (Uint32)SDL_GetTicks();
            if (dead_at == 0) dead_at = now;
            else if (now - dead_at > DEAD_HOLD_MS) running = 0;
        }
    }

    if (child > 0) {
        kill(child, SIGHUP);
        waitpid(child, NULL, WNOHANG);
    }
    if (term.fd >= 0) close(term.fd);
    free(scratch);
    term_free(&term);
    gl_destroy(&gl);
    SDL_GL_DeleteContext(glctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;

cleanup_term:
    term_free(&term);
cleanup_gl:
    gl_destroy(&gl);
cleanup_ctx:
    SDL_GL_DeleteContext(glctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 1;
}

#endif /* MSHELL_USE_SDL */

/* ----------------------------------------------------------------------- main */

static void usage(void)
{
    printf(
        "usage: mshell [options]\n"
        "\n"
        "  (no options)   run a shell in the terminal; typing !experiment opens\n"
        "                 the OpenGL ES 2.0 window (no root required)\n"
        "  --gl           start with the OpenGL window straight away\n"
        "  --no-mirror    launcher mode without mirroring output to this terminal\n"
        "  -h, --help     this message\n"
        "\n"
        "  MSHELL_SHELL=/path/to/shell mshell   choose the shell\n");
}

int main(int argc, char **argv)
{
    const char *self;
    int mirror = 1;
    int gl_first = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (!strcmp(argv[i], "--gl")) {
            gl_first = 1;
        } else if (!strcmp(argv[i], "--no-mirror")) {
            mirror = 0;
        }
    }

    self = self_path(argv[0]);
    signal(SIGPIPE, SIG_IGN);

#ifdef MSHELL_USE_SDL
    if (gl_first) {
        /* Detach from the invoking terminal: this window owns its own pty, and
         * leaving the real tty alone avoids fighting the launcher for it. */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            if (!mirror) dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        return run_gl(self);
    }
#else
    if (gl_first) {
        fprintf(stderr, "mshell: built without OpenGL support (MSHELL_USE_SDL)\n");
        return 1;
    }
#endif

    return run_launcher(self, mirror);
}
