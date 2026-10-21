#include "debug.h"
#include "colprim.hpp"
#include "dng_main.hpp"
#include "imgui.h"
#include "mainloop.hpp"
#include "map.hpp"
#include "mapparts.hpp"
#include "mg_math.hpp"
#include "mglib.hpp"
#include "render.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

typedef struct {
    const char *name;
    const char *label;
    int on;
} Helper;

enum { HELPER_HITBOXES, HELPER_HURTBOXES, HELPER_BODIES, HELPER_COLLISION, HELPER_TRIGGERS, HELPER_XRAY, HELPER_COUNT };
enum { CIRCLE_SEGMENTS = 16, POLY_LIMIT = 4096, CHARACTER_LIMIT = 128 };

static Helper helpers[HELPER_COUNT] = {
    {"hitboxes", "hitboxes (attacks: red hurts the player, orange hurts monsters)", 0},
    {"hurtboxes", "hurtboxes (where attacks land, green)", 0},
    {"bodies", "body spheres (pushing, cyan)", 0},
    {"collision", "map collision near the player (blue floors, yellow walls)", 0},
    {"triggers", "event triggers and doors (magenta, doors white)", 0},
    {"xray", "draw helpers through walls", 1},
};

static const u8 red[4] = {255, 60, 60, 128};
static const u8 orange[4] = {255, 160, 40, 128};
static const u8 green[4] = {80, 255, 80, 128};
static const u8 cyan[4] = {60, 220, 255, 100};
static const u8 floor_blue[4] = {70, 120, 255, 70};
static const u8 wall_yellow[4] = {255, 220, 60, 70};
static const u8 magenta[4] = {255, 70, 255, 128};
static const u8 white[4] = {255, 255, 255, 128};

static CCPoly polys[POLY_LIMIT];
static int found_polys;
static int drawn_lines;
static int dropped_lines;

static float plane_distance(const float *point, const float *plane) {
    return point[0] * plane[0] + point[1] * plane[1] + point[2] * plane[2] + point[3] * plane[3] + plane[4];
}

static int keep_side(float *a, float *b, const float *plane) {
    float from = plane_distance(a, plane), to = plane_distance(b, plane);
    if (from < 0.0f && to < 0.0f) return 0;
    if (from >= 0.0f && to >= 0.0f) return 1;
    float *moved = from < 0.0f ? a : b;
    float *fixed = moved == a ? b : a;
    float outside = moved == a ? from : to, inside = moved == a ? to : from;
    float t = outside / (outside - inside);
    for (int i = 0; i < 4; ++i) moved[i] += (fixed[i] - moved[i]) * t;
    return 1;
}

static int clip_line(float *a, float *b) {
    float planes[6][5] = {
        {0, 0, 0, 1, -mgRenderInfo.clip_min[2] * 1.001f}, {0, 0, 0, -1, mgRenderInfo.clip_max[2] * 0.999f},
        {1, 0, 0, -1, 0},  {-1, 0, 0, 4094, 0},
        {0, 1, 0, -1, 0},  {0, -1, 0, 4094, 0},
    };
    for (const float *plane : planes)
        if (!keep_side(a, b, plane)) return 0;
    return 1;
}

static int screen_point(const float *clip, int *out) {
    float x = clip[0] / clip[3], y = clip[1] / clip[3];
    if (x < 0.0f || x > 4095.0f || y < 0.0f || y > 4095.0f) return 0;
    out[0] = (int)(16.0f * x);
    out[1] = (int)(16.0f * y);
    out[2] = (int)(clip[2] / clip[3]);
    out[3] = 0;
    return 1;
}

static void line(mgCDrawPrim *prim, const float *a, const float *b, const u8 *color) {
    alignas(16) float from[4] = {a[0], a[1], a[2], 1.0f};
    alignas(16) float to[4] = {b[0], b[1], b[2], 1.0f};
    alignas(16) float start_clip[4], end_clip[4];
    sceVu0ApplyMatrix(start_clip, mgRenderInfo.world_screen, from);
    sceVu0ApplyMatrix(end_clip, mgRenderInfo.world_screen, to);
    int start[4], end[4];
    if (!clip_line(start_clip, end_clip) || !screen_point(start_clip, start) || !screen_point(end_clip, end)) {
        dropped_lines++;
        return;
    }
    drawn_lines++;
    prim->Color(color[0], color[1], color[2], color[3]);
    prim->Vertex4(start);
    prim->Vertex4(end);
}

static void ellipsoid(mgCDrawPrim *prim, const float *center, float x, float y, float z, const u8 *color) {
    float radii[3] = {x, y, z};
    for (int axis = 0; axis < 3; ++axis) {
        int first = (axis + 1) % 3, second = (axis + 2) % 3;
        float previous[3];
        for (int i = 0; i <= CIRCLE_SEGMENTS; ++i) {
            float angle = 6.2831853f * (float)i / (float)CIRCLE_SEGMENTS;
            float point[3] = {center[0], center[1], center[2]};
            point[first] += radii[first] * cosf(angle);
            point[second] += radii[second] * sinf(angle);
            if (i) line(prim, previous, point, color);
            memcpy(previous, point, sizeof previous);
        }
    }
}

static void sphere(mgCDrawPrim *prim, const float *center, float radius, const u8 *color) {
    ellipsoid(prim, center, radius, radius, radius, color);
}

static void capsule(mgCDrawPrim *prim, const float *a, const float *b, float radius, const u8 *color) {
    sphere(prim, a, radius, color);
    sphere(prim, b, radius, color);
    float offsets[4][3] = {{radius, 0, 0}, {-radius, 0, 0}, {0, 0, radius}, {0, 0, -radius}};
    for (const float *offset : offsets) {
        float from[3] = {a[0] + offset[0], a[1] + offset[1], a[2] + offset[2]};
        float to[3] = {b[0] + offset[0], b[1] + offset[1], b[2] + offset[2]};
        line(prim, from, to, color);
    }
}

static CScene *current_scene(void) {
    int loop = GetNowLoopNo();
    if (loop == LOOP_DUNGEON) return DngMainScene;
    if (loop == LOOP_EDIT) return GetMainScene();
    return NULL;
}

static void draw_hitboxes(mgCDrawPrim *prim) {
    if (GetNowLoopNo() != LOOP_DUNGEON || !ColPrimMan.scene) return;
    for (CColPrim &attack : ColPrimMan.prim) {
        if (!attack.active || !attack.param) continue;
        int hurts_player = (attack.target & DAMAGE_TARGET_PLAYER) ||
                           ((attack.target & DAMAGE_TARGET_BY_OWNER) && attack.owner != 0);
        const u8 *color = hurts_player ? red : orange;
        float radius = 2.0f * attack.radius;
        if (attack.param->shape & DAMAGE_SHAPE_LINE) capsule(prim, attack.pos[0], attack.pos[1], radius, color);
        else sphere(prim, attack.pos[0], radius, color);
    }
}

static void draw_entries(mgCDrawPrim *prim, CScene *scene, int group, const u8 *color) {
    for (int slot = 0; slot < CHARACTER_LIMIT; ++slot) {
        CCharacter2 *character = scene->GetCharacter(slot);
        if (!character) continue;
        for (CHARA_ENTRY_OBJECT &entry : character->entry_object) {
            if (!entry.enable || entry.group != group || !entry.frame) continue;
            alignas(16) float center[4];
            entry.frame->GetWorldPosition0(center);
            sphere(prim, center, 2.0f * entry.size, color);
        }
    }
}

static int player_position(CScene *scene, float *position) {
    CCharacter2 *player = scene->player_chara >= 0 ? scene->GetCharacter(scene->player_chara) : NULL;
    if (!player) return 0;
    player->GetPosition(position);
    return 1;
}

static void draw_collision(mgCDrawPrim *prim, CScene *scene) {
    alignas(16) float position[4];
    if (!player_position(scene, position)) return;
    mgVu0FBOX box;
    for (int i = 0; i < 3; ++i) {
        box.max[i] = position[i] + 200.0f;
        box.min[i] = position[i] - 200.0f;
    }
    box.max[3] = 1.0f;
    box.min[3] = 1.0f;
    int count = scene->GetColPoly(polys, box, POLY_LIMIT);
    found_polys = count;
    for (int i = 0; i < count; ++i) {
        const CCPoly *poly = &polys[i];
        float length = sqrtf(poly->normal[0] * poly->normal[0] + poly->normal[1] * poly->normal[1] +
                             poly->normal[2] * poly->normal[2]);
        const u8 *color = fabsf(poly->normal[1]) > 0.7f * length ? floor_blue : wall_yellow;
        line(prim, poly->vertex[0], poly->vertex[1], color);
        line(prim, poly->vertex[1], poly->vertex[2], color);
        line(prim, poly->vertex[2], poly->vertex[0], color);
    }
}

static void draw_trigger(mgCDrawPrim *prim, CFuncPoint *point) {
    if (!point->Check(NULL)) return;
    sceVu0FMATRIX matrix;
    point->frame.GetLWMatrix(matrix);
    ellipsoid(prim, matrix[3], mgDistVector(matrix[0]), mgDistVector(matrix[1]), mgDistVector(matrix[2]),
              (point->event.flag & FUNC_EVENT_DOOR) ? white : magenta);
}

static void draw_triggers(mgCDrawPrim *prim, CScene *scene) {
    CMap *map = scene->active_map >= 0 ? scene->GetMap(scene->active_map) : NULL;
    if (!map) return;
    map->func_point.GetStart(FUNC_POINT_EVENT);
    for (CFuncPoint *point = map->func_point.Get(); point; point = map->func_point.Get()) draw_trigger(prim, point);
    for (int i = 0; i < map->place_parts_max; ++i) {
        CMapParts *parts = &map->place_parts[i];
        CFuncPointMngr *manager = &parts->func_point_mngr;
        if (!(manager->flag & FUNC_POINT_MNGR_EVENT) || !parts->name[0] || !parts->GetShow()) continue;
        manager->GetStart(FUNC_POINT_EVENT);
        for (CFuncPoint *point = manager->Get(); point; point = manager->Get()) {
            point->frame.SetReference(&parts->frame);
            draw_trigger(prim, point);
            point->frame.DeleteReference();
        }
    }
}

static int any_helper(void) {
    for (int i = 0; i < HELPER_XRAY; ++i)
        if (helpers[i].on) return 1;
    return 0;
}

extern "C" void debug_overlay(void) {
    found_polys = drawn_lines = dropped_lines = 0;
    if (!any_helper()) return;
    CScene *scene = current_scene();
    if (!scene) return;
    mgCDrawPrim prim;
    prim.Initialize(0, 0);
    prim.AlphaBlendEnable(1);
    prim.AlphaBlend(MG_ALPHA_BLEND_NORMAL);
    prim.TextureMapEnable(0);
    prim.Bilinear(0);
    prim.Coord(1);
    prim.Shading(0);
    prim.ZMask(MG_Z_MASK_MASKED);
    prim.DepthTestEnable(!helpers[HELPER_XRAY].on);
    prim.DepthTest(MG_DEPTH_TEST_GEQUAL);
    prim.Begin(MG_PRIM_LINE);
    if (helpers[HELPER_COLLISION].on) draw_collision(&prim, scene);
    if (helpers[HELPER_TRIGGERS].on) draw_triggers(&prim, scene);
    if (helpers[HELPER_BODIES].on) draw_entries(&prim, scene, CHARA_ENTRY_GROUP_BODY, cyan);
    if (helpers[HELPER_HURTBOXES].on) draw_entries(&prim, scene, CHARA_ENTRY_GROUP_HIT, green);
    if (helpers[HELPER_HITBOXES].on) draw_hitboxes(&prim);
    prim.End();
}

void debug_helpers_panel(void) {
    bool wireframe = render_toggles.wireframe != 0;
    if (ImGui::Checkbox("wireframe", &wireframe)) render_toggles.wireframe = wireframe;
    for (Helper &helper : helpers) {
        bool on = helper.on != 0;
        if (ImGui::Checkbox(helper.label, &on)) helper.on = on;
    }
    ImGui::Text("%d collision triangles, %d lines drawn, %d off screen", found_polys, drawn_lines, dropped_lines);
}

static int command_helpers(const DebugArgs *args, DebugReply *reply) {
    const char *name = debug_arg(args, "name", "");
    const char *value = debug_arg(args, "value", "");
    int found = !name[0];
    if (!strcmp(name, "wireframe")) {
        found = 1;
        if (value[0]) render_toggles.wireframe = atoi(value) != 0;
    }
    for (Helper &helper : helpers) {
        if (strcmp(helper.name, name)) continue;
        found = 1;
        if (value[0]) helper.on = atoi(value) != 0;
    }
    if (!found) return debug_fail(reply, "name=wireframe|hitboxes|hurtboxes|bodies|collision|triggers|xray value=0|1");
    debug_print(reply, "{\"wireframe\": %d", render_toggles.wireframe);
    for (const Helper &helper : helpers) debug_print(reply, ", \"%s\": %d", helper.name, helper.on);
    debug_print(reply, ", \"triangles\": %d, \"drawn\": %d, \"dropped\": %d}", found_polys, drawn_lines, dropped_lines);
    return 0;
}

extern const DebugCommand debug_overlay_commands[] = {
    {"helpers", command_helpers, 1, "[name=wireframe|hitboxes|hurtboxes|bodies|collision|triggers|xray value=0|1] draws debug helpers; no arguments lists them"},
    {NULL, NULL, 0, NULL},
};
