#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform vec4 uTint; // множитель цвета/альфы поверх текстуры (для затухания, подсветки и т.п.)
uniform bool uUseTexture; // false — сплошной цвет uTint (например маркер-точка без текстуры)

void main() {
    vec4 texel = uUseTexture ? texture(uTexture, vUV) : vec4(1.0);
    vec4 color = texel * uTint;
    if (color.a <= 0.01) discard;
    FragColor = color;
}
