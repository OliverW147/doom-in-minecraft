#version 330
#extension GL_ARB_separate_shader_objects : require

// Pass 1: copy the view rotation written by the sky shader into a 16x1 target; when the sky strip was
// covered by something this frame (marker missing) keep the last good values from the previous frame.
uniform sampler2D InSampler;
uniform sampler2D PrevSampler;

layout(std140) uniform SamplerInfo {
    vec2 OutSize;
    vec2 InSize;
};

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

void main() {
    int i = int(gl_FragCoord.x);
    uvec3 mk = uvec3(texelFetch(InSampler, ivec2(11, int(InSize.y * 0.25)), 0).rgb * 255.0 + 0.5) >> 4u;
    if (mk == uvec3(10u, 5u, 12u)) fragColor = vec4(texelFetch(InSampler, ivec2(i, int(InSize.y * 0.25)), 0).rgb, 1.0);
    else fragColor = vec4(texelFetch(PrevSampler, ivec2(i, 0), 0).rgb, 1.0);
}
