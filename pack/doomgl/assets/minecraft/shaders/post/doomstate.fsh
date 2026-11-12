#version 330
#extension GL_ARB_separate_shader_objects : require
#include <minecraft:doomdata.glsl>

// Reads the data words the item shader drew into the screen (copy A at the top, copy B at the bottom) into a state texture,
// one 16 bit word per pixel (red = high byte, green = low byte). A copy counts when its check byte matches
// (blue = red xor green xor 0x5C); if neither copy does, the previous value is kept.
uniform sampler2D InSampler;
uniform sampler2D PrevSampler;

layout(std140) uniform SamplerInfo {
    vec2 OutSize;
    vec2 InSize;
};

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragColor;

ivec3 cell(int k, bool second) {
    int col = DATA_COL0 + k % DATA_PERROW, row = k / DATA_PERROW;
    float fromEdge = (float(row) + 0.5) * DATA_ROW_H * 0.5;            // fraction of the screen height
    float y = second ? InSize.y * fromEdge : InSize.y * (1.0 - fromEdge);
    ivec2 p = ivec2(int((float(col) + 0.5) / float(DATA_COLS) * InSize.x), int(y));
    return ivec3(texelFetch(InSampler, p, 0).rgb * 255.0 + 0.5);
}

bool valid(ivec3 c) { return c.b == ((c.r ^ c.g) ^ 0x5C); }

void main() {
    int i = int(gl_FragCoord.x);
    ivec3 a = cell(i, false), b = cell(i, true);
    if (valid(a)) fragColor = vec4(vec2(a.rg) / 255.0, 0.0, 1.0);
    else if (valid(b)) fragColor = vec4(vec2(b.rg) / 255.0, 0.0, 1.0);
    else fragColor = vec4(texelFetch(PrevSampler, ivec2(i, 0), 0).rgb, 1.0);
}
