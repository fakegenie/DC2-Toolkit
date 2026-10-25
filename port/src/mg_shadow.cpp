#include "mg_shadow.hpp"
#include "draw.hpp"
#include "mg_drawenv.hpp"
#include "mg_math.hpp"
#include "mglib.hpp"
#include <cmath>
#include <cstring>
#include <vector>

ShadowSetup shadow_setup;

PORT_OVERRIDE int mgCShadowMDT::CreateFacePacket(u_int *packet, mgCFace *face) {
    (void)packet;
    (void)face;
    return 0;
}

PORT_OVERRIDE uptr mgCShadowMDT::CreatePacket(mgCDrawManager *draw_manager) {
    (void)draw_manager;
    return 0;
}

PORT_OVERRIDE int mgCShadowMDT::CreateRenderInfoPacket(u_int *packet, float (*matrix)[4], mgRENDER_INFO *info) {
    (void)packet;
    info->GetpLightInfo();
    sceVu0FMATRIX clip;
    mgMulMatrix(shadow_setup.camera, info->view_clip_full, info->view);
    mgMulMatrix(clip, shadow_setup.camera, matrix);
    memcpy(shadow_setup.clip, clip, sizeof shadow_setup.clip);
    sceVu0FMATRIX shadow;
    mgMulMatrix(shadow_setup.caster, shadow_setup.camera, info->shadow);
    mgMulMatrix(shadow, shadow_setup.caster, matrix);
    memcpy(shadow_setup.shadow, shadow, sizeof shadow_setup.shadow);
    sceVu0CopyMatrix(shadow_setup.world, matrix);
    memcpy(shadow_setup.screen, info->clip_screen_full, sizeof shadow_setup.screen);
    shadow_setup.key = this;
    float a[3];
    for (int i = 0; i < 3; i++)
        a[i] = matrix[i][0] * info->shadow_light_dir[0] + matrix[i][1] * info->shadow_light_dir[1] +
               matrix[i][2] * info->shadow_light_dir[2];
    float length = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    float scale = length > 0.0f ? -0.1f / length : 0.0f;
    for (int k = 0; k < 4; k++)
        shadow_setup.light[k] = (a[0] * clip[0][k] + a[1] * clip[1][k] + a[2] * clip[2][k]) * scale;
    mgCDrawEnv env = draw_env != NULL ? *draw_env : info->draw_env[0];
    env.SetZBuf(MG_ZBUF_NO_WRITE);
    env.test.bits.zte = 1;
    env.test.bits.ztst = SCE_GS_ZGEQUAL;
    env.test.bits.ate = 0;
    env.test.bits.afail = 0;
    env.test.bits.date = 0;
    gs_base_state(&shadow_setup.state);
    shadow_setup.state.blend = 1;
    shadow_setup.state.tests = gs_tests(env.test);
    shadow_setup.state.depth_write = 0;
    return 0;
}

static void transform(float out[4], const float matrix[4][4], const float *v) {
    for (int k = 0; k < 4; k++) out[k] = v[0] * matrix[0][k] + v[1] * matrix[1][k] + v[2] * matrix[2][k] + v[3] * matrix[3][k];
}

static int outside(const float c[4]) {
    float w = fabsf(c[3]);
    return c[0] > w || c[0] < -w || c[1] > w || c[1] < -w || c[2] > w || c[2] < -w;
}

static void project(RenderVertex *vertex, const ShadowSetup &setup, const float *clip, const RenderScreenMapping &screen,
                    float depth) {
    render_shadow_place(vertex, clip, &setup.screen[0][0], &screen, depth);
    vertex->u = vertex->v = 0.0f;
    vertex->r = vertex->g = vertex->b = 1;
    vertex->a = 64;
    vertex->fog = 1.0f;
}

static int is_back(const RenderVertex v[4]) {
    return (v[2].x - v[0].x) * (v[1].y - v[0].y) - (v[2].y - v[0].y) * (v[1].x - v[0].x) < 0.0f;
}

struct ShadowFaces {
    std::vector<RenderVertex> vertices;
    std::vector<ShadowPoint> points;
};

static ShadowFaces shadow_faces[2];

static void draw_faces(const ShadowSetup &setup, int back, int record) {
    ShadowFaces &faces = shadow_faces[back];
    if (faces.vertices.empty()) return;
    RenderState state = setup.state;
    state.blending = back ? RenderBlending{RENDER_BLEND_ZERO, RENDER_BLEND_SOURCE, RENDER_BLEND_FIX,
                                           RENDER_BLEND_DESTINATION, GS_ALPHA_ONE}
                          : RenderBlending{RENDER_BLEND_SOURCE, RENDER_BLEND_ZERO, RENDER_BLEND_FIX,
                                           RENDER_BLEND_DESTINATION, GS_ALPHA_ONE};
    render_source = RENDER_SOURCE_SHADOW;
    render_shadow(&state, faces.vertices.data(), faces.points.data(), (int)faces.vertices.size(), record);
}

static void keep_faces(const RenderVertex *vertices, const ShadowPoint *points, int quads, int back) {
    ShadowFaces &faces = shadow_faces[back];
    faces.vertices.insert(faces.vertices.end(), vertices, vertices + quads * 6);
    faces.points.insert(faces.points.end(), points, points + quads * 6);
}

static void add_quad(RenderVertex *out, ShadowPoint *points, const RenderVertex *const v[4], const RenderVertex *model,
                     const RenderVertex *cast, const float *const source[3], const int index[3], int prism, int back,
                     int hidden) {
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++) {
        const RenderVertex *corner = v[order[i]];
        int cast_side = corner >= cast && corner < cast + 3;
        int corner_index = (int)(cast_side ? corner - cast : corner - model);
        const float *position = source[corner_index];
        out[i] = hidden ? *v[0] : *corner;
        points[i] = {{position[0], position[1], position[2]}, cast_side, back, index[corner_index], prism};
    }
}

static void draw_triangle(const ShadowSetup &setup, const RenderScreenMapping &screen, float depth, int record, const float *const v[3],
                          const int index[3], int prism) {
    float c[3][4], t[3][4];
    for (int j = 0; j < 3; j++) {
        float position[4] = {v[j][0], v[j][1], v[j][2], 1.0f};
        transform(c[j], setup.clip, position);
        transform(t[j], setup.shadow, position);
        if (outside(c[j])) return;
    }
    for (int j = 0; j < 3; j++)
        if (outside(t[j])) return;
    float e1[3] = {c[1][0] - c[0][0], c[1][1] - c[0][1], c[1][2] - c[0][2]};
    float e2[3] = {c[2][0] - c[0][0], c[2][1] - c[0][1], c[2][2] - c[0][2]};
    float n[3] = {e2[1] * e1[2] - e2[2] * e1[1], e2[2] * e1[0] - e2[0] * e1[2], e2[0] * e1[1] - e2[1] * e1[0]};
    float q = n[0] * setup.light[0] + n[1] * setup.light[1] + n[2] * setup.light[2];
    if (fabsf(q) <= 1e-4f) return;
    RenderVertex model[3] = {}, cast[3] = {};
    for (int j = 0; j < 3; j++) {
        project(&model[j], setup, c[j], screen, depth);
        project(&cast[j], setup, t[j], screen, depth);
    }
    const RenderVertex *mc[3] = {&model[0], &model[1], &model[2]};
    const RenderVertex *mt[3] = {&cast[0], &cast[1], &cast[2]};
    const RenderVertex *quads[5][4];
    if (q < 0.0f) {
        const RenderVertex *b[5][4] = {{mc[0], mc[1], mt[1], mt[0]}, {mc[1], mc[2], mt[2], mt[1]}, {mc[2], mc[0], mt[0], mt[2]},
                                       {mt[0], mt[1], mt[2], mt[2]}, {mc[2], mc[1], mc[0], mc[0]}};
        memcpy(quads, b, sizeof quads);
    } else {
        const RenderVertex *a[5][4] = {{mc[0], mt[0], mt[1], mc[1]}, {mc[1], mt[1], mt[2], mc[2]}, {mc[2], mt[2], mt[0], mc[0]},
                                       {mt[2], mt[1], mt[0], mt[0]}, {mc[0], mc[1], mc[2], mc[2]}};
        memcpy(quads, a, sizeof quads);
    }
    RenderVertex fronts[5 * 6], backs[5 * 6];
    ShadowPoint front_points[5 * 6], back_points[5 * 6];
    int front_count = 0, back_count = 0, facing = 0;
    for (int g = 0; g < 5; g++) {
        RenderVertex corners[4] = {*quads[g][0], *quads[g][1], *quads[g][2], *quads[g][3]};
        int back = is_back(corners);
        facing += !back;
        if (record >= 0 || back) {
            add_quad(&backs[6 * back_count], &back_points[6 * back_count], quads[g], model, cast, v, index, prism, 1, !back);
            back_count++;
        }
        if (record >= 0 || !back) {
            add_quad(&fronts[6 * front_count], &front_points[6 * front_count], quads[g], model, cast, v, index, prism, 0, back);
            front_count++;
        }
    }
    if (!facing) return;
    keep_faces(fronts, front_points, front_count, 0);
    keep_faces(backs, back_points, back_count, 1);
}

void shadow_draw(const mgCVisualMDT *visual, const ShadowSetup &setup, const mgFACE_GROUP *group) {
    if (render_toggles.no_shadows) return;
    RenderScreenMapping screen;
    float depth;
    gs_screen_mapping(&screen, &depth);
    int prisms = 0;
    for (const mgCFace *face = group->face; face != NULL; face = face->next) prisms += face->vertex_num;
    int record = render_shadow_setup(setup.key, &setup.world[0][0], &setup.camera[0][0], &setup.caster[0][0],
                                     &setup.screen[0][0], &screen, depth, (const float (*)[4])visual->vertex,
                                     visual->vertex_num, prisms);
    int prism = 0;
    shadow_faces[0].vertices.clear();
    shadow_faces[0].points.clear();
    shadow_faces[1].vertices.clear();
    shadow_faces[1].points.clear();
    for (const mgCFace *face = group->face; face != NULL; face = face->next) {
        const int *index = face->index;
        for (int i = 0; i < face->vertex_num; i++, index += 3) {
            const float *corners[3] = {visual->vertex[index[0]], visual->vertex[index[1]], visual->vertex[index[2]]};
            draw_triangle(setup, screen, depth, record, corners, index, prism++);
        }
    }
    draw_faces(setup, 0, record);
    draw_faces(setup, 1, record);
}
