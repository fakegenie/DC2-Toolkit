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

struct FaceMesh {
    std::vector<MeshVertex> vertices;
    u_short type;
    u_int used;
    int resident = -1;
};

enum { MESH_IDLE_FRAMES = 600, MESH_CACHE_LIMIT = 30000 };

struct VisualDraw {
    mgCVisualMDT *visual;
    MeshUniforms uniforms;
    RenderState state;
    int cull;
    int skinned;
    float bones[384];
    ShadowSetup shadow;
    const void *common;
};

static std::unordered_map<const mgCFace *, FaceMesh> face_meshes;
static u_int mesh_frame;
static std::vector<VisualDraw> sorted_draws;
static VisualDraw current;

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

static void draw_group(const VisualDraw &draw, const mgFACE_GROUP *group) {
    if (group->vu_program == 1) {
        shadow_draw(draw.visual, draw.shadow, group);
        return;
    }
    if (group->vu_program != 0) return;
    mgMaterial *material = draw.visual->material + group->material;
    MeshUniforms uniforms = draw.uniforms;
    memcpy(uniforms.diffuse, material->diffuse, sizeof uniforms.diffuse);
    memcpy(uniforms.specular, material->unk_10, sizeof uniforms.specular);
    uniforms.flags[2] = 3;
    gs_screen_mapping(uniforms.screen, &uniforms.depth[0]);
    RenderState textured = draw.state;
    gs_texture_state(&textured, material->texture, &uniforms.depth[1], &uniforms.depth[2]);
    for (const mgCFace *face = group->face; face != NULL; face = face->next) {
        auto found = face_meshes.find(face);
        if (found == face_meshes.end()) continue;
        found->second.used = mesh_frame;
        if (found->second.vertices.empty()) continue;
        RenderState state = (found->second.type & MG_FACE_NO_TEXTURE) ? draw.state : textured;
        uniforms.flags[1] = found->second.type;
        render_mesh(&state, draw.cull, &uniforms, draw.skinned ? draw.bones : NULL, found->second.vertices.data(),
                    (int)found->second.vertices.size(), found->second.resident);
    }
}

static void draw_visual(const VisualDraw &draw) {
    for (const mgFACE_GROUP *group = draw.visual->face_group; group != NULL; group = group->next)
        draw_group(draw, group);
}

int visual_packet_draw(const void *common, const void *packet) {
    u_int index = *(const u_int *)common;
    if (index >= sorted_draws.size() || sorted_draws[index].common != common) return 0;
    draw_group(sorted_draws[index], (const mgFACE_GROUP *)packet);
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
    if (attr->program_mode & 2) {
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
    uniforms.fog[0] = info->fog.offset;
    uniforms.fog[1] = info->fog.near_value;
    uniforms.fog[2] = info->fog.far_value;
    uniforms.fog[3] = info->fog.scale;
    u_int flags = 0;
    if ((info->clip | info->scissor) != 0) flags |= 0x1;
    if (info->scissor != 0) flags |= 0x2;
    if (attr->program_mode & 1) flags |= 0x8;
    if (attr->program_mode & 2) flags |= 0x100;
    if (attr->program_option != 0) flags |= 0x4;
    if (info->plight_hit != 0) flags |= 0x10;
    if (info->motion != 0) flags |= 0x40;
    if (attr->no_light != 0 || info->lighting_enabled == 0 || attr->ambient_boost != 0) flags |= 0x20;
    if (attr->unk_84 == 1) flags |= 0x80;
    uniforms.flags[0] = flags;
    int fogged = attr->fog != 0 && info->fog_enable != 0;
    prmode = (u_int)(fogged << 5) | 0x58;
    u_int fog_colour = info->fog.r | info->fog.g << 8 | info->fog.b << 16;
    if (attr->fog > MG_FOG_SCENE) {
        if (attr->fog == MG_FOG_BLACK) fog_colour = 0;
        if (attr->fog == MG_FOG_WHITE) fog_colour = 0xffffff;
    }
    mgCDrawEnv env;
    SetDrawEnvGifTag((u_long128 *)&env, info, draw_env != NULL ? draw_env : &info->draw_env[0]);
    gs_base_state(&draw.state);
    draw.state.blend = 1;
    draw.state.alpha = env.alpha.value;
    draw.state.test = env.test.value;
    draw.state.depth_write = !env.zbuf.bits.zmsk;
    draw.state.fog = fogged;
    draw.state.fog_color = fog_colour;
    draw.cull = RENDER_CULL_NONE;
    if (attr->program_option != 0) draw.cull = mirrored(matrix) ? RENDER_CULL_FRONT : RENDER_CULL_BACK;
    if (info->motion != 0) {
        static u_int bone_packet[4 + 32 * 16];
        memset(bone_packet, 0, sizeof bone_packet);
        int size = CreateExtRenderInfoPacket(bone_packet, matrix, info);
        int slots = size > 1 ? (size - 1) / 4 : 0;
        for (int b = 0; b < 32; b++) {
            const float *bone = (const float *)&bone_packet[4 + b * 16];
            for (int k = 0; k < 3; k++)
                for (int c = 0; c < 4; c++)
                    draw.bones[(b * 3 + k) * 4 + c] = b < slots ? bone[c * 4 + k] : (k == c ? 1.0f : 0.0f);
        }
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
    if (tag != NULL) {
        draw_visual(current);
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
    gs_write(SCE_GS_ZBUF_1, *(u_long *)&env.zbuf);
    gs_write(SCE_GS_ALPHA_1, env.alpha.value);
    return 0;
}
