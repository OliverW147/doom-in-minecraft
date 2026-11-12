#version 330
#extension GL_ARB_separate_shader_objects : require

#include <minecraft:light.glsl>
#include <minecraft:fog.glsl>
#include <minecraft:dynamictransforms.glsl>
#include <minecraft:projection.glsl>
#include <minecraft:sample_lightmap.glsl>
#include <minecraft:doomdata.glsl>

layout(location = 0) in vec3 Position;
layout(location = 1) in vec4 Color;
layout(location = 2) in vec2 UV0;
layout(location = 3) in ivec2 UV1;
layout(location = 4) in ivec2 UV2;
#ifdef GLINT_SPECIAL
layout(location = 5) in vec2 UV3;
#endif
layout(location = 6) in vec3 Normal;

uniform sampler2D Sampler0;
#ifndef OIT_ALPHA_ONLY
uniform sampler2D Sampler1;
uniform sampler2D Sampler2;

layout(location = 0) out float sphericalVertexDistance;
layout(location = 1) out float cylindricalVertexDistance;
#endif
layout(location = 2) out vec4 vertexColor;
#ifndef OIT_ALPHA_ONLY
layout(location = 3) out vec4 lightMapColor;
layout(location = 4) out vec4 overlayColor;
#endif

layout(location = 5) out vec2 texCoord0;
#ifdef GLINT
layout(location = 6) out vec2 texCoordGlint;
#endif
layout(location = 7) flat out vec4 dataColor;   // alpha 1: this quad carries a number for the Doom shader

void main() {
    dataColor = vec4(0.0);
    vec4 clip = ProjMat * ModelViewMat * vec4(Position, 1.0);

    // Doom data quads (see tools/bake_data_items.py): the texel the quad points at is (cell & 255, 0x50 | cell >> 8, 0xC3, 255).
    // Pin the quad to its cell (copy A at the top of the screen, copy B at the bottom); the fragment shader writes the
    // tint colour (the data) there.
    vec4 marker = texelFetch(Sampler0, ivec2(UV0 * vec2(textureSize(Sampler0, 0))), 0);
    float g255 = marker.g * 255.0;
    if (g255 > 79.5 && g255 < 83.5 && abs(marker.b * 255.0 - 195.0) < 0.5 && marker.a > 0.99) {
        int cell = int(marker.r * 255.0 + 0.5) + 256 * (int(marker.g * 255.0 + 0.5) - 80);
        bool second = cell >= DATA_WORDS;
        int local = second ? cell - DATA_WORDS : cell;
        int k = gl_VertexIndex & 3;                                   // corner of the quad: 0 1 2 3 counter clockwise
        float cx = (k == 1 || k == 2) ? 1.0 : 0.0;
        float cy = (k == 2 || k == 3) ? 1.0 : 0.0;
        float w = 2.0 / float(DATA_COLS);
        float x0 = -1.0 + w * float(DATA_COL0 + local % DATA_PERROW);   // the leftmost columns stay free: mods draw a minimap and icons there
        float rowTop = float(local / DATA_PERROW);
        float yTop = second ? (-1.0 + DATA_ROW_H * (rowTop + 1.0)) : (1.0 - DATA_ROW_H * rowTop);
        clip = vec4(x0 + cx * w, yTop - (1.0 - cy) * DATA_ROW_H, 1.0, 1.0);
        dataColor = vec4(Color.rgb, 1.0);
    }
    gl_Position = clip;

    #ifndef OIT_ALPHA_ONLY
    sphericalVertexDistance = fog_spherical_distance(Position);
    cylindricalVertexDistance = fog_cylindrical_distance(Position);
    #endif
    vertexColor = minecraft_mix_light(Light0_Direction, Light1_Direction, Normal, Color);
    #ifndef OIT_ALPHA_ONLY
    lightMapColor = sample_lightmap(Sampler2, UV2);
    overlayColor = texelFetch(Sampler1, UV1, 0);
    #endif

    texCoord0 = UV0;
    #ifdef GLINT
    #ifdef GLINT_SPECIAL
    texCoordGlint = (TextureMat * vec4(UV3, 0.0, 1.0)).xy;
    #else
    texCoordGlint = (TextureMat * vec4(UV0, 0.0, 1.0)).xy;
    #endif
    #endif
}
