#version 450

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 local;
layout(location = 2) in vec2 half_size;
layout(location = 3) in vec4 color;
layout(location = 4) in vec4 shape;
layout(location = 5) in vec2 uv;

layout(set = 1, binding = 0) uniform Screen {
    vec4 scale;
};

layout(location = 0) out vec2 frag_local;
layout(location = 1) flat out vec2 frag_half;
layout(location = 2) out vec4 frag_color;
layout(location = 3) flat out vec4 frag_shape;
layout(location = 4) out vec2 frag_uv;

void main() {
    gl_Position = vec4(position.x * scale.x - 1.0, 1.0 - position.y * scale.y, 0.0, 1.0);
    frag_local = local;
    frag_half = half_size;
    frag_color = color;
    frag_shape = shape;
    frag_uv = uv;
}
