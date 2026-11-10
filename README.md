# mshell — no-root Termux terminal with on-demand OpenGL window and Nadrino OS

A Termux-native terminal that works without root and without any display server.
Type **`!experiment`** at the shell prompt to open an OpenGL ES 2.0 terminal
window that shares the same shell session, or **`!adaptivenadrino`** to launch
the Nadrino OS desktop — a Windows 95/98-style virtual operating system
running inside the terminal grid.

Type **`$simpleinterface`** (or run **`--simple`**) to open the dock-based
configuration UI: add named containers, pick a theme (Classic / Win7 Aero),
set a box64 stability preset, and hit **LAUNCH** to boot the chosen OS.

## How it works

```
mshell (launcher)
  │
  ├── Runs a shell behind a pty, mirrors I/O to your real terminal
  ├── Sees every byte typed; detects `!experiment` and `!adaptivenadrino`
  ├── On `!experiment` → fork/execs itself as `mshell --gl`
  │   └── OpenGL window: SDL2 + GLES2 + same pty, same session
  │   └── When you close the window, the shell session continues
  │
  ├── On `!adaptivenadrino` → fork/execs itself as `mshell --os`
  │   └── Nadrino OS desktop: SDL2 + GLES2, classic Windows 95 UI
  │   └── Floating windows, Start menu, taskbar, desktop icons
  │
  ├── On `$simpleinterface` → fork/execs itself as `mshell --simple`
  │   └── Dock-based configuration UI: containers, themes, stability
  │
  └── If built without SDL2 → `make launcher` → just a wrapper shell
```

No root needed. No display server needed for the launcher. The `--gl`,
`--os`, and `--simple` windows need SDL's video driver (Termux:X11, KMS
with root, or a Wayland compositor).

## Building

### Prerequisites (Termux)

```bash
pkg update
pkg install clang make libutil-dev
```

### Two build modes

```bash
# Full build: launcher + GL window (needs SDL2 + EGL/GLESv2)
make                       # or: make all

# Launcher only (no SDL, no display server needed at all)
make launcher
```

### SDL2 packages (for the GL window)

```bash
pkg install libsdl2
# libutil-dev is already installed — provides forkpty()
# libGLESv2 and libEGL are provided by Android's bionic at runtime
```

## Usage

### Launcher mode (default)

```bash
./mshell                           # starts a shell
!experiment                        # opens the OpenGL window
!adaptivenadrino                   # opens the Nadrino OS desktop
$simpleinterface                   # opens the dock-based configuration UI
# close the window → shell continues
exit                               # done
```

### GL-only mode

```bash
./mshell --gl                      # OpenGL window straight away
```

### Nadrino OS mode

```bash
./mshell --os                      # Nadrino OS desktop straight away
```

### Simple Interface (dock + containers + box64 presets)

```bash
./mshell --simple                  # dock UI straight away
```

### Pick a shell

```bash
MSHELL_SHELL=/usr/bin/bash ./mshell
```

### No-mirror mode

```bash
./mshell --no-mirror               # shell output goes to GL window only
```

## Controls (GL window)

| Action              | Key(s)            |
|---------------------|-------------------|
| Scroll back         | PgUp              |
| Scroll forward      | PgDn              |
| Interrupt (Ctrl-C)  | Ctrl+C            |
| Exit shell          | Ctrl-D or exit    |
| Function keys       | F1–F12            |

## Nadrino OS

Type **`!adaptivenadrino`** or run **`./mshell --os`** to launch the Nadrino OS
desktop — a Windows 95/98-style virtual operating system rendered in the terminal
grid. Features:

- **Classic Windows 95/98 palette**: teal desktop, navy title bars, gray taskbar
- **Floating windows**: drag by the title bar, close with `x`, minimize
- **Start menu**: click Start to toggle the menu
- **Desktop icons**: double-click (or tap) to launch apps
- **Built-in apps**:
  - **Terminal** — echo shell output
  - **Calculator** — basic arithmetic
  - **Text Editor** — simple text input
  - **File Manager** — browses files
  - **About** — system information
- **Taskbar**: Start button + window buttons + clock

### Controls (Nadrino OS)

| Action              | Key(s)            |
|---------------------|-------------------|
| Click / drag        | Mouse             |
| Function keys       | F1–F12            |
| Close window        | Click `x` button  |
| Toggle Start menu   | Click Start       |

## Simple Interface

Type **`$simpleinterface`** or run **`./mshell --simple`** for a full-screen
dock UI used to configure and boot an environment.

Layout: a **circular dock** at the bottom center holds the launchable apps,
a **LAUNCH** bar sits along the bottom edge, named **containers** fill the
left pane and **settings** the right pane.

| Action                       | Key(s)                    |
|------------------------------|---------------------------|
| Move between zones           | Tab                       |
| Move selection               | Up / Down                 |
| Add container                | `N`                       |
| Rename selected container    | `R`                       |
| Delete selected container    | `D`                       |
| Open / toggle                | Enter / Space, or click   |
| Change setting               | Left / Right              |
| Launch chosen OS             | Click LAUNCH, or `L`      |
| Launch dock app              | Click a dock icon         |
| Back out / quit              | Esc                       |

Settings:

- **OS** — Nadrino OS or the GL terminal
- **Theme** — Classic (Win95) or Aero (Windows 7)
- **box64 preset** — `Basic`, `Enhanced`, or `Stability x3`

A **container** is just a named group: give it a name, put app icons in it,
and it shows up in the dock when that container is selected. Names are up to
31 characters and are edited in place.

Everything is drawn with the built-in 5×7 bitmap font (ASCII 0x20–0x7E), so
no assets or locale data are required.

## Windows 7 / Aero theme

Nadrino OS ships two looks, selectable from the Start menu or the simple
interface:

- **Classic** — the original Win95/98 palette: teal desktop, navy title bars.
- **Aero** — a Windows 7-style translucency: blue-glass title bars, lighter
  window chrome, soft highlights. Applied as a post-render recolour pass, so
  every app and window picks it up automatically.

## Display servers

The `--gl` window needs a display:

- **Termux:X11** (no root): install from the Tur repo, start the X server,
  `SDL_VIDEODRIVER=x11 ./mshell --gl`
- **KMS/DRM** (root): `SDL_VIDEODRIVER=kmsdrm ./mshell --gl`
- **The launcher itself** needs no display — it runs entirely in the terminal.

## Project structure

```
termux-gl/
├── main.c      # Two-mode entry point: launcher + GL window + Nadrino OS
├── term.c      # Grid model, ANSI/VT parser, 500-line scrollback
├── term.h      # Public API for the terminal model
├── gl.c        # GLES2 renderer (shaders, atlas, quad batching)
├── gl.h        # Renderer API
├── font.c      # 5×7 bitmap font, 2× scaled into atlas
├── font.h      # Font constants and atlas API
├── os_nadrino.c# Nadrino OS desktop (Win95/Aero UI, apps, windows)
├── os_nadrino.h# Nadrino OS data structures and API
├── simple_ui.c # $simpleinterface: dock, containers, box64 presets
├── simple_ui.h # Simple interface API
├── Makefile    # Two targets: mshell (full), mshell-launcher (no SDL)
└── README.md   # This file
```

## Architecture

### Launcher mode

1. `forkpty()` creates a pty master fd running a shell.
2. `select()` polls both stdin (your keyboard) and the pty master.
3. Bytes from the shell are mirrored to stdout; your keystrokes are forwarded
   to the pty.
4. `!experiment` is detected by scanning mirrored output — it must start a
   command (after prompt, newline, `;`, `|`, `&`), with no quotes.
5. On trigger: `fork()` a child that re-exec's `mshell --gl`; the launcher
   waits on it, then continues.

### GL window mode

1. SDL2 creates an OpenGL ES 2.0 context on the display surface.
2. Same `forkpty()` + `select()` + `term_feed()` pipeline as the launcher.
3. Grid is rendered each frame: background pass + glyph pass = one `glDrawArrays`.
4. SDL keyboard events map to terminal key sequences via `term_send_key()`.
5. When the window closes, the shell session (on the pty) survives.

## Technical notes

- **Colors are packed `0xAABBGGRR`** — uploads to GLES2 as `UNSIGNED_BYTE`
  with `GL_RGBA` without swizzling.
- **The atlas uses `GL_LUMINANCE`** — the fragment shader reads `.r` as coverage
  and multiplies the vertex colour by it.
- **`forkpty()`** creates a pty master set to non-blocking mode.
- **Scrollback** is a ring buffer of rows. `term_cell()` with `off > 0` returns
  cells from the ring; `off == 0` returns the live grid.
- **OSC 0/2** window titles update the SDL window title.

## License

MIT — use it however you like.
