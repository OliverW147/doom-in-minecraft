#version 330
#extension GL_ARB_separate_shader_objects : require

uniform sampler2D DoomSampler;

layout(std140) uniform SamplerInfo {
    vec2 OutSize;
    vec2 InSize;
};

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

// Shows the 4:3 Doom picture in the middle of the window, black bars around it.
void main() {
    float aspect = OutSize.x / OutSize.y;
    vec2 uv = texCoord - 0.5;
    if (aspect > 4.0 / 3.0) uv.x *= aspect / (4.0 / 3.0); else uv.y *= (4.0 / 3.0) / aspect;
    uv += 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    fragColor = vec4(texture(DoomSampler, uv).rgb, 1.0);
}
