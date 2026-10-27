#include "mg_texture.hpp"
#include "mglib.hpp"
#include "render.h"
#include "sce/libgraph.h"
#include "wavetable.hpp"

int GetZBufVram(int *size);
int CheckCopyToZBufVram(mgCTexture *texture, int *size);

static void place_textures(mgCTextureManager *manager, int block, int loading) {
    int vram = manager->vram_top;
    int fix = manager->vram_fix;
    int zbuf_size;
    int zbuf = GetZBufVram(&zbuf_size);
    int zbuf_end = zbuf + zbuf_size;
    for (mgCTexture *texture = manager->blocks[block].texture; texture != NULL; texture = texture->next) {
        int size;
        if (CheckCopyToZBufVram(texture, &size) && zbuf + size < zbuf_end) {
            texture->tex0.TBP0 = zbuf;
            texture->tex0.PSM = SCE_GS_PSMT8H;
            zbuf += size;
        } else {
            texture->tex0.TBP0 = vram;
            vram += texture->vram_size;
            if (texture->bpp == 8) texture->tex0.PSM = SCE_GS_PSMT8;
        }
        if (texture->bpp <= 8) {
            fix -= MG_TEXTURE_CLUT_BLOCKS;
            texture->tex0.CBP = fix;
        }
        for (int level = 0; loading && level < MG_TEXTURE_LEVEL_MAX && texture->image[level] != NULL; level++)
            port_texture_loaded(texture->image[level]);
    }
}

PORT_OVERRIDE void mgCTextureManager::EndEnterTexture(int block) {
    if (last_block != block) place_textures(this, block, 0);
}

PORT_OVERRIDE void mgCTextureManager::ReloadTexture(int index, sceVif1Packet *packet) {
    if (index < 0 || index >= block_max) {
        last_block = -1;
        return;
    }
    int previous = last_block;
    if (previous != index) place_textures(this, index, 1);
    last_block = index;
    if (previous == index || blocks[index].anime == NULL) return;
    render_set_animating(1);
    blocks[index].anime->TexAnime(index, packet);
    render_set_animating(0);
}

PORT_OVERRIDE void CWaveTable::CreateTexture(mgCTexture *texture) {
    render_set_animating(1);
    CreateTextureRetail(texture);
    render_set_animating(0);
}

PORT_OVERRIDE void mgCTextureManager::ReloadCLUT(mgCTexture *texture, sceVif1Packet *packet) {
    (void)texture;
    (void)packet;
}
