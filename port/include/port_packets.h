#pragma once
#include <stdint.h>

typedef struct {
    uint32_t *current;
    uint32_t *base;
} PortVifPacket;

void sceVif1PkInit(PortVifPacket *packet, uint32_t *base);
void sceVif1PkReset(PortVifPacket *packet);
uint32_t *sceVif1PkTerminate(PortVifPacket *packet);
void sceVif1PkReserve(PortVifPacket *packet, int count);
