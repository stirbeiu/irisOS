/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_compositor/src/font.c
 * Purpose: FreeType 2 (Inter Display Regular/Bold/Italic) & 8x8 Fallback Text Renderer.
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system server.
 */

#include "compositor.h"

/* FreeType 2 State */
static FT_Library g_ft_lib = NULL;
static FT_Face    g_ft_faces[FONT_STYLE_COUNT] = { NULL, NULL, NULL };

/* ==============================================================================
 * Embedded 8x8 Bitmap Font (Fallback)
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

void draw_string_8x8(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                     int32_t x, int32_t y, const char* str, uint32_t color) {
    if (!pixels || !str || y < 0 || (uint32_t)(y + 8) > buf_h) {
        return;
    }

    int32_t cx = x;
    for (size_t i = 0; str[i] != '\0'; ++i) {
        uint8_t ch = (uint8_t)str[i];
        if (ch < 32 || ch > 126) {
            ch = '?';
        }
        const uint8_t* glyph = k_font8x8[ch - 32];
        for (int row = 0; row < 8; ++row) {
            int32_t py = y + row;
            if (py < 0 || (uint32_t)py >= buf_h) continue;
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; ++col) {
                if (bits & (0x80u >> col)) {
                    int32_t px = cx + col;
                    if (px >= 0 && (uint32_t)px < buf_w) {
                        pixels[(size_t)py * buf_w + (size_t)px] = color;
                    }
                }
            }
        }
        cx += 8;
        if ((uint32_t)cx >= buf_w) {
            break;
        }
    }
}

/* ==============================================================================
 * FreeType 2 Subsystem Initialization & Anti-Aliased Rendering
 * ============================================================================== */

zx_status_t init_font_subsystem(void) {
    if (FT_Init_FreeType(&g_ft_lib) != 0) {
        log_status(STATUS_FAIL, "iris_compositor", "Failed to initialize FreeType 2 engine.");
        return ZX_ERR_INTERNAL;
    }

    /* 1. Try loading Inter Display Regular (or legacy Inter.otf) */
    if (FT_New_Face(g_ft_lib, INTER_REGULAR_PATH, 0, &g_ft_faces[FONT_STYLE_REGULAR]) != 0) {
        if (FT_New_Face(g_ft_lib, INTER_LEGACY_PATH, 0, &g_ft_faces[FONT_STYLE_REGULAR]) != 0) {
            log_status(STATUS_WARN, "iris_compositor",
                       "Could not load Inter Display Regular from %s or %s; using 8x8 fallback.",
                       INTER_REGULAR_PATH, INTER_LEGACY_PATH);
            return ZX_OK;
        }
    }

    /* 2. Try loading Inter Display Bold */
    if (FT_New_Face(g_ft_lib, INTER_BOLD_PATH, 0, &g_ft_faces[FONT_STYLE_BOLD]) != 0) {
        g_ft_faces[FONT_STYLE_BOLD] = g_ft_faces[FONT_STYLE_REGULAR];
    }

    /* 3. Try loading Inter Display Italic */
    if (FT_New_Face(g_ft_lib, INTER_ITALIC_PATH, 0, &g_ft_faces[FONT_STYLE_ITALIC]) != 0) {
        g_ft_faces[FONT_STYLE_ITALIC] = g_ft_faces[FONT_STYLE_REGULAR];
    }

    log_status(STATUS_OKAY, "iris_compositor",
               "Initialized FreeType 2 typography (Inter Display Regular/Bold/Italic loaded).");
    return ZX_OK;
}

/* ==============================================================================
 * O(1) FreeType Glyph Bitmap & Metrics Cache
 * ============================================================================== */

#define GLYPH_CACHE_MIN_CHAR   32u
#define GLYPH_CACHE_MAX_CHAR   126u
#define GLYPH_CACHE_NUM_CHARS  (GLYPH_CACHE_MAX_CHAR - GLYPH_CACHE_MIN_CHAR + 1u)
#define GLYPH_CACHE_MAX_SIZES  8u

typedef struct cached_glyph {
    bool     valid;
    bool     load_failed;
    int16_t  bitmap_left;
    int16_t  bitmap_top;
    int16_t  advance_x;
    uint16_t width;
    uint16_t rows;
    uint8_t* alpha;
} cached_glyph_t;

typedef struct cached_size_bucket {
    uint32_t       pixel_size;
    cached_glyph_t glyphs[GLYPH_CACHE_NUM_CHARS];
} cached_size_bucket_t;

static cached_size_bucket_t g_glyph_cache[FONT_STYLE_COUNT][GLYPH_CACHE_MAX_SIZES];

static const cached_glyph_t* get_cached_glyph(font_style_t style, FT_Face face,
                                              uint32_t pixel_size, uint8_t ch) {
    if (ch < GLYPH_CACHE_MIN_CHAR || ch > GLYPH_CACHE_MAX_CHAR || style >= FONT_STYLE_COUNT) {
        return NULL;
    }

    cached_size_bucket_t* buckets = g_glyph_cache[style];
    cached_size_bucket_t* bucket  = NULL;

    for (uint32_t i = 0; i < GLYPH_CACHE_MAX_SIZES; ++i) {
        if (buckets[i].pixel_size == pixel_size) {
            bucket = &buckets[i];
            break;
        }
        if (buckets[i].pixel_size == 0 && bucket == NULL) {
            buckets[i].pixel_size = pixel_size;
            bucket = &buckets[i];
            break;
        }
    }

    /* If all size buckets are full, evict bucket 0 */
    if (!bucket) {
        bucket = &buckets[0];
        for (uint32_t c = 0; c < GLYPH_CACHE_NUM_CHARS; ++c) {
            if (bucket->glyphs[c].alpha) {
                free(bucket->glyphs[c].alpha);
            }
        }
        memset(bucket, 0, sizeof(*bucket));
        bucket->pixel_size = pixel_size;
    }

    uint32_t idx = (uint32_t)(ch - GLYPH_CACHE_MIN_CHAR);
    cached_glyph_t* g = &bucket->glyphs[idx];
    if (g->valid) {
        return g->load_failed ? NULL : g;
    }

    g->valid = true;
    FT_Set_Pixel_Sizes(face, 0, pixel_size);
    if (FT_Load_Char(face, (FT_ULong)ch, FT_LOAD_RENDER) != 0) {
        g->load_failed = true;
        return NULL;
    }

    FT_GlyphSlot slot = face->glyph;
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
            int pitch = bmp->pitch < 0 ? -bmp->pitch : bmp->pitch;
            for (unsigned int r = 0; r < bmp->rows; ++r) {
                memcpy(g->alpha + ((size_t)r * bmp->width),
                       bmp->buffer + ((size_t)r * (unsigned int)pitch),
                       bmp->width);
            }
        }
    }

    return g;
}

void cleanup_font_subsystem(void) {
    for (int s = 0; s < FONT_STYLE_COUNT; ++s) {
        for (uint32_t b = 0; b < GLYPH_CACHE_MAX_SIZES; ++b) {
            for (uint32_t c = 0; c < GLYPH_CACHE_NUM_CHARS; ++c) {
                if (g_glyph_cache[s][b].glyphs[c].alpha != NULL) {
                    free(g_glyph_cache[s][b].glyphs[c].alpha);
                    g_glyph_cache[s][b].glyphs[c].alpha = NULL;
                }
            }
        }
    }
    memset(g_glyph_cache, 0, sizeof(g_glyph_cache));

    for (int i = 0; i < FONT_STYLE_COUNT; ++i) {
        if (g_ft_faces[i] != NULL) {
            /* If bold or italic pointed to regular, avoid double free */
            if (i > 0 && g_ft_faces[i] == g_ft_faces[FONT_STYLE_REGULAR]) {
                g_ft_faces[i] = NULL;
                continue;
            }
            FT_Done_Face(g_ft_faces[i]);
            g_ft_faces[i] = NULL;
        }
    }
    if (g_ft_lib != NULL) {
        FT_Done_FreeType(g_ft_lib);
        g_ft_lib = NULL;
    }
}

void draw_text_ft_styled(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                         int32_t x, int32_t y, uint32_t pixel_size,
                         font_style_t style, const char* str, uint32_t argb_color) {
    if (!pixels || !str) {
        return;
    }

    font_style_t resolved_style = (style < FONT_STYLE_COUNT) ? style : FONT_STYLE_REGULAR;
    FT_Face face = g_ft_faces[resolved_style];
    if (!face) {
        resolved_style = FONT_STYLE_REGULAR;
        face = g_ft_faces[FONT_STYLE_REGULAR];
    }
    if (!face) {
        draw_string_8x8(pixels, buf_w, buf_h, x, y, str, argb_color);
        return;
    }

    uint32_t fg_r = (argb_color >> 16) & 0xFFu;
    uint32_t fg_g = (argb_color >>  8) & 0xFFu;
    uint32_t fg_b = (argb_color >>  0) & 0xFFu;
    uint32_t solid_fg = 0xFF000000u | (fg_r << 16) | (fg_g << 8) | fg_b;

    int32_t pen_x      = x;
    int32_t baseline_y = y + (int32_t)pixel_size - 2;

    for (size_t i = 0; str[i] != '\0'; ++i) {
        uint8_t ch = (uint8_t)str[i];
        if (ch < GLYPH_CACHE_MIN_CHAR || ch > GLYPH_CACHE_MAX_CHAR) {
            continue;
        }

        const cached_glyph_t* g = get_cached_glyph(resolved_style, face, pixel_size, ch);
        if (!g) {
            continue;
        }

        if (g->alpha != NULL && g->width > 0 && g->rows > 0) {
            for (uint16_t row = 0; row < g->rows; ++row) {
                int32_t dst_y = baseline_y - g->bitmap_top + (int32_t)row;
                if (dst_y < 0 || (uint32_t)dst_y >= buf_h) {
                    continue;
                }
                const uint8_t* src_row = g->alpha + ((size_t)row * g->width);
                uint32_t*      dst_row = pixels + ((size_t)dst_y * buf_w);

                for (uint16_t col = 0; col < g->width; ++col) {
                    int32_t dst_x = pen_x + g->bitmap_left + (int32_t)col;
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
                        uint32_t bg    = dst_row[dst_x];
                        uint32_t bg_r  = (bg >> 16) & 0xFFu;
                        uint32_t bg_g  = (bg >>  8) & 0xFFu;
                        uint32_t bg_b  = (bg >>  0) & 0xFFu;
                        uint32_t inv   = 255u - cov;
                        uint32_t tr    = fg_r * cov + bg_r * inv + 128u;
                        uint32_t tg    = fg_g * cov + bg_g * inv + 128u;
                        uint32_t tb    = fg_b * cov + bg_b * inv + 128u;
                        uint32_t out_r = (tr + (tr >> 8)) >> 8;
                        uint32_t out_g = (tg + (tg >> 8)) >> 8;
                        uint32_t out_b = (tb + (tb >> 8)) >> 8;
                        dst_row[dst_x] = 0xFF000000u | (out_r << 16) | (out_g << 8) | out_b;
                    }
                }
            }
        }

        pen_x += g->advance_x;
        if (pen_x >= (int32_t)buf_w) {
            break;
        }
    }
}

void draw_text_ft(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                  int32_t x, int32_t y, uint32_t pixel_size,
                  const char* str, uint32_t argb_color) {
    draw_text_ft_styled(pixels, buf_w, buf_h, x, y, pixel_size, FONT_STYLE_REGULAR, str, argb_color);
}
