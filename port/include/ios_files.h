#pragma once

#include <stddef.h>

struct SDL_Window;

void port_ios_saves(void);
void port_ios_saves_name(char *name, size_t size);
void port_ios_pick_saves(struct SDL_Window *window);
const char *port_ios_documents(void);
void port_ios_disc(char *path, size_t size);
void port_ios_pick_disc(struct SDL_Window *window, char *path, size_t size);
