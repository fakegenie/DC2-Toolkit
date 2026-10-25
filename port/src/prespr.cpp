#include "prespr.hpp"
#include "draw.hpp"

PORT_OVERRIDE void CPreSprite::SetScirror(int x, int y, int w, int h) {
    sceGsScissor scissor = {};
    scissor.SCAX0 = x;
    scissor.SCAX1 = x + w;
    scissor.SCAY0 = y;
    scissor.SCAY1 = y + h;
    Direct(SCE_GS_SCISSOR_1, scissor.value);
}

PORT_OVERRIDE void CPreSprite::SetAlphaBlend(int mode) {
    if (mode == MG_ALPHA_BLEND_NORMAL)
        Direct(SCE_GS_ALPHA_1, SCE_GS_SET_ALPHA(SCE_GS_ALPHA_CS, SCE_GS_ALPHA_CD, SCE_GS_ALPHA_AS, SCE_GS_ALPHA_CD, 0));
    if (mode == MG_ALPHA_BLEND_ADD)
        Direct(SCE_GS_ALPHA_1, SCE_GS_SET_ALPHA(SCE_GS_ALPHA_CS, SCE_GS_ALPHA_ZERO, SCE_GS_ALPHA_AS, SCE_GS_ALPHA_CD, 0));
    if (mode == MG_ALPHA_BLEND_SUB)
        Direct(SCE_GS_ALPHA_1, SCE_GS_SET_ALPHA(SCE_GS_ALPHA_ZERO, SCE_GS_ALPHA_CS, SCE_GS_ALPHA_AS, SCE_GS_ALPHA_CD, 0));
    if (mode == MG_ALPHA_BLEND_NONE)
        Direct(SCE_GS_ALPHA_1, SCE_GS_SET_ALPHA(SCE_GS_ALPHA_ZERO, SCE_GS_ALPHA_ZERO, SCE_GS_ALPHA_FIX, SCE_GS_ALPHA_CS, GS_ALPHA_ONE));
}
