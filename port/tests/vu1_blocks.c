#include "../src/vu1.c"
#include <assert.h>

static uint32_t random_state = 0x102d78a9;

struct execution {
    struct port_vu1_registers registers;
    unsigned char data[16384];
    uint16_t mac_values[4];
    uint64_t mac_times[4];
    uint32_t clip_values[4];
    uint64_t clip_times[4];
    uint32_t q_values[32];
    uint64_t q_times[32];
    uint32_t clip_latest;
    uint32_t p_value;
    uint64_t time;
    uint64_t p_time;
    unsigned q_first;
    unsigned q_last;
    int next_branch;
    int end_pending;
    int result;
};

int port_gif_feed(const void *data, size_t size, unsigned path)
{
    (void)data;
    (void)size;
    (void)path;
    return -1;
}

static uint32_t random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void pair(unsigned pc, uint32_t upper_word, uint32_t lower_word)
{
    memcpy(port_vu1_code + pc * 8, &lower_word, 4);
    memcpy(port_vu1_code + pc * 8 + 4, &upper_word, 4);
}

static void program(unsigned output)
{
    if (!output) {
        pair(0x222, 0x2ff, encode_lower(0, 15, 25, 7, 1));
        pair(0x223, encode_upper(21, 15, 22, 21, 14), encode_lower(0, 15, 20, 8, 1));
        pair(0x224, 0x2ff, encode_lower(64, 0, 9, 9, (31 << 6) | 50));
        pair(0x225, 0x2ff, encode_lower(64, 0, 7, 7, (1 << 6) | 50));
        pair(0x226, 0x2ff, encode_lower(41, 0, 9, 0, (unsigned)-5));
        pair(0x227, encode_upper(42, 15, 21, 20, 25), encode_special(53, 15, 8, 22));
    } else {
        pair(0x3a1, encode_upper(44, 13, 19, 16, 22), encode_lower(64, 0, 9, 8, 50));
        pair(0x3a2, encode_upper(0, 14, 26, 16, 0), encode_lower(64, 0, 8, 7, 50));
        pair(0x3a3, encode_upper(21, 1, 27, 28, 29), encode_special(52, 15, 25, 5));
        pair(0x3a4, encode_upper(60, 15, 5, 20, 21), encode_lower(26, 0, 7, 10, 0));
        pair(0x3a5, 0x2ff, encode_lower(26, 0, 7, 7, 0));
        pair(0x3a6, 0x2ff, encode_lower(64, 0, 8, 9, (1 << 6) | 52));
        pair(0x3a7, encode_upper(18, 1, 26, 27, 29), encode_lower(64, 0, 7, 1, (1 << 6) | 52));
        pair(0x3a8, 0x2ff, encode_special(52, 15, 16, 2));
        pair(0x3a9, 0x2ff, encode_lower(40, 0, 1, 10, 5));
        pair(0x3aa, 0x2ff, encode_special(52, 15, 20, 4));
        pair(0x3ab, encode_upper(34, 1, 26, 26, 0), encode_special(48, 0, 0, 0));
        pair(0x3ac, 0x2ff, encode_special(48, 0, 0, 0));
        pair(0x3ad, 0x2ff, encode_special(48, 0, 0, 0));
        pair(0x3ae, 0x2ff, encode_special(48, 0, 0, 0));
        pair(0x3af, encode_upper(61, 15, 5, 26, 17), encode_special(53, 15, 6, 25));
        pair(0x3b0, encode_upper(60, 1, 6, 0, 29), encode_lower(64, 0, 11, 11, (31 << 6) | 50));
        pair(0x3b1, encode_upper(11, 1, 28, 29, 16), encode_special(53, 15, 6, 21));
        pair(0x3b2, encode_upper(44, 13, 18, 15, 16), encode_lower(41, 0, 11, 0, (unsigned)-18));
        pair(0x3b3, 0x2ff, encode_special(53, 15, 6, 17));
    }
    unsigned first = output ? 0x3a1 : 0x222;
    unsigned after = output ? 0x3b4 : 0x228;
    pair(0, encode_upper(44, 15, 3, 1, 2), 0x806203bc);
    pair(1, 0x2ff, encode_lower(32, 0, 0, 0, first - 2));
    pair(2, 0x2ff, encode_special(48, 0, 0, 0));
    for (unsigned pc = after; pc < after + 18; pc++) pair(pc, 0x2ff, encode_special(48, 0, 0, 0));
    pair(after + 16, 0x400002ff, encode_special(48, 0, 0, 0));
}

static void record(struct execution *out, int result)
{
    memset(out, 0, sizeof(*out));
    out->registers = port_vu1;
    memcpy(out->data, port_vu1_data, sizeof(out->data));
    memcpy(out->mac_values, mac_queue, sizeof(mac_queue));
    memcpy(out->mac_times, mac_due, sizeof(mac_due));
    memcpy(out->clip_values, clip_queue, sizeof(clip_queue));
    memcpy(out->clip_times, clip_due, sizeof(clip_due));
    memcpy(out->q_values, q_queue, sizeof(q_queue));
    memcpy(out->q_times, q_due, sizeof(q_due));
    out->clip_latest = clip_issued;
    out->p_value = p_pending;
    out->time = cycle;
    out->p_time = p_due;
    out->q_first = q_head;
    out->q_last = q_tail;
    out->next_branch = branch;
    out->end_pending = ending;
    out->result = result;
}

static void differential(unsigned mode, unsigned count, unsigned mask)
{
    struct port_vu1_registers initial = {0};
    unsigned char memory[16384];
    for (unsigned reg = 0; reg < 32; reg++)
        for (unsigned lane = 0; lane < 4; lane++) initial.vf[reg][lane] = random_word();
    const uint32_t edges[16] = {0, 0x80000000u, 1, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00800000u, 0x80800000u,
                                0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fffffffu, 0xffffffffu, 0x3f000000u, 0x4f000000u};
    for (unsigned reg = 0; reg < 32; reg++)
        for (unsigned lane = 0; lane < 4; lane++)
            if (!(random_word() & 3)) initial.vf[reg][lane] = edges[random_word() & 15];
    for (unsigned reg = 0; reg < 16; reg++) initial.vi[reg] = random_word();
    for (unsigned lane = 0; lane < 4; lane++) initial.acc[lane] = random_word();
    initial.i = random_word();
    initial.q = random_word();
    initial.p = random_word();
    initial.mac = random_word();
    initial.status = random_word();
    initial.clip = random_word() & 0xffffff;
    initial.vi[mode ? 11 : 9] = count;
    initial.vi[10] = mask;
    for (unsigned byte = 0; byte < sizeof(memory); byte += 4) {
        uint32_t value = random_word();
        memcpy(memory + byte, &value, 4);
    }
    struct execution reference;
    struct execution optimized;
    for (unsigned pass = 0; pass < 2; pass++) {
        port_vu1_reset();
        disable_blocks = !pass;
        program(mode);
        port_vu1 = initial;
        memcpy(port_vu1_data, memory, sizeof(memory));
        int result = port_vu1_start(0);
        record(pass ? &optimized : &reference, result);
    }
    assert(memcmp(&reference, &optimized, sizeof(reference)) == 0);
}

static void guards(void)
{
    for (unsigned mode = 0; mode < 2; mode++) {
        port_vu1_reset();
        program(mode);
        unsigned first = mode ? 0x3a1 : 0x222;
        unsigned after = mode ? 0x3b4 : 0x228;
        assert(mode ? matches_outputc() : matches_vcolor());
        for (unsigned byte = first * 8; byte < after * 8; byte++) {
            for (unsigned bit = 0; bit < 8; bit++) {
                port_vu1_code[byte] ^= 1u << bit;
                assert(!(mode ? matches_outputc() : matches_vcolor()));
                port_vu1_code[byte] ^= 1u << bit;
            }
        }
    }
}

int main(void)
{
    guards();
    for (unsigned j = 0; j < 512; j++) differential(j & 1, j % 12 + 1, j & 2 ? 208 : 0);
    differential(0, 0, 208);
    differential(1, 0, 0);
    differential(1, 0, 65535);
    puts("VU1 block equivalence checks passed");
    return 0;
}
