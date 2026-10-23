#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { LAUNCHER_FIRST = 32, LAUNCHER_GLYPHS = 224 };
enum { LAUNCHER_LIGHT, LAUNCHER_REGULAR, LAUNCHER_SEMIBOLD, LAUNCHER_FACES };

typedef struct {
    int width;
    int height;
    unsigned char *pixels;
} LauncherPicture;

typedef struct {
    float x0;
    float y0;
    float x1;
    float y1;
    float left;
    float top;
    float advance;
} LauncherGlyph;

typedef struct {
    LauncherPicture atlas;
    int face;
    float pixels;
    float ascent;
    LauncherGlyph glyphs[LAUNCHER_GLYPHS];
} LauncherFont;

int port_launcher(int count, char **values, int show);
int launcher_font_bake(int face, float pixels, LauncherFont *font);
float launcher_font_kern(const LauncherFont *font, int first, int second);

#ifdef __cplusplus
}
#endif
