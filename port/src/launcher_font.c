#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "launcher.h"
#include <stdlib.h>

enum { ATLAS_SMALLEST = 256, ATLAS_LARGEST = 4096 };

static const unsigned char light_face[] = {
#embed "../launcher/Manrope-Light.ttf"
};

static const unsigned char regular_face[] = {
#embed "../launcher/Manrope-Regular.ttf"
};

static const unsigned char semibold_face[] = {
#embed "../launcher/Manrope-SemiBold.ttf"
};

static const unsigned char *const faces[LAUNCHER_FACES] = {light_face, regular_face, semibold_face};

static stbtt_fontinfo infos[LAUNCHER_FACES];
static int infos_ready[LAUNCHER_FACES];

static const stbtt_fontinfo *info_of(int face) {
    if (!infos_ready[face]) infos_ready[face] = stbtt_InitFont(&infos[face], faces[face], 0);
    return infos_ready[face] ? &infos[face] : NULL;
}

static unsigned char *baked_coverage(int face, float pixels, int *size, stbtt_bakedchar *baked) {
    for (*size = ATLAS_SMALLEST; *size <= ATLAS_LARGEST; *size *= 2) {
        unsigned char *coverage = calloc((size_t)*size * (size_t)*size, 1);
        if (!coverage) return NULL;
        if (stbtt_BakeFontBitmap(faces[face], 0, pixels, coverage, *size, *size, LAUNCHER_FIRST, LAUNCHER_GLYPHS,
                                 baked) > 0)
            return coverage;
        free(coverage);
    }
    return NULL;
}

int launcher_font_bake(int face, float pixels, LauncherFont *font) {
    const stbtt_fontinfo *info = info_of(face);
    stbtt_bakedchar baked[LAUNCHER_GLYPHS];
    int size = 0;
    unsigned char *coverage = info ? baked_coverage(face, pixels, &size, baked) : NULL;
    if (!coverage) return 0;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &gap);
    font->face = face;
    font->pixels = pixels;
    font->ascent = ascent * stbtt_ScaleForPixelHeight(info, pixels);
    for (int glyph = 0; glyph < LAUNCHER_GLYPHS; glyph++)
        font->glyphs[glyph] = (LauncherGlyph){baked[glyph].x0,   baked[glyph].y0,   baked[glyph].x1, baked[glyph].y1,
                                              baked[glyph].xoff, baked[glyph].yoff, baked[glyph].xadvance};
    font->atlas = (LauncherPicture){size, size, malloc((size_t)size * (size_t)size * 4)};
    for (int at = 0; font->atlas.pixels && at < size * size; at++) {
        unsigned char *pixel = font->atlas.pixels + 4 * at;
        pixel[0] = pixel[1] = pixel[2] = 255;
        pixel[3] = coverage[at];
    }
    free(coverage);
    return font->atlas.pixels != NULL;
}

float launcher_font_kern(const LauncherFont *font, int first, int second) {
    const stbtt_fontinfo *info = info_of(font->face);
    if (!info) return 0;
    return stbtt_GetCodepointKernAdvance(info, first, second) * stbtt_ScaleForPixelHeight(info, font->pixels);
}
