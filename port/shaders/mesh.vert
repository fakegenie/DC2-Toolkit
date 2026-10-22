#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(location = 3) in vec4 color;
layout(location = 4) in vec4 bone;
layout(location = 5) in vec4 weight;

struct Model {
    mat4 model_screen;
    mat4 model_world;
    mat4 clip_or_view;
    vec4 light_direction[3];
    vec4 light_color[4];
    vec4 ambient;
    vec4 object_color;
    vec4 eye;
    vec4 diffuse;
    vec4 specular;
    vec4 point_light[4];
    vec4 point_color[4];
    vec4 fog;
    vec4 screen;
    vec4 depth;
    uvec4 flags;
};

layout(std430, set = 0, binding = 0) readonly buffer Models {
    Model models[];
};

layout(std430, set = 0, binding = 1) readonly buffer Bones {
    vec4 bones[];
};

#define model_screen models[gl_InstanceIndex].model_screen
#define model_world models[gl_InstanceIndex].model_world
#define clip_or_view models[gl_InstanceIndex].clip_or_view
#define light_direction models[gl_InstanceIndex].light_direction
#define light_color models[gl_InstanceIndex].light_color
#define ambient models[gl_InstanceIndex].ambient
#define object_color models[gl_InstanceIndex].object_color
#define eye models[gl_InstanceIndex].eye
#define diffuse models[gl_InstanceIndex].diffuse
#define specular models[gl_InstanceIndex].specular
#define point_light models[gl_InstanceIndex].point_light
#define point_color models[gl_InstanceIndex].point_color
#define fog models[gl_InstanceIndex].fog
#define screen models[gl_InstanceIndex].screen
#define depth models[gl_InstanceIndex].depth
#define flags models[gl_InstanceIndex].flags

layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec2 frag_uv;
layout(location = 2) out float frag_fog;
layout(location = 3) flat out vec4 frag_limits;

const uint REAL = 0x008u;
const uint POINT_LIGHTS = 0x010u;
const uint UNLIT = 0x020u;
const uint BLEND = 0x040u;
const uint COLOR_ADD = 0x080u;
const uint REF_SPHERE = 0x100u;

const uint FACE_COLOR = 0x100u;
const uint FACE_NO_NORMAL = 0x200u;

const float MODULATE = 255.0 / 128.0;
const float VU_MAX = 3.40282347e38;

float vu_rsqrt(float value) {
    return value > 0.0 ? inversesqrt(value) : 0.0;
}

vec3 vu_unit(vec3 value) {
    return value * vu_rsqrt(dot(value, value));
}

void skin(inout vec3 p, inout vec3 n) {
    vec3 blended = vec3(0.0);
    vec3 blended_normal = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        float w = weight[i];
        int b = int(flags.w) * 96 + int(min(bone[i] * 255.0 + 0.5, 31.0)) * 3;
        vec4 r0 = bones[b];
        vec4 r1 = bones[b + 1];
        vec4 r2 = bones[b + 2];
        vec4 pw = vec4(p * w, w);
        blended += vec3(dot(r0, pw), dot(r1, pw), dot(r2, pw));
        vec3 nw = n * w;
        vec3 c0 = vec3(r0.x, r1.x, r2.x);
        vec3 c1 = vec3(r0.y, r1.y, r2.y);
        vec3 c2 = vec3(r0.z, r1.z, r2.z);
        blended_normal += c0 * nw.x + c1 * nw.y + (i == 3 ? c1 : c2) * nw.z;
    }
    p = blended;
    n = blended_normal;
}

void main() {
    uint f = flags.x;
    uint t = flags.y;
    vec3 p = position;
    vec3 n = normal;
    vec2 st = uv;

    if ((f & REF_SPHERE) != 0u) st = (vu_unit(mat3(clip_or_view) * normal).xy + 1.0) * 0.5;
    if ((f & BLEND) != 0u) skin(p, n);

    mat3 world = mat3(model_world);
    vec3 lights[4];
    for (int c = 0; c < 4; c++) {
        vec3 direction = vec3(light_direction[0][c], light_direction[1][c], light_direction[2][c]);
        lights[c] = vu_unit(vec3(dot(direction, world[0]), dot(direction, world[1]), dot(direction, world[2])));
    }

    vec4 d = diffuse;
    vec4 lit;
    if ((f & UNLIT) != 0u || (t & FACE_NO_NORMAL) != 0u) {
        lit = min(object_color * d, vec4(255.0));
    } else {
        lit = ambient * d;
        float shade[4];
        for (int c = 0; c < 4; c++) {
            shade[c] = max(0.0, dot(lights[c], n));
            lit += light_color[c] * d * shade[c];
        }
        if ((f & POINT_LIGHTS) != 0u) {
            vec3 pw = world * p;
            vec3 nw = world * n;
            float inverse_normal = vu_rsqrt(dot(nw, nw));
            for (int k = 0; k < 4; k++) {
                vec3 offset = point_light[k].xyz - pw;
                float inverse_distance = vu_rsqrt(dot(offset, offset));
                float cosine = max(0.0, dot(offset, nw)) * inverse_distance * inverse_normal;
                float a = point_light[k].w * inverse_distance;
                lit += point_color[k] * d * (cosine * min(1.0, a * a));
            }
        } else if ((f & REAL) != 0u) {
            vec3 view = eye.xyz - p;
            float inverse_view = vu_rsqrt(dot(view, view));
            float vn = dot(view, n);
            uint passes = flags.z == 0u ? 32u : min(flags.z, 32u);
            for (int c = 0; c < 4; c++) {
                float on = min(1.0, 1.0e4 * shade[c]);
                float r = max(0.0, 2.0 * vn * dot(lights[c], n) - dot(lights[c], view)) * inverse_view;
                float x = r * r;
                for (uint j = 0u; j < passes; j++) x = min(x * x, VU_MAX);
                lit += light_color[c] * specular * (on * x);
            }
        }
        lit = min(lit, vec4(255.0));
    }

    if ((t & FACE_COLOR) != 0u) {
        vec4 vc = color * MODULATE;
        if ((f & COLOR_ADD) != 0u) {
            lit.rgb = min(lit.rgb + 128.0 * vc.rgb, vec3(255.0));
            lit.a = min(128.0 * vc.a * vc.a, 255.0);
        } else {
            lit = min(lit * vc, vec4(255.0));
        }
    }

    vec4 clip = model_screen * vec4(p, 1.0);
    gl_Position = vec4(clip.x * screen.x + clip.w * screen.y, clip.y * screen.z + clip.w * screen.w,
                       clip.z * depth.x, clip.w);
    frag_color = floor(clamp(lit, 0.0, 255.0)) / 255.0;
    frag_uv = st * depth.yz;
    float fogged = clip.w > 0.0 ? fog.x + fog.w / clip.w : fog.x;
    frag_fog = floor(max(max(min(fogged, fog.y), fog.z), 0.0)) / 255.0;
    frag_limits = vec4(0.0);
}
