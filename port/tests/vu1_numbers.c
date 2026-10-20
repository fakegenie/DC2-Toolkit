#define _POSIX_C_SOURCE 200809L
#include "../src/vu1.c"
#include <assert.h>
#include <stdlib.h>
#include <time.h>

static uint64_t random_state = 0x9b4137a9021cde85u;
static volatile uint64_t checksum;

int port_gif_feed(const void *data, size_t size, unsigned path)
{
    (void)data;
    (void)size;
    (void)path;
    return -1;
}

static uint64_t next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static double reference_number(uint32_t value)
{
    unsigned exponent = (value >> 23) & 255;
    if (!exponent) return value >> 31 ? -0.0 : 0.0;
    double result = ldexp(1.0 + (value & 0x7fffff) / 8388608.0, (int)exponent - 127);
    return value >> 31 ? -result : result;
}

static uint32_t reference_rounded(double value)
{
    unsigned sign = signbit(value) ? 0x80000000u : 0;
    double magnitude = fabs(value);
    if (magnitude < 0x1p-126) return sign;
    if (magnitude >= 0x1.ffffffp128) return sign | 0x7fffffffu;
    int exponent;
    double fraction = frexp(magnitude, &exponent);
    uint32_t mantissa = (uint32_t)ldexp(fraction, 24);
    return sign | ((unsigned)(exponent + 126) << 23) | (mantissa & 0x7fffff);
}

static void compare_number(uint32_t value)
{
    double old_value = reference_number(value);
    double new_value = number(value);
    assert(memcmp(&old_value, &new_value, 8) == 0);
    double packed[2];
    _mm_storeu_pd(packed, packed_number(_mm_set_epi32(0, 0, value ^ 0x80000000u, value)));
    assert(memcmp(&old_value, &packed[0], 8) == 0);
    double opposite = reference_number(value ^ 0x80000000u);
    assert(memcmp(&opposite, &packed[1], 8) == 0);
}

static void compare_rounded(uint64_t encoded)
{
    if ((encoded & 0x7ff0000000000000u) == 0x7ff0000000000000u && (encoded & 0xfffffffffffffu)) return;
    double value;
    memcpy(&value, &encoded, 8);
    assert(reference_rounded(value) == rounded(value));
    double values[2] = {value, -value};
    uint32_t converted[4];
    __m128d source = _mm_loadu_pd(values);
    _mm_storeu_si128((__m128i *)converted, packed_rounded(source));
    double shortened[2];
    _mm_storeu_pd(shortened, packed_truncated(source));
    for (unsigned lane = 0; lane < 2; lane++) {
        uint32_t expected = reference_rounded(values[lane]);
        assert(converted[lane] == expected);
        double reconstructed = reference_number(expected);
        assert(memcmp(&reconstructed, &shortened[lane], 8) == 0);
    }
}

static double seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void benchmark(unsigned count)
{
    uint64_t sum = 0;
    double start = seconds();
    for (unsigned j = 0; j < count; j++) {
        double value = reference_number(j * 2654435761u);
        sum += reference_rounded(value * 0.625 + 0.125);
    }
    double old_time = seconds() - start;
    checksum = sum;
    sum = 0;
    start = seconds();
    for (unsigned j = 0; j < count; j++) {
        double value = number(j * 2654435761u);
        sum += rounded(value * 0.625 + 0.125);
    }
    double new_time = seconds() - start;
    assert(checksum == sum);
    checksum = sum;
    printf("VU1 numeric conversion: %u operations, before %.6f s, after %.6f s, speedup %.2fx\n", count, old_time, new_time, old_time / new_time);
}

int main(int argc, char **argv)
{
    for (unsigned exponent = 0; exponent < 256; exponent++) {
        const uint32_t mantissas[] = {0, 1, 2, 0x3fffff, 0x400000, 0x7ffffe, 0x7fffff};
        for (unsigned sign = 0; sign < 2; sign++)
            for (unsigned j = 0; j < sizeof(mantissas) / sizeof(*mantissas); j++)
                compare_number((sign << 31) | (exponent << 23) | mantissas[j]);
    }
    for (unsigned exponent = 0; exponent < 2048; exponent++) {
        const uint64_t mantissas[] = {0, 1, 0x1fffffff, 0x20000000, 0xffffffeffffffu, 0xfffffff000000u, 0xfffffffffffffu};
        for (unsigned sign = 0; sign < 2; sign++)
            for (unsigned j = 0; j < sizeof(mantissas) / sizeof(*mantissas); j++)
                compare_rounded(((uint64_t)sign << 63) | ((uint64_t)exponent << 52) | mantissas[j]);
    }
    for (unsigned j = 0; j < 10000000; j++) {
        uint64_t value = next_random();
        compare_number((uint32_t)value);
        compare_rounded(value);
        unsigned shifts[4] = {0, 4, 12, 15};
        double scales[4] = {1, 16, 4096, 32768};
        unsigned mode = j & 3;
        double original = number((uint32_t)value);
        assert(ldexp(original, shifts[mode]) == original * scales[mode]);
        original = (double)(int32_t)value;
        assert(ldexp(original, -(int)shifts[mode]) == original / scales[mode]);
        assert(from_integer((uint32_t)value, shifts[mode]) == reference_rounded(original / scales[mode]));
        original = number((uint32_t)value) * scales[mode];
        int32_t integer = original >= 2147483647.0 ? INT32_MAX : original <= -2147483648.0 ? INT32_MIN : (int32_t)original;
        assert(to_integer((uint32_t)value, shifts[mode]) == (uint32_t)integer);
    }
    puts("VU1 numeric equivalence checks passed");
    if (argc > 1) benchmark((unsigned)strtoul(argv[1], NULL, 10));
    return 0;
}
