#version 330 core
#include <sage/common.glsl>
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSource;
uniform float uTime;
void main() {
    vec2 uv = vUV + vec2(sin(vUV.y * 30.0 + uTime * 2.0), cos(vUV.x * 24.0 + uTime * 1.7)) * 0.003;
    vec3 c = texture(uSource, uv).rgb;
    FragColor = vec4(c * vec3(0.45, 0.7, 1.0), 1.0);
}
