#include "mg_drawprim.hpp"
#include "draw.hpp"
#include "mglib.hpp"
#include "mg_texture.hpp"
#include "sce/libgraph.h"
#include "vu0_lanes.hpp"
#include <cstring>

enum { DIRECT_LIMIT = 4096 };

static u_long packed_registers;
static int packed_count;
static int packed_index;
static int packed_mode;
static u_long128 direct_buffer[DIRECT_LIMIT];
static int direct_pending;

static void packed_data(const u_int *words) {
    int reg = packed_mode ? (int)((packed_registers >> (packed_index * 4)) & 15) : 0xe;
    gs_packed(reg, words);
    if (packed_mode && packed_count > 0) packed_index = (packed_index + 1) % packed_count;
}

static void drain() {
    int count = direct_pending;
    direct_pending = 0;
    for (int i = 0; i < count; ++i) packed_data((const u_int *)&direct_buffer[i]);
}

PORT_OVERRIDE void mgCDrawPrim::Begin2() {
    drain();
    disabled = 1;
    if (memory == NULL || vif_packet == NULL || draw_manager == NULL || draw_manager->render_info == NULL) return;
    disabled = 0;
    sceGsZbuf zbuf __attribute__((aligned(4))) = sceGsZbuf(draw_manager->render_info->draw_env[0].zbuf);
    draw_env.zbuf = zbuf;
    draw_env.SetZBuf(z_mask);
    gs_write(SCE_GS_TEST_1, draw_env.test.value);
    gs_write(SCE_GS_ZBUF_1, *(u_long *)&draw_env.zbuf);
    gs_write(SCE_GS_ALPHA_1, draw_env.alpha.value);
}

PORT_OVERRIDE void mgCDrawPrim::BeginDma() {
    drain();
    packed_mode = 0;
    gs_write(SCE_GS_PRIM, *(u_long *)&prim);
}

PORT_OVERRIDE void mgCDrawPrim::EndDma() {
    drain();
}

PORT_OVERRIDE void mgCDrawPrim::BeginPrim2(int type) {
    drain();
    prim.PRIM = type;
    q = 1.0f;
    BeginDma();
}

PORT_OVERRIDE void mgCDrawPrim::BeginPrim2(int type, u_int data_a, u_int data_b, int unit_count) {
    drain();
    prim.PRIM = type;
    q = 1.0f;
    gs_write(SCE_GS_PRIM, *(u_long *)&prim & 0x7ff);
    packed_mode = 1;
    packed_registers = (u_long)data_a | (u_long)data_b << 32;
    packed_count = unit_count;
    packed_index = 0;
}

PORT_OVERRIDE void mgCDrawPrim::EndPrim2() {
    drain();
    packed_mode = 0;
}

PORT_OVERRIDE void mgCDrawPrim::End2() {
    drain();
    packed_mode = 0;
}

PORT_OVERRIDE void mgCDrawPrim::Data(int *data) {
    drain();
    packed_data((const u_int *)data);
}

void mgCDrawPrim::Data0(float *data) {
    int lanes[4] = {vu0::Ftoi(data[0]), vu0::Ftoi(data[1]), vu0::Ftoi(data[2]), vu0::Ftoi(data[3])};
    Data(lanes);
}

void mgCDrawPrim::Data4(float *data) {
    int lanes[4] = {vu0::Ftoi4(data[0]), vu0::Ftoi4(data[1]), vu0::Ftoi4(data[2]), vu0::Ftoi4(data[3])};
    Data(lanes);
}

PORT_OVERRIDE u_long128 *mgCDrawPrim::DirectData(int count) {
    drain();
    if (count > DIRECT_LIMIT) count = DIRECT_LIMIT;
    memset(direct_buffer, 0, sizeof(u_long128) * (size_t)count);
    direct_pending = count;
    return direct_buffer;
}

PORT_OVERRIDE void mgCDrawPrim::Vertex4(int x, int y, int z) {
    drain();
    int offset_x = 0;
    int offset_y = 0;
    GetOffset(&offset_x, &offset_y);
    gs_write(SCE_GS_XYZ2, ((u_long)(u_int)z << 32) | (u_long)((x + offset_x) & 0xffff) |
                              (u_long)((y + offset_y) & 0xffff) << 16);
}

PORT_OVERRIDE void mgCDrawPrim::Color(int r, int g, int b, int a) {
    drain();
    gs_write(SCE_GS_RGBAQ, ((u_long)q_bits << 32) | (u_long)(r & 255) | (u_long)(g & 255) << 8 |
                               (u_long)(b & 255) << 16 | (u_long)(a & 255) << 24);
}

PORT_OVERRIDE void mgCDrawPrim::TextureCrd4(int u, int v) {
    drain();
    gs_write(SCE_GS_UV, (u_long)(u & 0x3fff) | (u_long)(v & 0x3fff) << 16);
}

PORT_OVERRIDE void mgCDrawPrim::Direct(unsigned long reg, unsigned long data) {
    drain();
    gs_write((int)reg, data);
}

PORT_OVERRIDE void mgCDrawPrim::Texture(mgCTexture *source) {
    drain();
    if (source == NULL) return;
    memcpy((void *)&texture, (const void *)source, sizeof texture);
    texture.Bilinear(bilinear);
    gs_texture_source(&texture);
    gs_write(SCE_GS_TEX1_1, texture.tex1_bits);
    gs_write(SCE_GS_TEX0_1, texture.tex0_bits);
}

PORT_OVERRIDE int mgCDrawManager::Draw(int group, sceVif1Packet *vif_packet) {
    (void)vif_packet;
    if (group < 0 || group >= texture_manager->block_max) return 0;
    if (draw_order != NULL) group = order_index[group];
    if (group < 0) return 0;
    mgSORT_PACKET **entries = packet_list[group];
    if (entries == NULL) return 1;
    for (int i = packet_num[group] - 1; i >= 0; i--)
        if (entries[i] != NULL) visual_packet_draw(entries[i]->common, entries[i]->packet);
    return 1;
}
