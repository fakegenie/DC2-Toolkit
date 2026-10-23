#version 450

layout(location = 0) in vec2 frag_local;
layout(location = 1) flat in vec2 frag_half;
layout(location = 2) in vec4 frag_color;
layout(location = 3) flat in vec4 frag_shape;
layout(location = 4) in vec2 frag_uv;

layout(set = 2, binding = 0) uniform sampler2D atlas;

layout(location = 0) out vec4 out_color;

float rounded_box(vec2 p, vec2 half_size, float radius) {
    vec2 q = abs(p) - half_size + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float segment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a;
    vec2 ba = b - a;
    return length(pa - ba * clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0));
}

float triangle(vec2 p, float radius) {
    const float k = sqrt(3.0);
    p = vec2(abs(p.x) - radius, -p.y + radius / k);
    if (p.x + k * p.y > 0.0) p = vec2(p.x - k * p.y, -k * p.x - p.y) / 2.0;
    p.x -= clamp(p.x, -2.0 * radius, 0.0);
    return -length(p) * sign(p.y);
}

float centred_triangle(vec2 p, float size, float corner) {
    float r = size * 0.8 - corner;
    return triangle(p - vec2(0.0, 0.289 * r), r) - corner;
}

float dpad_arm(vec2 p, vec2 half_size, float corner) {
    float x = abs(p.x);
    float y = half_size.y - p.y;
    float w = half_size.x - corner;
    float l = 2.0 * half_size.y - corner;
    return max(max(x - w, y - l), (x - y) * 0.7071 + corner) - corner;
}

void main() {
    int kind = int(frag_shape.x + 0.5);
    float stroke = frag_shape.y;
    float radius = frag_shape.z;
    vec2 p = frag_local;
    if (kind == 0) {
        out_color = vec4(frag_color.rgb, frag_color.a * texture(atlas, frag_uv).a);
        return;
    }
    if (kind == 8) {
        float fall = clamp(1.0 - length(p) / frag_half.x, 0.0, 1.0);
        out_color = vec4(frag_color.rgb, frag_color.a * fall * fall);
        return;
    }
    float d;
    if (kind == 1) d = length(p) - frag_half.x;
    else if (kind == 2) d = abs(length(p) - frag_half.x + stroke / 2.0) - stroke / 2.0;
    else if (kind == 3) d = rounded_box(p, frag_half, radius);
    else if (kind == 4) d = abs(rounded_box(p, frag_half - stroke / 2.0, max(radius - stroke / 2.0, 0.0))) - stroke / 2.0;
    else if (kind == 5) d = abs(centred_triangle(p, frag_half.x, stroke * 0.5)) - stroke / 2.0;
    else if (kind == 6) {
        float s = frag_half.x * 0.75;
        d = min(segment(p, vec2(-s), vec2(s)), segment(p, vec2(-s, s), vec2(s, -s))) - stroke / 2.0;
    } else if (kind == 9) {
        d = dpad_arm(p, frag_half, radius);
        if (stroke > 0.0) d = abs(d + stroke / 2.0) - stroke / 2.0;
    } else d = centred_triangle(p, frag_half.x, frag_half.x * 0.12);
    float edge = max(fwidth(d), 0.0001);
    out_color = vec4(frag_color.rgb, frag_color.a * clamp(0.5 - d / edge, 0.0, 1.0));
}
