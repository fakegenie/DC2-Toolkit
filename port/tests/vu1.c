#include "vu1.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned kicks;
static size_t kicked_size;
static unsigned char kicked[16384];

int port_gif_feed(const void *data, size_t size, unsigned path)
{
    assert(path == 1);
    assert(size <= sizeof(kicked));
    memcpy(kicked, data, size);
    kicked_size = size;
    kicks++;
    return 0;
}

static void pair(unsigned pc, uint32_t upper, uint32_t lower)
{
    memcpy(port_vu1_code + pc * 8, &lower, 4);
    memcpy(port_vu1_code + pc * 8 + 4, &upper, 4);
}

static uint32_t fb(float value)
{
    uint32_t result;
    memcpy(&result, &value, 4);
    return result;
}

static uint32_t read_data(unsigned address, unsigned lane)
{
    uint32_t result;
    memcpy(&result, port_vu1_data + address * 16 + lane * 4, 4);
    return result;
}

static void data(unsigned address, float x, float y, float z, float w)
{
    float values[4] = {x, y, z, w};
    memcpy(port_vu1_data + address * 16, values, 16);
}

static void stop(unsigned pc)
{
    pair(pc, 0x400002ff, 0x8000033c);
    pair(pc + 1, 0x000002ff, 0x8000033c);
}

static void constants_and_masks(void)
{
    port_vu1_reset();
    pair(0, 0x800002ff, fb(2));
    pair(1, 0x81e00062, fb(3));
    pair(2, 0x00800062, 0x8000033c);
    stop(3);
    assert(port_vu1_start(0) == 0);
    assert(port_vu1.vf[1][0] == fb(2));
    assert(port_vu1.vf[1][1] == fb(3));
    assert(port_vu1.vf[1][2] == fb(2));
    assert(port_vu1.vf[1][3] == fb(3));
    assert(port_vu1.vf[0][3] == fb(1));
    assert(port_vu1.i == fb(3));
}

static void branch_resume_and_integer_store(void)
{
    port_vu1_reset();
    pair(0, 0x000002ff, 0x10010020);
    pair(1, 0x400002ff, 0x40000002);
    pair(2, 0x000002ff, 0x0b01000a);
    pair(4, 0x000002ff, 0x10020009);
    stop(5);
    assert(port_vu1_start(0) == 0);
    assert(read_data(10, 0) == 32);
    assert(port_vu1.pc == 4);
    assert(port_vu1.vi[2] == 0);
    assert(port_vu1_continue() == 0);
    assert(port_vu1.vi[2] == 9);
}

static void paired_reads(void)
{
    port_vu1_reset();
    data(10, 8, 9, 10, 11);
    port_vu1.vf[1][0] = fb(2);
    pair(0, 0x010008a8, 0x0101000a);
    stop(1);
    assert(port_vu1_start(0) == 0);
    assert(port_vu1.vf[2][0] == fb(2));
    assert(port_vu1.vf[1][0] == fb(8));
}

static void mac_delay(void)
{
    port_vu1_reset();
    port_vu1.vi[15] = 0xffff;
    port_vu1.vf[1][0] = fb(-2);
    pair(0, 0x010008ac, 0x8000033c);
    for (unsigned pc = 1; pc < 5; pc++) pair(pc, 0x000002ff, 0x34007800 | (pc << 16));
    stop(5);
    assert(port_vu1_start(0) == 0);
    assert(port_vu1.vi[1] == 0);
    assert(port_vu1.vi[4] == 128);
}

static void q_delay_and_wait(void)
{
    port_vu1_reset();
    port_vu1.vf[1][0] = fb(2);
    port_vu1.q = fb(3);
    pair(0, 0x000002ff, 0x806103bc);
    pair(1, 0x01000120, 0x8000033c);
    pair(2, 0x01000160, 0x800003bf);
    stop(3);
    assert(port_vu1_start(0) == 0);
    assert(port_vu1.vf[4][0] == fb(3));
    assert(port_vu1.vf[5][0] == fb(0.5f));
    assert(port_vu1.q == fb(0.5f));
}

static void clip_history(void)
{
    port_vu1_reset();
    port_vu1.vf[1][0] = fb(2);
    port_vu1.vf[1][1] = fb(-2);
    port_vu1.vf[1][3] = fb(1);
    port_vu1.vf[2][2] = fb(2);
    port_vu1.vf[2][3] = fb(1);
    pair(0, 0x01c109ff, 0x8000033c);
    pair(1, 0x01c211ff, 0x8000033c);
    pair(2, 0x000002ff, 0x8000033c);
    pair(3, 0x000002ff, 0x8000033c);
    pair(4, 0x000002ff, 0x38030000);
    pair(5, 0x000002ff, 0x38040000);
    stop(6);
    assert(port_vu1_start(0) == 0);
    assert(port_vu1.vi[3] == 9);
    assert(port_vu1.vi[4] == (9 * 64 + 16));
}

static void gif_wrap(void)
{
    port_vu1_reset();
    uint32_t tag[4] = {0x8001, 0x10000000, 0xe, 0};
    memcpy(port_vu1_data + 1023 * 16, tag, 16);
    data(0, 1, 2, 3, 4);
    port_vu1.vi[1] = 1023;
    pair(0, 0x000002ff, 0x80000efc);
    stop(1);
    assert(port_vu1_start(0) == 0);
    assert(kicks == 1 && kicked_size == 32);
    assert(memcmp(kicked, tag, 16) == 0);
    assert(memcmp(kicked + 16, port_vu1_data, 16) == 0);
}

static void retail_setup(const char *path)
{
    port_vu1_reset();
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fread(port_vu1_code, 1, 16384, file) == 16384);
    fclose(file);
    for (unsigned row = 0; row < 4; row++) {
        float v[4] = {0, 0, 0, 0};
        v[row] = 1;
        memcpy(port_vu1_data + (4 + row) * 16, v, 16);
        memcpy(port_vu1_data + (8 + row) * 16, v, 16);
        if (row < 3) {
            v[3] = 0;
            memcpy(port_vu1_data + (12 + row) * 16, v, 16);
        }
    }
    data(24, 2, 3, 5, 1);
    data(57, 2300, 2250, 0, 1000);
    data(58, 1800, 1850, 0, 5);
    assert(port_vu1_start(0) == 0);
    assert(port_vu1.pc == 2);
    assert(read_data(21, 0) == fb(4095));
    assert(read_data(22, 0) == fb(1));
    assert(read_data(30, 0) == 0);
    assert(read_data(44, 0) == fb(2));
    assert(read_data(44, 1) == fb(3));
    assert(read_data(44, 2) == fb(5));
    data(10, 0, 0, -1, 0);
    assert(port_vu1_start(0) == 0);
    assert(read_data(30, 0) == 32);
    uint32_t scissor = 2;
    memcpy(port_vu1_data + 38 * 16, &scissor, 4);
    assert(port_vu1_start(0) == 0);
    assert(read_data(21, 0) == fb(2300));
    assert(read_data(22, 0) == fb(1800));
}

static void retail_sprite(const char *path)
{
    port_vu1_reset();
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fread(port_vu1_code, 1, 16384, file) == 16384);
    fclose(file);
    data(4, 2, 0, 0, 0);
    data(5, 0, 2, 0, 0);
    data(6, 0, 0, 1, 0);
    data(7, 8192, 8192, 40, 4);
    data(25, 2, 0, 0, 0);
    data(26, 0, 2, 0, 0);
    data(57, 4095, 4095, 0, 200);
    data(58, 1, 1, 0, 1);
    data(59, 0.5f, 1, 0, 8);
    assert(port_vu1_start(0) == 0);
    port_vu1_top = 60;
    uint32_t tag[4] = {0x8001, 0x50000000, 0x53531, 0};
    memcpy(port_vu1_data + 60 * 16, tag, 16);
    uint32_t header[4] = {1, 0, 0, 6};
    memcpy(port_vu1_data + 61 * 16, header, 16);
    data(62, 0, 0, 0, 1);
    data(63, 8, 6, 0, 0);
    data(64, 128, 128, 128, 96);
    data(65, 0, 0, 0, 0);
    data(66, 16, 16, 0, 0);
    kicks = 0;
    assert(port_vu1_start(2) == 0);
    assert(port_vu1.pc == 100);
    assert(kicks == 1 && kicked_size == 96);
    assert(read_data(601, 0) == 128);
    assert(read_data(603, 0) == 32736);
    assert(read_data(603, 1) == 32744);
    assert(read_data(603, 2) == 160);
    assert(read_data(603, 3) == 16);
    assert(read_data(605, 0) == 32800);
    assert(read_data(605, 1) == 32792);
    assert(port_vu1_continue() == 0);
    assert(port_vu1.vi[13] == 420);
    assert(kicks == 2);
}

static void retail_water(const char *path)
{
    port_vu1_reset();
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fread(port_vu1_code, 1, 16384, file) == 16384);
    fclose(file);
    for (unsigned lane = 0; lane < 4; lane++) {
        float values[4] = {0, 0, 0, 0};
        values[lane] = 1;
        memcpy(port_vu1_data + (4 + lane) * 16, values, 16);
    }
    data(57, 4095, 4095, 0, 1000);
    data(58, 1, 1, 0, 0.8f);
    assert(port_vu1_start(0) == 0);
    data(24, 640, 447, 0, 0);
    data(25, 1728, 1824, 0, 0);
    data(26, 128, 128, 128, 128);
    data(27, 1, 2, 0, 0);
    port_vu1_top = 60;
    port_vu1.vi[8] = port_vu1.vi[9] = 208;
    uint32_t tag[4] = {0x8001, 0x30000000, 0x431, 0};
    memcpy(port_vu1_data + 60 * 16, tag, 16);
    uint32_t header[4] = {1, 1, 0, 0};
    memcpy(port_vu1_data + 61 * 16, header, 16);
    data(62, 2048, 2048, 0, 1);
    data(63, 0, 0, 0, 1);
    kicks = 0;
    assert(port_vu1_start(2) == 0);
    assert(kicks == 1 && kicked_size == 64);
    assert(read_data(601, 3) == 128);
    assert(read_data(602, 0) == 5120);
    assert(read_data(602, 1) == 3584);
    assert(read_data(603, 0) == 32768);
    assert(read_data(603, 1) == 32768);
    assert(read_data(603, 3) == 0);
    assert(port_vu1.pc == 100);
}

static void retail_visual(const char *path)
{
    port_vu1_reset();
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fread(port_vu1_code, 1, 16384, file) == 16384);
    fclose(file);
    for (unsigned lane = 0; lane < 4; lane++) {
        float values[4] = {0, 0, 0, 0};
        values[lane] = 1;
        memcpy(port_vu1_data + (4 + lane) * 16, values, 16);
    }
    data(20, 128, 64, 32, 128);
    data(40, 0.5f, 0.5f, 0.5f, 1);
    data(57, 4095, 4095, 0, 1000);
    data(58, 1, 1, 0, 0.8f);
    uint32_t flags = 32;
    memcpy(port_vu1_data + 38 * 16, &flags, 4);
    assert(port_vu1_start(0) == 0);
    port_vu1_top = 60;
    uint32_t tag[4] = {0x8003, 0x30000000, 0x412, 0};
    memcpy(port_vu1_data + 60 * 16, tag, 16);
    uint32_t header[4] = {3, 3, 3, 0};
    memcpy(port_vu1_data + 61 * 16, header, 16);
    data(62, 2048, 2048, 1, 1);
    data(63, 2050, 2048, 1, 1);
    data(64, 2048, 2050, 1, 1);
    for (unsigned address = 65; address < 71; address++) data(address, 0, 0, 1, 0);
    kicks = 0;
    assert(port_vu1_start(2) == 0);
    assert(port_vu1.pc == 100);
    assert(kicks == 1 && kicked_size == 160);
    assert(read_data(602, 0) == 64);
    assert(read_data(602, 1) == 32);
    assert(read_data(602, 2) == 16);
    assert(read_data(602, 3) == 128);
    assert(read_data(603, 0) == 32768);
    assert(read_data(603, 1) == 32768);
    data(8, 1, 0, 0, 0);
    data(9, 0, 1, 0, 0);
    data(10, 0, 0, 1, 0);
    data(11, 0, 0, 0, 1);
    data(12, 0, 0, 0, 0);
    data(13, 0, 0, 0, 0);
    data(14, 1, 0, 0, 0);
    data(15, 128, 64, 0, 0);
    data(19, 32, 32, 32, 128);
    data(40, 1, 1, 1, 1);
    flags = 0;
    memcpy(port_vu1_data + 38 * 16, &flags, 4);
    assert(port_vu1_start(0) == 0);
    for (unsigned address = 65; address < 68; address++) data(address, 0, 0, 1, 0);
    assert(port_vu1_start(2) == 0);
    assert(read_data(602, 0) == 160);
    assert(read_data(602, 1) == 96);
    assert(read_data(602, 2) == 32);
    assert(read_data(602, 3) == 128);
}

int main(int argc, char **argv)
{
    constants_and_masks();
    branch_resume_and_integer_store();
    paired_reads();
    mac_delay();
    q_delay_and_wait();
    clip_history();
    gif_wrap();
    if (argc > 1) retail_setup(argv[1]);
    if (argc > 2) retail_sprite(argv[2]);
    if (argc > 3) retail_water(argv[3]);
    if (argc > 4) retail_visual(argv[4]);
    port_vu1_reset();
    pair(0, 0x000002ff, 0xfe000000);
    assert(port_vu1_start(0) == -1);
    assert(port_vu1.fault_lower == 0xfe000000);
    puts("VU1 checks passed");
    return 0;
}
