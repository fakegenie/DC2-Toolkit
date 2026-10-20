#include "mg_drawprim.hpp"
#include <cassert>
#include <cstdint>
#include <cstring>

int mgSendVuProg(u_int *, int) {
    assert(false);
    return 0;
}

extern "C" u_int *sceVif1PkTerminate(sceVif1Packet *) {
    assert(false);
    return nullptr;
}

extern "C" void sceVif1PkReserve(sceVif1Packet *, int) {
    assert(false);
}

static void packet_tables() {
    alignas(mgCDrawManager) unsigned char manager_bytes[sizeof(mgCDrawManager)] = {};
    alignas(mgCTextureManager) unsigned char textures_bytes[sizeof(mgCTextureManager)] = {};
    mgCDrawManager *manager = reinterpret_cast<mgCDrawManager *>(manager_bytes);
    mgCTextureManager *textures = reinterpret_cast<mgCTextureManager *>(textures_bytes);
    u_long128 buffer[1024];
    memset(buffer, 0xa5, sizeof(buffer));
    mgCMemory memory;
    memory.stSetBuffer(buffer, 1024);
    textures->block_max = 9;
    manager->texture_manager = textures;
    manager->sort_num = 4;
    manager->BeginDraw(&memory, nullptr);
    assert((uintptr_t)(manager->packet_list + 9) <= (uintptr_t)manager->unk_14);
    for (int i = 0; i < 9; i++) {
        assert(manager->packet_list[i] == nullptr);
        assert(manager->unk_14[i] == 0 && manager->packet_num[i] == 0);
    }
    for (int i = 0; i < 4; i++) assert(manager->sort_table[i] == nullptr);
    mgSORT_PACKET nodes[3] = {};
    nodes[0].group = 8;
    nodes[0].next = nodes + 1;
    nodes[1].group = 3;
    nodes[1].next = nodes + 2;
    nodes[2].group = 8;
    manager->sort_table[0] = nodes;
    manager->packet_num[8] = 2;
    manager->packet_num[3] = 1;
    manager->PreEndDraw();
    assert(manager->packet_list[8][0] == nodes && manager->packet_list[8][1] == nodes + 2);
    assert(manager->packet_list[3][0] == nodes + 1);
    assert(manager->packet_cursor[8] == manager->packet_list[8] + 2);
    assert(manager->packet_cursor[3] == manager->packet_list[3] + 1);
    assert((uintptr_t)(manager->packet_cursor + 9) <= (uintptr_t)manager->packet_list[3]);
    manager->ClearTable();
    for (int i = 0; i < 9; i++) assert(manager->packet_list[i] == nullptr);
    for (int i = 0; i < 4; i++) assert(manager->sort_table[i] == nullptr);
    manager->packet_list[0] = reinterpret_cast<mgSORT_PACKET **>(UINT64_C(0x1122334455667788));
    assert(manager->Draw(1, nullptr) == 1);
}

int main() {
    packet_tables();
    alignas(mgCDrawPrim) unsigned char primitive_bytes[sizeof(mgCDrawPrim)] = {};
    alignas(mgCTexture) unsigned char texture_bytes[sizeof(mgCTexture)] = {};
    mgCDrawPrim *primitive = reinterpret_cast<mgCDrawPrim *>(primitive_bytes);
    mgCTexture *texture = reinterpret_cast<mgCTexture *>(texture_bytes);
    u_long packet[6] = {};
    texture->tex0_bits = UINT64_C(0x123456789abcdef0);
    texture->width = 256;
    texture->height = 384;
    texture->bpp = 8;
    texture->block = 3;
    strcpy(texture->name, "texture");
    primitive->bilinear = 1;
    primitive->z_mask = 37;
    primitive->command_write = packet;
    primitive->Texture(texture);
    assert(primitive->texture.tex0_bits == texture->tex0_bits);
    assert(primitive->texture.width == 256 && primitive->texture.height == 384);
    assert(primitive->texture.block == 3 && primitive->texture.bpp == 8);
    assert(!strcmp(primitive->texture.name, "texture"));
    assert(packet[0] == 0 && packet[1] == SCE_GS_TEXFLUSH);
    assert(packet[2] == 0x60 && packet[3] == SCE_GS_TEX1_1);
    assert(packet[4] == texture->tex0_bits && packet[5] == SCE_GS_TEX0_1);
    assert(primitive->command_write == packet + 6);
    assert(primitive->bilinear == 1 && primitive->z_mask == 37);
}
