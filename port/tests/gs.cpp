#include "libgraph.h"
#include <cassert>
#include <cstring>

extern "C" void port_depth_download(int x, int y, int width, int height, unsigned *pixels) {
    for (int i = 0; i < width * height; i++) pixels[i] = static_cast<unsigned>(x + y + i);
}

int main() {
    sceGsDBuff db;
    std::memset(&db, 0xff, sizeof db);
    sceGsSetDefDBuff(&db, SCE_GS_PSMCT32, 640, 448, SCE_GS_ZGEQUAL, SCE_GS_PSMZ24, 1);
    assert(db.draw0.frame1.FBW == 10 && db.draw1.frame1.FBW == 10);
    assert(db.draw0.frame1.PSM == SCE_GS_PSMCT32 && db.draw0.frame1.FBP == 0);
    assert(db.draw1.zbuf1.PSM == SCE_GS_PSMZ24 - SCE_GS_PSMZ32 && !db.draw1.zbuf1.ZMSK);
    assert(db.clear0.rgbaq.value == 0);
    sceGsStoreImage image;
    unsigned pixels[16];
    sceGsSetDefStoreImage(&image, 512, 10, SCE_GS_PSMZ32, 12, 34, 4, 4);
    sceGsExecStoreImage(&image, reinterpret_cast<u_long128 *>(pixels));
    assert(pixels[0] == 46 && pixels[15] == 61);
    std::memset(pixels, 0xff, sizeof pixels);
    sceGsSetDefStoreImage(&image, 0, 10, SCE_GS_PSMCT32, 0, 0, 4, 4);
    sceGsExecStoreImage(&image, reinterpret_cast<u_long128 *>(pixels));
    for (unsigned pixel : pixels) assert(pixel == 0);
}
