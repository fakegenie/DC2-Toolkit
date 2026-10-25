#version 450

layout(location = 0) in vec4 frag_color;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) in float frag_fog;
layout(location = 3) flat in vec4 frag_limits;
layout(location = 4) flat in vec4 frag_fog_curve;

layout(set = 2, binding = 0) uniform sampler2D source;
layout(set = 2, binding = 1) uniform sampler2D destination;

layout(set = 3, binding = 0) uniform State {
    uint flags;
    uint alpha_reference;
    uint alpha_method;
    uint blend_fix;
    uint texture_alpha;
    uint fog_color;
    uint tint;
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
const uint DEST_TEST = 8192u;
const uint DEST_SET = 16384u;
const uint TINT = 32768u;

const uint ALPHA_NEVER = 0u;
const uint ALPHA_ALWAYS = 1u;
const uint ALPHA_LESS = 2u;
const uint ALPHA_LEQUAL = 3u;
const uint ALPHA_EQUAL = 4u;
const uint ALPHA_GEQUAL = 5u;
const uint ALPHA_GREATER = 6u;

const float MODULATE = 255.0 / 128.0;

bool alpha_passes(float value) {
    int level = int(value * 128.0 + 0.5);
    int reference = int(alpha_reference);
    if (alpha_method == ALPHA_NEVER) return false;
    if (alpha_method == ALPHA_ALWAYS) return true;
    if (alpha_method == ALPHA_LESS) return level < reference;
    if (alpha_method == ALPHA_LEQUAL) return level <= reference;
    if (alpha_method == ALPHA_EQUAL) return level == reference;
    if (alpha_method == ALPHA_GEQUAL) return level >= reference;
    if (alpha_method == ALPHA_GREATER) return level > reference;
    return level != reference;
}

float fog_amount() {
    if (frag_fog_curve == vec4(0.0)) return frag_fog;
    float fogged = frag_fog_curve.x + frag_fog_curve.w * gl_FragCoord.w;
    return floor(max(max(min(fogged, frag_fog_curve.y), frag_fog_curve.z), 0.0)) / 255.0;
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
    if ((flags & DEST_TEST) != 0u) {
        bool set = texelFetch(destination, ivec2(gl_FragCoord.xy), 0).a > 254.0 / 255.0;
        if (set != ((flags & DEST_SET) != 0u)) discard;
    }
    vec4 color = vec4(frag_color.rgb, frag_color.a * MODULATE);
    if ((flags & TEXTURED) != 0u) {
        vec2 uv = frag_limits == vec4(0.0) ? frag_uv : clamp(frag_uv, frag_limits.xy, frag_limits.zw);
        vec4 texel = texture(source, uv);
        if ((flags & TEXA) != 0u) {
            bool black = texel.r + texel.g + texel.b == 0.0;
            texel.a = (flags & AEM) != 0u && black ? 0.0 : float(texture_alpha) / 128.0;
        }
        if ((flags & VERTEX_ALPHA) != 0u) texel.a = frag_color.a * MODULATE;
        else if ((flags & DECAL) == 0u) texel.a = texel.a * frag_color.a * MODULATE;
        if ((flags & DECAL) != 0u) color = texel;
        else color = vec4(texel.rgb * frag_color.rgb * MODULATE, texel.a);
    }
    if ((flags & FOG) != 0u) {
        vec3 fog = vec3(fog_color & 255u, (fog_color >> 8) & 255u, (fog_color >> 16) & 255u) / 255.0;
        color.rgb = mix(fog, color.rgb, fog_amount());
    }
    if ((flags & ALPHA_TEST) != 0u && !alpha_passes(color.a)) discard;
    if ((flags & TINT) != 0u) color *= vec4(tint & 255u, (tint >> 8) & 255u, (tint >> 16) & 255u, tint >> 24) / 128.0;
    if ((flags & OVERDRAW) != 0u) {
        result = vec4(0.1, 0.1, 0.1, 1.0);
        return;
    }
    float factor = (flags & FACTOR_FIX) != 0u ? float(blend_fix) / 128.0 : color.a;
    vec3 rgb = color.rgb;
    if ((flags & SCALE_SOURCE) != 0u) rgb = rgb * (1.0 + factor);
    if ((flags & FACTOR_COLOR) != 0u) rgb = vec3(factor);
    result = vec4(rgb, color.a);
}
