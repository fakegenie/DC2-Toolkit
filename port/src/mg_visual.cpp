#include "mg_visual.hpp"
#include "draw.hpp"
#include "port_stats.h"
#include "mg_drawprim.hpp"
#include "mg_math.hpp"
#include "mg_memory.hpp"
#include "mg_texture.hpp"
#include "mglib.hpp"
#include "sce/libvu0.h"
#include "visualmotion.hpp"
#include <cstring>
#include <unordered_map>
#include <vector>

int motion_bones(mgCVisualMotionMDT *visual, float (*matrix)[4], float (*bones)[4][4]);

struct FaceMesh {
    std::vector<MeshVertex> vertices;
    u_short type;
    u_int used;
    int resident = -1;
};

enum { MESH_IDLE_FRAMES = 600, MESH_CACHE_LIMIT = 30000 };
enum { PROGRAM_REAL = 1, PROGRAM_REF_SPHERE = 2, SPECULAR_PASSES = 3 };

struct VisualDraw {
    mgCVisualMDT *visual;
    MeshUniforms uniforms;
    RenderState state;
    int cull;
    int skinned;
    RenderBone bones[RENDER_BONE_SLOTS];
    ShadowSetup shadow;
    const void *common;
};

static std::unordered_map<const mgCFace *, FaceMesh> face_meshes;
static u_int mesh_frame;
static std::vector<VisualDraw> sorted_draws;
static std::vector<VisualDraw> outlined;
static int outlining;
PORT_GAME_BEGIN
static VisualDraw current;
PORT_GAME_END

static u_char colour_byte(float value) {
    float scaled = value * 128.0f;
    if (!(scaled > 0.0f)) return 0;
    if (scaled >= 255.0f) return 255;
    return (u_char)(scaled + 0.5f);
}

static MeshVertex corner_vertex(const mgCVisualMDT *visual, const mgCFace *face, int corner,
                                 const mgVertexWeight *weights) {
    const int *cursor = face->index + corner * face->index_stride;
    MeshVertex vertex;
    memset(&vertex, 0, sizeof vertex);
    int next = 1;
    const float *position = visual->vertex[cursor[0]];
    memcpy(vertex.position, position, sizeof vertex.position);
    if (!(face->type & MG_FACE_NO_NORMAL)) memcpy(vertex.normal, visual->normal[cursor[next++]], sizeof vertex.normal);
    if (!(face->type & MG_FACE_NO_TEXTURE)) memcpy(vertex.uv, visual->uv[cursor[next++]], sizeof vertex.uv);
    if (face->type & MG_FACE_COLOUR) {
        const float *colour = visual->colour[cursor[next++]];
        for (int i = 0; i < 4; i++) vertex.color[i] = colour_byte(colour[i]);
    } else {
        for (int i = 0; i < 4; i++) vertex.color[i] = 0x80;
    }
    vertex.weight[0] = 1.0f;
    if (weights) {
        const mgVertexWeight *weight = &weights[cursor[0]];
        for (int i = 0; i < 4; i++) {
            vertex.bone[i] = (u_char)(weight->matrix[i] / 4);
            vertex.weight[i] = weight->weight[i];
        }
    }
    return vertex;
}

void visual_build_face(const mgCVisualMDT *visual, const mgCFace *face, const mgVertexWeight *weights) {
    FaceMesh &mesh = face_meshes[face];
    mesh.used = mesh_frame;
    port_stats.mesh_builds++;
    port_stats.cached_meshes = (uint32_t)face_meshes.size();
    mesh.type = face->type;
    render_mesh_forget(mesh.resident, (int)mesh.vertices.size());
    mesh.resident = -1;
    mesh.vertices.clear();
    int corners = face->vertex_num;
    if (corners < 3 || face->index == NULL) return;
    std::vector<MeshVertex> points((size_t)corners);
    for (int corner = 0; corner < corners; corner++)
        points[(size_t)corner] = corner_vertex(visual, face, corner, weights);
    if ((face->type & MG_FACE_PRIM_MASK) == MG_PRIM_TRIANGLE_STRIP) {
        for (int t = 0; t + 2 < corners; t++) {
            int odd = t & 1;
            mesh.vertices.push_back(points[(size_t)(odd ? t + 1 : t)]);
            mesh.vertices.push_back(points[(size_t)(odd ? t : t + 1)]);
            mesh.vertices.push_back(points[(size_t)(t + 2)]);
        }
    } else {
        for (int corner = 0; corner + 2 < corners; corner += 3)
            for (int i = 0; i < 3; i++) mesh.vertices.push_back(points[(size_t)(corner + i)]);
    }
    mesh.resident = render_mesh_store(mesh.vertices.data(), (int)mesh.vertices.size());
}

static int mirrored(float (*matrix)[4]) {
    float determinant = matrix[0][0] * (matrix[1][1] * matrix[2][2] - matrix[1][2] * matrix[2][1]) -
                        matrix[0][1] * (matrix[1][0] * matrix[2][2] - matrix[1][2] * matrix[2][0]) +
                        matrix[0][2] * (matrix[1][0] * matrix[2][1] - matrix[1][1] * matrix[2][0]);
    return determinant < 0.0f;
}

static void copy_matrix(float *out, float (*matrix)[4]) {
    memcpy(out, matrix, 16 * sizeof(float));
}

static void draw_group(const VisualDraw &draw, const mgFACE_GROUP *group, u_int tint, int edge) {
    if (group->vu_program == 1) {
        if (!edge) shadow_draw(draw.visual, draw.shadow, group);
        return;
    }
    if (group->vu_program != 0) return;
    mgMaterial *material = draw.visual->material + group->material;
    MeshUniforms uniforms = draw.uniforms;
    memcpy(uniforms.diffuse, material->diffuse, sizeof uniforms.diffuse);
    memcpy(uniforms.specular, material->unk_10, sizeof uniforms.specular);
    uniforms.flags.specular_passes = SPECULAR_PASSES;
    uniforms.edge = (float)edge;
    gs_screen_mapping(&uniforms.screen, &uniforms.depth_scale);
    RenderState textured = draw.state;
    gs_texture_state(&textured, material->texture, &uniforms.texture_scale_u, &uniforms.texture_scale_v);
    gs_sample_limits(&textured, uniforms.texture_scale_u, uniforms.texture_scale_v, &uniforms.limits);
    for (const mgCFace *face = group->face; face != NULL; face = face->next) {
        auto found = face_meshes.find(face);
        if (found == face_meshes.end()) continue;
        found->second.used = mesh_frame;
        if (found->second.vertices.empty()) continue;
        RenderState state = (found->second.type & MG_FACE_NO_TEXTURE) ? draw.state : textured;
        if (tint) {
            state.blending = {RENDER_BLEND_SOURCE, RENDER_BLEND_DESTINATION, RENDER_BLEND_SOURCE, RENDER_BLEND_DESTINATION, 0};
            state.tint = tint;
        }
        if (edge) state.depth_write = 0;
        uniforms.flags.face_type = found->second.type;
        render_mesh(&state, draw.cull, &uniforms, draw.skinned ? draw.bones : NULL, found->second.vertices.data(),
                    (int)found->second.vertices.size(), found->second.resident);
    }
}

static void draw_visual(const VisualDraw &draw, u_int tint, int edge) {
    for (const mgFACE_GROUP *group = draw.visual->face_group; group != NULL; group = group->next)
        draw_group(draw, group, tint, edge);
}

void visual_outline_begin() {
    outlined.clear();
    outlining = 1;
}

void visual_outline_end(int offset, u_int edge_tint, u_int body_tint) {
    static const int sides[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    outlining = 0;
    for (int side = 0; offset > 0 && side < 4; side++) {
        gs_shift_screen(sides[side][0] * offset, sides[side][1] * offset);
        for (const VisualDraw &draw : outlined) draw_visual(draw, edge_tint, side + 1);
    }
    gs_shift_screen(0, 0);
    for (const VisualDraw &draw : outlined) draw_visual(draw, body_tint, 0);
}

int visual_packet_draw(const void *common, const void *packet) {
    u_int index = *(const u_int *)common;
    if (index >= sorted_draws.size() || sorted_draws[index].common != common) return 0;
    draw_group(sorted_draws[index], (const mgFACE_GROUP *)packet, 0, 0);
    return 1;
}

void visual_next_frame() {
    sorted_draws.clear();
    if (++mesh_frame % MESH_IDLE_FRAMES != 0 || face_meshes.size() <= MESH_CACHE_LIMIT) return;
    for (auto entry = face_meshes.begin(); entry != face_meshes.end();) {
        if (mesh_frame - entry->second.used > MESH_IDLE_FRAMES) {
            render_mesh_forget(entry->second.resident, (int)entry->second.vertices.size());
            entry = face_meshes.erase(entry);
        } else {
            ++entry;
        }
    }
}

static size_t put(unsigned char *out, size_t at, const void *data, size_t size) {
    if (out) memcpy(out + at, data, size);
    return at + size;
}

size_t visual_save(unsigned char *out) {
    uint32_t count = (uint32_t)face_meshes.size();
    size_t at = put(out, 0, &count, sizeof count);
    at = put(out, at, &mesh_frame, sizeof mesh_frame);
    for (const auto &entry : face_meshes) {
        uint32_t vertices = (uint32_t)entry.second.vertices.size();
        at = put(out, at, &entry.first, sizeof entry.first);
        at = put(out, at, &entry.second.type, sizeof entry.second.type);
        at = put(out, at, &entry.second.used, sizeof entry.second.used);
        at = put(out, at, &vertices, sizeof vertices);
        at = put(out, at, entry.second.vertices.data(), vertices * sizeof(MeshVertex));
    }
    return at;
}

size_t visual_keys(const unsigned char *in, std::vector<size_t> *offsets) {
    uint32_t count;
    memcpy(&count, in, sizeof count);
    size_t at = sizeof count + sizeof mesh_frame;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t vertices;
        offsets->push_back(at);
        at += sizeof(const mgCFace *) + sizeof(u_short) + sizeof(u_int);
        memcpy(&vertices, in + at, sizeof vertices);
        at += sizeof vertices + vertices * sizeof(MeshVertex);
    }
    return at;
}

size_t visual_load(const unsigned char *in) {
    for (const auto &entry : face_meshes)
        render_mesh_forget(entry.second.resident, (int)entry.second.vertices.size());
    face_meshes.clear();
    uint32_t count;
    size_t at = 0;
    memcpy(&count, in, sizeof count);
    at += sizeof count;
    memcpy(&mesh_frame, in + at, sizeof mesh_frame);
    at += sizeof mesh_frame;
    for (uint32_t i = 0; i < count; i++) {
        const mgCFace *face;
        uint32_t vertices;
        memcpy(&face, in + at, sizeof face);
        at += sizeof face;
        FaceMesh &mesh = face_meshes[face];
        memcpy(&mesh.type, in + at, sizeof mesh.type);
        at += sizeof mesh.type;
        memcpy(&mesh.used, in + at, sizeof mesh.used);
        at += sizeof mesh.used;
        memcpy(&vertices, in + at, sizeof vertices);
        at += sizeof vertices;
        mesh.vertices.resize(vertices);
        memcpy(mesh.vertices.data(), in + at, vertices * sizeof(MeshVertex));
        at += vertices * sizeof(MeshVertex);
        mesh.resident = vertices ? render_mesh_store(mesh.vertices.data(), (int)vertices) : -1;
    }
    sorted_draws.clear();
    port_stats.cached_meshes = (uint32_t)face_meshes.size();
    return at;
}

PORT_OVERRIDE int mgCVisualMDT::CreateFacePacket(u_int *packet, mgCFace *face) {
    (void)packet;
    if (face != NULL) visual_build_face(this, face, NULL);
    return 0;
}

PORT_OVERRIDE uptr mgCVisualMDT::CreatePacket(mgCDrawManager *manager) {
    (void)manager;
    for (mgFACE_GROUP *group = face_group; group != NULL; group = group->next)
        for (mgCFace *face = group->face; face != NULL; face = face->next) CreateFacePacket(NULL, face);
    return 0;
}

PORT_OVERRIDE uptr mgCVisualFixMDT::CreatePacket(mgCDrawManager *manager) {
    (void)manager;
    return 0;
}

PORT_OVERRIDE int mgCVisualMDT::CreateRenderInfoPacket(u_int *packet, float (*matrix)[4], mgRENDER_INFO *info) {
    (void)packet;
    VisualDraw &draw = current;
    draw.visual = this;
    draw.skinned = 0;
    if (info->attr == NULL) return 0;
    mgCFrameAttr *attr = info->attr;
    mgLIGHT_INFO *lighting = info->GetpLightInfo();
    MeshUniforms &uniforms = draw.uniforms;
    memset(&uniforms, 0, sizeof uniforms);
    sceVu0FMATRIX model_screen;
    if (attr->depth_bias > 1.0f) {
        sceVu0FMATRIX projection;
        sceVu0FMATRIX world_screen;
        sceVu0CopyMatrix(projection, info->screen);
        projection[3][2] *= 1.005f;
        mgMulMatrix(world_screen, projection, info->world_view);
        mgMulMatrix(model_screen, world_screen, matrix);
    } else {
        mgMulMatrix(model_screen, info->world_screen, matrix);
    }
    copy_matrix(uniforms.model_screen, model_screen);
    copy_matrix(uniforms.model_world, matrix);
    for (int k = 0; k < 3; k++) memcpy(uniforms.light_direction[k], lighting->light_dir[k], sizeof uniforms.light_direction[k]);
    memcpy(uniforms.light_color, lighting->light_color, sizeof uniforms.light_color);
    memcpy(uniforms.ambient, lighting->ambient, sizeof uniforms.ambient);
    uniforms.ambient[3] *= attr->obj_alpha;
    if (attr->ambient_boost != 0) {
        for (int i = 0; i < 3; i++) uniforms.object_color[i] = 0.3f * lighting->light_color[0][i] + lighting->ambient[i];
        uniforms.object_color[3] = info->object_color[3];
    } else {
        memcpy(uniforms.object_color, info->object_color, sizeof uniforms.object_color);
    }
    uniforms.object_color[3] *= attr->obj_alpha;
    if (attr->program_mode != 0) {
        sceVu0FVECTOR eye = {info->camera_pos[0], info->camera_pos[1], info->camera_pos[2], 1.0f};
        sceVu0FMATRIX inverse;
        sceVu0CopyMatrix(inverse, matrix);
        sceVu0InversMatrix(inverse, inverse);
        sceVu0ApplyMatrix(eye, inverse, eye);
        memcpy(uniforms.eye, eye, sizeof uniforms.eye);
    }
    if (attr->program_mode & PROGRAM_REF_SPHERE) {
        sceVu0FMATRIX view_model;
        mgMulMatrix(view_model, info->view, matrix);
        copy_matrix(uniforms.clip_or_view, view_model);
    }
    if (info->plight_hit != 0 && info->lighting_enabled != 0)
        for (int i = 0; i < 4; i++) {
            mgPOINT_LIGHT *light = &lighting->point_light[i];
            for (int k = 0; k < 3; k++) uniforms.point_light[i][k] = light->pos[k] - matrix[3][k];
            uniforms.point_light[i][3] = light->power;
            memcpy(uniforms.point_color[i], light->color, sizeof uniforms.point_color[i]);
        }
    uniforms.fog = {info->fog.offset, info->fog.near_value, info->fog.far_value, info->fog.scale};
    u_int flags = 0;
    if ((info->clip | info->scissor) != 0) flags |= MESH_CLIP;
    if (info->scissor != 0) flags |= MESH_SCISSOR;
    if (attr->program_mode & PROGRAM_REAL) flags |= MESH_REAL;
    if (attr->program_mode & PROGRAM_REF_SPHERE) flags |= MESH_REF_SPHERE;
    if (attr->program_option != 0) flags |= MESH_CULL;
    if (info->plight_hit != 0) flags |= MESH_POINT_LIGHTS;
    if (info->motion != 0) flags |= MESH_BLEND;
    if (attr->no_light != 0 || info->lighting_enabled == 0 || attr->ambient_boost != 0) flags |= MESH_UNLIT;
    if (attr->unk_84 == 1) flags |= MESH_COLOR_ADD;
    uniforms.flags.program = flags;
    int fogged = attr->fog != 0 && info->fog_enable != 0;
    u_int fog_colour = info->fog.r | info->fog.g << 8 | info->fog.b << 16;
    if (attr->fog > MG_FOG_SCENE) {
        if (attr->fog == MG_FOG_BLACK) fog_colour = 0;
        if (attr->fog == MG_FOG_WHITE) fog_colour = 0xffffff;
    }
    mgCDrawEnv env;
    SetDrawEnvAttributes(&env, info, draw_env != NULL ? draw_env : &info->draw_env[0]);
    gs_base_state(&draw.state);
    draw.state.blend = 1;
    draw.state.blending = gs_blending(env.alpha);
    draw.state.tests = gs_tests(env.test);
    draw.state.depth_write = !env.zbuf.bits.zmsk;
    draw.state.fog = fogged;
    draw.state.fog_color = fog_colour;
    draw.cull = RENDER_CULL_NONE;
    if (attr->program_option != 0) draw.cull = mirrored(matrix) ? RENDER_CULL_FRONT : RENDER_CULL_BACK;
    if (info->motion != 0) {
        float bones[RENDER_BONE_SLOTS][4][4] = {};
        int slots = motion_bones(static_cast<mgCVisualMotionMDT *>(this), matrix, bones);
        for (int b = 0; b < RENDER_BONE_SLOTS; b++)
            for (int k = 0; k < 3; k++)
                for (int c = 0; c < 4; c++)
                    draw.bones[b].rows[k][c] = b < slots ? bones[b][c][k] : (k == c ? 1.0f : 0.0f);
        draw.skinned = 1;
    }
    return 0;
}

PORT_OVERRIDE int mgCVisualMDT::Draw(u_int *tag, float (*matrix)[4], mgCDrawManager *draw_manager) {
    if (draw_manager == NULL) draw_manager = &mgDrawManager;
    mgRENDER_INFO *info = draw_manager->render_info;
    texture_manager = draw_manager->texture_manager;
    prev_tex = 0;
    if (info->attr == NULL) return 0;
    current.visual = this;
    current.skinned = 0;
    CreateRenderInfoPacket(NULL, matrix, info);
    current.shadow = shadow_setup;
    CreatePacket(draw_manager);
    if (tag != NULL && outlining) {
        outlined.push_back(current);
        return 0;
    }
    if (tag != NULL) {
        draw_visual(current, 0, 0);
        return 0;
    }
    u_long128 *common = draw_manager->data_memory->Alloc(1);
    if (common == NULL) return 0;
    *(u_int *)common = (u_int)sorted_draws.size();
    current.common = common;
    sorted_draws.push_back(current);
    port_stats.sorted_draws++;
    for (mgFACE_GROUP *group = face_group; group != NULL; group = group->next) {
        mgCTexture *texture = (material + group->material)->texture;
        draw_manager->AddPacket(texture != NULL ? texture->block : -1, common, (u_long128 *)group, group->vu_program);
    }
    return 0;
}

PORT_OVERRIDE int mgCVisualPrim::CreateRenderInfoPacket(u_int *packet, float (*matrix)[4], mgRENDER_INFO *info) {
    (void)packet;
    (void)matrix;
    mgCDrawEnv env;
    if (draw_env != NULL) env = *draw_env;
    else SetDrawEnv(&env, &attr, &info->draw_env[0]);
    gs_write(SCE_GS_TEST_1, env.test.value);
    gs_write(SCE_GS_ZBUF_1, env.zbuf.value);
    gs_write(SCE_GS_ALPHA_1, env.alpha.value);
    return 0;
}
