#pragma once
#include <stdint.h>

typedef unsigned __int128 PortQword;
typedef struct {
    uint32_t *current;
    uint32_t *base;
    uint32_t *dma;
    uint32_t *direct;
    uint32_t *reserved[4];
} PortVifPacket;
typedef struct {
    uint32_t *current;
    uint32_t *base;
    uint32_t *dma;
    uint32_t *gif;
} PortGifPacket;

void sceVif1PkInit(PortVifPacket *packet, uint32_t *base);
void sceVif1PkReset(PortVifPacket *packet);
uint32_t *sceVif1PkTerminate(PortVifPacket *packet);
uint32_t *sceVif1PkEnd(PortVifPacket *packet, uint32_t flags);
void sceVif1PkCnt(PortVifPacket *packet, int flags);
void sceVif1PkCall(PortVifPacket *packet, PortQword *address, uint32_t flags);
void sceVif1PkReserve(PortVifPacket *packet, int count);
void sceVif1PkOpenDirectCode(PortVifPacket *packet, int irq);
void sceVif1PkCloseDirectCode(PortVifPacket *packet);
void sceVif1PkOpenGifTag(PortVifPacket *packet, PortQword tag);
void sceVif1PkCloseGifTag(PortVifPacket *packet);
void sceVif1PkAddGsAD(PortVifPacket *packet, int reg, uint64_t data);
void sceGifPkInit(PortGifPacket *packet, PortQword *base);
void sceGifPkReset(PortGifPacket *packet);
uint32_t *sceGifPkTerminate(PortGifPacket *packet);
uint32_t *sceGifPkEnd(PortGifPacket *packet, int data0, int data1, int flags);
void sceGifPkCnt(PortGifPacket *packet, int data0, int data1, int flags);
uint32_t *sceGifPkReserve(PortGifPacket *packet, int count);
void sceGifPkRef(PortGifPacket *packet, PortQword *address, int count, int data0, int data1, int flags);
void sceGifPkOpenGifTag(PortGifPacket *packet, PortQword tag);
void sceGifPkCloseGifTag(PortGifPacket *packet);
void sceGifPkAddGsAD(PortGifPacket *packet, int reg, uint64_t data);
