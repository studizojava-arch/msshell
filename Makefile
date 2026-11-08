# mshell — no-root Termux terminal + on-demand OpenGL ES 2.0 window
#
# Typing  !experiment  at the shell opens the GLES2 terminal window
# (the launcher itself needs no display and no root).
#
# Run inside Termux:  pkg install clang make libutil-dev   [+ libsdl2 for GL]
#
# Usage:
#   make            -> mshell          (launcher + GL window, needs libsdl2)
#   make launcher   -> mshell-launcher (plain shell wrapper, no SDL needed)
#   ./mshell                          then type:  !experiment
#   ./mshell --gl                     GL window straight away
#   MSHELL_SHELL=/usr/bin/bash ./mshell   pick a shell

CC       = clang
CFLAGS   = -O2 -std=c11 -Wall -Wextra \
           -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
LDLIBS   = -lutil

# OpenGL build: SDL2 + GLES2 (launched on demand by the !experiment command)
GL_CFLAGS = $(CFLAGS) -DMSHELL_USE_SDL
GL_LIBS   = $(LDLIBS) -lSDL2 -lGLESv2 -lEGL

.PHONY: all launcher clean

all: mshell

mshell: main.c term.c gl.c font.c
	$(CC) $(GL_CFLAGS) -o $@ $^ $(GL_LIBS)

mshell-launcher: main.c term.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

launcher: mshell-launcher

clean:
	rm -f mshell mshell-launcher
