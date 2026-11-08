/*
 * font.h — bitmap glyph atlas for the GL terminal.
 *
 * Source glyphs are a classic 5x7 ASCII matrix, scaled 2x to 10x14 and laid
 * out in a padded cell so linear filtering cannot bleed neighbours together.
 * The resulting cell (12x16) is the terminal's character advance.
 */
#ifndef MSHELL_FONT_H
#define MSHELL_FONT_H

#include <stdint.h>

#define FONT_SRC_W   5
#define FONT_SRC_H   7
#define FONT_SCALE   2
#define FONT_WIDTH   (FONT_SRC_W * FONT_SCALE)   /* 10 */
#define FONT_HEIGHT  (FONT_SRC_H * FONT_SCALE)   /* 14 */

/* Cell = glyph + 1px padding right and bottom. The terminal grid advances by
 * the cell size, so these are also the on-screen character metrics. */
#define CELL_W  12
#define CELL_H  16

#define FONT_FIRST  0x20
#define FONT_LAST   0x7E
#define FONT_GLYPHS (FONT_LAST - FONT_FIRST + 1)   /* 95 */

#define ATLAS_COLS 16
#define ATLAS_ROWS ((FONT_GLYPHS + ATLAS_COLS - 1) / ATLAS_COLS)  /* 6 */

/* Extra strip at the bottom holding a solid white block, reused for the
 * per-cell background quads so the renderer needs one texture and one shader. */
#define SOLID_SIZE 8
#define ATLAS_W (ATLAS_COLS * CELL_W)                                   /* 192 */
#define ATLAS_H (ATLAS_ROWS * CELL_H + SOLID_SIZE)                      /* 104 */
#define SOLID_Y (ATLAS_ROWS * CELL_H)

/* One byte per pixel, 0 or 255. Caller owns `out`, must be ATLAS_W*ATLAS_H. */
void font_build_atlas(uint8_t *out);

/* Column index [0..15] and row index [0..ATLAS_ROWS) of a codepoint. */
void font_cell(int ch, int *col, int *row);

#endif /* MSHELL_FONT_H */
