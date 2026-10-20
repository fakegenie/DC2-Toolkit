#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <libvu0.h>
void test1(float (*)[4], float (*)[4], float (*)[4], float *, float *);
void test2(float *, float *);
void PlaneNormalXZ(float *, float *, float *, float *);

static std::uint32_t random_state = 0x76543210;
static std::uint64_t hash = 14695981039346656037ull;

static float number() {
    random_state = random_state * 1664525u + 1013904223u;
    return static_cast<float>(static_cast<int>(random_state >> 16) - 32768) / 32768.0f;
}

static void record(const float *values, int count) {
    for (int i = 0; i < count; i++) {
        std::uint32_t bits;
        std::memcpy(&bits, &values[i], sizeof(bits));
        hash ^= bits;
        hash *= 1099511628211ull;
    }
}

int main() {
    timespec start, end;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start);
    for (int iteration = 0; iteration < 500000; iteration++) {
        sceVu0FMATRIX first, second, product;
        sceVu0UnitMatrix(first);
        sceVu0UnitMatrix(second);
        float offset[4] = {number(), number(), number(), 1};
        sceVu0TransMatrix(first, first, offset);
        sceVu0RotMatrixY(second, second, number() * 3.0f);
        sceVu0MulMatrix(product, first, second);
        float vector[4] = {number(), number(), number(), 1};
        sceVu0ApplyMatrix(vector, product, vector);
        record(vector, 4);
        sceVu0Normalize(vector, vector);
        record(vector, 4);
        float corners[8][4];
        for (int vertex = 0; vertex < 8; vertex++) {
            corners[vertex][0] = vertex & 1 ? vector[0] : -vector[0];
            corners[vertex][1] = vertex & 2 ? vector[1] : -vector[1];
            corners[vertex][2] = vertex & 4 ? vector[2] : -vector[2];
            corners[vertex][3] = vertex & 1 ? 1.0f : -1.0f;
        }
        float high[4], low[4];
        test1(corners, first, second, high, low);
        test2(high, low);
        record(high, 4);
        record(low, 4);
        PlaneNormalXZ(vector, corners[0], corners[3], corners[5]);
        record(vector, 4);
    }
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end);
    std::printf("hash=%016llx cpu_ns=%lld\n", static_cast<unsigned long long>(hash),
        static_cast<long long>(end.tv_sec - start.tv_sec) * 1000000000ll + end.tv_nsec - start.tv_nsec);
}
