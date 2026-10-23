#include "icon.h"

static const unsigned char icon_png[] = {
#embed "../launcher/icon.png"
};

void port_window_icon(SDL_Window *window) {
    SDL_Surface *icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(icon_png, sizeof icon_png), true);
    if (!icon) return;
    SDL_SetWindowIcon(window, icon);
    SDL_DestroySurface(icon);
}
