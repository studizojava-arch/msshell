/*
 * gl.c — draws the terminal grid with GL ES 2.0.
 *
 * Coordinate system: we compute NDC on the CPU, so the vertex shader just
 * passes position through. Cells map 1:1 to CELL_W x CELL_H pixels.
 *
 * Fragment shader multiplies the vertex colour by the atlas sample, which is
 * 1.0 inside the solid block (backgrounds) and the glyph coverage elsewhere.
 */
#include "gl.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

static const char *VS =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "attribute vec4 aCol;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "void main(void) {\n"
    "    vUV = aUV;\n"
    "    vCol = aCol;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char *FS =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "varying vec4 vCol;\n"
    "uniform sampler2D uTex;\n"
    "void main(void) {\n"
    "    float cov = texture2D(uTex, vUV).r;\n"
    "    gl_FragColor = vec4(vCol.rgb * cov, vCol.a * cov);\n"
    "}\n";

/* ------------------------------------------------------------------ utils */

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);

    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int link_program(void)
{
    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }

    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aUV");
    glBindAttribLocation(p, 2, "aCol");
    glLinkProgram(p);

    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "program link failed: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint make_atlas(void)
{
    uint8_t *px = malloc((size_t)ATLAS_W * ATLAS_H);
    if (!px) return 0;
    font_build_atlas(px);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    /* NPOT is fine in ES2 as long as we never mipmap or repeat. */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, ATLAS_W, ATLAS_H, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, px);
    free(px);
    return tex;
}

/* ------------------------------------------------------------ geometry */

static inline float ndc_x(int px, int w) { return (float)px * 2.0f / (float)w - 1.0f; }
static inline float ndc_y(int py, int h) { return 1.0f - (float)py * 2.0f / (float)h; }

/* Push one quad. x0,y0 are the top-left pixel of the cell. */
static void push_quad(gl_vert_t *v, int x0, int y0, int w, int h,
                      float u0, float v0, float u1, float v1,
                      uint32_t col, int sw, int sh)
{
    float x1 = x0 + (float)w;
    float y1 = y0 + (float)h;
    float X0 = ndc_x(x0, sw), X1 = ndc_x((int)x1, sw);
    float Y0 = ndc_y(y0, sh), Y1 = ndc_y((int)y1, sh);

    uint8_t r = (uint8_t)(col & 0xFF);
    uint8_t g = (uint8_t)((col >> 8) & 0xFF);
    uint8_t b = (uint8_t)((col >> 16) & 0xFF);
    uint8_t a = (uint8_t)((col >> 24) & 0xFF);
    if (a == 0) a = 255;

    /* Two triangles: 0-1-2, 0-2-3. */
    v[0].px = X0; v[0].py = Y0; v[0].u = u0; v[0].v = v0;
    v[1].px = X1; v[1].py = Y0; v[1].u = u1; v[1].v = v0;
    v[2].px = X1; v[2].py = Y1; v[2].u = u1; v[2].v = v1;
    v[3].px = X0; v[3].py = Y0; v[3].u = u0; v[3].v = v0;
    v[4].px = X1; v[4].py = Y1; v[4].u = u1; v[4].v = v1;
    v[5].px = X0; v[5].py = Y1; v[5].u = u0; v[5].v = v1;
    for (int i = 0; i < 6; i++) { v[i].r = r; v[i].g = g; v[i].b = b; v[i].a = a; }
}

/* Atlas rectangle for a glyph, excluding the 1px padding. */
static void glyph_uv(int cp, float *u0, float *v0, float *u1, float *v1)
{
    int col, row;
    font_cell(cp, &col, &row);
    int x = col * CELL_W;
    int y = row * CELL_H;
    *u0 = (float)x / ATLAS_W;
    *v0 = (float)y / ATLAS_H;
    *u1 = (float)(x + FONT_WIDTH) / ATLAS_W;
    *v1 = (float)(y + FONT_HEIGHT) / ATLAS_H;
}

/* Centre of the solid block — constant UV, always fully covered. */
#define SOLID_U ((float)SOLID_SIZE / 2.0f / ATLAS_W)
#define SOLID_V ((float)(SOLID_Y + SOLID_SIZE / 2) / ATLAS_H)

/* ------------------------------------------------------------------ API */

int gl_init(gl_renderer_t *gl, int screen_w, int screen_h)
{
    memset(gl, 0, sizeof(*gl));

    gl->prog = link_program();
    if (!gl->prog) return -1;

    gl->tex = make_atlas();
    if (!gl->tex) { glDeleteProgram(gl->prog); return -1; }

    gl->a_pos = glGetAttribLocation(gl->prog, "aPos");
    gl->a_uv  = glGetAttribLocation(gl->prog, "aUV");
    gl->a_col = glGetAttribLocation(gl->prog, "aCol");
    gl->u_tex = glGetUniformLocation(gl->prog, "uTex");

    gl->verts = malloc(sizeof(gl_vert_t) * MAX_QUADS * VERTS_PER_QUAD);
    if (!gl->verts) return -1;

    glGenBuffers(1, &gl->vbo);
    glBindTexture(GL_TEXTURE_2D, gl->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    gl_resize(gl, screen_w, screen_h);
    return 0;
}

void gl_destroy(gl_renderer_t *gl)
{
    if (gl->vbo) glDeleteBuffers(1, &gl->vbo);
    if (gl->tex) glDeleteTextures(1, &gl->tex);
    if (gl->prog) glDeleteProgram(gl->prog);
    free(gl->verts);
    memset(gl, 0, sizeof(*gl));
}

void gl_resize(gl_renderer_t *gl, int screen_w, int screen_h)
{
    gl->screen_w = screen_w;
    gl->screen_h = screen_h;
    gl->cols = screen_w / CELL_W;
    gl->rows = screen_h / CELL_H;
    if (gl->cols > TERM_MAX_COLS) gl->cols = TERM_MAX_COLS;
    if (gl->rows > TERM_MAX_ROWS) gl->rows = TERM_MAX_ROWS;
    glViewport(0, 0, screen_w, screen_h);
}

void gl_render(gl_renderer_t *gl, term_t *t, int off, int cursor_on)
{
    int sw = gl->screen_w, sh = gl->screen_h;
    int cols = gl->cols, rows = gl->rows;
    gl_vert_t *base = gl->verts;
    int nquad = 0;
    int maxq = MAX_QUADS;

    glClearColor(((t->def_bg) & 0xFF) / 255.0f,
                 ((t->def_bg >> 8) & 0xFF) / 255.0f,
                 ((t->def_bg >> 16) & 0xFF) / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!gl->prog || !gl->verts) return;

    /* --- pass 1: backgrounds --- */
    for (int y = 0; y < rows && nquad < maxq; y++) {
        for (int x = 0; x < cols && nquad < maxq; x++) {
            const term_cell_t *c = term_cell(t, x, y, off);
            if (!c) continue;
            uint32_t bg = c->bg;
            if (c->style & S_INVERSE) bg = c->fg;
            if (bg == t->def_bg) continue;      /* nothing to draw */
            push_quad(base + nquad * VERTS_PER_QUAD,
                      x * CELL_W, y * CELL_H, CELL_W, CELL_H,
                      SOLID_U, SOLID_V, SOLID_U, SOLID_V, bg, sw, sh);
            nquad++;
        }
    }

    /* --- pass 2: glyphs --- */
    for (int y = 0; y < rows && nquad < maxq; y++) {
        int sy = y + off;
        for (int x = 0; x < cols && nquad < maxq; x++) {
            const term_cell_t *c = term_cell(t, x, y, off);
            if (!c || c->cp == ' ' || c->cp == 0) continue;

            uint32_t fg = c->fg;
            uint32_t bg = c->bg;
            if (c->style & S_INVERSE) { fg = c->bg; bg = c->fg; }
            if (c->style & S_BOLD)    fg |= 0x00202020;

            /* Draw the cursor as a solid block with inverted text. */
            int is_cur = cursor_on && (x == t->cx) && (sy == t->cy) && off == 0;
            if (is_cur) { fg = bg; bg = c->fg; }

            float u0, v0, u1, v1;
            glyph_uv((int)c->cp, &u0, &v0, &u1, &v1);
            push_quad(base + nquad * VERTS_PER_QUAD,
                      x * CELL_W, y * CELL_H, CELL_W, CELL_H,
                      u0, v0, u1, v1, fg, sw, sh);
            nquad++;
        }
    }

    /* Cursor block when the cell under it is empty. */
    if (cursor_on && off == 0 && nquad < maxq) {
        const term_cell_t *c = term_cell(t, t->cx, t->cy, 0);
        if (!c || c->cp == ' ' || c->cp == 0) {
            push_quad(base + nquad * VERTS_PER_QUAD,
                      t->cx * CELL_W, t->cy * CELL_H, CELL_W, CELL_H,
                      SOLID_U, SOLID_V, SOLID_U, SOLID_V, t->pen_fg, sw, sh);
            nquad++;
        }
    }

    if (nquad == 0) return;

    glUseProgram(gl->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl->tex);
    glUniform1i(gl->u_tex, 0);

    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)nquad * VERTS_PER_QUAD * (GLsizei)sizeof(gl_vert_t),
                 gl->verts, GL_STREAM_DRAW);

    GLsizei stride = (GLsizei)sizeof(gl_vert_t);
    glEnableVertexAttribArray(gl->a_pos);
    glVertexAttribPointer(gl->a_pos, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)offsetof(gl_vert_t, px));
    glEnableVertexAttribArray(gl->a_uv);
    glVertexAttribPointer(gl->a_uv, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)offsetof(gl_vert_t, u));
    glEnableVertexAttribArray(gl->a_col);
    glVertexAttribPointer(gl->a_col, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                          (const void *)offsetof(gl_vert_t, r));

    glDrawArrays(GL_TRIANGLES, 0, nquad * VERTS_PER_QUAD);

    glDisableVertexAttribArray(gl->a_pos);
    glDisableVertexAttribArray(gl->a_uv);
    glDisableVertexAttribArray(gl->a_col);
}
