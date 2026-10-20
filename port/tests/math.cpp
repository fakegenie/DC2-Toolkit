#include <cassert>
#include <cmath>
#include <climits>
#include <libvu0.h>
void test1(float (*)[4], float (*)[4], float (*)[4], float *, float *);
void test2(float *, float *);
int ClipBoxXZ(float *, float *, float *, float *);
void PlaneNormalXZ(float *, float *, float *, float *);
int main() {
    sceVu0FMATRIX matrix, inverse, product;
    sceVu0UnitMatrix(matrix);
    float offset[4] = {4, 5, 6, 1};
    sceVu0TransMatrix(matrix, matrix, offset);
    sceVu0InversMatrix(inverse, matrix);
    sceVu0MulMatrix(product, matrix, inverse);
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) assert(product[c][r] == float(c == r));
    float vector[4] = {1, 2, 3, 1};
    sceVu0ApplyMatrix(vector, matrix, vector);
    assert(vector[0] == 5 && vector[1] == 7 && vector[2] == 9 && vector[3] == 1);
    float zero[4] = {};
    sceVu0Normalize(zero, zero);
    for (int i = 0; i < 4; i++) assert(zero[i] == 0);
    float fixed[4] = {1.99f, -1.99f, INFINITY, -INFINITY};
    int integer[4];
    sceVu0FTOI4Vector(integer, fixed);
    assert(integer[0] == 31 && integer[1] == -31 && integer[2] == INT_MAX && integer[3] == INT_MIN);
    sceVu0UnitMatrix(matrix);
    sceVu0RotMatrixY(matrix, matrix, std::acos(-1.0f) * 0.5f);
    float axis[4] = {1, 0, 0, 0};
    sceVu0ApplyMatrix(axis, matrix, axis);
    assert(std::fabs(axis[0]) < 0.000001f && std::fabs(axis[2] + 1) < 0.000001f);
    float first[4] = {1, 2, 3, 7};
    float second[4] = {5, 6, 7, 8};
    sceVu0InterVectorXYZ(first, first, second, 0.25f);
    assert(first[0] == 4 && first[1] == 5 && first[2] == 6 && first[3] == 7);
    float corners[8][4];
    for (int i = 0; i < 8; i++) {
        corners[i][0] = i & 1 ? 2 : -2;
        corners[i][1] = i & 2 ? 4 : -4;
        corners[i][2] = i & 4 ? 6 : -6;
        corners[i][3] = -2;
    }
    float high[4], low[4];
    sceVu0UnitMatrix(matrix);
    test1(corners, matrix, matrix, high, low);
    assert(high[0] == 2 && low[1] == -4 && high[3] == -2);
    test2(high, low);
    assert(high[0] == 1 && low[1] == -2 && high[2] == 6 && low[3] == -2);
    for (int i = 0; i < 8; i++) {
        corners[i][0] = 0;
        corners[i][1] = 0;
        corners[i][3] = 0;
    }
    test1(corners, matrix, matrix, high, low);
    test2(high, low);
    assert(high[0] == 0 && low[1] == 0);
    float max_a[4] = {1, 1, 1, 1}, min_a[4] = {-1, -1, -1, 1};
    float max_b[4] = {3, 3, 3, 1}, min_b[4] = {1, 2, 1, 1};
    assert(ClipBoxXZ(max_a, min_a, max_b, min_b));
    min_b[2] = 2;
    assert(!ClipBoxXZ(max_a, min_a, max_b, min_b));
    float p0[4] = {}, p1[4] = {1, 5, 0, 1}, p2[4] = {0, 6, 1, 1}, normal[4];
    PlaneNormalXZ(normal, p0, p1, p2);
    assert(normal[0] == 0 && normal[1] == -1 && normal[2] == 0);
    PlaneNormalXZ(normal, p0, p2, p1);
    assert(normal[1] == 1);
}
