#include "vu1.h"
#include <emmintrin.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern int port_gif_feed(const void *data, size_t size, unsigned path);
unsigned char port_vu1_code[16384];
unsigned char port_vu1_data[16384];
uint16_t port_vu1_top;
uint16_t port_vu1_itop;
struct port_vu1_registers port_vu1;
static uint16_t mac_queue[4];
static uint64_t mac_due[4];
static uint32_t clip_queue[4];
static uint64_t clip_due[4];
static uint32_t clip_issued;
static uint32_t q_queue[32];
static uint64_t q_due[32];
static unsigned q_head;
static unsigned q_tail;
static uint64_t cycle;
static uint64_t p_due;
static uint32_t p_pending;
static int branch;
static int ending;
static int disable_blocks;

static uint32_t word(const unsigned char *p)
{
    uint32_t value;
    memcpy(&value, p, 4);
    return value;
}

static uint32_t bits(float value)
{
    uint32_t result;
    memcpy(&result, &value, 4);
    return result;
}

static double number(uint32_t value)
{
    unsigned exponent = (value >> 23) & 255;
    uint64_t encoded = (uint64_t)(value & 0x80000000u) << 32;
    if (exponent) encoded |= ((uint64_t)(exponent + 896) << 52) | ((uint64_t)(value & 0x7fffff) << 29);
    double result;
    memcpy(&result, &encoded, 8);
    return result;
}

static uint32_t rounded(double value)
{
    uint64_t encoded;
    memcpy(&encoded, &value, 8);
    unsigned sign = (encoded >> 32) & 0x80000000u;
    uint64_t magnitude = encoded & 0x7fffffffffffffffu;
    if (magnitude < 0x3810000000000000u) return sign;
    if (magnitude >= 0x47fffffff0000000u) return sign | 0x7fffffffu;
    return sign | ((unsigned)((magnitude >> 52) - 896) << 23) | ((magnitude >> 29) & 0x7fffff);
}

static uint32_t to_integer(uint32_t value, unsigned shift)
{
    int exponent = (int)((value >> 23) & 255) - 127 + (int)shift;
    if (exponent < 0 || !(value & 0x7f800000)) return 0;
    if (exponent >= 31) return value >> 31 ? 0x80000000u : 0x7fffffffu;
    uint32_t magnitude = (value & 0x7fffff) | 0x800000;
    magnitude = exponent >= 23 ? magnitude << (exponent - 23) : magnitude >> (23 - exponent);
    return value >> 31 ? 0u - magnitude : magnitude;
}

static uint32_t from_integer(uint32_t value, unsigned shift)
{
    if (!value) return 0;
    uint32_t sign = value & 0x80000000u;
    uint32_t magnitude = sign ? 0u - value : value;
    unsigned exponent = 31 - __builtin_clz(magnitude);
    uint32_t mantissa = exponent >= 23 ? magnitude >> (exponent - 23) : magnitude << (23 - exponent);
    return sign | ((exponent + 127 - shift) << 23) | (mantissa & 0x7fffff);
}

static void advance(uint64_t target)
{
    while (cycle < target) {
        cycle++;
        unsigned slot = cycle & 3;
        if (mac_due[slot] == cycle) port_vu1.mac = mac_queue[slot];
        if (clip_due[slot] == cycle) port_vu1.clip = clip_queue[slot];
        while (q_head != q_tail && q_due[q_head & 31] <= cycle) {
            port_vu1.q = q_queue[q_head & 31];
            q_head++;
        }
        if (p_due && p_due <= cycle) {
            port_vu1.p = p_pending;
            p_due = 0;
        }
    }
}

static void q_issue(uint32_t value, unsigned latency)
{
    q_queue[q_tail & 31] = value;
    q_due[q_tail & 31] = cycle + latency;
    q_tail++;
}

static void q_wait(void)
{
    if (q_head != q_tail) advance(q_due[(q_tail - 1) & 31]);
}

static int sign_extend(unsigned value, unsigned width)
{
    return (int)(value ^ (1u << (width - 1))) - (int)(1u << (width - 1));
}

static void vi_write(unsigned reg, unsigned value)
{
    if (reg & 15) port_vu1.vi[reg & 15] = value;
}

static void vector_load(unsigned reg, unsigned address, unsigned mask)
{
    if (!reg) return;
    for (unsigned lane = 0; lane < 4; lane++)
        if (mask & (8u >> lane)) port_vu1.vf[reg][lane] = word(port_vu1_data + (address & 1023) * 16 + lane * 4);
}

static void vector_store(unsigned reg, unsigned address, unsigned mask)
{
    for (unsigned lane = 0; lane < 4; lane++)
        if (mask & (8u >> lane)) memcpy(port_vu1_data + (address & 1023) * 16 + lane * 4, &port_vu1.vf[reg][lane], 4);
}

static void integer_load(unsigned reg, unsigned address, unsigned mask)
{
    for (unsigned lane = 0; lane < 4; lane++)
        if (mask & (8u >> lane)) vi_write(reg, word(port_vu1_data + (address & 1023) * 16 + lane * 4));
}

static void integer_store(unsigned reg, unsigned address, unsigned mask)
{
    uint32_t value = port_vu1.vi[reg & 15];
    for (unsigned lane = 0; lane < 4; lane++)
        if (mask & (8u >> lane)) memcpy(port_vu1_data + (address & 1023) * 16 + lane * 4, &value, 4);
}

static int kick(unsigned address)
{
    static unsigned char packet[16384];
    unsigned size = 0;
    for (;;) {
        if (size + 16 > sizeof(packet)) return -1;
        unsigned char tag[16];
        memcpy(tag, port_vu1_data + (address & 1023) * 16, 16);
        unsigned loops = word(tag) & 32767;
        unsigned high = word(tag + 4);
        unsigned mode = (high >> 26) & 3;
        unsigned regs = high >> 28;
        if (!regs) regs = 16;
        unsigned count = mode == 0 ? loops * regs : mode == 1 ? (loops * regs + 1) / 2 : loops;
        if (count > (sizeof(packet) - size - 16) / 16) return -1;
        for (unsigned j = 0; j <= count; j++) {
            memcpy(packet + size, port_vu1_data + (address & 1023) * 16, 16);
            size += 16;
            address++;
        }
        if (word(tag) & 32768) break;
    }
    return port_gif_feed(packet, size, 1);
}

static __m128d packed_number(__m128i input)
{
    __m128i zero = _mm_setzero_si128();
    __m128i wide = _mm_unpacklo_epi32(input, zero);
    __m128i exponent = _mm_and_si128(_mm_srli_epi64(wide, 23), _mm_set1_epi64x(255));
    __m128i normal = _mm_slli_epi64(_mm_add_epi64(exponent, _mm_set1_epi64x(896)), 52);
    normal = _mm_or_si128(normal, _mm_slli_epi64(_mm_and_si128(wide, _mm_set1_epi64x(0x7fffff)), 29));
    __m128i empty = _mm_cmpeq_epi32(_mm_and_si128(input, _mm_set1_epi32(0x7f800000)), zero);
    empty = _mm_unpacklo_epi32(empty, empty);
    __m128i sign = _mm_slli_epi64(_mm_and_si128(wide, _mm_set1_epi64x(0x80000000u)), 32);
    return _mm_castsi128_pd(_mm_or_si128(sign, _mm_andnot_si128(empty, normal)));
}

static __m128d packed_truncated(__m128d value)
{
    __m128d magnitude = _mm_and_pd(value, _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffu)));
    __m128d sign = _mm_and_pd(value, _mm_castsi128_pd(_mm_set1_epi64x(0x8000000000000000u)));
    __m128d small = _mm_cmplt_pd(magnitude, _mm_set1_pd(0x1p-126));
    __m128d large = _mm_cmpge_pd(magnitude, _mm_set1_pd(0x1.ffffffp128));
    __m128d result = _mm_and_pd(value, _mm_castsi128_pd(_mm_set1_epi64x(0xffffffffe0000000u)));
    result = _mm_or_pd(_mm_and_pd(small, sign), _mm_andnot_pd(small, result));
    __m128d limit = _mm_or_pd(sign, _mm_set1_pd(0x1.fffffep128));
    return _mm_or_pd(_mm_and_pd(large, limit), _mm_andnot_pd(large, result));
}

static __m128i packed_rounded(__m128d value)
{
    __m128i encoded = _mm_castpd_si128(value);
    __m128i sign = _mm_and_si128(_mm_srli_epi64(encoded, 32), _mm_set1_epi64x(0x80000000u));
    __m128i fraction = _mm_and_si128(_mm_srli_epi64(encoded, 29), _mm_set1_epi64x(0x7fffff));
    __m128i exponent = _mm_sub_epi64(_mm_and_si128(_mm_srli_epi64(encoded, 52), _mm_set1_epi64x(2047)), _mm_set1_epi64x(896));
    __m128i result = _mm_or_si128(_mm_slli_epi64(exponent, 23), fraction);
    __m128d magnitude = _mm_and_pd(value, _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffu)));
    __m128i small = _mm_castpd_si128(_mm_cmplt_pd(magnitude, _mm_set1_pd(0x1p-126)));
    __m128i large = _mm_castpd_si128(_mm_cmpge_pd(magnitude, _mm_set1_pd(0x1.ffffffp128)));
    result = _mm_andnot_si128(small, result);
    result = _mm_or_si128(_mm_and_si128(large, _mm_set1_epi64x(0x7fffffff)), _mm_andnot_si128(large, result));
    result = _mm_or_si128(sign, result);
    return _mm_shuffle_epi32(result, _MM_SHUFFLE(2, 0, 2, 0));
}

static int packed_arithmetic(unsigned op, unsigned accumulator, unsigned fd, unsigned scalar, uint32_t scalar_value, const uint32_t source[2][4])
{
    const unsigned reversed[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    __m128d values[2];
    __m128d a[2];
    __m128d b[2];
    for (unsigned half = 0; half < 2; half++) {
        a[half] = packed_number(_mm_loadl_epi64((const __m128i *)(source[0] + half * 2)));
        b[half] = packed_number(scalar ? _mm_set1_epi32(scalar_value) : _mm_loadl_epi64((const __m128i *)(source[1] + half * 2)));
    }
    if (op == 40) {
        for (unsigned half = 0; half < 2; half++) values[half] = _mm_add_pd(a[half], b[half]);
    } else if (op == 42) {
        for (unsigned half = 0; half < 2; half++) values[half] = _mm_mul_pd(a[half], b[half]);
    } else if (op == 43 || op == 47) {
        for (unsigned half = 0; half < 2; half++) values[half] = op == 43 ? _mm_max_pd(a[half], b[half]) : _mm_min_pd(a[half], b[half]);
    } else if (op == 44) {
        for (unsigned half = 0; half < 2; half++) values[half] = _mm_sub_pd(a[half], b[half]);
    } else if (op == 41 || op == 45) {
        for (unsigned half = 0; half < 2; half++) {
            __m128d product = packed_truncated(_mm_mul_pd(a[half], b[half]));
            __m128d acc = packed_number(_mm_loadl_epi64((const __m128i *)(port_vu1.acc + half * 2)));
            values[half] = op == 41 ? _mm_add_pd(acc, product) : _mm_sub_pd(acc, product);
        }
    } else return -1;
    __m128i first = packed_rounded(values[0]);
    __m128i second = packed_rounded(values[1]);
    __m128i result = _mm_unpacklo_epi64(first, second);
    if (accumulator) _mm_storeu_si128((__m128i *)port_vu1.acc, result);
    else if (fd) _mm_storeu_si128((__m128i *)port_vu1.vf[fd], result);
    if (op == 43 || op == 47) return 0;
    unsigned negative = _mm_movemask_ps(_mm_castsi128_ps(result));
    unsigned zero = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(result, _mm_set1_epi32(0x7fffffff)), _mm_setzero_si128())));
    unsigned small = 0;
    unsigned large = 0;
    for (unsigned half = 0; half < 2; half++) {
        __m128d magnitude = _mm_and_pd(values[half], _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffu)));
        __m128d under = _mm_and_pd(_mm_cmpneq_pd(magnitude, _mm_setzero_pd()), _mm_cmplt_pd(magnitude, _mm_set1_pd(0x1p-126)));
        small |= _mm_movemask_pd(under) << (half * 2);
        large |= _mm_movemask_pd(_mm_cmpge_pd(magnitude, _mm_set1_pd(0x1p128))) << (half * 2);
    }
    mac_queue[cycle & 3] = reversed[zero] | (reversed[negative] << 4) | (reversed[small] << 8) | (reversed[large] << 12);
    mac_due[cycle & 3] = cycle + 4;
    return 0;
}

static int upper(uint32_t instruction, const uint32_t source[2][4])
{
    unsigned op = instruction & 63;
    unsigned ft = (instruction >> 16) & 31;
    unsigned fd = (instruction >> 6) & 31;
    unsigned mask = (instruction >> 21) & 15;
    unsigned accumulator = 0;
    unsigned scalar = 0;
    uint32_t scalar_value = 0;
    if (op >= 60) {
        unsigned special = fd * 4 + (op & 3);
        if (special == 47) return 0;
        if (special == 31) {
            double bound = fabs(number(source[1][3]));
            unsigned flags = 0;
            for (unsigned lane = 0; lane < 3; lane++) {
                double value = number(source[0][lane]);
                if (value > bound) flags |= 1u << (lane * 2);
                if (value < -bound) flags |= 2u << (lane * 2);
            }
            clip_issued = ((clip_issued << 6) | flags) & 0xffffff;
            clip_queue[cycle & 3] = clip_issued;
            clip_due[cycle & 3] = cycle + 4;
            return 0;
        }
        if (special >= 16 && special <= 23) {
            const unsigned shifts[4] = {0, 4, 12, 15};
            unsigned shift = shifts[special & 3];
            for (unsigned lane = 0; lane < 4; lane++) {
                if (!(mask & (8u >> lane)) || !ft) continue;
                port_vu1.vf[ft][lane] = special < 20 ? from_integer(source[0][lane], shift) : to_integer(source[0][lane], shift);
            }
            return 0;
        }
        if (special == 29) {
            for (unsigned lane = 0; lane < 4; lane++)
                if (ft && (mask & (8u >> lane))) port_vu1.vf[ft][lane] = source[0][lane] & 0x7fffffff;
            return 0;
        }
        accumulator = 1;
        if (special < 16) op = special;
        else if (special >= 24 && special <= 27) op = special;
        else if (special == 46) op = 46;
        else if (special == 28 || special == 30 || (special >= 32 && special <= 45 && special != 43)) op = special;
        else return -1;
    }
    if (op < 28) {
        scalar = 1;
        scalar_value = source[1][op & 3];
        unsigned group = op >> 2;
        const unsigned operations[7] = {40, 44, 41, 45, 43, 47, 42};
        op = operations[group];
    } else if (op < 40) {
        scalar = 1;
        scalar_value = (op == 28 || op == 32 || op == 33 || op == 36 || op == 37) ? port_vu1.q : port_vu1.i;
        const unsigned operations[12] = {42,43,42,47,40,41,40,41,44,45,44,45};
        op = operations[op - 28];
    }
    if (op > 47) return -1;
    if (mask == 15 && op >= 40 && op <= 47 && op != 46) return packed_arithmetic(op, accumulator, fd, scalar, scalar_value, source);
    uint16_t flags = 0;
    for (unsigned lane = 0; lane < 4; lane++) {
        if (!(mask & (8u >> lane))) continue;
        double a = number(source[0][lane]);
        double b = number(scalar ? scalar_value : source[1][lane]);
        double acc = number(port_vu1.acc[lane]);
        double value;
        if (op == 40) value = a + b;
        else if (op == 41) value = acc + number(rounded(a * b));
        else if (op == 42) value = a * b;
        else if (op == 43) value = a > b ? a : b;
        else if (op == 44) value = a - b;
        else if (op == 45) value = acc - number(rounded(a * b));
        else if (op == 47) value = a < b ? a : b;
        else if (op == 46) {
            if (lane == 3) continue;
            double product = number(source[0][(lane + 1) % 3]) * number(source[1][(lane + 2) % 3]);
            value = accumulator ? product : acc - number(rounded(product));
        } else return -1;
        uint32_t result = rounded(value);
        if (accumulator) port_vu1.acc[lane] = result;
        else if (fd) port_vu1.vf[fd][lane] = result;
        if (!(result & 0x7fffffff)) flags |= 8u >> lane;
        if (result >> 31) flags |= 128u >> lane;
        if (value != 0 && fabs(value) < 0x1p-126) flags |= 2048u >> lane;
        if (fabs(value) >= 0x1p128) flags |= 32768u >> lane;
    }
    if (op != 43 && op != 47) {
        mac_queue[cycle & 3] = flags;
        mac_due[cycle & 3] = cycle + 4;
    }
    return 0;
}

static int lower(uint32_t instruction)
{
    unsigned top = instruction >> 25;
    unsigned fs = (instruction >> 11) & 31;
    unsigned ft = (instruction >> 16) & 31;
    unsigned fd = (instruction >> 6) & 31;
    unsigned mask = (instruction >> 21) & 15;
    unsigned is = fs & 15;
    unsigned it = ft & 15;
    unsigned a = port_vu1.vi[is];
    unsigned b = port_vu1.vi[it];
    int immediate = sign_extend(instruction & 2047, 11);
    unsigned imm15 = ((instruction >> 21) & 15) * 2048 + (instruction & 2047);
    unsigned imm24 = instruction & 0xffffff;
    unsigned address = a + immediate;
    unsigned target = (port_vu1.pc + 1 + immediate) & 2047;
    if (top == 0) vector_load(ft, address, mask);
    else if (top == 1) vector_store(fs, b + immediate, mask);
    else if (top == 4) integer_load(it, address, mask);
    else if (top == 5) integer_store(it, address, mask);
    else if (top == 8) vi_write(it, a + imm15);
    else if (top == 9) vi_write(it, a - imm15);
    else if (top == 16) vi_write(1, port_vu1.clip == imm24);
    else if (top == 17) port_vu1.clip = clip_issued = imm24;
    else if (top == 18) vi_write(1, (port_vu1.clip & imm24) != 0);
    else if (top == 19) vi_write(1, (port_vu1.clip | imm24) == 0xffffff);
    else if (top == 24) vi_write(it, port_vu1.mac == a);
    else if (top == 26) vi_write(it, port_vu1.mac & a);
    else if (top == 27) vi_write(it, port_vu1.mac | a);
    else if (top == 28) vi_write(it, port_vu1.clip & 4095);
    else if (top == 32 || top == 33) {
        branch = (int)target;
        if (top == 33) vi_write(it, port_vu1.pc + 2);
    } else if (top == 36 || top == 37) {
        branch = a & 2047;
        if (top == 37) vi_write(it, port_vu1.pc + 2);
    } else if (top == 40) { if (a == b) branch = (int)target; }
    else if (top == 41) { if (a != b) branch = (int)target; }
    else if (top == 44) { if ((int16_t)a < 0) branch = (int)target; }
    else if (top == 45) { if ((int16_t)a > 0) branch = (int)target; }
    else if (top == 46) { if ((int16_t)a <= 0) branch = (int)target; }
    else if (top == 47) { if ((int16_t)a >= 0) branch = (int)target; }
    else if (top == 64) {
        unsigned op = instruction & 63;
        if (op == 48) vi_write(fd, a + b);
        else if (op == 49) vi_write(fd, a - b);
        else if (op == 50) vi_write(it, a + sign_extend(fd, 5));
        else if (op == 52) vi_write(fd, a & b);
        else if (op == 53) vi_write(fd, a | b);
        else if (op >= 60) {
            unsigned special = fd * 4 + (op & 3);
            unsigned fsf = (instruction >> 21) & 3;
            unsigned ftf = (instruction >> 23) & 3;
            if (special == 48 || special == 49) {
                uint32_t values[4];
                for (unsigned lane = 0; lane < 4; lane++) values[lane] = port_vu1.vf[fs][(lane + (special == 49)) & 3];
                for (unsigned lane = 0; lane < 4; lane++)
                    if (ft && (mask & (8u >> lane))) port_vu1.vf[ft][lane] = values[lane];
            } else if (special == 52 || special == 54) {
                if (special == 54) vi_write(is, --a);
                vector_load(ft, a, mask);
                if (special == 52) vi_write(is, a + 1);
            } else if (special == 53 || special == 55) {
                if (special == 55) vi_write(it, --b);
                vector_store(fs, b, mask);
                if (special == 53) vi_write(it, b + 1);
            } else if (special >= 56 && special <= 58) {
                double numerator = number(port_vu1.vf[fs][fsf]);
                double denominator = number(port_vu1.vf[ft][ftf]);
                if (special == 57) numerator = 1;
                if (special != 56) denominator = sqrt(fabs(denominator));
                uint32_t result;
                if (denominator == 0) result = ((signbit(numerator) != signbit(denominator)) ? 0x80000000u : 0) | 0x7fffffff;
                else result = rounded(numerator / denominator);
                q_issue(result, special == 58 ? 13 : 7);
            } else if (special == 59) q_wait();
            else if (special == 60) vi_write(it, port_vu1.vf[fs][fsf]);
            else if (special == 61) {
                uint32_t value = (uint32_t)(int32_t)(int16_t)a;
                for (unsigned lane = 0; lane < 4; lane++)
                    if (ft && (mask & (8u >> lane))) port_vu1.vf[ft][lane] = value;
            } else if (special == 62) integer_load(it, a, mask);
            else if (special == 63) integer_store(it, a, mask);
            else if (special == 100) {
                for (unsigned lane = 0; lane < 4; lane++)
                    if (ft && (mask & (8u >> lane))) port_vu1.vf[ft][lane] = port_vu1.p;
            } else if (special == 104) vi_write(it, port_vu1_top);
            else if (special == 105) vi_write(it, port_vu1_itop);
            else if (special == 108) return kick(a);
            else if (special == 115 || special == 121) {
                double value = number(port_vu1.vf[fs][fsf]);
                if (special == 115) {
                    value = 0;
                    for (unsigned lane = 0; lane < 3; lane++) {
                        double component = number(port_vu1.vf[fs][lane]);
                        value += component * component;
                    }
                }
                p_pending = rounded(value == 0 ? 0x1.fffffep127 : 1 / sqrt(fabs(value)));
                p_due = cycle + (special == 115 ? 24 : 18);
            } else if (special == 123) {
                if (p_due) advance(p_due);
            } else return -1;
        } else return -1;
    } else return -1;
    return 0;
}

static uint32_t encode_upper(unsigned op, unsigned mask, unsigned fd, unsigned fs, unsigned ft)
{
    return (mask << 21) | (ft << 16) | (fs << 11) | (fd << 6) | op;
}

static uint32_t encode_lower(unsigned op, unsigned mask, unsigned ft, unsigned fs, unsigned immediate)
{
    return (op << 25) | (mask << 21) | (ft << 16) | (fs << 11) | (immediate & 2047);
}

static uint32_t encode_special(unsigned op, unsigned mask, unsigned ft, unsigned fs)
{
    return encode_lower(64, mask, ft, fs, ((op >> 2) << 6) | 60 | (op & 3));
}

static int pair_matches(unsigned pc, uint32_t upper_word, uint32_t lower_word)
{
    return word(port_vu1_code + pc * 8 + 4) == upper_word && word(port_vu1_code + pc * 8) == lower_word;
}

static int matches_vcolor(void)
{
    return pair_matches(0x222, 0x2ff, encode_lower(0, 15, 25, 7, 1)) &&
           pair_matches(0x223, encode_upper(21, 15, 22, 21, 14), encode_lower(0, 15, 20, 8, 1)) &&
           pair_matches(0x224, 0x2ff, encode_lower(64, 0, 9, 9, (31 << 6) | 50)) &&
           pair_matches(0x225, 0x2ff, encode_lower(64, 0, 7, 7, (1 << 6) | 50)) &&
           pair_matches(0x226, 0x2ff, encode_lower(41, 0, 9, 0, (unsigned)-5)) &&
           pair_matches(0x227, encode_upper(42, 15, 21, 20, 25), encode_special(53, 15, 8, 22));
}

static void vcolor_block(void)
{
    uint32_t source[2][4];
    vector_load(25, port_vu1.vi[7] + 1, 15);
    advance(cycle + 1);
    vector_load(20, port_vu1.vi[8] + 1, 15);
    memcpy(source[0], port_vu1.vf[21], 16);
    memcpy(source[1], port_vu1.vf[14], 16);
    packed_arithmetic(47, 0, 22, 1, port_vu1.vf[14][1], source);
    advance(cycle + 1);
    vi_write(9, port_vu1.vi[9] - 1);
    advance(cycle + 1);
    vi_write(7, port_vu1.vi[7] + 1);
    advance(cycle + 1);
    advance(cycle + 1);
    vector_store(22, port_vu1.vi[8], 15);
    vi_write(8, port_vu1.vi[8] + 1);
    memcpy(source[0], port_vu1.vf[20], 16);
    memcpy(source[1], port_vu1.vf[25], 16);
    packed_arithmetic(42, 0, 21, 0, 0, source);
    advance(cycle + 1);
    port_vu1.pc = port_vu1.vi[9] ? 0x222 : 0x228;
}

static void mac_w(unsigned fd, int accumulator, double value)
{
    uint32_t result = rounded(value);
    if (accumulator) port_vu1.acc[3] = result;
    else port_vu1.vf[fd][3] = result;
    uint16_t flags = !(result & 0x7fffffff);
    if (result >> 31) flags |= 16;
    if (value != 0 && fabs(value) < 0x1p-126) flags |= 256;
    if (fabs(value) >= 0x1p128) flags |= 4096;
    mac_queue[cycle & 3] = flags;
    mac_due[cycle & 3] = cycle + 4;
}

static int matches_outputc(void)
{
    return pair_matches(0x3a1, encode_upper(44, 13, 19, 16, 22), encode_lower(64, 0, 9, 8, 50)) &&
           pair_matches(0x3a2, encode_upper(0, 14, 26, 16, 0), encode_lower(64, 0, 8, 7, 50)) &&
           pair_matches(0x3a3, encode_upper(21, 1, 27, 28, 29), encode_special(52, 15, 25, 5)) &&
           pair_matches(0x3a4, encode_upper(60, 15, 5, 20, 21), encode_lower(26, 0, 7, 10, 0)) &&
           pair_matches(0x3a5, 0x2ff, encode_lower(26, 0, 7, 7, 0)) &&
           pair_matches(0x3a6, 0x2ff, encode_lower(64, 0, 8, 9, (1 << 6) | 52)) &&
           pair_matches(0x3a7, encode_upper(18, 1, 26, 27, 29), encode_lower(64, 0, 7, 1, (1 << 6) | 52)) &&
           pair_matches(0x3a8, 0x2ff, encode_special(52, 15, 16, 2)) &&
           pair_matches(0x3a9, 0x2ff, encode_lower(40, 0, 1, 10, 5)) &&
           pair_matches(0x3aa, 0x2ff, encode_special(52, 15, 20, 4)) &&
           pair_matches(0x3ab, encode_upper(34, 1, 26, 26, 0), encode_special(48, 0, 0, 0)) &&
           pair_matches(0x3ac, 0x2ff, encode_special(48, 0, 0, 0)) &&
           pair_matches(0x3ad, 0x2ff, encode_special(48, 0, 0, 0)) &&
           pair_matches(0x3ae, 0x2ff, encode_special(48, 0, 0, 0)) &&
           pair_matches(0x3af, encode_upper(61, 15, 5, 26, 17), encode_special(53, 15, 6, 25)) &&
           pair_matches(0x3b0, encode_upper(60, 1, 6, 0, 29), encode_lower(64, 0, 11, 11, (31 << 6) | 50)) &&
           pair_matches(0x3b1, encode_upper(11, 1, 28, 29, 16), encode_special(53, 15, 6, 21)) &&
           pair_matches(0x3b2, encode_upper(44, 13, 18, 15, 16), encode_lower(41, 0, 11, 0, (unsigned)-18)) &&
           pair_matches(0x3b3, 0x2ff, encode_special(53, 15, 6, 17));
}

static unsigned outputc_block(void)
{
    uint32_t source[2][4];
    memcpy(source[0], port_vu1.vf[16], 16);
    memcpy(source[1], port_vu1.vf[22], 16);
    vi_write(9, port_vu1.vi[8]);
    upper(encode_upper(44, 13, 19, 16, 22), source);
    advance(cycle + 1);
    vi_write(8, port_vu1.vi[7]);
    uint16_t flags = 0;
    for (unsigned lane = 0; lane < 3; lane++) {
        uint32_t result = port_vu1.vf[16][lane];
        if (!(result & 0x7f800000)) result = 0;
        port_vu1.vf[26][lane] = result;
        if (!(result & 0x7fffffff)) flags |= 8u >> lane;
        if (result >> 31) flags |= 128u >> lane;
        if ((result & 0x7f800000) == 0x7f800000) flags |= 32768u >> lane;
    }
    mac_queue[cycle & 3] = flags;
    mac_due[cycle & 3] = cycle + 4;
    advance(cycle + 1);
    vector_load(25, port_vu1.vi[5], 15);
    vi_write(5, port_vu1.vi[5] + 1);
    double fog = number(port_vu1.vf[28][3]);
    double upper_bound = number(port_vu1.vf[29][1]);
    port_vu1.vf[27][3] = rounded(fog < upper_bound ? fog : upper_bound);
    advance(cycle + 1);
    vi_write(7, port_vu1.mac & port_vu1.vi[10]);
    for (unsigned lane = 0; lane < 4; lane++) port_vu1.vf[21][lane] = to_integer(port_vu1.vf[20][lane], 0);
    advance(cycle + 1);
    vi_write(7, port_vu1.mac & port_vu1.vi[7]);
    advance(cycle + 1);
    vi_write(1, port_vu1.vi[9] & port_vu1.vi[8]);
    advance(cycle + 1);
    vi_write(1, port_vu1.vi[1] & port_vu1.vi[7]);
    fog = number(port_vu1.vf[27][3]);
    double lower_bound = number(port_vu1.vf[29][2]);
    port_vu1.vf[26][3] = rounded(fog > lower_bound ? fog : lower_bound);
    advance(cycle + 1);
    vector_load(16, port_vu1.vi[2], 15);
    vi_write(2, port_vu1.vi[2] + 1);
    advance(cycle + 1);
    int passed = port_vu1.vi[1] == port_vu1.vi[10];
    advance(cycle + 1);
    vector_load(20, port_vu1.vi[4], 15);
    vi_write(4, port_vu1.vi[4] + 1);
    advance(cycle + 1);
    if (!passed) {
        mac_w(26, 0, number(port_vu1.vf[26][3]) + number(port_vu1.i));
        advance(cycle + 4);
    }
    vector_store(25, port_vu1.vi[6], 15);
    vi_write(6, port_vu1.vi[6] + 1);
    for (unsigned lane = 0; lane < 4; lane++) port_vu1.vf[17][lane] = to_integer(port_vu1.vf[26][lane], 4);
    advance(cycle + 1);
    vi_write(11, port_vu1.vi[11] - 1);
    mac_w(0, 1, number(port_vu1.vf[29][0]));
    advance(cycle + 1);
    vector_store(21, port_vu1.vi[6], 15);
    vi_write(6, port_vu1.vi[6] + 1);
    double product = number(port_vu1.vf[29][3]) * number(port_vu1.vf[16][3]);
    mac_w(28, 0, number(port_vu1.acc[3]) + number(rounded(product)));
    advance(cycle + 1);
    memcpy(source[0], port_vu1.vf[15], 16);
    memcpy(source[1], port_vu1.vf[16], 16);
    upper(encode_upper(44, 13, 18, 15, 16), source);
    advance(cycle + 1);
    vector_store(17, port_vu1.vi[6], 15);
    vi_write(6, port_vu1.vi[6] + 1);
    advance(cycle + 1);
    port_vu1.pc = port_vu1.vi[11] ? 0x3a1 : 0x3b4;
    return passed ? 15 : 19;
}

void port_vu1_reset(void)
{
    memset(&port_vu1, 0, sizeof(port_vu1));
    memset(port_vu1_code, 0, sizeof(port_vu1_code));
    memset(port_vu1_data, 0, sizeof(port_vu1_data));
    memset(mac_due, 0, sizeof(mac_due));
    memset(clip_due, 0, sizeof(clip_due));
    memset(mac_queue, 0, sizeof(mac_queue));
    memset(clip_queue, 0, sizeof(clip_queue));
    port_vu1.vf[0][3] = bits(1.0f);
    port_vu1.halted = 1;
    port_vu1_top = port_vu1_itop = 0;
    clip_issued = cycle = q_head = q_tail = p_due = 0;
    branch = -1;
    ending = 0;
}

int port_vu1_continue(void)
{
    port_vu1.halted = 0;
    port_vu1.steps = 0;
    int fast_vcolor = !disable_blocks && matches_vcolor();
    int fast_outputc = !disable_blocks && matches_outputc();
    while (port_vu1.steps++ < 1000000) {
        unsigned pc = port_vu1.pc & 2047;
        if (pc == 0x222 && fast_vcolor && branch < 0 && !ending && port_vu1.steps <= 999995) {
            vcolor_block();
            port_vu1.steps += 5;
            continue;
        }
        if (pc == 0x3a1 && fast_outputc && branch < 0 && !ending && port_vu1.steps <= 999982) {
            port_vu1.steps += outputc_block() - 1;
            continue;
        }
        uint32_t lo = word(port_vu1_code + pc * 8);
        uint32_t hi = word(port_vu1_code + pc * 8 + 4);
        uint32_t source[2][4];
        int upper_nop = (hi & 0x07ffffffu) == 0x2ff;
        if (!upper_nop) {
            memcpy(source[0], port_vu1.vf[(hi >> 11) & 31], 16);
            memcpy(source[1], port_vu1.vf[(hi >> 16) & 31], 16);
        }
        int previous_branch = branch;
        int previous_ending = ending;
        branch = -1;
        ending = (hi >> 30) & 1;
        int result = 0;
        if (!(hi >> 31) && lo != 0x8000033c) result = lower(lo);
        if (!result && !upper_nop) result = upper(hi, source);
        if (hi >> 31) port_vu1.i = lo;
        if (result) {
            port_vu1.fault_upper = hi;
            port_vu1.fault_lower = lo;
            fprintf(stderr, "port: VU1 failed at %04x: %08x %08x\n", pc, hi, lo);
            port_vu1.halted = 1;
            return -1;
        }
        port_vu1.pc = previous_branch >= 0 ? (unsigned)previous_branch : (pc + 1) & 2047;
        advance(cycle + 1);
        if (previous_ending || (ending && previous_branch >= 0)) {
            if (branch >= 0) {
                port_vu1.pc = (unsigned)branch;
                branch = -1;
            }
            ending = 0;
            port_vu1.halted = 1;
            return 0;
        }
    }
    fprintf(stderr, "port: VU1 exceeded instruction budget at %04x\n", port_vu1.pc);
    port_vu1.halted = 1;
    return -1;
}

int port_vu1_start(unsigned entry)
{
    if (entry >= 2048) return -1;
    port_vu1.pc = entry;
    branch = -1;
    ending = 0;
    port_vu1.vi[0] = 0;
    memset(port_vu1.vf[0], 0, 12);
    port_vu1.vf[0][3] = bits(1.0f);
    return port_vu1_continue();
}
