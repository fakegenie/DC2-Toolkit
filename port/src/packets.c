#include "port_packets.h"
#include "port_window.h"
#include <stddef.h>
#include <string.h>

static uint32_t *packet_align(uint32_t *cursor) {
    while ((uintptr_t)cursor & 15) *cursor++ = 0;
    return cursor;
}

static uint32_t *packet_terminate(uint32_t *cursor, uint32_t *dma) {
    cursor = packet_align(cursor);
    if (dma) *dma += (uint32_t)(cursor - dma) / 4 - 1;
    return cursor;
}

static uint32_t *packet_close_gif(uint32_t *cursor, uint32_t *tag) {
    uint64_t value;
    memcpy(&value, tag, 8);
    unsigned format = (unsigned)(value >> 58) & 3;
    unsigned registers = (unsigned)(value >> 60);
    if (!registers) registers = 16;
    unsigned size = (unsigned)(cursor - tag - 4) / 2;
    if (format != 1) size /= 2;
    unsigned loops = format == 2 ? size : (size + registers - 1) / registers;
    value += loops;
    memcpy(tag, &value, 8);
    return packet_align(cursor);
}

static void packet_ad(uint32_t *cursor, int reg, uint64_t data) {
    uint64_t address = (uint32_t)reg;
    memcpy(cursor, &data, 8);
    memcpy(cursor + 2, &address, 8);
}

void sceVif1PkInit(PortVifPacket *packet, uint32_t *base) {
    packet->current = packet->base = base;
    packet->dma = NULL;
}

void sceVif1PkReset(PortVifPacket *packet) {
    packet->current = packet->base;
    packet->dma = NULL;
}

uint32_t *sceVif1PkTerminate(PortVifPacket *packet) {
    packet->current = packet_terminate(packet->current, packet->dma);
    packet->dma = NULL;
    return packet->current;
}

static void vif_dma(PortVifPacket *packet, uint32_t command, uint32_t address) {
    sceVif1PkTerminate(packet);
    packet->dma = packet->current;
    *packet->current++ = command;
    *packet->current++ = address;
    packet->direct = NULL;
}

void sceVif1PkCnt(PortVifPacket *packet, int flags) {
    vif_dma(packet, UINT32_C(0x10000000) | (uint32_t)flags, 0);
}

void sceVif1PkCall(PortVifPacket *packet, PortQword *address, uint32_t flags) {
    vif_dma(packet, UINT32_C(0x50000000) | flags, port_address(address) & UINT32_C(0x9fffffff));
}

uint32_t *sceVif1PkEnd(PortVifPacket *packet, uint32_t flags) {
    vif_dma(packet, UINT32_C(0x70000000) | flags, 0);
    return packet->current;
}

void sceVif1PkReserve(PortVifPacket *packet, int count) {
    packet->current += count;
}

void sceVif1PkOpenDirectCode(PortVifPacket *packet, int irq) {
    while (((uintptr_t)packet->current & 15) != 12) *packet->current++ = 0;
    packet->direct = packet->current;
    *packet->current++ = irq ? UINT32_C(0xd0000000) : UINT32_C(0x50000000);
}

void sceVif1PkCloseDirectCode(PortVifPacket *packet) {
    *packet->direct += (uint32_t)(packet->current - packet->direct - 1) / 4;
    packet->direct = NULL;
}

void sceVif1PkOpenGifTag(PortVifPacket *packet, PortQword tag) {
    packet->reserved[1] = packet->current;
    memcpy(packet->current, &tag, 16);
    packet->current += 4;
}

void sceVif1PkCloseGifTag(PortVifPacket *packet) {
    packet->current = packet_close_gif(packet->current, packet->reserved[1]);
    packet->reserved[1] = NULL;
}

void sceVif1PkAddGsAD(PortVifPacket *packet, int reg, uint64_t data) {
    packet_ad(packet->current, reg, data);
    packet->current += 4;
}

void sceGifPkInit(PortGifPacket *packet, PortQword *base) {
    packet->current = packet->base = (uint32_t *)base;
    packet->dma = NULL;
}

void sceGifPkReset(PortGifPacket *packet) {
    packet->current = packet->base;
    packet->dma = NULL;
}

uint32_t *sceGifPkTerminate(PortGifPacket *packet) {
    packet->current = packet_terminate(packet->current, packet->dma);
    packet->dma = NULL;
    return packet->current;
}

static void gif_dma(PortGifPacket *packet, uint32_t command, uint32_t address, int data0, int data1) {
    sceGifPkTerminate(packet);
    packet->dma = packet->current;
    *packet->current++ = command;
    *packet->current++ = address;
    *packet->current++ = (uint32_t)data0;
    *packet->current++ = (uint32_t)data1;
}

void sceGifPkCnt(PortGifPacket *packet, int data0, int data1, int flags) {
    gif_dma(packet, UINT32_C(0x10000000) | (uint32_t)flags, 0, data0, data1);
}

uint32_t *sceGifPkEnd(PortGifPacket *packet, int data0, int data1, int flags) {
    gif_dma(packet, UINT32_C(0x70000000) | (uint32_t)flags, 0, data0, data1);
    return packet->current;
}

void sceGifPkRef(PortGifPacket *packet, PortQword *address, int count, int data0, int data1, int flags) {
    gif_dma(packet, UINT32_C(0x30000000) | (uint32_t)count | (uint32_t)flags,
            port_address(address) & UINT32_C(0x9fffffff), data0, data1);
    packet->dma = NULL;
}

uint32_t *sceGifPkReserve(PortGifPacket *packet, int count) {
    uint32_t *start = packet->current;
    packet->current += count;
    return start;
}

void sceGifPkOpenGifTag(PortGifPacket *packet, PortQword tag) {
    packet->gif = packet->current;
    memcpy(packet->current, &tag, 16);
    packet->current += 4;
}

void sceGifPkCloseGifTag(PortGifPacket *packet) {
    packet->current = packet_close_gif(packet->current, packet->gif);
    packet->gif = NULL;
}

void sceGifPkAddGsAD(PortGifPacket *packet, int reg, uint64_t data) {
    packet_ad(packet->current, reg, data);
    packet->current += 4;
}
