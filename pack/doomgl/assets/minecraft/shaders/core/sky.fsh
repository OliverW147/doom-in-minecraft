#version 330
#extension GL_ARB_separate_shader_objects : require

#include <minecraft:fog.glsl>
#include <minecraft:dynamictransforms.glsl>

layout(location = 0) in float sphericalVertexDistance;
layout(location = 1) in float cylindricalVertexDistance;
layout(location = 2) flat in vec3 rotCol0;
layout(location = 3) flat in vec3 rotCol1;
layout(location = 4) flat in vec3 rotCol2;
layout(location = 5) flat in vec3 trans;
layout(location = 6) flat in vec2 proj;

layout(location = 0) out vec4 fragColor;

// -1..1 as a 12 bit number in three nibbles; every channel byte sits in the middle of a 16 step bucket,
// so small additive noise from later draws does not change what the post shader reads back
vec3 enc(float v) {
    uint u = uint(clamp(v * 0.5 + 0.5, 0.0, 1.0) * 4095.0 + 0.5);
    return (vec3(float((u >> 8) & 15u), float((u >> 4) & 15u), float(u & 15u)) * 16.0 + 8.0) / 255.0;
}

void main() {
    int i = int(gl_FragCoord.x);
    float v = 0.0;
    if (i < 3) v = rotCol0[i];
    else if (i < 6) v = rotCol1[i - 3];
    else if (i < 9) v = rotCol2[i - 6];
    else if (i < 11) v = proj[i - 9] * 0.125;
    if (i == 11) fragColor = vec4(vec3(10.0 * 16.0 + 8.0, 5.0 * 16.0 + 8.0, 12.0 * 16.0 + 8.0) / 255.0, 1.0);   // marker: nibbles A 5 C
    else fragColor = vec4(enc(v), 1.0);
}
