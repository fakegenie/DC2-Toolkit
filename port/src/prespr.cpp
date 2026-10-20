#include "prespr.hpp"

PORT_OVERRIDE void CPreSprite::SetScirror(int x, int y, int w, int h) {
    Direct(SCE_GS_SCISSOR_1, (u_long)x | (u_long)(x + w) << 16 | (u_long)y << 32 | (u_long)(y + h) << 48);
}

PORT_OVERRIDE void CPreSprite::SetAlphaBlend(int mode) {
    if (mode == MG_ALPHA_BLEND_NORMAL) Direct(SCE_GS_ALPHA_1, 0x44);
    if (mode == MG_ALPHA_BLEND_ADD) Direct(SCE_GS_ALPHA_1, 0x48);
    if (mode == MG_ALPHA_BLEND_SUB) Direct(SCE_GS_ALPHA_1, 0x42);
    if (mode == MG_ALPHA_BLEND_NONE) Direct(SCE_GS_ALPHA_1, 0x2a | (u_long)0x80 << 32);
}
