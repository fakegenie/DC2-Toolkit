#include <cassert>
#include <cmath>
#include <cstring>
#include "collision.hpp"
#include "gameutil.hpp"
#include "mg_math.hpp"

int ClipBoxXZ(float *, float *, float *, float *);
void PlaneNormalXZ(float *, float *, float *, float *);
void pre_trance_normal(float (*)[4]);
void trance_normal(float *, float *, float *, float *);

static void plane(CCPoly *polygon, float height, int mask)
{
    std::memset(polygon, 0, sizeof(*polygon));
    const float vertices[3][4] = {{-2, height, -2, 1}, {2, height, -2, 1}, {0, height, 2, 1}};
    std::memcpy(polygon->vertex, vertices, sizeof(vertices));
    polygon->normal[1] = -16;
    polygon->ignore_mask = mask;
}

static void ray_queries()
{
    CCPoly polygons[3];
    plane(&polygons[0], -2, 0);
    plane(&polygons[1], 2, 0);
    plane(&polygons[2], 4, 8);
    CollisionInfo info = {};
    info.count = 3;
    info.polys = polygons;
    float from[4] = {0, 5, 0, 1};
    float to[4] = {0, -5, 0, 1};
    float hit[4] = {};
    assert(CheckHit(&info, from, to, hit, 0, 8) == 0 && hit[1] == -2);
    assert(CheckHit(&info, from, to, hit, 1, 8) == 1 && hit[1] == 2);
    assert(CheckHit(&info, from, to, hit, 1, 0) == 2 && hit[1] == 4);
    assert(CheckHit(&info, to, from, hit, 1, 0) == 0 && hit[1] == -2);
    int indices[4] = {-1, -1, -1, -1};
    float points[4][4] = {};
    assert(CheckHits(&info, from, to, 4, indices, points, 0, 8) == 2);
    assert(indices[0] == 0 && indices[1] == 1);
    assert(points[0][3] == 7 && points[1][3] == 3);
    assert(CheckHits(&info, from, to, 4, indices, points, 1, 8) == 2);
    assert(indices[0] == 1 && indices[1] == 0);
    assert(CheckHits(&info, from, to, 4, indices, points, -1, 8) == 2);
    assert(indices[0] == 1 && indices[1] == 0);
    assert(CheckHits(&info, from, to, 1, indices, points, 1, 0) == 1);
    assert(indices[0] == 0 && points[0][1] == -2);
    indices[0] = 99;
    assert(CheckHits(&info, from, to, 0, indices, points, 1, 0) == 0);
    assert(indices[0] == 99);
    from[0] = to[0] = 10;
    assert(CheckHit(&info, from, to, hit, 1, 0) == -1);
    from[0] = to[0] = 0;
    from[1] = 10;
    to[1] = 9;
    assert(CheckHit(&info, from, to, hit, 1, 0) == -1);
    from[1] = to[1] = 2;
    assert(CheckHit(&info, from, to, hit, 1, 0) == -1);
    from[0] = to[0] = -2;
    from[2] = to[2] = -2;
    from[1] = 3;
    to[1] = 1;
    assert(CheckHit(&info, from, to, hit, 1, 0) == 1);
    info.count = 0;
    assert(CheckHit(&info, from, to, hit, 1, 0) == -1);
    assert(CheckHits(&info, from, to, 4, indices, points, 1, 0) == 0);
}

static void transformed_triangles()
{
    float matrix[4][4] = {{-2, 0, 0, 0}, {0, 3, 0, 0}, {0, 0, 4, 0}, {10, 20, 30, 1}};
    float vertices[3][4] = {{0, 0, 0, 1}, {1, 0, 0, 1}, {0, 0, 1, 1}};
    float normal[4];
    pre_trance_normal(matrix);
    trance_normal(vertices[0], vertices[1], vertices[2], normal);
    assert(vertices[0][0] == 10 && vertices[0][1] == 20 && vertices[0][2] == 30 && vertices[0][3] == 1);
    assert(vertices[1][0] == 8 && vertices[2][2] == 34);
    assert(normal[0] == 0 && normal[1] == 8 && normal[2] == 0);
    PlaneNormalXZ(normal, vertices[0], vertices[1], vertices[2]);
    assert(normal[0] == 0 && normal[1] == 8 && normal[2] == 0);
    PlaneNormalXZ(vertices[0], vertices[0], vertices[1], vertices[2]);
    assert(vertices[0][0] == 0 && vertices[0][1] == 8 && vertices[0][2] == 0);
}

static void horizontal_bounds()
{
    for (int ax = -4; ax <= 4; ax++) {
        for (int az = -4; az <= 4; az++) {
            for (int bx = -4; bx <= 4; bx++) {
                for (int bz = -4; bz <= 4; bz++) {
                    float first_min[4] = {float(ax), 100, float(az), 1};
                    float first_max[4] = {float(ax + 2), 101, float(az + 2), 1};
                    float second_min[4] = {float(bx), -100, float(bz), 0};
                    float second_max[4] = {float(bx + 1), -99, float(bz + 1), 0};
                    int overlaps = ax <= bx + 1 && bx <= ax + 2 && az <= bz + 1 && bz <= az + 2;
                    assert(ClipBoxXZ(first_max, first_min, second_max, second_min) == overlaps);
                    assert(ClipBoxXZ(second_max, second_min, first_max, first_min) == overlaps);
                }
            }
        }
    }
    float maximum[4] = {-0.0f, 0, 1, 0};
    float minimum[4] = {-1, 0, -1, 0};
    float other_max[4] = {1, 0, 1, 0};
    float other_min[4] = {0, 0, -1, 0};
    assert(ClipBoxXZ(maximum, minimum, other_max, other_min) == 0);
}

int main()
{
    horizontal_bounds();
    transformed_triangles();
    ray_queries();
}
