#include "port_packets.h"

void sceVif1PkInit(PortVifPacket *packet, uint32_t *base) {
    packet->current = packet->base = base;
}

void sceVif1PkReset(PortVifPacket *packet) {
    packet->current = packet->base;
}

uint32_t *sceVif1PkTerminate(PortVifPacket *packet) {
    return packet->current;
}

void sceVif1PkReserve(PortVifPacket *packet, int count) {
    packet->current += count;
}
