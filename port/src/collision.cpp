#include <cmath>

int ClipBoxXZ(float *max_a, float *min_a, float *max_b, float *min_b) {
    for (int axis = 0; axis <= 2; axis += 2) {
        if (std::signbit(max_a[axis] - min_b[axis]) || std::signbit(max_b[axis] - min_a[axis])) {
            return 0;
        }
    }
    return 1;
}

void PlaneNormalXZ(float *normal, float *p0, float *p1, float *p2) {
    float ax = p1[0] - p0[0];
    float az = p1[2] - p0[2];
    float bx = p2[0] - p0[0];
    float bz = p2[2] - p0[2];
    normal[0] = 0.0f;
    normal[1] = az * bx - ax * bz;
    normal[2] = 0.0f;
    normal[3] = 0.0f;
}
