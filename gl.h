/*
 * gl.h — GL ES 2.0 renderer for the terminal grid.
 *
 * One shader, one texture atlas, one VBO of textured quads. Backgrounds are
 * drawn from a solid white block in the atlas, glyphs from the 5x7 font, so a
 * frame is a single draw call per layer.
 */
#ifndef MSHELL_GL_H
#define MSHELL_GL_H

#include <stdint.h>

#ifdef __ANDROID__
#include <GLES2/gl2.h>
#else
#include <SDL2/SDL_opengles2.h>
#endif

#include "term.h"
#include "font.h"

#define MAX_QUADS (TERM_MAX_COLS * TERM_MAX_ROWS * 2)
#define VERTS_PER_QUAD 6

typedef struct {
    float px, py;   /* NDC position */
    float u, v;     /* atlas coordinate */
    uint8_t r, g, b, a;
} gl_vert_t;

typedef struct {
    int screen_w, screen_h;
    int cols, rows;         /* cells that fit on screen */
    GLuint prog;
    GLuint tex;
    GLuint vbo;
    GLint a_pos, a_uv, a_col;
    GLint u_tex;
    gl_vert_t *verts;       /* MAX_QUADS * VERTS_PER_QUAD */
} gl_renderer_t;

/* Compile shaders, build the atlas texture, allocate the vertex buffer.
 * Returns 0 on success. */
int  gl_init(gl_renderer_t *gl, int screen_w, int screen_h);
void gl_destroy(gl_renderer_t *gl);

/* Rebuild the projection after the window changes size. */
void gl_resize(gl_renderer_t *gl, int screen_w, int screen_h);

/* Draw one frame. `off` scrolls the grid up by that many rows (scrollback). */
void gl_render(gl_renderer_t *gl, term_t *t, int off, int cursor_on);

#endif /* MSHELL_GL_H */
