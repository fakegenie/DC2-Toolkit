#include "mg_shadow.hpp"
#include "draw.hpp"
#include "mg_drawenv.hpp"
#include "mg_math.hpp"
#include "mglib.hpp"
#include <cmath>
#include <cstring>

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
    mgMulMatrix(clip, info->view_clip_full, info->view);
    mgMulMatrix(clip, clip, matrix);
    memcpy(shadow_setup.clip, clip, sizeof shadow_setup.clip);
    sceVu0FMATRIX shadow;
    mgMulMatrix(shadow, info->shadow, matrix);
    mgMulMatrix(shadow, info->view, shadow);
    mgMulMatrix(shadow, info->view_clip_full, shadow);
    memcpy(shadow_setup.shadow, shadow, sizeof shadow_setup.shadow);
    memcpy(shadow_setup.screen, info->clip_screen_full, sizeof shadow_setup.screen);
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
    shadow_setup.state.test = env.test.value;
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

static void project(RenderVertex *vertex, const ShadowSetup &setup, const float *clip) {
    float p[4];
    transform(p, setup.screen, clip);
    float q = p[3] != 0.0f ? 1.0f / p[3] : 0.0f;
    float screen[4];
    float depth;
    gs_screen_mapping(screen, &depth);
    float x = p[0] * q, y = p[1] * q, z = p[2] * q;
    vertex->x = (x * screen[0] + screen[1] + 1.0f) / screen[0];
    vertex->y = (y * screen[2] + screen[3] - 1.0f) / screen[2];
    float scaled = (float)(long long)z * depth;
    vertex->z = scaled < 0.0f ? 0.0f : scaled > 1.0f ? 1.0f : scaled;
    vertex->u = vertex->v = 0.0f;
    vertex->r = vertex->g = vertex->b = 1;
    vertex->a = 64;
    vertex->fog = 1.0f;
}

static int is_back(const RenderVertex v[4]) {
    return (v[2].x - v[0].x) * (v[1].y - v[0].y) - (v[2].y - v[0].y) * (v[1].x - v[0].x) < 0.0f;
}

static void draw_quad(const ShadowSetup &setup, const RenderVertex v[4], int back) {
    RenderVertex out[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
    RenderState state = setup.state;
    state.alpha = back ? 0x8000000062ull : 0x8000000068ull;
    render_source = RENDER_SOURCE_SHADOW;
    render_draw(&state, RENDER_TRIANGLES, out, 6);
}

static void draw_triangle(const ShadowSetup &setup, const float *v0, const float *v1, const float *v2) {
    float c[3][4], t[3][4];
    const float *v[3] = {v0, v1, v2};
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
    const float *quads[5][4];
    if (q < 0.0f) {
        const float *b[5][4] = {{c[0], c[1], t[1], t[0]}, {c[1], c[2], t[2], t[1]}, {c[2], c[0], t[0], t[2]},
                                {t[0], t[1], t[2], t[2]}, {c[2], c[1], c[0], c[0]}};
        memcpy(quads, b, sizeof quads);
    } else {
        const float *a[5][4] = {{c[0], t[0], t[1], c[1]}, {c[1], t[1], t[2], c[2]}, {c[2], t[2], t[0], c[0]},
                                {t[2], t[1], t[0], t[0]}, {c[0], c[1], c[2], c[2]}};
        memcpy(quads, a, sizeof quads);
    }
    RenderVertex projected[5][4] = {};
    int back[5];
    int fronts = 0;
    for (int g = 0; g < 5; g++) {
        for (int i = 0; i < 4; i++) project(&projected[g][i], setup, quads[g][i]);
        back[g] = is_back(projected[g]);
        if (back[g]) continue;
        draw_quad(setup, projected[g], 0);
        fronts++;
    }
    if (!fronts) return;
    for (int g = 0; g < 5; g++)
        if (back[g]) draw_quad(setup, projected[g], 1);
}

void shadow_draw(const mgCVisualMDT *visual, const ShadowSetup &setup, const mgFACE_GROUP *group) {
    if (render_toggles.no_shadows) return;
    for (const mgCFace *face = group->face; face != NULL; face = face->next) {
        const int *index = face->index;
        for (int i = 0; i < face->vertex_num; i++, index += 3)
            draw_triangle(setup, visual->vertex[index[0]], visual->vertex[index[1]], visual->vertex[index[2]]);
    }
}
