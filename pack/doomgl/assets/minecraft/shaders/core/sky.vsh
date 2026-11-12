#version 330
#extension GL_ARB_separate_shader_objects : require

#include <minecraft:fog.glsl>
#include <minecraft:dynamictransforms.glsl>
#include <minecraft:projection.glsl>

layout(location = 0) in vec3 Position;

layout(location = 0) out float sphericalVertexDistance;
layout(location = 1) out float cylindricalVertexDistance;
layout(location = 2) flat out vec3 rotCol0;
layout(location = 3) flat out vec3 rotCol1;
layout(location = 4) flat out vec3 rotCol2;
layout(location = 5) flat out vec3 trans;
layout(location = 6) flat out vec2 proj;

// Doom beacon: the sky is the first thing drawn each frame and its model view matrix is the camera rotation.
// Squeeze all of its geometry into a thin strip in the bottom left corner of the screen and let the
// fragment shader write the matrix into those pixels as colours. A post effect reads them back.
void main() {
    vec2 corner[4] = vec2[](vec2(-1.0, -0.52), vec2(-0.6, -0.52), vec2(-0.6, -0.48), vec2(-1.0, -0.48));   // left edge, a quarter up: clear of the data cells at the top and bottom
    vec2 c = corner[gl_VertexIndex & 3];
    gl_Position = vec4(c, 0.0, 1.0);
    rotCol0 = ModelViewMat[0].xyz;
    rotCol1 = ModelViewMat[1].xyz;
    rotCol2 = ModelViewMat[2].xyz;
    trans = ModelViewMat[3].xyz;
    proj = vec2(ProjMat[0][0], ProjMat[1][1]);
    sphericalVertexDistance = fog_spherical_distance(Position);
    cylindricalVertexDistance = fog_cylindrical_distance(Position);
}
