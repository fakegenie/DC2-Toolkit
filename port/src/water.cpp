#include "water.hpp"
#include "draw.hpp"
#include "mg_drawenv.hpp"
#include "mg_math.hpp"
#include "mg_texture.hpp"
#include "mglib.hpp"
#include "sce/libgraph.h"
#include "sce/libvu0.h"
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

enum { STRIP_COLUMNS = 27, FLAG_X = 0x80, FLAG_Y = 0x40, FLAG_W = 0x10, FLAG_ALL = 0xd0 };

struct WaterSlope {
    float row;
    float column;
    float weight;
};

struct WaterGrid {
    int rows;
    int columns;
    std::vector<WaterSlope> slope;
};

struct WaterView {
    float clip[4][4];
    RenderFogCurve fog;
    float far;
    float offset_x;
    float offset_y;
    float width;
    float height;
    float colour[4];
    float param0;
    float param1;
    float min_y;
    float texture_width;
    float texture_height;
    float fog_enabled;
};

static std::unordered_map<const CWater *, WaterGrid> grids;

static void set_weight(WaterGrid &grid, int row, int column, float value) {
    if (row < 0 || row >= grid.rows || column < 0 || column >= grid.columns) return;
    grid.slope[(size_t)(row * grid.columns + column)].weight = value;
}

static void build_grid(const CWater *water) {
    if (water->height == NULL || water->rows < 2 || water->columns < 2) return;
    WaterGrid &grid = grids[water];
    int rows = water->rows, columns = water->columns, count = rows * columns;
    grid.rows = rows;
    grid.columns = columns;
    grid.slope.assign((size_t)count, WaterSlope{});
    for (int row = 0; row < rows; row++)
        for (int column = 0; column < columns; column++) {
            int cell = row * columns + column;
            int above = row == 0 ? cell : cell - columns;
            WaterSlope &slope = grid.slope[(size_t)cell];
            slope.row = water->height[above] - water->height[cell];
            slope.column = cell + 1 < count ? water->height[cell] - water->height[cell + 1] : 0.0f;
            slope.weight = 1.0f;
        }
    for (int row = 0; row < rows; row++) {
        set_weight(grid, row, columns - 1, 0.0f);
        set_weight(grid, row, 0, 0.0f);
        set_weight(grid, row, columns - 2, 0.6f);
        set_weight(grid, row, 1, 0.6f);
        set_weight(grid, row, columns - 3, 0.3f);
        set_weight(grid, row, 2, 0.3f);
    }
    for (int column = 0; column < columns; column++) {
        set_weight(grid, rows - 1, column, 0.0f);
        set_weight(grid, 0, column, 0.0f);
        set_weight(grid, rows - 2, column, 0.6f);
        set_weight(grid, 1, column, 0.6f);
        set_weight(grid, rows - 3, column, 0.3f);
        set_weight(grid, 2, column, 0.3f);
    }
}

static u_char colour_byte(float value) {
    if (!(value > 0.0f)) return 0;
    if (value >= 255.0f) return 255;
    return (u_char)value;
}

static float clamp(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}

static int water_vertex(RenderVertex *vertex, const WaterView &view, const WaterSlope &slope, float x, float z) {
    float y = view.min_y * view.param0;
    float clip[4];
    for (int k = 0; k < 4; k++)
        clip[k] = x * view.clip[0][k] + y * view.clip[1][k] + z * view.clip[2][k] + view.clip[3][k];
    float q = clip[3] != 0.0f ? 1.0f / clip[3] : 0.0f;
    float xw = clip[0] * q, yw = clip[1] * q, zw = clip[2] * q;
    int flags = 0;
    if (xw > 1.0f && xw < 4095.0f) flags |= FLAG_X;
    if (yw > 1.0f && yw < 4095.0f) flags |= FLAG_Y;
    if (q > 1.0f / view.far && q < view.far) flags |= FLAG_W;
    RenderScreenMapping screen;
    float depth;
    gs_screen_mapping(&screen, &depth);
    vertex->x = xw - view.offset_x;
    vertex->y = yw - view.offset_y;
    vertex->z = clamp((float)(long long)zw * depth, 0.0f, 1.0f);
    float u = clamp(xw - view.offset_x + slope.row * view.param1, 1.0f, view.width);
    float v = clamp(yw - view.offset_y + slope.column * view.param1, 1.0f, view.height);
    vertex->u = u / view.texture_width;
    vertex->v = v / view.texture_height;
    float fade = clamp(375.0f * q - 0.25f, 0.0f, 1.0f);
    vertex->r = colour_byte(view.colour[0]);
    vertex->g = colour_byte(view.colour[1]);
    vertex->b = colour_byte(view.colour[2]);
    vertex->a = colour_byte(view.colour[3] * slope.weight * fade);
    float fog = view.fog.offset + view.fog.scale * q;
    fog = fmaxf(fminf(fog, view.fog.near_value), view.fog.far_value);
    vertex->fog = fog / 255.0f;
    return flags;
}

static void water_strip(std::vector<RenderVertex> &out, const WaterView &view, const CWater *water,
                        const WaterGrid &grid, int row, int first, int count) {
    float row_step = (water->max[0] - water->min[0]) / (float)(water->rows - 1);
    float column_step = (water->max[2] - water->min[2]) / (float)(water->columns - 1);
    RenderVertex strip[2 * STRIP_COLUMNS] = {};
    int flags[2 * STRIP_COLUMNS];
    for (int i = 0; i < 2 * count; i++) {
        int line = row + (i & 1);
        int column = first + i / 2;
        const WaterSlope &slope = grid.slope[(size_t)(line * grid.columns + column)];
        flags[i] = water_vertex(&strip[i], view, slope, water->min[0] + (float)line * row_step,
                                water->min[2] + (float)column * column_step);
    }
    for (int i = 2; i < 2 * count; i++) {
        if ((flags[i - 2] & flags[i - 1] & flags[i]) != FLAG_ALL) continue;
        out.push_back(strip[i - 2]);
        out.push_back(strip[i - 1]);
        out.push_back(strip[i]);
    }
}

PORT_OVERRIDE uptr CWater::CreatePacket(mgCDrawManager *draw_manager) {
    (void)draw_manager;
    build_grid(this);
    return 0;
}

PORT_OVERRIDE int CWater::CreateRenderInfoPacket(u_int *packet, float (*matrix)[4], mgRENDER_INFO *info) {
    (void)packet;
    (void)matrix;
    info->scissor = 0;
    return 0;
}

PORT_OVERRIDE int CWater::Draw(u_int *tag, float (*matrix)[4], mgCDrawManager *draw_manager) {
    if (draw_manager == NULL) draw_manager = &mgDrawManager;
    mgRENDER_INFO *info = draw_manager->render_info;
    texture_manager = draw_manager->texture_manager;
    CreateRenderInfoPacket(NULL, matrix, info);
    if (tag == NULL || texture == NULL || info->attr == NULL || render_toggles.no_water) return 0;
    auto found = grids.find(this);
    if (found == grids.end() || found->second.rows != rows || found->second.columns != columns) {
        build_grid(this);
        found = grids.find(this);
        if (found == grids.end()) return 0;
    }
    int fogged = info->attr->fog != 0 && info->fog_enable != 0;
    u_int fog_colour = info->fog.r | info->fog.g << 8 | info->fog.b << 16;
    if (info->attr->fog > MG_FOG_SCENE) {
        if (info->attr->fog == MG_FOG_BLACK) fog_colour = 0;
        if (info->attr->fog == MG_FOG_WHITE) fog_colour = 0xffffff;
    }
    sceGsTexa texa = {};
    texa.TA0 = GS_ALPHA_ONE;
    texa.TA1 = GS_ALPHA_ONE;
    gs_write(SCE_GS_TEXA, texa.value);
    gs_write(SCE_GS_FOGCOL, fog_colour);

    mgCDrawEnv env;
    SetDrawEnvAttributes(&env, info, draw_env != NULL ? draw_env : &info->draw_env[0]);
    RenderState state;
    gs_base_state(&state);
    state.blend = 1;
    state.blending = gs_blending(env.alpha);
    state.tests = gs_tests(env.test);
    state.depth_write = !env.zbuf.bits.zmsk;
    state.fog = fogged;
    state.fog_color = fog_colour;
    state.expansion = gs_expansion(texa);
    state.texa = 1;
    state.linear = texture->tex1.MMAG;
    state.texture = gs_sampled(gs_texture(texture->tex0.TBP0, mgScreenWidth, mgScreenHeight), mgScreenWidth,
                               mgScreenHeight, 0);
    if (!state.texture) return 0;

    WaterView view;
    mgMulMatrix(view.clip, info->world_screen, matrix);
    view.fog = {info->fog.offset, info->fog.near_value, info->fog.far_value, info->fog.scale};
    view.far = info->guard_max[3];
    view.offset_x = (float)mgScreenOffx;
    view.offset_y = (float)mgScreenOffy;
    view.width = (float)mgScreenWidth;
    view.height = (float)(mgScreenHeight - 1);
    for (int k = 0; k < 4; k++) view.colour[k] = (float)color[k];
    view.param0 = surface_param0;
    view.param1 = surface_param1;
    view.min_y = min[1];
    view.texture_width = (float)render_width(state.texture);
    view.texture_height = (float)render_height(state.texture);

    std::vector<RenderVertex> out;
    for (int row = 0; row < rows - 1; row++)
        for (int first = 0, remaining = columns; remaining > 0; first += STRIP_COLUMNS - 1, remaining -= STRIP_COLUMNS) {
            int count = remaining < STRIP_COLUMNS ? remaining : STRIP_COLUMNS;
            water_strip(out, view, this, found->second, row, first, count);
        }
    render_source = RENDER_SOURCE_WATER;
    gs_drawing_scaled(state.texture);
    render_draw(&state, RENDER_TRIANGLES, out.data(), (int)out.size());
    return 0;
}
