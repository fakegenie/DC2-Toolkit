#include "libgraph.h"
#include "port_graphics.h"
#include <cstring>

constexpr int FRAME_WIDTH_UNIT = 64;

static std::size_t image_bytes(int psm, int width, int height) {
    std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    switch (psm) {
    case SCE_GS_PSMCT24:
    case SCE_GS_PSMZ24:
        return pixels * 3;
    case SCE_GS_PSMCT16:
    case SCE_GS_PSMCT16S:
        return pixels * 2;
    case SCE_GS_PSMT8:
    case SCE_GS_PSMT8H:
        return pixels;
    case SCE_GS_PSMT4:
        return (pixels + 1) / 2;
    default:
        return pixels * 4;
    }
}

void sceGsResetGraph(short, short, short, short) {}

void sceGsResetPath() {}

int sceGsSyncPath(int, u_short) {
    return 0;
}

static void set_draw_buffer(sceGsDrawEnv1 *draw, int psm, int width, int ztest, int zpsm) {
    draw->frame1.FBW = (width + FRAME_WIDTH_UNIT - 1) / FRAME_WIDTH_UNIT;
    draw->frame1.PSM = psm;
    draw->zbuf1.PSM = zpsm - SCE_GS_PSMZ32;
    draw->zbuf1.ZMSK = !ztest;
}

void sceGsSetDefDBuff(sceGsDBuff *db, int psm, int w, int, int ztest, int zpsm, int) {
    *db = {};
    set_draw_buffer(&db->draw0, psm, w, ztest, zpsm);
    set_draw_buffer(&db->draw1, psm, w, ztest, zpsm);
}

void sceGsSwapDBuff(sceGsDBuff *, int) {}

void sceGsSetDefStoreImage(sceGsStoreImage *si, short sbp, short sbw, short spsm, short ssax, short ssay, short rrw,
                           short rrh) {
    *si = {sbp, sbw, spsm, ssax, ssay, rrw, rrh};
}

void sceGsExecStoreImage(sceGsStoreImage *si, u_long128 *dest) {
    std::memset(dest, 0, image_bytes(si->spsm, si->rrw, si->rrh));
    if (si->spsm == SCE_GS_PSMZ32 || si->spsm == SCE_GS_PSMZ24)
        port_depth_download(si->ssax, si->ssay, si->rrw, si->rrh, reinterpret_cast<unsigned *>(dest));
}
