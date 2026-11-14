/*
 * irisOS - Modern 64-bit Microkernel Operating System
 *
 * File: servers/iris_compositor/src/font.c
 * Purpose: FreeType 2 Variable Font Engine (Inter Display, Inter, Inter Tight,
 *          JetBrains Mono, Material Icons) & 8x8 Fallback Text Renderer.
 *
 * Licensing: MIT License. Permissive Zone 1 / Ring 3 system server.
 */

#include "compositor.h"
#include FT_MULTIPLE_MASTERS_H

/* FreeType 2 State */
static FT_Library g_ft_lib = NULL;
static FT_Face    g_family_faces[FONT_FAMILY_COUNT][2]; /* [family][0=upright, 1=italic] */
static FT_Face    g_ft_material_face = NULL;

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
 * FreeType 2 Subsystem Initialization, Variable Axes & Anti-Aliased Rendering
 * ============================================================================== */

static void apply_variable_axes(FT_Face face, font_family_t family, uint16_t weight) {
    if (!face || !g_ft_lib) {
        return;
    }
    FT_MM_Var* mm_var = NULL;
    if (FT_Get_MM_Var(face, &mm_var) != 0 || !mm_var) {
        return;
    }

    FT_Fixed coords[8];
    FT_UInt count = mm_var->num_axis;
    if (count > 8u) count = 8u;

    for (FT_UInt i = 0; i < count; ++i) {
        FT_Fixed val = mm_var->axis[i].def;
        FT_ULong tag = mm_var->axis[i].tag;
        if (tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
            FT_Fixed req = (FT_Fixed)((FT_Long)weight << 16);
            if (req < mm_var->axis[i].minimum) req = mm_var->axis[i].minimum;
            if (req > mm_var->axis[i].maximum) req = mm_var->axis[i].maximum;
            val = req;
        } else if (tag == FT_MAKE_TAG('o', 'p', 's', 'z')) {
            FT_Long target_opsz = (family == FONT_FAMILY_INTER_DISPLAY) ? 32L : 14L;
            FT_Fixed req = (FT_Fixed)(target_opsz << 16);
            if (req < mm_var->axis[i].minimum) req = mm_var->axis[i].minimum;
            if (req > mm_var->axis[i].maximum) req = mm_var->axis[i].maximum;
            val = req;
        }
        coords[i] = val;
    }

    FT_Set_Var_Design_Coordinates(face, count, coords);
    FT_Done_MM_Var(g_ft_lib, mm_var);
}

static FT_Face load_face_with_fallback(const char* primary, const char* fallback1, const char* fallback2) {
    FT_Face face = NULL;
    if (primary && FT_New_Face(g_ft_lib, primary, 0, &face) == 0) {
        return face;
    }
    if (fallback1 && FT_New_Face(g_ft_lib, fallback1, 0, &face) == 0) {
        return face;
    }
    if (fallback2 && FT_New_Face(g_ft_lib, fallback2, 0, &face) == 0) {
        return face;
    }
    return NULL;
}

zx_status_t init_material_icons_from_memory(const uint8_t* ttf_bytes, size_t len) {
    if (!ttf_bytes || len == 0) return ZX_ERR_INVALID_ARGS;
    if (!g_ft_lib && FT_Init_FreeType(&g_ft_lib) != 0) {
        return ZX_ERR_INTERNAL;
    }
    if (g_ft_material_face) {
        FT_Done_Face(g_ft_material_face);
        g_ft_material_face = NULL;
    }
    if (FT_New_Memory_Face(g_ft_lib, ttf_bytes, (FT_Long)len, 0, &g_ft_material_face) != 0) {
        g_ft_material_face = NULL;
        return ZX_ERR_IO;
    }
    return ZX_OK;
}

zx_status_t init_font_subsystem(void) {
    memset(g_family_faces, 0, sizeof(g_family_faces));

    if (!g_ft_lib && FT_Init_FreeType(&g_ft_lib) != 0) {
        log_status(STATUS_FAIL, "iris_compositor", "Failed to initialize FreeType 2 engine.");
        return ZX_ERR_INTERNAL;
    }

    /* 1. Inter / Inter Display Variable (same VF with opsz=32 vs opsz=14) */
    g_family_faces[FONT_FAMILY_INTER_DISPLAY][0] =
        load_face_with_fallback(INTER_VAR_PATH, INTER_REGULAR_PATH, INTER_LEGACY_PATH);
    g_family_faces[FONT_FAMILY_INTER_DISPLAY][1] =
        load_face_with_fallback(INTER_IT_VAR_PATH, INTER_ITALIC_PATH, NULL);
    if (!g_family_faces[FONT_FAMILY_INTER_DISPLAY][1]) {
        g_family_faces[FONT_FAMILY_INTER_DISPLAY][1] = g_family_faces[FONT_FAMILY_INTER_DISPLAY][0];
    }

    g_family_faces[FONT_FAMILY_INTER][0] = g_family_faces[FONT_FAMILY_INTER_DISPLAY][0];
    g_family_faces[FONT_FAMILY_INTER][1] = g_family_faces[FONT_FAMILY_INTER_DISPLAY][1];

    /* 2. Inter Tight Variable */
    g_family_faces[FONT_FAMILY_INTER_TIGHT][0] =
        load_face_with_fallback(INTER_TIGHT_VAR_PATH, NULL, NULL);
    if (!g_family_faces[FONT_FAMILY_INTER_TIGHT][0]) {
        g_family_faces[FONT_FAMILY_INTER_TIGHT][0] = g_family_faces[FONT_FAMILY_INTER_DISPLAY][0];
    }
    g_family_faces[FONT_FAMILY_INTER_TIGHT][1] =
        load_face_with_fallback(INTER_TIGHT_IT_VAR_PATH, NULL, NULL);
    if (!g_family_faces[FONT_FAMILY_INTER_TIGHT][1]) {
        g_family_faces[FONT_FAMILY_INTER_TIGHT][1] = g_family_faces[FONT_FAMILY_INTER_DISPLAY][1];
    }

    /* 3. JetBrains Mono Variable */
    g_family_faces[FONT_FAMILY_JETBRAINS][0] =
        load_face_with_fallback(JETBRAINS_VAR_PATH, "/System/Resources/Fonts/JetBrainsMono-Regular.ttf", NULL);
    if (!g_family_faces[FONT_FAMILY_JETBRAINS][0]) {
        g_family_faces[FONT_FAMILY_JETBRAINS][0] = g_family_faces[FONT_FAMILY_INTER_DISPLAY][0];
    }
    g_family_faces[FONT_FAMILY_JETBRAINS][1] =
        load_face_with_fallback(JETBRAINS_IT_VAR_PATH, NULL, NULL);
    if (!g_family_faces[FONT_FAMILY_JETBRAINS][1]) {
        g_family_faces[FONT_FAMILY_JETBRAINS][1] = g_family_faces[FONT_FAMILY_JETBRAINS][0];
    }

    /* 4. Instrument Serif (Display Headers) */
    g_family_faces[FONT_FAMILY_INSTRUMENT_SERIF][0] =
        load_face_with_fallback(INSTRUMENT_SERIF_REG_PATH, NULL, NULL);
    if (!g_family_faces[FONT_FAMILY_INSTRUMENT_SERIF][0]) {
        g_family_faces[FONT_FAMILY_INSTRUMENT_SERIF][0] = g_family_faces[FONT_FAMILY_INTER_DISPLAY][0];
    }
    g_family_faces[FONT_FAMILY_INSTRUMENT_SERIF][1] =
        load_face_with_fallback(INSTRUMENT_SERIF_IT_PATH, NULL, NULL);
    if (!g_family_faces[FONT_FAMILY_INSTRUMENT_SERIF][1]) {
        g_family_faces[FONT_FAMILY_INSTRUMENT_SERIF][1] = g_family_faces[FONT_FAMILY_INSTRUMENT_SERIF][0];
    }

    /* 5. Material Icons (keep memory face if already loaded from BOOTFS) */
    if (!g_ft_material_face) {
        g_ft_material_face = load_face_with_fallback(MATERIAL_ICONS_PATH, NULL, NULL);
    }

    if (!g_family_faces[FONT_FAMILY_INTER_DISPLAY][0]) {
        log_status(STATUS_WARN, "iris_compositor",
                   "Could not load Inter fonts; using 8x8 fallback.");
        return ZX_OK;
    }

    log_status(STATUS_OKAY, "iris_compositor",
               "Initialized FreeType 2 variable typography (Inter Display/Tight, JetBrains Mono, Material Icons=%s).",
               g_ft_material_face ? "yes" : "no");
    return ZX_OK;
}

/* ==============================================================================
 * O(1) FreeType Glyph Bitmap & Metrics Cache
 * ============================================================================== */

#define GLYPH_CACHE_MIN_CHAR   32u
#define GLYPH_CACHE_MAX_CHAR   126u
#define GLYPH_CACHE_NUM_CHARS  (GLYPH_CACHE_MAX_CHAR - GLYPH_CACHE_MIN_CHAR + 1u)
#define GLYPH_CACHE_BUCKETS    32u
#define MAT_ICON_CACHE_SLOTS   64u

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

typedef struct cached_glyph_bucket {
    bool           in_use;
    font_family_t  family;
    uint16_t       weight;
    bool           italic;
    uint32_t       pixel_size;
    uint32_t       last_used;
    cached_glyph_t glyphs[GLYPH_CACHE_NUM_CHARS];
} cached_glyph_bucket_t;

typedef struct cached_mat_icon {
    bool           in_use;
    uint32_t       codepoint;
    uint32_t       pixel_size;
    uint32_t       last_used;
    cached_glyph_t glyph;
} cached_mat_icon_t;

static cached_glyph_bucket_t g_glyph_buckets[GLYPH_CACHE_BUCKETS];
static cached_mat_icon_t     g_mat_icon_cache[MAT_ICON_CACHE_SLOTS];
static uint32_t              g_cache_use_seq = 1u;

static void free_bucket_glyphs(cached_glyph_bucket_t* bucket) {
    for (uint32_t c = 0; c < GLYPH_CACHE_NUM_CHARS; ++c) {
        if (bucket->glyphs[c].alpha != NULL) {
            free(bucket->glyphs[c].alpha);
            bucket->glyphs[c].alpha = NULL;
        }
    }
    memset(bucket, 0, sizeof(*bucket));
}

static FT_Face resolve_face(font_family_t family, bool italic) {
    if (family >= FONT_FAMILY_COUNT) family = FONT_FAMILY_INTER_DISPLAY;
    FT_Face face = g_family_faces[family][italic ? 1 : 0];
    if (!face) face = g_family_faces[family][0];
    if (!face) face = g_family_faces[FONT_FAMILY_INTER_DISPLAY][0];
    return face;
}

static const cached_glyph_t* get_cached_glyph(font_family_t family, uint16_t weight, bool italic,
                                              FT_Face face, uint32_t pixel_size, uint8_t ch) {
    if (ch < GLYPH_CACHE_MIN_CHAR || ch > GLYPH_CACHE_MAX_CHAR || !face || pixel_size == 0) {
        return NULL;
    }

    cached_glyph_bucket_t* bucket = NULL;
    uint32_t lru_idx = 0;
    uint32_t lru_seq = UINT32_MAX;

    for (uint32_t i = 0; i < GLYPH_CACHE_BUCKETS; ++i) {
        if (g_glyph_buckets[i].in_use &&
            g_glyph_buckets[i].family == family &&
            g_glyph_buckets[i].weight == weight &&
            g_glyph_buckets[i].italic == italic &&
            g_glyph_buckets[i].pixel_size == pixel_size) {
            bucket = &g_glyph_buckets[i];
            break;
        }
        if (!g_glyph_buckets[i].in_use && bucket == NULL) {
            bucket = &g_glyph_buckets[i];
        } else if (g_glyph_buckets[i].in_use && g_glyph_buckets[i].last_used < lru_seq) {
            lru_seq = g_glyph_buckets[i].last_used;
            lru_idx = i;
        }
    }

    if (!bucket) {
        bucket = &g_glyph_buckets[lru_idx];
        free_bucket_glyphs(bucket);
    }
    if (!bucket->in_use) {
        bucket->in_use     = true;
        bucket->family     = family;
        bucket->weight     = weight;
        bucket->italic     = italic;
        bucket->pixel_size = pixel_size;
    }
    bucket->last_used = g_cache_use_seq++;

    uint32_t idx = (uint32_t)(ch - GLYPH_CACHE_MIN_CHAR);
    cached_glyph_t* g = &bucket->glyphs[idx];
    if (g->valid) {
        return g->load_failed ? NULL : g;
    }

    g->valid = true;
    apply_variable_axes(face, family, weight);
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

static const cached_glyph_t* get_cached_material_glyph(uint32_t codepoint, uint32_t pixel_size) {
    if (!g_ft_material_face || pixel_size == 0) {
        return NULL;
    }

    cached_mat_icon_t* slot_entry = NULL;
    uint32_t lru_idx = 0;
    uint32_t lru_seq = UINT32_MAX;

    for (uint32_t i = 0; i < MAT_ICON_CACHE_SLOTS; ++i) {
        if (g_mat_icon_cache[i].in_use &&
            g_mat_icon_cache[i].codepoint == codepoint &&
            g_mat_icon_cache[i].pixel_size == pixel_size) {
            g_mat_icon_cache[i].last_used = g_cache_use_seq++;
            return g_mat_icon_cache[i].glyph.load_failed ? NULL : &g_mat_icon_cache[i].glyph;
        }
        if (!g_mat_icon_cache[i].in_use && slot_entry == NULL) {
            slot_entry = &g_mat_icon_cache[i];
        } else if (g_mat_icon_cache[i].in_use && g_mat_icon_cache[i].last_used < lru_seq) {
            lru_seq = g_mat_icon_cache[i].last_used;
            lru_idx = i;
        }
    }

    if (!slot_entry) {
        slot_entry = &g_mat_icon_cache[lru_idx];
        if (slot_entry->glyph.alpha) {
            free(slot_entry->glyph.alpha);
        }
        memset(slot_entry, 0, sizeof(*slot_entry));
    }

    slot_entry->in_use     = true;
    slot_entry->codepoint  = codepoint;
    slot_entry->pixel_size = pixel_size;
    slot_entry->last_used  = g_cache_use_seq++;

    cached_glyph_t* g = &slot_entry->glyph;
    g->valid = true;

    FT_Set_Pixel_Sizes(g_ft_material_face, 0, pixel_size);
    if (FT_Load_Char(g_ft_material_face, (FT_ULong)codepoint, FT_LOAD_RENDER) != 0) {
        g->load_failed = true;
        return NULL;
    }

    FT_GlyphSlot slot = g_ft_material_face->glyph;
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

typedef struct unicode_glyph_entry {
    bool           in_use;
    font_family_t  family;
    uint16_t       weight;
    bool           italic;
    uint32_t       pixel_size;
    uint32_t       codepoint;
    uint32_t       last_used;
    cached_glyph_t glyph;
} unicode_glyph_entry_t;

#define UNICODE_CACHE_SLOTS 32u
static unicode_glyph_entry_t g_unicode_cache[UNICODE_CACHE_SLOTS];

void cleanup_font_subsystem(void) {
    for (uint32_t b = 0; b < GLYPH_CACHE_BUCKETS; ++b) {
        free_bucket_glyphs(&g_glyph_buckets[b]);
    }
    for (uint32_t i = 0; i < MAT_ICON_CACHE_SLOTS; ++i) {
        if (g_mat_icon_cache[i].glyph.alpha != NULL) {
            free(g_mat_icon_cache[i].glyph.alpha);
            g_mat_icon_cache[i].glyph.alpha = NULL;
        }
    }
    memset(g_mat_icon_cache, 0, sizeof(g_mat_icon_cache));
    for (uint32_t i = 0; i < UNICODE_CACHE_SLOTS; ++i) {
        if (g_unicode_cache[i].glyph.alpha != NULL) {
            free(g_unicode_cache[i].glyph.alpha);
            g_unicode_cache[i].glyph.alpha = NULL;
        }
    }
    memset(g_unicode_cache, 0, sizeof(g_unicode_cache));

    /* Deduplicate FT_Face pointers before calling FT_Done_Face */
    FT_Face unique_faces[FONT_FAMILY_COUNT * 2 + 1];
    size_t num_unique = 0;
    for (int f = 0; f < FONT_FAMILY_COUNT; ++f) {
        for (int s = 0; s < 2; ++s) {
            FT_Face face = g_family_faces[f][s];
            g_family_faces[f][s] = NULL;
            if (!face) continue;
            bool seen = false;
            for (size_t u = 0; u < num_unique; ++u) {
                if (unique_faces[u] == face) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                unique_faces[num_unique++] = face;
            }
        }
    }
    if (g_ft_material_face) {
        unique_faces[num_unique++] = g_ft_material_face;
        g_ft_material_face = NULL;
    }
    for (size_t u = 0; u < num_unique; ++u) {
        FT_Done_Face(unique_faces[u]);
    }
    if (g_ft_lib != NULL) {
        FT_Done_FreeType(g_ft_lib);
        g_ft_lib = NULL;
    }
}

static void blend_glyph_bitmap(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                               int32_t dst_x0, int32_t dst_y0,
                               const cached_glyph_t* g, uint32_t argb_color) {
    if (!g || !g->alpha || g->width == 0 || g->rows == 0) {
        return;
    }

    int32_t clip_x0 = 0;
    int32_t clip_y0 = 0;
    int32_t clip_x1 = (int32_t)buf_w;
    int32_t clip_y1 = (int32_t)buf_h;
    if (pixels == g_backbuf_pixels && g_active_clip.w > 0 && g_active_clip.h > 0) {
        if (g_active_clip.x > clip_x0) clip_x0 = g_active_clip.x;
        if (g_active_clip.y > clip_y0) clip_y0 = g_active_clip.y;
        int32_t cx1 = g_active_clip.x + (int32_t)g_active_clip.w;
        int32_t cy1 = g_active_clip.y + (int32_t)g_active_clip.h;
        if (cx1 < clip_x1) clip_x1 = cx1;
        if (cy1 < clip_y1) clip_y1 = cy1;
    }

    uint32_t fg_a = (argb_color >> 24) & 0xFFu;
    uint32_t fg_r = (argb_color >> 16) & 0xFFu;
    uint32_t fg_g = (argb_color >>  8) & 0xFFu;
    uint32_t fg_b = (argb_color >>  0) & 0xFFu;
    if (fg_a == 0) return;

    uint32_t solid_fg = 0xFF000000u | (fg_r << 16) | (fg_g << 8) | fg_b;

    for (uint16_t row = 0; row < g->rows; ++row) {
        int32_t dst_y = dst_y0 + (int32_t)row;
        if (dst_y < clip_y0 || dst_y >= clip_y1) {
            continue;
        }
        const uint8_t* src_row = g->alpha + ((size_t)row * g->width);
        uint32_t*      dst_row = pixels + ((size_t)dst_y * buf_w);

        for (uint16_t col = 0; col < g->width; ++col) {
            int32_t dst_x = dst_x0 + (int32_t)col;
            if (dst_x < clip_x0 || dst_x >= clip_x1) {
                continue;
            }
            uint32_t cov = src_row[col];
            if (cov == 0) {
                continue;
            }
            if (fg_a < 255u) {
                uint32_t tc = cov * fg_a + 128u;
                cov = (tc + (tc >> 8)) >> 8;
                if (cov == 0) continue;
            }
            if (cov >= 255u) {
                dst_row[dst_x] = solid_fg;
            } else {
                uint32_t bg    = dst_row[dst_x];
                uint32_t bg_a  = (bg >> 24) & 0xFFu;
                uint32_t bg_r  = (bg >> 16) & 0xFFu;
                uint32_t bg_g  = (bg >>  8) & 0xFFu;
                uint32_t bg_b  = (bg >>  0) & 0xFFu;
                uint32_t inv   = 255u - cov;
                uint32_t ta    = 255u * cov + bg_a * inv + 128u;
                uint32_t tr    = fg_r * cov + bg_r * inv + 128u;
                uint32_t tg    = fg_g * cov + bg_g * inv + 128u;
                uint32_t tb    = fg_b * cov + bg_b * inv + 128u;
                uint32_t out_a = (ta + (ta >> 8)) >> 8;
                uint32_t out_r = (tr + (tr >> 8)) >> 8;
                uint32_t out_g = (tg + (tg >> 8)) >> 8;
                uint32_t out_b = (tb + (tb >> 8)) >> 8;
                dst_row[dst_x] = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
            }
        }
    }
}

static uint32_t decode_utf8_cp(const char* s, size_t* io_idx) {
    uint8_t b0 = (uint8_t)s[*io_idx];
    if (b0 < 0x80u) {
        (*io_idx) += 1u;
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && (s[*io_idx + 1u] & 0xC0u) == 0x80u) {
        uint32_t cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)((uint8_t)s[*io_idx + 1u] & 0x3Fu);
        (*io_idx) += 2u;
        return cp;
    }
    if ((b0 & 0xF0u) == 0xE0u && (s[*io_idx + 1u] & 0xC0u) == 0x80u && (s[*io_idx + 2u] & 0xC0u) == 0x80u) {
        uint32_t cp = ((uint32_t)(b0 & 0x0Fu) << 12) |
                      ((uint32_t)((uint8_t)s[*io_idx + 1u] & 0x3Fu) << 6) |
                      (uint32_t)((uint8_t)s[*io_idx + 2u] & 0x3Fu);
        (*io_idx) += 3u;
        return cp;
    }
    if ((b0 & 0xF8u) == 0xF0u && (s[*io_idx + 1u] & 0xC0u) == 0x80u &&
        (s[*io_idx + 2u] & 0xC0u) == 0x80u && (s[*io_idx + 3u] & 0xC0u) == 0x80u) {
        uint32_t cp = ((uint32_t)(b0 & 0x07u) << 18) |
                      ((uint32_t)((uint8_t)s[*io_idx + 1u] & 0x3Fu) << 12) |
                      ((uint32_t)((uint8_t)s[*io_idx + 2u] & 0x3Fu) << 6) |
                      (uint32_t)((uint8_t)s[*io_idx + 3u] & 0x3Fu);
        (*io_idx) += 4u;
        return cp;
    }
    (*io_idx) += 1u;
    return 0xFFFDu;
}

static const cached_glyph_t* get_cached_unicode_glyph(font_family_t family, uint16_t weight, bool italic,
                                                      FT_Face face, uint32_t pixel_size, uint32_t cp) {
    if (!face || pixel_size == 0) return NULL;
    unicode_glyph_entry_t* slot_entry = NULL;
    uint32_t lru_idx = 0;
    uint32_t lru_seq = UINT32_MAX;

    for (uint32_t i = 0; i < UNICODE_CACHE_SLOTS; ++i) {
        if (g_unicode_cache[i].in_use &&
            g_unicode_cache[i].family == family &&
            g_unicode_cache[i].weight == weight &&
            g_unicode_cache[i].italic == italic &&
            g_unicode_cache[i].pixel_size == pixel_size &&
            g_unicode_cache[i].codepoint == cp) {
            g_unicode_cache[i].last_used = g_cache_use_seq++;
            return g_unicode_cache[i].glyph.load_failed ? NULL : &g_unicode_cache[i].glyph;
        }
        if (!g_unicode_cache[i].in_use && !slot_entry) {
            slot_entry = &g_unicode_cache[i];
        } else if (g_unicode_cache[i].in_use && g_unicode_cache[i].last_used < lru_seq) {
            lru_seq = g_unicode_cache[i].last_used;
            lru_idx = i;
        }
    }

    if (!slot_entry) {
        slot_entry = &g_unicode_cache[lru_idx];
        if (slot_entry->glyph.alpha) free(slot_entry->glyph.alpha);
        memset(slot_entry, 0, sizeof(*slot_entry));
    }
    slot_entry->in_use     = true;
    slot_entry->family     = family;
    slot_entry->weight     = weight;
    slot_entry->italic     = italic;
    slot_entry->pixel_size = pixel_size;
    slot_entry->codepoint  = cp;
    slot_entry->last_used  = g_cache_use_seq++;

    cached_glyph_t* g = &slot_entry->glyph;
    g->valid = true;
    apply_variable_axes(face, family, weight);
    FT_Set_Pixel_Sizes(face, 0, pixel_size);
    if (FT_Load_Char(face, (FT_ULong)cp, FT_LOAD_RENDER) != 0) {
        g->load_failed = true;
        return NULL;
    }
    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap* bmp = &slot->bitmap;
    g->bitmap_left = (int16_t)slot->bitmap_left;
    g->bitmap_top  = (int16_t)slot->bitmap_top;
    g->advance_x   = (int16_t)(slot->advance.x >> 6);
    g->width       = (uint16_t)bmp->width;
    g->rows        = (uint16_t)bmp->rows;
    g->alpha       = NULL;
    if (bmp->buffer && bmp->width > 0 && bmp->rows > 0) {
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

void draw_text_ft_ex(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                     int32_t x, int32_t y, uint32_t pixel_size,
                     font_family_t family, uint16_t weight, bool italic,
                     const char* str, uint32_t argb_color) {
    if (!pixels || !str) {
        return;
    }

    FT_Face face = resolve_face(family, italic);
    if (!face) {
        draw_string_8x8(pixels, buf_w, buf_h, x, y, str, argb_color);
        return;
    }

    int32_t pen_x      = x;
    int32_t baseline_y = y + (int32_t)pixel_size - 2;

    size_t i = 0;
    while (str[i] != '\0') {
        uint32_t cp = decode_utf8_cp(str, &i);
        const cached_glyph_t* g = NULL;
        if (cp >= GLYPH_CACHE_MIN_CHAR && cp <= GLYPH_CACHE_MAX_CHAR) {
            g = get_cached_glyph(family, weight, italic, face, pixel_size, (uint8_t)cp);
        } else if (cp >= 0x80u) {
            g = get_cached_unicode_glyph(family, weight, italic, face, pixel_size, cp);
        }
        if (!g) {
            continue;
        }

        blend_glyph_bitmap(pixels, buf_w, buf_h,
                           pen_x + g->bitmap_left,
                           baseline_y - g->bitmap_top,
                           g, argb_color);

        pen_x += g->advance_x;
        if (pen_x >= (int32_t)buf_w) {
            break;
        }
    }
}

int32_t measure_text_ft_ex(font_family_t family, uint16_t weight, bool italic,
                           uint32_t pixel_size, const char* str) {
    if (!str) return 0;
    FT_Face face = resolve_face(family, italic);
    if (!face) {
        return (int32_t)strlen(str) * 8;
    }
    int32_t total_w = 0;
    size_t i = 0;
    while (str[i] != '\0') {
        uint32_t cp = decode_utf8_cp(str, &i);
        const cached_glyph_t* g = NULL;
        if (cp >= GLYPH_CACHE_MIN_CHAR && cp <= GLYPH_CACHE_MAX_CHAR) {
            g = get_cached_glyph(family, weight, italic, face, pixel_size, (uint8_t)cp);
        } else if (cp >= 0x80u) {
            g = get_cached_unicode_glyph(family, weight, italic, face, pixel_size, cp);
        }
        if (g) {
            total_w += g->advance_x;
        }
    }
    return total_w;
}

bool draw_material_icon(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                        int32_t x, int32_t y, uint32_t pixel_size,
                        uint32_t codepoint, uint32_t argb_color) {
    if (!pixels || !g_ft_material_face) {
        return false;
    }
    const cached_glyph_t* g = get_cached_material_glyph(codepoint, pixel_size);
    if (!g) {
        return false;
    }

    int32_t baseline_y = y + (int32_t)pixel_size;
    int32_t offset_x = ((int32_t)pixel_size - g->advance_x) / 2;
    blend_glyph_bitmap(pixels, buf_w, buf_h,
                       x + offset_x + g->bitmap_left,
                       baseline_y - g->bitmap_top,
                       g, argb_color);
    return true;
}

void draw_text_ft_styled(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                         int32_t x, int32_t y, uint32_t pixel_size,
                         font_style_t style, const char* str, uint32_t argb_color) {
    uint16_t weight = FONT_WEIGHT_REGULAR;
    bool italic = false;
    switch (style) {
        case FONT_STYLE_BOLD:      weight = FONT_WEIGHT_BOLD; break;
        case FONT_STYLE_ITALIC:    italic = true; break;
        case FONT_STYLE_THIN:      weight = FONT_WEIGHT_THIN; break;
        case FONT_STYLE_LIGHT:     weight = FONT_WEIGHT_LIGHT; break;
        case FONT_STYLE_MEDIUM:    weight = FONT_WEIGHT_MEDIUM; break;
        case FONT_STYLE_SEMIBOLD:  weight = FONT_WEIGHT_SEMIBOLD; break;
        case FONT_STYLE_EXTRABOLD: weight = FONT_WEIGHT_EXTRABOLD; break;
        case FONT_STYLE_BLACK:     weight = FONT_WEIGHT_BLACK; break;
        default: break;
    }
    draw_text_ft_ex(pixels, buf_w, buf_h, x, y, pixel_size,
                    FONT_FAMILY_INTER_DISPLAY, weight, italic, str, argb_color);
}

void draw_text_ft(uint32_t* pixels, uint32_t buf_w, uint32_t buf_h,
                  int32_t x, int32_t y, uint32_t pixel_size,
                  const char* str, uint32_t argb_color) {
    draw_text_ft_ex(pixels, buf_w, buf_h, x, y, pixel_size,
                    FONT_FAMILY_INTER_DISPLAY, FONT_WEIGHT_REGULAR, false, str, argb_color);
}
