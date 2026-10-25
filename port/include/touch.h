#pragma once

#include "launcher.h"

#ifdef __cplusplus
extern "C" {
#endif

struct port_input_state;
union SDL_Event;

enum { TOUCH_OFF, TOUCH_ON, TOUCH_SMART };
enum { TOUCH_TEXT, TOUCH_DISC, TOUCH_RING, TOUCH_BOX, TOUCH_BOX_LINE, TOUCH_TRIANGLE_LINE, TOUCH_CROSS, TOUCH_ARROW, TOUCH_GLOW, TOUCH_DPAD_ARM };

typedef struct {
    float x;
    float y;
    float half_width;
    float half_height;
    float angle;
    float red;
    float green;
    float blue;
    float alpha;
    float stroke;
    float radius;
    int kind;
    float u0;
    float v0;
    float u1;
    float v1;
} TouchShape;

void port_touch_set_mode(int mode);
void port_touch_set_haptics(int on);
void port_touch_set_states(int on);
void port_touch_read(struct port_input_state *state);
void port_touch_event(const union SDL_Event *event);
int port_touch_shapes(TouchShape *shapes, int limit, int pixel_width, int pixel_height);
const LauncherPicture *port_touch_atlas(void);

#ifdef __cplusplus
}
#endif
