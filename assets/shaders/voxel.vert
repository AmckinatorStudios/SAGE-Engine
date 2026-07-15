#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec3 aColor;
layout (location = 3) in vec2 aTexCoords;

out vec3 FragPos;
out vec3 Normal;
out vec3 VertexColor;
out vec2 TexCoords;
out vec4 FragPosLightSpace; // позиция фрагмента в пространстве света (для теней)

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightSpace; // проекция * вид из точки зрения солнца (карта теней)

void main() {
    FragPos = vec3(uModel * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(uModel))) * aNormal;
    VertexColor = aColor;
    TexCoords = aTexCoords;
    FragPosLightSpace = uLightSpace * vec4(FragPos, 1.0);
    gl_Position = uProjection * uView * vec4(FragPos, 1.0);
}
