#version 450

layout(location = 0) in vec4 frag_color;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in float frag_fog;
layout(location = 3) flat in vec4 frag_limits;

layout(set = 2, binding = 0) uniform sampler2D source;

layout(set = 3, binding = 0) uniform State {
    uint flags;
    uint alpha;
    uint fog_color;
    uint unused;
};

layout(location = 0) out vec4 result;
layout(location = 1) out vec4 stored_depth;

const uint TEXTURED = 1u;
const uint TEXA = 2u;
const uint AEM = 4u;
const uint FOG = 8u;
const uint ALPHA_TEST = 16u;
const uint FACTOR_FIX = 32u;
const uint SCALE_SOURCE = 64u;
const uint FACTOR_COLOR = 128u;
const uint DECAL = 256u;
const uint VERTEX_ALPHA = 512u;
const uint DEPTH_READ = 1024u;
const uint SOLID = 2048u;
const uint OVERDRAW = 4096u;

const float MODULATE = 255.0 / 128.0;

bool alpha_passes(float value) {
    int level = int(value * 128.0 + 0.5);
    int reference = int(alpha & 255u);
    uint method = (alpha >> 8) & 7u;
    if (method == 0u) return false;
    if (method == 1u) return true;
    if (method == 2u) return level < reference;
    if (method == 3u) return level <= reference;
    if (method == 4u) return level == reference;
    if (method == 5u) return level >= reference;
    if (method == 6u) return level > reference;
    return level != reference;
}

void main() {
    stored_depth = vec4(gl_FragCoord.z);
    if ((flags & DEPTH_READ) != 0u) {
        uint depth = uint(texture(source, frag_uv).r * 16777215.0 + 0.5);
        result = vec4(float(depth & 255u), float((depth >> 8) & 255u), float((depth >> 16) & 255u), 255.0) / 255.0;
        return;
    }
    if ((flags & SOLID) != 0u) {
        result = vec4(1.0, 0.0, 1.0, 1.0);
        return;
    }
    vec4 color = vec4(frag_color.rgb, frag_color.a * MODULATE);
    if ((flags & TEXTURED) != 0u) {
        vec2 uv = frag_limits == vec4(0.0) ? frag_uv : clamp(frag_uv, frag_limits.xy, frag_limits.zw);
        vec4 texel = texture(source, uv);
        if ((flags & TEXA) != 0u) {
            bool black = texel.r + texel.g + texel.b == 0.0;
            texel.a = (flags & AEM) != 0u && black ? 0.0 : float(alpha >> 24) / 128.0;
        }
        if ((flags & VERTEX_ALPHA) != 0u) texel.a = frag_color.a * MODULATE;
        else if ((flags & DECAL) == 0u) texel.a = texel.a * frag_color.a * MODULATE;
        if ((flags & DECAL) != 0u) color = texel;
        else color = vec4(texel.rgb * frag_color.rgb * MODULATE, texel.a);
    }
    if ((flags & FOG) != 0u) {
        vec3 fog = vec3(fog_color & 255u, (fog_color >> 8) & 255u, (fog_color >> 16) & 255u) / 255.0;
        color.rgb = mix(fog, color.rgb, frag_fog);
    }
    if ((flags & ALPHA_TEST) != 0u && !alpha_passes(color.a)) discard;
    if ((flags & OVERDRAW) != 0u) {
        result = vec4(0.1, 0.1, 0.1, 1.0);
        return;
    }
    float factor = (flags & FACTOR_FIX) != 0u ? float((alpha >> 16) & 255u) / 128.0 : color.a;
    vec3 rgb = color.rgb;
    if ((flags & SCALE_SOURCE) != 0u) rgb = rgb * (1.0 + factor);
    if ((flags & FACTOR_COLOR) != 0u) rgb = vec3(factor);
    result = vec4(rgb, color.a);
}
