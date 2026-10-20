#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(location = 3) in float fog;
layout(location = 4) in vec4 limits;

layout(set = 1, binding = 0) uniform Target {
    vec4 scale;
};

layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec2 frag_uv;
layout(location = 2) out float frag_fog;
layout(location = 3) flat out vec4 frag_limits;

void main() {
    gl_Position = vec4(position.x * scale.x - 1.0, 1.0 - position.y * scale.y, position.z, 1.0);
    frag_color = color;
    frag_uv = uv;
    frag_fog = fog;
    frag_limits = limits;
}
