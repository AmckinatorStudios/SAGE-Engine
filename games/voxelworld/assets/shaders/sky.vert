#version 330 core
#include <sage/common.glsl>
out vec2 vNdc;
void main() {
    vec4 p = sage_fullscreen_pos(gl_VertexID);
    vNdc = p.xy;
    gl_Position = vec4(p.xy, 1.0, 1.0);
}
