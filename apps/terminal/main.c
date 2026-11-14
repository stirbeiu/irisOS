/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: apps/terminal/main.c
 * Purpose: Interactive Ring 3 Graphical Terminal Emulator (`apps/terminal`).
 *          Creates a translucent desktop window via `libiris_gui`, spawns
 *          Toybox `/bin/sh` attached to `iris_pty`, bridges keyboard events
 *          to PTY input and PTY output to a VT100/ANSI character cell grid,
 *          and renders anti-aliased FreeType 2 typography over a translucent
 *          surface (`0xC8101014`).
 *
 * Licensing: MIT License (Zone 1/2 compatible).
 */

#include <iris/gui.h>
#include <iris/ipc.h>
#include <iris/pty.h>

#include <zircon/process.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#include <fcntl.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ==============================================================================
 * Terminal Window & Grid Constants
 * ============================================================================== */

#define TERM_WIN_W            580u
#define TERM_WIN_H            360u
#define TERM_WIN_X            52
#define TERM_WIN_Y            72

#define TERM_PAD_X            16
#define TERM_PAD_Y            12
#define TERM_CELL_W           8
#define TERM_CELL_H           18
#define TERM_COLS             68
#define TERM_ROWS             18
#define TERM_FONT_PX          13u

/* Premultiplied ARGB8888 colors */
#define TERM_BG_DEFAULT       0xC8101014u /* ~78% opacity premultiplied dark */
#define TERM_FG_DEFAULT       0xFFD8DEE9u /* Crisp off-white terminal foreground */
#define TERM_FG_BRIGHT        0xFFFFFFFFu
#define TERM_CURSOR_COLOR     0xFFE5E9F0u

#define JETBRAINS_VAR_PATH     "/System/Resources/Fonts/JetBrainsMono-Variable.ttf"
#define JETBRAINS_IT_VAR_PATH  "/System/Resources/Fonts/JetBrainsMono-Italic-Variable.ttf"
#define JETBRAINS_REGULAR_PATH "/System/Resources/Fonts/JetBrainsMono-Regular.ttf"
#define JETBRAINS_BOLD_PATH    "/System/Resources/Fonts/JetBrainsMono-Bold.ttf"
#define INTER_VAR_PATH         "/System/Resources/Fonts/Inter-Variable.ttf"
#define INTER_REGULAR_PATH     "/System/Resources/Fonts/InterDisplay-Regular.otf"
#define INTER_BOLD_PATH        "/System/Resources/Fonts/InterDisplay-Bold.otf"

extern char** environ;

/* ==============================================================================
 * ANSI 16-Color Palette (ARGB8888)
 * ============================================================================== */

static const uint32_t k_ansi_colors[16] = {
    0xFF1E222Au, /* 0: Black */
    0xFFE06C75u, /* 1: Red */
    0xFF98C379u, /* 2: Green */
    0xFFE5C07Bu, /* 3: Yellow */
    0xFF61AFEFu, /* 4: Blue */
    0xFFC678DDu, /* 5: Magenta */
    0xFF56B6C2u, /* 6: Cyan */
    0xFFD8DEE9u, /* 7: White (default fg) */
    0xFF5C6370u, /* 8: Bright Black (Gray) */
    0xFFEF596Fu, /* 9: Bright Red */
    0xFF89CA78u, /* 10: Bright Green */
    0xFFD19A66u, /* 11: Bright Yellow */
    0xFF61AFEFu, /* 12: Bright Blue */
    0xFFD55FDEu, /* 13: Bright Magenta */
    0xFF2BBAC5u, /* 14: Bright Cyan */
    0xFFFFFFFFu  /* 15: Bright White */
};

/* ==============================================================================
 * Embedded 8x8 Fallback Font & Blocky "codename:alexia" Header Glyphs
 * ============================================================================== */

static const uint8_t k_font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* '!' */
    {0x6C,0x6C,0x48,0x00,0x00,0x00,0x00,0x00}, /* '"' */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* '#' */
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, /* '$' */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* '%' */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* '&' */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ''' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* '(' */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ')' */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* '*' */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* '+' */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* ',' */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* '.' */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* '/' */
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, /* '0' */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* '1' */
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, /* '2' */
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, /* '3' */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* '4' */
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, /* '5' */
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, /* '6' */
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, /* '7' */
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, /* '8' */
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, /* '9' */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* ':' */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ';' */
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, /* '<' */
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00}, /* '=' */
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, /* '>' */
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, /* '?' */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* '@' */
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* 'A' */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* 'B' */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* 'C' */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* 'D' */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* 'E' */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* 'F' */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* 'G' */
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* 'H' */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 'I' */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* 'J' */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* 'K' */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* 'L' */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* 'M' */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* 'N' */
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* 'O' */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* 'P' */
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06}, /* 'Q' */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* 'R' */
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, /* 'S' */
    {0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00}, /* 'T' */
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* 'U' */
    {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* 'V' */
    {0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00}, /* 'W' */
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, /* 'X' */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, /* 'Y' */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* 'Z' */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* '[' */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* '\' */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ']' */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* '_' */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* 'a' */
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, /* 'b' */
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, /* 'c' */
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, /* 'd' */
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, /* 'e' */
    {0x3C,0x66,0x60,0xF8,0x60,0x60,0xF0,0x00}, /* 'f' */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* 'g' */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* 'h' */
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* 'i' */
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C}, /* 'j' */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* 'k' */
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* 'l' */
    {0x00,0x00,0xEC,0xFE,0xD6,0xC6,0xC6,0x00}, /* 'm' */
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, /* 'n' */
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, /* 'o' */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* 'p' */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* 'q' */
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, /* 'r' */
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, /* 's' */
    {0x30,0x30,0xFC,0x30,0x30,0x36,0x1C,0x00}, /* 't' */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* 'u' */
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* 'v' */
    {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00}, /* 'w' */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* 'x' */
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC}, /* 'y' */
    {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00}, /* 'z' */
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* '{' */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* '|' */
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* '}' */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /* '~' */
};

static const uint8_t k_block_glyphs[11][9] = {
    /* 0: 'c' */ {0x00, 0x00, 0x1F, 0x18, 0x18, 0x18, 0x18, 0x1F, 0x00},
    /* 1: 'o' */ {0x00, 0x00, 0x1F, 0x1B, 0x1B, 0x1B, 0x1B, 0x1F, 0x00},
    /* 2: 'd' */ {0x03, 0x03, 0x1F, 0x1B, 0x1B, 0x1B, 0x1B, 0x1F, 0x00},
    /* 3: 'e' */ {0x00, 0x00, 0x1F, 0x1B, 0x1F, 0x18, 0x18, 0x1F, 0x00},
    /* 4: 'n' */ {0x00, 0x00, 0x1F, 0x1B, 0x1B, 0x1B, 0x1B, 0x1B, 0x00},
    /* 5: 'a' */ {0x00, 0x00, 0x1F, 0x03, 0x1F, 0x1B, 0x1B, 0x1F, 0x00},
    /* 6: 'm' */ {0x00, 0x00, 0x1F, 0x15, 0x15, 0x15, 0x15, 0x15, 0x00},
    /* 7: ':' */ {0x00, 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00, 0x00},
    /* 8: 'l' */ {0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0E, 0x00},
    /* 9: 'x' */ {0x00, 0x00, 0x1B, 0x1B, 0x0E, 0x0E, 0x1B, 0x1B, 0x00},
    /* 10:'i' */ {0x0C, 0x00, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x00}
};

static int block_glyph_index(char ch) {
    switch (ch) {
        case 'c': return 0;
        case 'o': return 1;
        case 'd': return 2;
        case 'e': return 3;
        case 'n': return 4;
        case 'a': return 5;
        case 'm': return 6;
        case ':': return 7;
        case 'l': return 8;
        case 'x': return 9;
        case 'i': return 10;
        default:  return -1;
    }
}

/* ==============================================================================
 * O(1) FreeType 2 Glyph Cache
 * ============================================================================== */

typedef struct term_glyph {
    bool     valid;
    bool     failed;
    int16_t  bitmap_left;
    int16_t  bitmap_top;
    int16_t  advance_x;
    uint16_t width;
    uint16_t rows;
    uint8_t* alpha;
} term_glyph_t;

static FT_Library   g_ft_lib  = NULL;
static FT_Face      g_ft_face = NULL;
static term_glyph_t g_glyph_cache[2][95]; /* [0=Regular 400, 1=Bold 700] */

static void apply_term_var_weight(uint16_t weight) {
    if (!g_ft_face || !g_ft_lib) return;
    FT_MM_Var* mm_var = NULL;
    if (FT_Get_MM_Var(g_ft_face, &mm_var) != 0 || !mm_var) return;

    FT_Fixed coords[4];
    FT_UInt count = mm_var->num_axis;
    if (count > 4u) count = 4u;
    for (FT_UInt i = 0; i < count; ++i) {
        FT_Fixed val = mm_var->axis[i].def;
        if (mm_var->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
            FT_Fixed req = (FT_Fixed)((FT_Long)weight << 16);
            if (req < mm_var->axis[i].minimum) req = mm_var->axis[i].minimum;
            if (req > mm_var->axis[i].maximum) req = mm_var->axis[i].maximum;
            val = req;
        }
        coords[i] = val;
    }
    FT_Set_Var_Design_Coordinates(g_ft_face, count, coords);
    FT_Done_MM_Var(g_ft_lib, mm_var);
}

static void init_terminal_font(void) {
    memset(g_glyph_cache, 0, sizeof(g_glyph_cache));
    if (FT_Init_FreeType(&g_ft_lib) != 0) {
        g_ft_lib = NULL;
        return;
    }
    if (FT_New_Face(g_ft_lib, JETBRAINS_VAR_PATH, 0, &g_ft_face) != 0) {
        if (FT_New_Face(g_ft_lib, JETBRAINS_REGULAR_PATH, 0, &g_ft_face) != 0) {
            if (FT_New_Face(g_ft_lib, JETBRAINS_BOLD_PATH, 0, &g_ft_face) != 0) {
                if (FT_New_Face(g_ft_lib, INTER_VAR_PATH, 0, &g_ft_face) != 0) {
                    if (FT_New_Face(g_ft_lib, INTER_REGULAR_PATH, 0, &g_ft_face) != 0) {
                        g_ft_face = NULL;
                        return;
                    }
                }
            }
        }
    }
    FT_Set_Pixel_Sizes(g_ft_face, 0, TERM_FONT_PX);
}

static const term_glyph_t* get_term_glyph(uint8_t ch, bool bold) {
    if (!g_ft_face || ch < 32u || ch > 126u) {
        return NULL;
    }
    int style_idx = bold ? 1 : 0;
    term_glyph_t* g = &g_glyph_cache[style_idx][ch - 32u];
    if (g->valid) {
        return g->failed ? NULL : g;
    }
    g->valid = true;
    apply_term_var_weight(bold ? 700u : 400u);
    FT_Set_Pixel_Sizes(g_ft_face, 0, TERM_FONT_PX);
    if (FT_Load_Char(g_ft_face, (FT_ULong)ch, FT_LOAD_RENDER) != 0) {
        g->failed = true;
        return NULL;
    }
    FT_GlyphSlot slot = g_ft_face->glyph;
    FT_Bitmap*   bmp  = &slot->bitmap;
    g->bitmap_left = (int16_t)slot->bitmap_left;
    g->bitmap_top  = (int16_t)slot->bitmap_top;
    g->advance_x   = (int16_t)(slot->advance.x >> 6);
    g->width       = (uint16_t)bmp->width;
    g->rows        = (uint16_t)bmp->rows;
    g->alpha       = NULL;

    if (bmp->buffer != NULL && bmp->width > 0 && bmp->rows > 0) {
        size_t total = (size_t)bmp->width * (size_t)bmp->rows;
        g->alpha = (uint8_t*)malloc(total);
        if (g->alpha) {
            int pitch = (bmp->pitch < 0) ? -bmp->pitch : bmp->pitch;
            for (unsigned int r = 0; r < bmp->rows; ++r) {
                memcpy(g->alpha + ((size_t)r * bmp->width),
                       bmp->buffer + ((size_t)r * (unsigned int)pitch),
                       bmp->width);
            }
        }
    }
    return g;
}

/* ==============================================================================
 * Terminal Grid State & VT100 Parser
 * ============================================================================== */

typedef struct term_cell {
    char     ch;
    uint8_t  bold;
    uint32_t fg;
    uint32_t bg;
} term_cell_t;

typedef enum vt_state {
    VT_STATE_GROUND = 0,
    VT_STATE_ESC,
    VT_STATE_CSI,
    VT_STATE_OSC
} vt_state_t;

static term_cell_t g_grid[TERM_ROWS][TERM_COLS];
static bool        g_row_dirty[TERM_ROWS];
static int         g_cur_row      = 3;
static int         g_cur_col      = 0;
static uint32_t    g_cur_fg       = TERM_FG_DEFAULT;
static uint32_t    g_cur_bg       = TERM_BG_DEFAULT;
static uint8_t     g_cur_bold     = 0;
static int         g_banner_rows  = 3; /* Top 3 rows reserved for codename:alexia until scrolled/cleared */

static vt_state_t  g_vt_state     = VT_STATE_GROUND;
static int         g_csi_params[8];
static int         g_csi_num      = 0;
static bool        g_csi_priv     = false;

static void clear_cell(term_cell_t* c) {
    c->ch   = ' ';
    c->bold = 0;
    c->fg   = TERM_FG_DEFAULT;
    c->bg   = TERM_BG_DEFAULT;
}

static void mark_all_rows_dirty(void) {
    for (int r = 0; r < TERM_ROWS; ++r) {
        g_row_dirty[r] = true;
    }
}

static void term_reset_grid(bool show_banner) {
    for (int r = 0; r < TERM_ROWS; ++r) {
        for (int c = 0; c < TERM_COLS; ++c) {
            clear_cell(&g_grid[r][c]);
        }
        g_row_dirty[r] = true;
    }
    g_banner_rows = show_banner ? 3 : 0;
    g_cur_row     = show_banner ? 3 : 0;
    g_cur_col     = 0;
    g_cur_fg      = TERM_FG_DEFAULT;
    g_cur_bg      = TERM_BG_DEFAULT;
    g_cur_bold    = 0;
}

static void term_scroll_up(void) {
    if (g_banner_rows > 0) {
        g_banner_rows--;
    }
    for (int r = 0; r < TERM_ROWS - 1; ++r) {
        memcpy(g_grid[r], g_grid[r + 1], sizeof(g_grid[r]));
        g_row_dirty[r] = true;
    }
    for (int c = 0; c < TERM_COLS; ++c) {
        clear_cell(&g_grid[TERM_ROWS - 1][c]);
    }
    g_row_dirty[TERM_ROWS - 1] = true;
}

static void term_newline(void) {
    g_row_dirty[g_cur_row] = true;
    g_cur_row++;
    if (g_cur_row >= TERM_ROWS) {
        term_scroll_up();
        g_cur_row = TERM_ROWS - 1;
    }
    g_row_dirty[g_cur_row] = true;
}

static void term_put_char(char ch) {
    if (g_cur_col >= TERM_COLS) {
        g_cur_col = 0;
        term_newline();
    }
    term_cell_t* cell = &g_grid[g_cur_row][g_cur_col];
    cell->ch   = ch;
    cell->bold = g_cur_bold;
    cell->fg   = g_cur_bold ? TERM_FG_BRIGHT : g_cur_fg;
    cell->bg   = g_cur_bg;
    g_row_dirty[g_cur_row] = true;
    g_cur_col++;

    /*
     * Normalize Toybox's `[~/]` prompt expansion (when PWD == HOME) to `[~]`
     * so `admin@irisOS-dev [~/] $ ` displays cleanly as `admin@irisOS-dev [~] $ `.
     */
    if (ch == ']' && g_cur_col >= 4) {
        term_cell_t* row = g_grid[g_cur_row];
        if (row[g_cur_col - 4].ch == '[' &&
            row[g_cur_col - 3].ch == '~' &&
            row[g_cur_col - 2].ch == '/' &&
            row[g_cur_col - 1].ch == ']') {
            row[g_cur_col - 2] = row[g_cur_col - 1];
            clear_cell(&row[g_cur_col - 1]);
            g_cur_col--;
        }
    }
}

static void handle_sgr(void) {
    if (g_csi_num == 0) {
        g_csi_params[0] = 0;
        g_csi_num = 1;
    }
    for (int i = 0; i < g_csi_num; ++i) {
        int p = g_csi_params[i];
        if (p == 0) {
            g_cur_fg   = TERM_FG_DEFAULT;
            g_cur_bg   = TERM_BG_DEFAULT;
            g_cur_bold = 0;
        } else if (p == 1) {
            g_cur_bold = 1;
        } else if (p == 22) {
            g_cur_bold = 0;
        } else if (p >= 30 && p <= 37) {
            g_cur_fg = k_ansi_colors[(p - 30) + (g_cur_bold ? 8 : 0)];
        } else if (p == 39) {
            g_cur_fg = TERM_FG_DEFAULT;
        } else if (p >= 40 && p <= 47) {
            g_cur_bg = k_ansi_colors[p - 40];
        } else if (p == 49) {
            g_cur_bg = TERM_BG_DEFAULT;
        } else if (p >= 90 && p <= 97) {
            g_cur_fg = k_ansi_colors[(p - 90) + 8];
        }
    }
}

static void handle_csi_dispatch(char final_ch) {
    int p0 = (g_csi_num >= 1 && g_csi_params[0] > 0) ? g_csi_params[0] : 1;
    int p1 = (g_csi_num >= 2 && g_csi_params[1] > 0) ? g_csi_params[1] : 1;

    g_row_dirty[g_cur_row] = true;

    switch (final_ch) {
        case 'A': /* Cursor Up */
            g_cur_row -= p0;
            if (g_cur_row < g_banner_rows) g_cur_row = g_banner_rows;
            break;
        case 'B': /* Cursor Down */
            g_cur_row += p0;
            if (g_cur_row >= TERM_ROWS) g_cur_row = TERM_ROWS - 1;
            break;
        case 'C': /* Cursor Forward */
            g_cur_col += p0;
            if (g_cur_col >= TERM_COLS) g_cur_col = TERM_COLS - 1;
            break;
        case 'D': /* Cursor Back */
            g_cur_col -= p0;
            if (g_cur_col < 0) g_cur_col = 0;
            break;
        case 'G': /* Cursor Horizontal Absolute */
            g_cur_col = p0 - 1;
            if (g_cur_col < 0) g_cur_col = 0;
            if (g_cur_col >= TERM_COLS) g_cur_col = TERM_COLS - 1;
            break;
        case 'H':
        case 'f': /* Cursor Position */
            if (g_csi_num == 0 || (g_csi_params[0] <= 1 && (g_csi_num < 2 || g_csi_params[1] <= 1))) {
                g_cur_row = g_banner_rows;
                g_cur_col = 0;
            } else {
                g_cur_row = p0 - 1;
                g_cur_col = p1 - 1;
                if (g_cur_row < 0) g_cur_row = 0;
                if (g_cur_row >= TERM_ROWS) g_cur_row = TERM_ROWS - 1;
                if (g_cur_col < 0) g_cur_col = 0;
                if (g_cur_col >= TERM_COLS) g_cur_col = TERM_COLS - 1;
            }
            break;
        case 'J': { /* Erase in Display */
            int mode = (g_csi_num >= 1) ? g_csi_params[0] : 0;
            if (mode == 2 || mode == 3) {
                term_reset_grid(false);
            } else if (mode == 0) {
                for (int c = g_cur_col; c < TERM_COLS; ++c) {
                    clear_cell(&g_grid[g_cur_row][c]);
                }
                for (int r = g_cur_row + 1; r < TERM_ROWS; ++r) {
                    for (int c = 0; c < TERM_COLS; ++c) {
                        clear_cell(&g_grid[r][c]);
                    }
                    g_row_dirty[r] = true;
                }
            }
            break;
        }
        case 'K': { /* Erase in Line */
            int mode = (g_csi_num >= 1) ? g_csi_params[0] : 0;
            if (mode == 0) {
                for (int c = g_cur_col; c < TERM_COLS; ++c) {
                    clear_cell(&g_grid[g_cur_row][c]);
                }
            } else if (mode == 1) {
                for (int c = 0; c <= g_cur_col && c < TERM_COLS; ++c) {
                    clear_cell(&g_grid[g_cur_row][c]);
                }
            } else if (mode == 2) {
                for (int c = 0; c < TERM_COLS; ++c) {
                    clear_cell(&g_grid[g_cur_row][c]);
                }
            }
            break;
        }
        case 'm': /* Select Graphic Rendition */
            handle_sgr();
            break;
        default:
            break;
    }

    g_row_dirty[g_cur_row] = true;
}

static void term_feed_byte(uint8_t b, iris_window_t* win) {
    (void)win;
    switch (g_vt_state) {
        case VT_STATE_GROUND:
            if (b == 0x1Bu) { /* ESC */
                g_vt_state = VT_STATE_ESC;
            } else if (b == '\r') {
                g_row_dirty[g_cur_row] = true;
                g_cur_col = 0;
            } else if (b == '\n') {
                g_row_dirty[g_cur_row] = true;
                g_cur_col = 0;
                term_newline();
            } else if (b == '\b' || b == 0x7Fu) {
                if (g_cur_col > 0) {
                    g_cur_col--;
                    clear_cell(&g_grid[g_cur_row][g_cur_col]);
                    g_row_dirty[g_cur_row] = true;
                }
            } else if (b == '\t') {
                int next_tab = (g_cur_col + 4) & ~3;
                while (g_cur_col < next_tab && g_cur_col < TERM_COLS) {
                    term_put_char(' ');
                }
            } else if (b >= 32u && b <= 126u) {
                term_put_char((char)b);
            }
            break;

        case VT_STATE_ESC:
            if (b == '[') {
                g_vt_state = VT_STATE_CSI;
                memset(g_csi_params, 0, sizeof(g_csi_params));
                g_csi_num  = 0;
                g_csi_priv = false;
            } else if (b == ']') {
                g_vt_state = VT_STATE_OSC;
            } else if (b == 'c') {
                term_reset_grid(false);
                g_vt_state = VT_STATE_GROUND;
            } else {
                g_vt_state = VT_STATE_GROUND;
            }
            break;

        case VT_STATE_CSI:
            if (b == '?') {
                g_csi_priv = true;
            } else if (b >= '0' && b <= '9') {
                if (g_csi_num == 0) g_csi_num = 1;
                g_csi_params[g_csi_num - 1] = g_csi_params[g_csi_num - 1] * 10 + (b - '0');
            } else if (b == ';') {
                if (g_csi_num == 0) g_csi_num = 1;
                if (g_csi_num < 8) {
                    g_csi_num++;
                }
            } else if (b >= 0x40u && b <= 0x7Eu) {
                if (!g_csi_priv) {
                    handle_csi_dispatch((char)b);
                }
                g_vt_state = VT_STATE_GROUND;
            } else {
                g_vt_state = VT_STATE_GROUND;
            }
            break;

        case VT_STATE_OSC:
            if (b == 0x07u || b == '\n') {
                g_vt_state = VT_STATE_GROUND;
            }
            break;
    }
}

/* ==============================================================================
 * Surface Pixel Rendering
 * ============================================================================== */

static void fill_rect_raw(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                          int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (x < 0) {
        if ((uint32_t)(-x) >= w) return;
        w -= (uint32_t)(-x);
        x = 0;
    }
    if (y < 0) {
        if ((uint32_t)(-y) >= h) return;
        h -= (uint32_t)(-y);
        y = 0;
    }
    if ((uint32_t)x + w > buf_w) {
        if ((uint32_t)x >= buf_w) return;
        w = buf_w - (uint32_t)x;
    }
    if ((uint32_t)y + h > buf_h) {
        if ((uint32_t)y >= buf_h) return;
        h = buf_h - (uint32_t)y;
    }
    for (uint32_t ry = 0; ry < h; ++ry) {
        uint32_t* row = pixels + ((size_t)(y + (int32_t)ry) * buf_w) + (size_t)x;
        for (uint32_t rx = 0; rx < w; ++rx) {
            row[rx] = color;
        }
    }
}

static void draw_codename_banner_raw(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                                     int32_t x, int32_t y) {
    const char* text = "codename:alexia";
    const uint32_t sx = 3u;
    const uint32_t sy = 4u;
    int32_t pen_x = x;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        int idx = block_glyph_index(text[i]);
        if (idx >= 0) {
            for (int r = 0; r < 9; ++r) {
                uint8_t row_bits = k_block_glyphs[idx][r];
                for (int c = 0; c < 5; ++c) {
                    if (row_bits & (0x10u >> c)) {
                        fill_rect_raw(pixels, buf_w, buf_h,
                                      pen_x + c * (int32_t)sx,
                                      y + r * (int32_t)sy,
                                      sx, sy, 0xFFFFFFFFu);
                    }
                }
            }
        }
        pen_x += (text[i] == ':' || text[i] == 'i' || text[i] == 'l') ? 14 : 18;
    }
}

static void draw_glyph_cell(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                            int32_t cell_x, int32_t cell_y, char ch, bool bold, uint32_t argb_color) {
    uint8_t uch = (uint8_t)ch;
    if (uch <= 32u || uch > 126u) {
        return;
    }

    const term_glyph_t* g = get_term_glyph(uch, bold);
    if (!g) {
        /* 8x8 bitmap fallback */
        const uint8_t* bits = k_font8x8[uch - 32u];
        int32_t gy = cell_y + 4;
        for (int r = 0; r < 8; ++r) {
            int32_t py = gy + r;
            if (py < 0 || (uint32_t)py >= buf_h) continue;
            uint8_t row_bits = bits[r];
            for (int c = 0; c < 8; ++c) {
                if (row_bits & (0x80u >> c)) {
                    int32_t px = cell_x + c;
                    if (px >= 0 && (uint32_t)px < buf_w) {
                        pixels[(size_t)py * buf_w + (size_t)px] = argb_color;
                    }
                }
            }
        }
        return;
    }

    if (!g->alpha || g->width == 0 || g->rows == 0) {
        return;
    }

    uint32_t fg_r = (argb_color >> 16) & 0xFFu;
    uint32_t fg_g = (argb_color >>  8) & 0xFFu;
    uint32_t fg_b = (argb_color >>  0) & 0xFFu;
    uint32_t solid_fg = 0xFF000000u | (fg_r << 16) | (fg_g << 8) | fg_b;

    int32_t baseline_y = cell_y + (int32_t)TERM_FONT_PX;
    int32_t offset_x   = 0;
    if (g->advance_x > 0 && g->advance_x < TERM_CELL_W) {
        offset_x = (TERM_CELL_W - (int32_t)g->advance_x) / 2;
    }

    for (uint16_t row = 0; row < g->rows; ++row) {
        int32_t dst_y = baseline_y - g->bitmap_top + (int32_t)row;
        if (dst_y < 0 || (uint32_t)dst_y >= buf_h) {
            continue;
        }
        const uint8_t* src_row = g->alpha + ((size_t)row * g->width);
        uint32_t*      dst_row = pixels + ((size_t)dst_y * buf_w);

        for (uint16_t col = 0; col < g->width; ++col) {
            int32_t dst_x = cell_x + offset_x + g->bitmap_left + (int32_t)col;
            if (dst_x < 0 || (uint32_t)dst_x >= buf_w) {
                continue;
            }
            uint32_t cov = src_row[col];
            if (cov == 0) {
                continue;
            }
            if (cov >= 255u) {
                dst_row[dst_x] = solid_fg;
            } else {
                uint32_t bg   = dst_row[dst_x];
                uint32_t bg_a = (bg >> 24) & 0xFFu;
                uint32_t bg_r = (bg >> 16) & 0xFFu;
                uint32_t bg_g = (bg >>  8) & 0xFFu;
                uint32_t bg_b = (bg >>  0) & 0xFFu;
                uint32_t inv  = 255u - cov;
                uint32_t ta   = 255u * cov + bg_a * inv + 128u;
                uint32_t tr   = fg_r * cov + bg_r * inv + 128u;
                uint32_t tg   = fg_g * cov + bg_g * inv + 128u;
                uint32_t tb   = fg_b * cov + bg_b * inv + 128u;
                uint32_t out_a = (ta + (ta >> 8)) >> 8;
                uint32_t out_r = (tr + (tr >> 8)) >> 8;
                uint32_t out_g = (tg + (tg >> 8)) >> 8;
                uint32_t out_b = (tb + (tb >> 8)) >> 8;
                dst_row[dst_x] = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
    }
}

/*
 * Check if row `r` looks like a shell prompt (`user@host [dir] $ ...`) and render
 * the prefix seamlessly with proportional kerning, then place typed characters on
 * the grid after the prompt.
 */
static int find_prompt_prefix_end(int r) {
    const term_cell_t* row = g_grid[r];
    int dollar_col = -1;
    for (int c = 0; c < TERM_COLS - 1; ++c) {
        if (row[c].ch == '$' && row[c + 1].ch == ' ') {
            /* Verify '[' and ']' precede '$' */
            bool has_brackets = false;
            for (int k = 0; k < c; ++k) {
                if (row[k].ch == '[') {
                    has_brackets = true;
                    break;
                }
            }
            if (has_brackets) {
                dollar_col = c + 2;
            }
            break;
        }
    }
    return dollar_col;
}

static int32_t draw_proportional_run(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                                     int32_t x, int32_t cell_y, const char* str, uint32_t argb_color) {
    uint32_t fg_r = (argb_color >> 16) & 0xFFu;
    uint32_t fg_g = (argb_color >>  8) & 0xFFu;
    uint32_t fg_b = (argb_color >>  0) & 0xFFu;
    uint32_t solid_fg = 0xFF000000u | (fg_r << 16) | (fg_g << 8) | fg_b;

    int32_t pen_x      = x;
    int32_t baseline_y = cell_y + (int32_t)TERM_FONT_PX;

    for (size_t i = 0; str[i] != '\0'; ++i) {
        uint8_t uch = (uint8_t)str[i];
        const term_glyph_t* g = get_term_glyph(uch, false);
        if (!g) {
            draw_glyph_cell(pixels, buf_w, buf_h, pen_x, cell_y, (char)uch, false, argb_color);
            pen_x += TERM_CELL_W;
            continue;
        }
        if (g->alpha && g->width > 0 && g->rows > 0) {
            for (uint16_t row = 0; row < g->rows; ++row) {
                int32_t dst_y = baseline_y - g->bitmap_top + (int32_t)row;
                if (dst_y < 0 || (uint32_t)dst_y >= buf_h) continue;
                const uint8_t* src_row = g->alpha + ((size_t)row * g->width);
                uint32_t*      dst_row = pixels + ((size_t)dst_y * buf_w);
                for (uint16_t col = 0; col < g->width; ++col) {
                    int32_t dst_x = pen_x + g->bitmap_left + (int32_t)col;
                    if (dst_x < 0 || (uint32_t)dst_x >= buf_w) continue;
                    uint32_t cov = src_row[col];
                    if (cov == 0) continue;
                    if (cov >= 255u) {
                        dst_row[dst_x] = solid_fg;
                    } else {
                        uint32_t bg   = dst_row[dst_x];
                        uint32_t bg_a = (bg >> 24) & 0xFFu;
                        uint32_t bg_r = (bg >> 16) & 0xFFu;
                        uint32_t bg_g = (bg >>  8) & 0xFFu;
                        uint32_t bg_b = (bg >>  0) & 0xFFu;
                        uint32_t inv  = 255u - cov;
                        uint32_t ta   = 255u * cov + bg_a * inv + 128u;
                        uint32_t tr   = fg_r * cov + bg_r * inv + 128u;
                        uint32_t tg   = fg_g * cov + bg_g * inv + 128u;
                        uint32_t tb   = fg_b * cov + bg_b * inv + 128u;
                        dst_row[dst_x] = (((ta + (ta >> 8)) >> 8) << 24) |
                                         (((tr + (tr >> 8)) >> 8) << 16) |
                                         (((tg + (tg >> 8)) >> 8) <<  8) |
                                         (((tb + (tb >> 8)) >> 8));
                    }
                }
            }
        }
        pen_x += g->advance_x;
    }
    return pen_x;
}

static bool render_terminal_surface(iris_window_t* win, bool force_full) {
    if (!win || !win->pixels) {
        return false;
    }

    int32_t min_dirty_y = (int32_t)win->height;
    int32_t max_dirty_y = 0;
    bool    any_dirty   = force_full;

    if (force_full) {
        fill_rect_raw(win->pixels, win->width, win->height,
                      0, 0, win->width, win->height, TERM_BG_DEFAULT);
        min_dirty_y = 0;
        max_dirty_y = (int32_t)win->height;
        mark_all_rows_dirty();
    }

    /* If the blocky codename:alexia header banner is visible, render it in the top rows */
    if (g_banner_rows > 0 && (force_full || g_row_dirty[0] || g_row_dirty[1] || g_row_dirty[2])) {
        uint32_t banner_h = (uint32_t)g_banner_rows * TERM_CELL_H + 8u;
        fill_rect_raw(win->pixels, win->width, win->height,
                      0, 0, win->width, banner_h, TERM_BG_DEFAULT);
        /* Vertical offset scrolls smoothly if banner_rows < 3 */
        int32_t banner_y = 16 - (3 - g_banner_rows) * TERM_CELL_H;
        draw_codename_banner_raw(win->pixels, win->width, win->height, TERM_PAD_X, banner_y);
        for (int r = 0; r < g_banner_rows; ++r) {
            g_row_dirty[r] = false;
        }
        min_dirty_y = 0;
        if ((int32_t)banner_h > max_dirty_y) {
            max_dirty_y = (int32_t)banner_h;
        }
        any_dirty = true;
    }

    for (int r = g_banner_rows; r < TERM_ROWS; ++r) {
        if (!g_row_dirty[r]) {
            continue;
        }
        g_row_dirty[r] = false;
        any_dirty = true;

        int32_t row_y = TERM_PAD_Y + r * TERM_CELL_H;
        fill_rect_raw(win->pixels, win->width, win->height,
                      0, row_y, win->width, TERM_CELL_H, TERM_BG_DEFAULT);

        int prompt_end = find_prompt_prefix_end(r);
        int start_col  = 0;
        int32_t base_x = TERM_PAD_X;

        if (prompt_end > 0) {
            char prefix_buf[TERM_COLS + 1];
            for (int c = 0; c < prompt_end; ++c) {
                prefix_buf[c] = g_grid[r][c].ch;
            }
            prefix_buf[prompt_end] = '\0';
            base_x = draw_proportional_run(win->pixels, win->width, win->height,
                                           TERM_PAD_X, row_y, prefix_buf, TERM_FG_DEFAULT);
            start_col = prompt_end;
        }

        for (int c = start_col; c < TERM_COLS; ++c) {
            const term_cell_t* cell = &g_grid[r][c];
            int32_t cell_x = (prompt_end > 0)
                           ? (base_x + (c - start_col) * TERM_CELL_W)
                           : (TERM_PAD_X + c * TERM_CELL_W);
            if (cell->bg != TERM_BG_DEFAULT) {
                fill_rect_raw(win->pixels, win->width, win->height,
                              cell_x, row_y, TERM_CELL_W, TERM_CELL_H, cell->bg);
            }
            if (cell->ch > ' ') {
                draw_glyph_cell(win->pixels, win->width, win->height,
                                cell_x, row_y, cell->ch, cell->bold != 0, cell->fg);
            }
        }

        if (r == g_cur_row && g_cur_col >= 0 && g_cur_col < TERM_COLS) {
            int32_t cur_x = (prompt_end > 0 && g_cur_col >= start_col)
                          ? (base_x + (g_cur_col - start_col) * TERM_CELL_W)
                          : (TERM_PAD_X + g_cur_col * TERM_CELL_W);
            fill_rect_raw(win->pixels, win->width, win->height,
                          cur_x, row_y + TERM_FONT_PX, 7u, 2u, TERM_CURSOR_COLOR);
        }

        if (row_y < min_dirty_y) min_dirty_y = row_y;
        if (row_y + TERM_CELL_H > max_dirty_y) max_dirty_y = row_y + TERM_CELL_H;
    }

    if (any_dirty && max_dirty_y > min_dirty_y) {
        if (min_dirty_y < 0) min_dirty_y = 0;
        if ((uint32_t)max_dirty_y > win->height) max_dirty_y = (int32_t)win->height;
        iris_window_damage(win, 0u, (uint32_t)min_dirty_y,
                           win->width, (uint32_t)(max_dirty_y - min_dirty_y));
    }

    return any_dirty;
}

/* ==============================================================================
 * Shell Spawning Helper
 * ============================================================================== */

static pid_t spawn_interactive_shell(zx_handle_t pty_ch) {
    /* Drain any leftover bytes in the PTY output buffer before launching sh */
    if (pty_ch != ZX_HANDLE_INVALID) {
        char drain[256];
        for (int i = 0; i < 16; ++i) {
            size_t got = 0;
            if (iris_pty_read_output(pty_ch, drain, sizeof(drain), &got) != ZX_OK || got == 0) {
                break;
            }
        }
    }

    /* Ensure /Users/admin exists and start in the user's home directory */
    mkdir("/Users", 0755);
    mkdir("/Users/admin", 0755);
    if (chdir("/Users/admin") != 0) {
        chdir("/");
    }

    char hostname[64];
    memset(hostname, 0, sizeof(hostname));
    if (gethostname(hostname, sizeof(hostname) - 1) != 0 || hostname[0] == '\0') {
        strcpy(hostname, "irisOS-dev");
    }

    const char* user = getenv("USER");
    if (!user || user[0] == '\0') {
        user = "admin";
    }

    static char env_home[] = "HOME=/Users/admin";
    static char env_path[] = "PATH=/System/Bin:/bin";
    static char env_term[] = "TERM=vt100";
    static char env_user[64];
    static char env_ps1[128];

    snprintf(env_user, sizeof(env_user), "USER=%s", user);
    snprintf(env_ps1, sizeof(env_ps1), "PS1=%s@\\h [\\w] $ ", user);

    char* sh_argv[] = { (char*)"sh", (char*)"-is", NULL };
    char* sh_envp[] = {
        env_home,
        env_path,
        env_term,
        env_user,
        env_ps1,
        NULL
    };

    const char* sh_path = "/System/Bin/sh";
    struct stat st;
    if (stat(sh_path, &st) != 0) {
        sh_path = "/bin/sh";
    }

    pid_t pid = -1;
    int rc = posix_spawn(&pid, sh_path, NULL, NULL, sh_argv, sh_envp);
    if (rc != 0) {
        return -1;
    }
    return pid;
}

/* ==============================================================================
 * Main Entrypoint
 * ============================================================================== */

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    /*
     * Retrieve the PTY channel from mlibc's process environment or PA_USER0(0).
     * Notice that mlibc's `_start` consumes PA_HND(PA_USER0, 0) into FD 0/1/2,
     * OR `iris_init` can pass a dedicated PTY handle in PA_HND(PA_USER0, 2).
     */
    zx_handle_t pty_ch = zx_take_startup_handle(PA_HND(PA_USER0, 2));
    if (pty_ch == ZX_HANDLE_INVALID) {
        pty_ch = zx_take_startup_handle(PA_HND(PA_USER0, 0));
    }

    init_terminal_font();
    term_reset_grid(true);

    iris_window_t* win = iris_window_create_ex("toybox - ~/",
                                               TERM_WIN_W,
                                               TERM_WIN_H,
                                               IRIS_WIN_FLAG_TRANSLUCENT,
                                               TERM_WIN_X,
                                               TERM_WIN_Y);
    if (!win) {
        return 1;
    }

    /* Render initial window frame with banner immediately */
    render_terminal_surface(win, true);

    /* Launch interactive Toybox shell `/bin/sh -is` */
    pid_t sh_pid = spawn_interactive_shell(pty_ch);

    bool running = true;
    while (running) {
        bool did_work = false;

        /* 1. Poll compositor input events (keyboard & mouse) */
        iris_compositor_poll_event_resp_t ev;
        memset(&ev, 0, sizeof(ev));
        if (iris_window_poll_event(win, &ev) == ZX_OK && ev.num_chars > 0) {
            did_work = true;
            if (pty_ch != ZX_HANDLE_INVALID) {
                for (uint32_t i = 0; i < ev.num_chars && i < sizeof(ev.chars); ++i) {
                    uint8_t ch = (uint8_t)ev.chars[i];
                    if (ch == 0x0Cu) { /* Ctrl+L clears screen */
                        term_reset_grid(false);
                        iris_pty_inject(pty_ch, "\n", 1);
                    } else {
                        iris_pty_inject(pty_ch, &ev.chars[i], 1);
                    }
                }
            }
        }

        /* 2. Drain any PTY output produced by Toybox `sh` or child commands */
        if (pty_ch != ZX_HANDLE_INVALID) {
            char out_buf[512];
            for (int iter = 0; iter < 8; ++iter) {
                size_t got = 0;
                zx_status_t rst = iris_pty_read_output(pty_ch, out_buf, sizeof(out_buf), &got);
                if (rst != ZX_OK || got == 0) {
                    break;
                }
                did_work = true;
                for (size_t i = 0; i < got; ++i) {
                    term_feed_byte((uint8_t)out_buf[i], win);
                }
            }
        }

        /* 3. Re-render any dirty rows and flush damage to iris_compositor */
        if (render_terminal_surface(win, false)) {
            did_work = true;
        }

        /* 4. Check if the shell process exited (e.g. `exit` or `Ctrl+D`) */
        if (sh_pid > 0) {
            int wstatus = 0;
            pid_t w = waitpid(sh_pid, &wstatus, WNOHANG);
            if (w == sh_pid) {
                /* Respawn shell so the terminal window stays usable */
                sh_pid = spawn_interactive_shell(pty_ch);
            }
        }

        if (!did_work) {
            zx_nanosleep(zx_deadline_after(ZX_MSEC(8)));
        }
    }

    iris_window_destroy(win);
    return 0;
}
