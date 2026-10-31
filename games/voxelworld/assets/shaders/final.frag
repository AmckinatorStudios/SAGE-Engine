#version 330 core
#include <sage/common.glsl>
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uSource;
uniform float uExposure;
uniform float uVignette;
void main() {
    vec3 c = texture(uSource, vUV).rgb * uExposure;
    c = sage_aces(c);
    vec2 d = vUV - 0.5;
    c *= 1.0 - uVignette * dot(d, d) * 1.6;
    FragColor = vec4(sage_linear_to_srgb(c), 1.0);
}
