#include "mg_sprite.hpp"
#include "draw.hpp"
#include "mg_math.hpp"
#include "mg_memory.hpp"
#include "mg_texture.hpp"
#include "mglib.hpp"
#include "sce/libgraph.h"
#include "sce/libvu0.h"
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

enum { SPRITE_ENV, SPRITE_TEXTURE, SPRITE_BATCH, SPRITE_ITEM };

constexpr float GS_COORDINATE_LIMIT = 4095.0f;

struct SpriteItem {
    float position[4];
    float width;
    float height;
    float sine;
    float cosine;
    float color[4];
    float u0;
    float v0;
    float u1;
    float v1;
};

struct SpriteOp {
    int kind;
    mgCDrawEnv env;
    const mgCTexture *texture;
    int mode;
    SpriteItem item;
};

static std::unordered_map<const void *, std::vector<SpriteOp>> sprite_packets;
static std::unordered_map<const void *, std::vector<SpriteOp>> previous_packets;

void sprite_next_frame() {
    previous_packets.swap(sprite_packets);
    sprite_packets.clear();
}

static std::vector<SpriteOp> &ops_for(const mgC3DSprite *sprite) {
    return sprite_packets[sprite->packet];
}

PORT_OVERRIDE void mgC3DSprite::BeginCreatePacket(int mode, mgCDrawManager *manager) {
    if (manager == NULL) manager = &mgDrawManager;
    memory = manager->data_memory;
    packet = memory->Alloc(1);
    packet_start = packet;
    packet_cur = packet;
    this->mode = mode;
    prog_started = 0;
    ops_for(this).clear();
}

PORT_OVERRIDE void mgC3DSprite::CPSetDrawEnv(mgCDrawEnv *env) {
    if (env == NULL) return;
    SpriteOp op;
    memset(&op, 0, sizeof op);
    op.kind = SPRITE_ENV;
    op.env = *env;
    ops_for(this).push_back(op);
}

PORT_OVERRIDE void mgC3DSprite::CPSetTexture(mgCTexture *texture) {
    if (texture == NULL) return;
    SpriteOp op;
    memset(&op, 0, sizeof op);
    op.kind = SPRITE_TEXTURE;
    op.texture = texture;
    ops_for(this).push_back(op);
}

PORT_OVERRIDE void mgC3DSprite::BeginCPSprite() {
    SpriteOp op;
    memset(&op, 0, sizeof op);
    op.kind = SPRITE_BATCH;
    op.mode = mode;
    ops_for(this).push_back(op);
    sprite_num = 0;
}

PORT_OVERRIDE void mgC3DSprite::CPSetSprite(float *first, float *second, float *third, float *fourth, float *fifth) {
    SpriteOp op;
    memset(&op, 0, sizeof op);
    op.kind = SPRITE_ITEM;
    op.mode = mode;
    memcpy(op.item.position, first, sizeof op.item.position);
    op.item.width = second[0];
    op.item.height = second[1];
    if (mode == MG_3DSPRITE_MODE_ROTATE) {
        op.item.sine = sinf(second[2]);
        op.item.cosine = cosf(second[2]);
    }
    memcpy(op.item.color, third, sizeof op.item.color);
    op.item.u0 = fourth[0];
    op.item.v0 = fourth[1];
    op.item.u1 = fifth[0];
    op.item.v1 = fifth[1];
    ops_for(this).push_back(op);
    sprite_num++;
}

PORT_OVERRIDE void mgC3DSprite::EndCPSprite() {}

PORT_OVERRIDE void mgC3DSprite::EndCreatePacket() {}

struct SpriteFrame {
    float local_screen[4][4];
    float axis_x;
    float axis_y;
    RenderFogCurve fog;
    float near;
    float far;
};

static u_char vu_byte(float value) {
    return (u_char)((int)value & 255);
}

static void sprite_vertex(RenderVertex *vertex, float x, float y, float z, float u, float v, const SpriteOp &item,
                          float fog, const RenderScreenMapping &screen, float depth) {
    vertex->x = (x * screen.scale_x + screen.offset_x + 1.0f) / screen.scale_x;
    vertex->y = (y * screen.scale_y + screen.offset_y - 1.0f) / screen.scale_y;
    vertex->z = z * depth > 1.0f ? 1.0f : z * depth;
    vertex->u = u;
    vertex->v = v;
    vertex->r = vu_byte(item.item.color[0]);
    vertex->g = vu_byte(item.item.color[1]);
    vertex->b = vu_byte(item.item.color[2]);
    vertex->a = vu_byte(item.item.color[3]);
    vertex->fog = fog / 255.0f;
}

static void draw_sprite(const SpriteFrame &frame, const SpriteOp &item, const RenderState &state, int width,
                        int height) {
    const float *p = item.item.position;
    float clip[4];
    for (int k = 0; k < 4; k++)
        clip[k] = p[0] * frame.local_screen[0][k] + p[1] * frame.local_screen[1][k] + p[2] * frame.local_screen[2][k] +
                  frame.local_screen[3][k];
    if (clip[3] == 0.0f) return;
    float q = 1.0f / clip[3];
    float cx = clip[0] * q, cy = clip[1] * q, cz = clip[2] * q;
    if (!(clip[3] > frame.near && clip[3] < frame.far)) return;
    float fog = frame.fog.offset + frame.fog.scale * q;
    fog = fmaxf(fminf(fog, frame.fog.near_value), frame.fog.far_value);
    float u0 = item.item.u0, v0 = item.item.v0, u1 = item.item.u1, v1 = item.item.v1;
    float width_scale = item.item.width, height_scale = item.item.height;
    RenderScreenMapping screen;
    float depth;
    gs_screen_mapping(&screen, &depth);
    RenderVertex out[6];
    if (item.mode != MG_3DSPRITE_MODE_ROTATE) {
        float hx = width_scale * frame.axis_x * 0.5f * q;
        float hy = height_scale * frame.axis_y * 0.5f * q;
        float x0 = cx - hx, y0 = cy - hy, x1 = cx + hx, y1 = cy + hy;
        if (!(x0 > 1.0f && y0 > 1.0f && x1 < GS_COORDINATE_LIMIT && y1 < GS_COORDINATE_LIMIT)) return;
        RenderVertex a = {}, b = {}, c = {}, d = {};
        sprite_vertex(&a, x0, y0, cz, u0, v0, item, fog, screen, depth);
        sprite_vertex(&b, x1, y0, cz, u1, v0, item, fog, screen, depth);
        sprite_vertex(&c, x0, y1, cz, u0, v1, item, fog, screen, depth);
        sprite_vertex(&d, x1, y1, cz, u1, v1, item, fog, screen, depth);
        out[0] = a, out[1] = b, out[2] = c, out[3] = b, out[4] = d, out[5] = c;
    } else {
        float sine = item.item.sine, cosine = item.item.cosine;
        float ax = cosine * frame.axis_x * width_scale * 0.5f * q, ay = sine * frame.axis_x * width_scale * 0.5f * q;
        float bx = -sine * frame.axis_y * height_scale * 0.5f * q, by = cosine * frame.axis_y * height_scale * 0.5f * q;
        float corners[4][2] = {{cx - ax - bx, cy - ay - by}, {cx + ax - bx, cy + ay - by},
                               {cx - ax + bx, cy - ay + by}, {cx + ax + bx, cy + ay + by}};
        float low_x = corners[0][0], low_y = corners[0][1], high_x = corners[3][0], high_y = corners[3][1];
        if (!(low_x > 1.0f && low_y > 1.0f && high_x < GS_COORDINATE_LIMIT && high_y < GS_COORDINATE_LIMIT)) return;
        float uvs[4][2] = {{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}};
        RenderVertex v[4] = {};
        for (int i = 0; i < 4; i++)
            sprite_vertex(&v[i], corners[i][0], corners[i][1], cz, uvs[i][0], uvs[i][1], item, fog, screen, depth);
        out[0] = v[0], out[1] = v[1], out[2] = v[2], out[3] = v[1], out[4] = v[3], out[5] = v[2];
    }
    for (int i = 0; i < 6; i++) {
        out[i].u /= (float)width;
        out[i].v /= (float)height;
    }
    render_source = RENDER_SOURCE_SPRITE;
    gs_drawing_scaled();
    render_sprite(&state, out, 6, &screen, depth);
}

PORT_OVERRIDE int mgC3DSprite::CreateRenderInfoPacket(u_int *dest, float (*matrix)[4], mgRENDER_INFO *render_info) {
    (void)dest;
    (void)matrix;
    render_info->scissor = 0;
    return 0;
}

PORT_OVERRIDE int mgC3DSprite::Draw(u_int *tag, float (*matrix)[4], mgCDrawManager *manager) {
    if (manager == NULL) manager = &mgDrawManager;
    mgRENDER_INFO *info = manager->render_info;
    texture_manager = manager->texture_manager;
    if (packet == 0 || tag == NULL || info->attr == NULL || render_toggles.no_sprites) return 0;
    auto found = sprite_packets.find(packet);
    if (found == sprite_packets.end()) {
        found = previous_packets.find(packet);
        if (found == previous_packets.end()) return 0;
    }
    info->scissor = 0;
    SpriteFrame frame;
    mgMulMatrix(frame.local_screen, info->world_screen, matrix);
    frame.axis_x = info->view_screen[0][0] * mgDistVector(matrix[0]);
    frame.axis_y = info->view_screen[1][1] * mgDistVector(matrix[1]);
    frame.fog = {info->fog.offset, info->fog.near_value, info->fog.far_value, info->fog.scale};
    frame.near = info->guard_min[3];
    frame.far = info->guard_max[3];
    int fogged = info->attr->fog != 0 && info->fog_enable != 0;
    u_int fog_colour = info->fog.r | info->fog.g << 8 | info->fog.b << 16;
    if (info->attr->fog > MG_FOG_SCENE) fog_colour = 0;
    RenderState state;
    gs_base_state(&state);
    state.blend = 1;
    state.blending = gs_blending(gs.alpha);
    state.tests = gs_tests(gs.test);
    state.depth_write = !gs.zbuf.ZMSK;
    state.fog = fogged;
    state.fog_color = fog_colour;
    int width = 1, height = 1;
    for (const SpriteOp &op : found->second) {
        if (op.kind == SPRITE_ENV) {
            state.tests = gs_tests(op.env.test);
            state.blending = gs_blending(op.env.alpha);
            state.depth_write = !op.env.zbuf.bits.zmsk;
            int macro = ((mgCDrawEnv &)op.env).GetAlphaMacroID();
            if (macro == 2 || macro == 3) state.fog_color = 0;
        } else if (op.kind == SPRITE_TEXTURE) {
            float scale_u, scale_v;
            RenderState textured = state;
            gs_texture_state(&textured, op.texture, &scale_u, &scale_v);
            state.texture = textured.texture;
            state.linear = textured.linear;
            state.decal = textured.decal;
            state.vertex_alpha = textured.vertex_alpha;
            state.texa = textured.texa;
            state.repeat_u = gs.clamp.WMS == GS_WRAP_REPEAT;
            state.repeat_v = gs.clamp.WMT == GS_WRAP_REPEAT;
            width = state.texture ? render_width(state.texture) : 1;
            height = state.texture ? render_height(state.texture) : 1;
        } else if (op.kind == SPRITE_ITEM) {
            draw_sprite(frame, op, state, width, height);
        }
    }
    return 0;
}

PORT_OVERRIDE uptr mgCSprite::CreatePacket(mgCDrawManager *manager) {
    (void)manager;
    if (texture != NULL) {
        gs_texture_source(texture);
        gs_write(SCE_GS_TEX1_1, texture->tex1_bits);
        gs_write(SCE_GS_TEX0_1, texture->tex0_bits);
    }
    int screen_z = 0;
    if (!(depth < 1.0f)) {
        float position[4] = {0.0f, 0.0f, depth, 1.0f};
        int projected[4];
        if (mgTransViewPrim(projected, position) != 0) screen_z = projected[2];
    }
    gs_write(SCE_GS_PRIM, SCE_GS_SET_PRIM(SCE_GS_PRIM_SPRITE, 0, texture != NULL, 0, 1, 0, 1, 0, 0));
    gs_write(SCE_GS_RGBAQ, color.value);
    sceGsUv corner_uv = {};
    sceGsXyz corner = {};
    corner.Z = (u_int)screen_z;
    corner_uv.U = uv.left;
    corner_uv.V = uv.top;
    corner.X = screen.left + mgScreenOffx * GS_SUBPIXELS;
    corner.Y = screen.top + mgScreenOffy * GS_SUBPIXELS;
    gs_write(SCE_GS_UV, corner_uv.value);
    gs_write(SCE_GS_XYZ2, corner.value);
    corner_uv.U = uv.right;
    corner_uv.V = uv.bottom;
    corner.X = screen.right + mgScreenOffx * GS_SUBPIXELS;
    corner.Y = screen.bottom + mgScreenOffy * GS_SUBPIXELS;
    gs_write(SCE_GS_UV, corner_uv.value);
    gs_write(SCE_GS_XYZ2, corner.value);
    return 0;
}
