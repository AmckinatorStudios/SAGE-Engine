// tinygltf-реализация развёрнута в Model.cpp — здесь только объявления.
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "SkinnedModel.h"
#include "sage/gi/GIUpload.h"

#include <ufbx.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include <stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "sage/core/Log.h"
#include "sage/render/PbrShader.h"
#include "sage/render/Shader.h"
#include "sage/render/LightingUpload.h"
#include "sage/rhi/GraphicsDevice.h"
#include "sage/scene/Light.h"

#include <string>

using namespace sage::rhi;
using namespace sage::anim;

namespace sage::render {

// ============================================================================
//  SkinnedMesh (GPU-геометрия)
// ============================================================================
SkinnedMesh::SkinnedMesh(const std::vector<SkinnedVertex>& vertices,
                         const std::vector<unsigned int>& indices) {
    m_indexCount = indices.size();
    VertexLayout layout;
    layout.Stride = sizeof(SkinnedVertex);
    layout.Attributes = {
        {0, 3, AttribType::Float, (int)offsetof(SkinnedVertex, Position)},
        {1, 3, AttribType::Float, (int)offsetof(SkinnedVertex, Normal)},
        {2, 2, AttribType::Float, (int)offsetof(SkinnedVertex, TexCoords)},
        {3, 4, AttribType::Float, (int)offsetof(SkinnedVertex, Joints)},
        {4, 4, AttribType::Float, (int)offsetof(SkinnedVertex, Weights)},
    };
    m_geometry = GraphicsDevice::Get().CreateGeometry(layout);
    m_geometry->SetVertexData(vertices.data(), vertices.size() * sizeof(SkinnedVertex), false);
    m_geometry->SetIndexData(indices.data(), indices.size(), false);
}

void SkinnedMesh::Draw() const { m_geometry->DrawIndexed(m_indexCount); }

// ============================================================================
//  Встроенный скиннинг-шейдер. Фрагментная стадия использует ОБЩИЙ PBR-блок
//  kPbrSharedGlsl (Cook-Torrance, metallic-roughness) — тот же, что у статических
//  инстансных/текстурных мешей, поэтому анимированные и статичные объекты
//  освещаются ФИЗИЧЕСКИ ОДИНАКОВО (единый источник модели освещения). Отличие
//  только в вершинной стадии: скиннинг палитрой костей. Uniform'ы освещения
//  названы как в UploadLighting/UploadShadowUniforms и заливаются без изменений.
// ============================================================================
namespace {

const char* kSkinVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec4 aJoints;
layout (location = 4) in vec4 aWeights;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoords;
out vec4 FragPosLightSpace;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightSpace;
const int MAX_BONES = 128;
uniform mat4 uBones[MAX_BONES];
uniform int uSkinned; // 0 — bind-поза (без палитры)

void main() {
    mat4 skin;
    float wsum = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
    if (uSkinned == 0 || wsum < 0.0001) {
        skin = mat4(1.0);
    } else {
        skin = aWeights.x * uBones[int(aJoints.x)]
             + aWeights.y * uBones[int(aJoints.y)]
             + aWeights.z * uBones[int(aJoints.z)]
             + aWeights.w * uBones[int(aJoints.w)];
    }
    vec4 skinnedPos = skin * vec4(aPos, 1.0);
    vec3 skinnedNormal = mat3(skin) * aNormal;

    vec4 worldPos = uModel * skinnedPos;
    FragPos = worldPos.xyz;
    // Обратно-транспонированная нормальная матрица — верные нормали при
    // неравномерном/отрицательном масштабе модели.
    Normal = transpose(inverse(mat3(uModel))) * skinnedNormal;
    TexCoords = aUV;
    FragPosLightSpace = uLightSpace * worldPos;
    gl_Position = uProjection * uView * worldPos;
}
)";

// Фрагментный шейдер: тот же общий PBR-блок (kPbrSharedGlsl), что у статических
// мешей — albedo из tint (× опциональная текстура), metallic/roughness из
// материала. Так скин и статика физически неразличимы по освещению.
std::string SkinFragSource() {
    return std::string(R"(#version 330 core
in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;
in vec4 FragPosLightSpace;
out vec4 FragColor;

uniform vec3 uObjectColor;
uniform sampler2D uTexture;
uniform bool uUseTexture;
uniform float uMetallic;
uniform float uRoughness;
)") + sage::render::kPbrSharedGlsl + R"(
void main() {
    vec3 albedo = uUseTexture ? texture(uTexture, TexCoords).rgb * uObjectColor : uObjectColor;
    vec3 N = normalize(Normal);
    if (uShadingMode == 2) { FragColor = vec4(N * 0.5 + 0.5, 1.0); return; }
    if (uShadingMode == 1) { FragColor = vec4(albedo, 1.0); return; }
    FragColor = vec4(ShadePBR(N, FragPos, FragPosLightSpace, albedo, uMetallic, uRoughness), 1.0);
}
)";
}

Shader& SkinShader() {
    // Намеренно НЕ уничтожаем: function-local static с деструктором Shader снёс
    // бы GL-программу при выходе из процесса — уже ПОСЛЕ разрушения GL-контекста
    // (segfault в glDeleteProgram). Утечка одной программы на выходе безвредна
    // (ОС всё освободит), зато нет обращения к мёртвому контексту.
    static Shader* shader = new Shader(Shader::FromSource(kSkinVert, SkinFragSource(), "SkinnedModel"));
    return *shader;
}

// Depth-only скиннинг-шейдер для карты теней: скиннинг в вершинной стадии,
// пустой фрагмент (пишется только глубина). Позиция/кости в тех же локациях,
// что и основной скиннинг-шейдер (0/3/4).
const char* kSkinDepthVert = R"(#version 330 core
layout (location = 0) in vec3 aPos;
layout (location = 3) in vec4 aJoints;
layout (location = 4) in vec4 aWeights;

uniform mat4 uLightSpace;
uniform mat4 uModel;
const int MAX_BONES = 128;
uniform mat4 uBones[MAX_BONES];
uniform int uSkinned;

void main() {
    mat4 skin;
    float wsum = aWeights.x + aWeights.y + aWeights.z + aWeights.w;
    if (uSkinned == 0 || wsum < 0.0001) {
        skin = mat4(1.0);
    } else {
        skin = aWeights.x * uBones[int(aJoints.x)]
             + aWeights.y * uBones[int(aJoints.y)]
             + aWeights.z * uBones[int(aJoints.z)]
             + aWeights.w * uBones[int(aJoints.w)];
    }
    gl_Position = uLightSpace * uModel * skin * vec4(aPos, 1.0);
}
)";

const char* kSkinDepthFrag = R"(#version 330 core
void main() {}
)";

Shader& SkinDepthShader() {
    static Shader* shader = new Shader(Shader::FromSource(kSkinDepthVert, kSkinDepthFrag, "SkinnedModelDepth"));
    return *shader;
}

} // namespace

void SkinnedModel::Draw(const glm::mat4& model, const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& viewPos, const LightingEnvironment& env,
                        const std::vector<glm::mat4>& bones,
                        const glm::mat4& lightMatrix, unsigned int shadowMap,
                        bool shadowsEnabled) const {
    Shader& shader = SkinShader();
    shader.Use();
    shader.SetMat4("uModel", model);
    shader.SetMat4("uView", view);
    shader.SetMat4("uProjection", proj);
    shader.SetVec3("uViewPos", viewPos);

    // Полное освещение сцены (ambient из скайбокса, солнце, точечные, прожекторы,
    // туман) — теми же uniform'ами, что и статический lit-проход.
    UploadLighting(shader, env);

    // GI: юниты сэмплеров общего PBR-блока обязательны всегда (конфликт типов
    // сэмплеров на юните 0 — см. SetGISamplerUnits). Объём проб к скиннингу
    // пока не подключён (сюда не проброшена сцена) — непрямой свет остаётся
    // полусферическим ambient.
    sage::gi::SetGISamplerUnits(shader);
    shader.SetInt("uGIVolumeEnabled", 0);

    // Тени от солнца: карта на юнит 1, матрица света + флаг — как у статики.
    if (shadowsEnabled && shadowMap) {
        sage::rhi::GraphicsDevice::Get().BindTexture2D(1, shadowMap);
    }
    UploadShadowUniforms(shader, lightMatrix, /*unit=*/1, shadowsEnabled);

    int boneCount = std::min((int)bones.size(), kMaxBones);
    if (boneCount > 0) {
        shader.SetInt("uSkinned", 1);
        shader.SetMat4Array("uBones", bones.data(), boneCount);
    } else {
        shader.SetInt("uSkinned", 0);
    }

    for (const auto& sub : m_subMeshes) {
        shader.SetVec3("uObjectColor", sub.Tint);
        shader.SetFloat("uMetallic", sub.Metallic);
        shader.SetFloat("uRoughness", sub.Roughness);
        if (sub.Diffuse) {
            shader.SetInt("uUseTexture", 1);
            shader.SetInt("uTexture", 0);
            sub.Diffuse->Bind(0);
        } else {
            shader.SetInt("uUseTexture", 0);
        }
        sub.Mesh->Draw();
    }
}

void SkinnedModel::DrawDepth(const glm::mat4& model, const glm::mat4& lightMatrix,
                             const std::vector<glm::mat4>& bones) const {
    Shader& shader = SkinDepthShader();
    shader.Use();
    shader.SetMat4("uLightSpace", lightMatrix);
    shader.SetMat4("uModel", model);

    int boneCount = std::min((int)bones.size(), kMaxBones);
    if (boneCount > 0) {
        shader.SetInt("uSkinned", 1);
        shader.SetMat4Array("uBones", bones.data(), boneCount);
    } else {
        shader.SetInt("uSkinned", 0);
    }

    for (const auto& sub : m_subMeshes) sub.Mesh->Draw();
}

// ============================================================================
//  Процедурная демонстрация: щупалец из segments костей вдоль +Y со встроенным
//  клипом «Wave» (бегущая волна изгиба). Показывает весь пайплайн без ассетов.
// ============================================================================
std::unique_ptr<SkinnedModel> SkinnedModel::CreateDemoTentacle(int segments) {
    segments = std::max(2, segments);
    auto m = std::unique_ptr<SkinnedModel>(new SkinnedModel());

    const float segLen = 0.6f; // длина сегмента
    const float halfW = 0.18f; // полутолщина квадратного сечения

    // --- скелет: цепочка костей вдоль +Y, каждая на segLen выше предыдущей ---
    Skeleton& sk = m->m_skeleton;
    sk.Joints.resize(segments);
    for (int i = 0; i < segments; ++i) {
        Joint& j = sk.Joints[i];
        j.Name = "bone" + std::to_string(i);
        j.Parent = i - 1;                        // 0 — корень
        j.Translation = {0.0f, i == 0 ? 0.0f : segLen, 0.0f}; // локально над родителем
        j.Rotation = glm::quat(1, 0, 0, 0);
        j.Scale = glm::vec3(1.0f);
        // inverseBind = обратная глобальная bind-матрица кости (кость i на высоте i*segLen).
        glm::mat4 bind = glm::translate(glm::mat4(1.0f), {0.0f, i * segLen, 0.0f});
        j.InverseBind = glm::inverse(bind);
    }

    // --- геометрия: коробчатый столб, вершины привязаны к ближайшей кости ---
    std::vector<SkinnedVertex> verts;
    std::vector<unsigned int> idx;
    auto ring = [&](float y, int bone) {
        // 4 вершины квадратного сечения на высоте y, полностью привязаны к bone.
        glm::vec2 corners[4] = {{-halfW, -halfW}, {halfW, -halfW}, {halfW, halfW}, {-halfW, halfW}};
        glm::vec3 normals[4] = {{0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}};
        for (int c = 0; c < 4; ++c) {
            SkinnedVertex v;
            v.Position = {corners[c].x, y, corners[c].y};
            v.Normal = normals[c];
            v.TexCoords = {(float)c / 4.0f, y};
            // Плавная привязка: вес делится между костью сегмента и соседней —
            // мягкий изгиб на стыках (настоящий скиннинг, не жёсткие сегменты).
            int b0 = glm::clamp(bone, 0, segments - 1);
            int b1 = glm::clamp(bone + 1, 0, segments - 1);
            v.Joints = {(float)b0, (float)b1, 0.0f, 0.0f};
            v.Weights = {0.7f, 0.3f, 0.0f, 0.0f};
            verts.push_back(v);
        }
    };
    int rings = segments; // кольцо в основании каждой кости + верхушка
    for (int i = 0; i <= rings; ++i) {
        float y = i * segLen;
        ring(y, std::min(i, segments - 1));
    }
    for (int i = 0; i < rings; ++i) {
        int a = i * 4, b = (i + 1) * 4;
        for (int c = 0; c < 4; ++c) {
            int c2 = (c + 1) % 4;
            idx.insert(idx.end(), {(unsigned)(a + c), (unsigned)(b + c), (unsigned)(b + c2)});
            idx.insert(idx.end(), {(unsigned)(a + c), (unsigned)(b + c2), (unsigned)(a + c2)});
        }
    }

    SkinnedSubMesh sub;
    sub.Mesh = std::make_shared<SkinnedMesh>(verts, idx);
    sub.Tint = {0.35f, 0.75f, 0.55f};
    m->m_subMeshes.push_back(std::move(sub));

    // --- клип «Wave»: каждая кость покачивается по Z с фазовым сдвигом ---
    AnimationClip clip;
    clip.Name = "Wave";
    clip.Duration = 2.0f;
    const int kKeys = 17;
    for (int b = 1; b < segments; ++b) { // корень (0) неподвижен
        AnimChannel ch;
        ch.Joint = b;
        ch.Target = AnimPath::Rotation;
        ch.Interp = AnimInterp::Linear;
        for (int k = 0; k < kKeys; ++k) {
            float t = clip.Duration * (float)k / (float)(kKeys - 1);
            float phase = (float)b * 0.7f;
            float angle = glm::radians(22.0f) * std::sin(t / clip.Duration * 6.2831853f + phase);
            glm::quat q = glm::angleAxis(angle, glm::vec3(0, 0, 1)); // изгиб вокруг Z
            ch.Times.push_back(t);
            ch.Values.push_back(glm::vec4(q.x, q.y, q.z, q.w));
        }
        clip.Channels.push_back(std::move(ch));
    }
    m->m_clips.push_back(std::move(clip));

    // --- клип «Curl»: щупалец собран в спираль (постоянный изгиб каждой кости) —
    //     ВТОРОЙ клип, чтобы демонстрировать/тестировать кросс-фейд между позами. ---
    AnimationClip curl;
    curl.Name = "Curl";
    curl.Duration = 1.0f;
    for (int b = 1; b < segments; ++b) {
        AnimChannel ch;
        ch.Joint = b;
        ch.Target = AnimPath::Rotation;
        ch.Interp = AnimInterp::Step;
        glm::quat q = glm::angleAxis(glm::radians(40.0f), glm::vec3(0, 0, 1)); // сильный изгиб
        ch.Times = {0.0f};
        ch.Values = {glm::vec4(q.x, q.y, q.z, q.w)};
        curl.Channels.push_back(std::move(ch));
    }
    m->m_clips.push_back(std::move(curl));

    LOG_INFO("Anim") << "SkinnedModel: процедурный щупалец (" << segments << " костей, "
                     << verts.size() << " вершин, клипы Wave/Curl)";
    return m;
}

// ============================================================================
//  Загрузка из glTF/GLB (skins + animations)
// ============================================================================
namespace {

bool GltfSkinImageLoader(tinygltf::Image* image, const int, std::string* err, std::string*,
                         int, int, const unsigned char* bytes, int size, void*) {
    int w, h, comp;
    unsigned char* data = stbi_load_from_memory(bytes, size, &w, &h, &comp, 4);
    if (!data) { if (err) *err += "Не удалось декодировать изображение glTF\n"; return false; }
    image->width = w; image->height = h; image->component = 4; image->bits = 8;
    image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image->image.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return true;
}

const unsigned char* AccessorBase(const tinygltf::Model& m, const tinygltf::Accessor& acc, size_t& stride) {
    const auto& view = m.bufferViews[acc.bufferView];
    const auto& buf = m.buffers[view.buffer];
    stride = acc.ByteStride(view);
    return buf.data.data() + view.byteOffset + acc.byteOffset;
}

std::vector<float> ReadFloats(const tinygltf::Model& m, int accessorIdx, int comps) {
    const auto& acc = m.accessors[accessorIdx];
    size_t stride; const unsigned char* base = AccessorBase(m, acc, stride);
    std::vector<float> out(acc.count * comps);
    for (size_t i = 0; i < acc.count; ++i) {
        const float* v = reinterpret_cast<const float*>(base + i * stride);
        for (int c = 0; c < comps; ++c) out[i * comps + c] = v[c];
    }
    return out;
}

// Индексы (для JOINTS_0 — ubyte/ushort, для indices — +uint).
std::vector<unsigned int> ReadUInts(const tinygltf::Model& m, int accessorIdx, int comps) {
    const auto& acc = m.accessors[accessorIdx];
    size_t stride; const unsigned char* base = AccessorBase(m, acc, stride);
    std::vector<unsigned int> out(acc.count * comps);
    for (size_t i = 0; i < acc.count; ++i) {
        const unsigned char* p = base + i * stride;
        for (int c = 0; c < comps; ++c) {
            switch (acc.componentType) {
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  out[i*comps+c] = ((const uint8_t*)p)[c]; break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: out[i*comps+c] = ((const uint16_t*)p)[c]; break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:   out[i*comps+c] = ((const uint32_t*)p)[c]; break;
                default: out[i*comps+c] = 0; break;
            }
        }
    }
    return out;
}

glm::mat4 NodeLocal(const tinygltf::Node& n) {
    if (n.matrix.size() == 16) {
        glm::mat4 m;
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) m[c][r] = (float)n.matrix[c*4+r];
        return m;
    }
    glm::mat4 t(1.0f), r(1.0f), s(1.0f);
    if (n.translation.size() == 3) t = glm::translate(glm::mat4(1.0f), glm::vec3(n.translation[0], n.translation[1], n.translation[2]));
    if (n.rotation.size() == 4) r = glm::mat4_cast(glm::quat((float)n.rotation[3], (float)n.rotation[0], (float)n.rotation[1], (float)n.rotation[2]));
    if (n.scale.size() == 3) s = glm::scale(glm::mat4(1.0f), glm::vec3(n.scale[0], n.scale[1], n.scale[2]));
    return t * r * s;
}

} // namespace

std::unique_ptr<SkinnedModel> SkinnedModel::Load(const std::string& path) {
    // Расширение → загрузчик. .fbx идёт через ufbx (скелет+скин+клипы), остальное
    // (.gltf/.glb) — через tinygltf.
    auto endsWith = [&](const char* suf) {
        size_t n = std::strlen(suf);
        if (path.size() < n) return false;
        std::string tail = path.substr(path.size() - n);
        for (auto& c : tail) c = (char)std::tolower((unsigned char)c);
        return tail == suf;
    };
    if (endsWith(".fbx")) return LoadFbx(path);
    return LoadGltf(path, endsWith(".glb"));
}

std::unique_ptr<SkinnedModel> SkinnedModel::LoadGltf(const std::string& path, bool binary) {
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(&GltfSkinImageLoader, nullptr);
    tinygltf::Model g;
    std::string err, warn;
    bool ok = binary ? loader.LoadBinaryFromFile(&g, &err, &warn, path)
                     : loader.LoadASCIIFromFile(&g, &err, &warn, path);
    if (!warn.empty()) LOG_WARN("Anim") << "glTF (" << path << "): " << warn;
    if (!ok) throw std::runtime_error("SkinnedModel: не загрузить glTF " + path + ": " + err);
    if (g.skins.empty()) throw std::runtime_error("SkinnedModel: в файле нет скина: " + path);

    auto model = std::unique_ptr<SkinnedModel>(new SkinnedModel());
    const tinygltf::Skin& skin = g.skins[0];
    int jointCount = (int)skin.joints.size();
    if (jointCount > kMaxBones) {
        LOG_WARN("Anim") << "SkinnedModel: костей " << jointCount << " > " << kMaxBones
                         << " — лишние не поместятся в палитру";
    }

    // node index -> индекс в нашем скелете (0..jointCount-1)
    std::unordered_map<int, int> nodeToJoint;
    for (int i = 0; i < jointCount; ++i) nodeToJoint[skin.joints[i]] = i;

    // Обратные bind-матрицы (по одной на кость).
    std::vector<glm::mat4> invBind(jointCount, glm::mat4(1.0f));
    if (skin.inverseBindMatrices >= 0) {
        std::vector<float> ibm = ReadFloats(g, skin.inverseBindMatrices, 16);
        for (int i = 0; i < jointCount && (i + 1) * 16 <= (int)ibm.size(); ++i)
            invBind[i] = glm::make_mat4(&ibm[i * 16]);
    }

    // Скелет: TRS из узлов + родитель из иерархии узлов.
    Skeleton& sk = model->m_skeleton;
    sk.Joints.resize(jointCount);
    for (int i = 0; i < jointCount; ++i) {
        const tinygltf::Node& n = g.nodes[skin.joints[i]];
        Joint& j = sk.Joints[i];
        j.Name = n.name;
        j.InverseBind = invBind[i];
        j.Translation = n.translation.size() == 3 ? glm::vec3(n.translation[0], n.translation[1], n.translation[2]) : glm::vec3(0.0f);
        j.Rotation = n.rotation.size() == 4 ? glm::quat((float)n.rotation[3], (float)n.rotation[0], (float)n.rotation[1], (float)n.rotation[2]) : glm::quat(1, 0, 0, 0);
        j.Scale = n.scale.size() == 3 ? glm::vec3(n.scale[0], n.scale[1], n.scale[2]) : glm::vec3(1.0f);
        j.Parent = -1;
    }
    // Родитель: у кого этот узел в children.
    for (int ni = 0; ni < (int)g.nodes.size(); ++ni) {
        for (int child : g.nodes[ni].children) {
            auto itC = nodeToJoint.find(child);
            auto itP = nodeToJoint.find(ni);
            if (itC != nodeToJoint.end() && itP != nodeToJoint.end())
                sk.Joints[itC->second].Parent = itP->second;
        }
    }

    // Меши со скином: собираем все примитивы, у которых есть JOINTS_0/WEIGHTS_0.
    std::unordered_map<int, std::shared_ptr<Texture>> texCache;
    auto loadTex = [&](int texIndex) -> std::shared_ptr<Texture> {
        if (texIndex < 0 || texIndex >= (int)g.textures.size()) return nullptr;
        int img = g.textures[texIndex].source;
        auto it = texCache.find(img);
        if (it != texCache.end()) return it->second;
        if (img < 0 || img >= (int)g.images.size() || g.images[img].image.empty()) return nullptr;
        auto t = std::make_shared<Texture>(g.images[img].image.data(), g.images[img].width,
                                           g.images[img].height, TextureFilter::Trilinear, true);
        texCache[img] = t;
        return t;
    };

    for (const auto& mesh : g.meshes) {
        for (const auto& prim : mesh.primitives) {
            if (prim.mode != TINYGLTF_MODE_TRIANGLES) continue;
            auto posIt = prim.attributes.find("POSITION");
            auto jIt = prim.attributes.find("JOINTS_0");
            auto wIt = prim.attributes.find("WEIGHTS_0");
            if (posIt == prim.attributes.end() || jIt == prim.attributes.end() || wIt == prim.attributes.end())
                continue; // не скиновый примитив

            std::vector<float> pos = ReadFloats(g, posIt->second, 3);
            auto nIt = prim.attributes.find("NORMAL");
            std::vector<float> nrm = nIt != prim.attributes.end() ? ReadFloats(g, nIt->second, 3) : std::vector<float>();
            auto uvIt = prim.attributes.find("TEXCOORD_0");
            std::vector<float> uv = uvIt != prim.attributes.end() ? ReadFloats(g, uvIt->second, 2) : std::vector<float>();
            std::vector<unsigned int> joints = ReadUInts(g, jIt->second, 4);
            std::vector<float> weights = ReadFloats(g, wIt->second, 4);

            size_t vc = pos.size() / 3;
            std::vector<SkinnedVertex> verts(vc);
            for (size_t i = 0; i < vc; ++i) {
                verts[i].Position = {pos[i*3], pos[i*3+1], pos[i*3+2]};
                verts[i].Normal = !nrm.empty() ? glm::vec3(nrm[i*3], nrm[i*3+1], nrm[i*3+2]) : glm::vec3(0, 1, 0);
                verts[i].TexCoords = !uv.empty() ? glm::vec2(uv[i*2], uv[i*2+1]) : glm::vec2(0.0f);
                verts[i].Joints = {(float)joints[i*4], (float)joints[i*4+1], (float)joints[i*4+2], (float)joints[i*4+3]};
                glm::vec4 w(weights[i*4], weights[i*4+1], weights[i*4+2], weights[i*4+3]);
                float sum = w.x + w.y + w.z + w.w;
                verts[i].Weights = sum > 0.0001f ? w / sum : glm::vec4(1, 0, 0, 0);
            }

            std::vector<unsigned int> indices;
            if (prim.indices >= 0) indices = ReadUInts(g, prim.indices, 1);
            else { indices.resize(vc); for (size_t i = 0; i < vc; ++i) indices[i] = (unsigned)i; }

            SkinnedSubMesh sub;
            sub.Mesh = std::make_shared<SkinnedMesh>(verts, indices);
            sub.Tint = glm::vec3(1.0f);
            if (prim.material >= 0 && prim.material < (int)g.materials.size()) {
                const auto& pbr = g.materials[prim.material].pbrMetallicRoughness;
                if (pbr.baseColorFactor.size() == 4)
                    sub.Tint = glm::vec3(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2]);
                if (pbr.baseColorTexture.index >= 0) sub.Diffuse = loadTex(pbr.baseColorTexture.index);
                sub.Metallic = (float)pbr.metallicFactor;   // glTF по умолчанию 1.0
                sub.Roughness = (float)pbr.roughnessFactor; // glTF по умолчанию 1.0
            }
            model->m_subMeshes.push_back(std::move(sub));
        }
    }

    // Анимации: каждый channel -> наш AnimChannel (только кости скина).
    for (const auto& anim : g.animations) {
        AnimationClip clip;
        clip.Name = anim.name.empty() ? ("clip" + std::to_string(model->m_clips.size())) : anim.name;
        for (const auto& ch : anim.channels) {
            auto jIt = nodeToJoint.find(ch.target_node);
            if (jIt == nodeToJoint.end()) continue; // канал не на кости скина
            const tinygltf::AnimationSampler& samp = anim.samplers[ch.sampler];
            AnimChannel out;
            out.Joint = jIt->second;
            if (ch.target_path == "translation") out.Target = AnimPath::Translation;
            else if (ch.target_path == "rotation") out.Target = AnimPath::Rotation;
            else if (ch.target_path == "scale") out.Target = AnimPath::Scale;
            else continue; // weights (morph) не поддерживаем
            out.Interp = (samp.interpolation == "STEP") ? AnimInterp::Step : AnimInterp::Linear;

            std::vector<float> times = ReadFloats(g, samp.input, 1);
            int comps = (out.Target == AnimPath::Rotation) ? 4 : 3;
            std::vector<float> vals = ReadFloats(g, samp.output, comps);
            size_t keyCount = times.size();
            out.Times = times;
            out.Values.resize(keyCount, glm::vec4(0.0f));
            for (size_t k = 0; k < keyCount; ++k) {
                for (int c = 0; c < comps; ++c) out.Values[k][c] = vals[k * comps + c];
                clip.Duration = std::max(clip.Duration, times[k]);
            }
            clip.Channels.push_back(std::move(out));
        }
        if (!clip.Channels.empty()) model->m_clips.push_back(std::move(clip));
    }

    LOG_INFO("Anim") << "SkinnedModel загружен: " << path << " (костей " << jointCount
                     << ", submesh " << model->m_subMeshes.size()
                     << ", клипов " << model->m_clips.size() << ")";
    if (model->m_subMeshes.empty())
        throw std::runtime_error("SkinnedModel: в файле нет скиновых мешей: " + path);
    return model;
}

// ============================================================================
//  FBX-загрузчик (ufbx): скелет + скин-веса + анимационные клипы
// ============================================================================
namespace {

// ufbx_matrix — аффинная 3x4, столбцы cols[0..3]. Переводим в glm::mat4.
glm::mat4 FbxToGlm(const ufbx_matrix& m) {
    glm::mat4 r(1.0f);
    r[0] = glm::vec4((float)m.m00, (float)m.m10, (float)m.m20, 0.0f);
    r[1] = glm::vec4((float)m.m01, (float)m.m11, (float)m.m21, 0.0f);
    r[2] = glm::vec4((float)m.m02, (float)m.m12, (float)m.m22, 0.0f);
    r[3] = glm::vec4((float)m.m03, (float)m.m13, (float)m.m23, 1.0f);
    return r;
}

// Раскладывает аффинную матрицу на перенос/поворот/масштаб (без сдвига — для
// костей его нет). Свой decompose, чтобы не тянуть экспериментальный glm/gtx.
void FbxDecompose(const glm::mat4& mtx, glm::vec3& t, glm::quat& rot, glm::vec3& s) {
    t = glm::vec3(mtx[3]);
    glm::vec3 c0(mtx[0]), c1(mtx[1]), c2(mtx[2]);
    s = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));
    if (s.x > 1e-8f) c0 /= s.x;
    if (s.y > 1e-8f) c1 /= s.y;
    if (s.z > 1e-8f) c2 /= s.z;
    glm::mat3 rm(c0, c1, c2);
    rot = glm::normalize(glm::quat_cast(rm));
}

// Мировая матрица узла в момент t: произведение локальных трансформов по цепочке
// от узла до корня (target_axes уже вшит ufbx в local_transform). Костей немного,
// так что рекурсивная композиция на кадр допустима на этапе загрузки.
glm::mat4 FbxEvalWorld(const ufbx_anim* anim, const ufbx_node* node, double time) {
    glm::mat4 world(1.0f);
    for (const ufbx_node* n = node; n != nullptr; n = n->parent) {
        ufbx_transform lt = ufbx_evaluate_transform(anim, n, time);
        ufbx_matrix lm = ufbx_transform_to_matrix(&lt);
        world = FbxToGlm(lm) * world;
    }
    return world;
}

} // namespace

std::unique_ptr<SkinnedModel> SkinnedModel::LoadFbx(const std::string& path) {
    ufbx_load_opts opts = {};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.generate_missing_normals = true;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &error);
    if (!scene) {
        char buf[512];
        ufbx_format_error(buf, sizeof(buf), &error);
        throw std::runtime_error("SkinnedModel: не загрузить FBX " + path + ": " + buf);
    }
    // RAII-освобождение сцены при любом выходе (в т.ч. через throw ниже).
    struct SceneGuard { ufbx_scene* s; ~SceneGuard() { if (s) ufbx_free_scene(s); } } guard{scene};

    if (scene->skin_deformers.count == 0)
        throw std::runtime_error("SkinnedModel: в FBX нет скина: " + path);

    auto model = std::unique_ptr<SkinnedModel>(new SkinnedModel());

    // --- Скелет: все кости-узлы из всех кластеров всех скин-деформеров. ---
    // Каждая кость — «корень» в нашем скелете (Parent = -1): её TRS хранит
    // АБСОЛЮТНУЮ мировую позу, поэтому Animator получает global = local напрямую,
    // без композиции иерархии (веса и inverse-bind при этом полностью корректны).
    std::unordered_map<const ufbx_node*, int> boneIndex;
    std::vector<const ufbx_node*> boneNodes;
    Skeleton& sk = model->m_skeleton;

    auto ensureBone = [&](const ufbx_node* n) -> int {
        auto it = boneIndex.find(n);
        if (it != boneIndex.end()) return it->second;
        int idx = (int)boneNodes.size();
        boneIndex[n] = idx;
        boneNodes.push_back(n);
        Joint j;
        j.Name = std::string(n->name.data, n->name.length);
        j.Parent = -1;
        glm::mat4 world = FbxToGlm(n->node_to_world);
        FbxDecompose(world, j.Translation, j.Rotation, j.Scale);
        j.InverseBind = glm::inverse(world); // world(bind) -> bone
        sk.Joints.push_back(std::move(j));
        return idx;
    };

    for (size_t di = 0; di < scene->skin_deformers.count; ++di) {
        const ufbx_skin_deformer* def = scene->skin_deformers.data[di];
        for (size_t ci = 0; ci < def->clusters.count; ++ci) {
            const ufbx_skin_cluster* cl = def->clusters.data[ci];
            if (cl->bone_node) ensureBone(cl->bone_node);
        }
    }
    if (sk.Joints.empty())
        throw std::runtime_error("SkinnedModel: в FBX нет костей скина: " + path);
    if (sk.Count() > kMaxBones)
        LOG_WARN("Anim") << "SkinnedModel FBX: костей " << sk.Count() << " > " << kMaxBones
                         << " — лишние не поместятся в палитру";

    // --- Меши со скином: вершины в МИРОВОМ пространстве (как inverse-bind). ---
    std::vector<uint32_t> triIndices;
    for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
        const ufbx_node* node = scene->nodes.data[ni];
        if (!node->mesh || node->mesh->skin_deformers.count == 0) continue;
        const ufbx_mesh* mesh = node->mesh;
        const ufbx_skin_deformer* skin = mesh->skin_deformers.data[0];
        const glm::mat4 g2w = FbxToGlm(node->geometry_to_world);
        const glm::mat3 g2wN = glm::transpose(glm::inverse(glm::mat3(g2w)));

        std::vector<SkinnedVertex> verts;
        std::vector<unsigned int> indices;
        triIndices.resize(mesh->max_face_triangles * 3);

        for (size_t fi = 0; fi < mesh->faces.count; ++fi) {
            ufbx_face face = mesh->faces.data[fi];
            size_t numTris = ufbx_triangulate_face(triIndices.data(), triIndices.size(), mesh, face);
            for (size_t t = 0; t < numTris * 3; ++t) {
                uint32_t corner = triIndices[t];
                uint32_t vtx = mesh->vertex_indices.data[corner]; // логический control-point

                SkinnedVertex v{};
                ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, corner);
                glm::vec4 pw = g2w * glm::vec4((float)p.x, (float)p.y, (float)p.z, 1.0f);
                v.Position = glm::vec3(pw);
                if (mesh->vertex_normal.exists) {
                    ufbx_vec3 n = ufbx_get_vertex_vec3(&mesh->vertex_normal, corner);
                    glm::vec3 nw = g2wN * glm::vec3((float)n.x, (float)n.y, (float)n.z);
                    float len = glm::length(nw);
                    v.Normal = len > 1e-6f ? nw / len : glm::vec3(0, 1, 0);
                }
                if (mesh->vertex_uv.exists) {
                    ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, corner);
                    v.TexCoords = {(float)uv.x, (float)uv.y};
                }

                // До 4 самых весомых костей вершины (skin отсортирован по убыванию веса).
                const ufbx_skin_vertex& sv = skin->vertices.data[vtx];
                glm::ivec4 ji(0);
                glm::vec4 wt(0.0f);
                uint32_t take = std::min<uint32_t>(sv.num_weights, 4);
                for (uint32_t k = 0; k < take; ++k) {
                    const ufbx_skin_weight& w = skin->weights.data[sv.weight_begin + k];
                    const ufbx_skin_cluster* cl = w.cluster_index < skin->clusters.count
                        ? skin->clusters.data[w.cluster_index] : nullptr;
                    int bi = 0;
                    if (cl && cl->bone_node) {
                        auto it = boneIndex.find(cl->bone_node);
                        if (it != boneIndex.end()) bi = it->second;
                    }
                    ji[k] = bi;
                    wt[k] = (float)w.weight;
                }
                float sum = wt.x + wt.y + wt.z + wt.w;
                if (sum > 1e-5f) wt /= sum; else wt = glm::vec4(1, 0, 0, 0);
                v.Joints = glm::vec4(ji);
                v.Weights = wt;

                indices.push_back((unsigned int)verts.size());
                verts.push_back(v);
            }
        }
        if (verts.empty()) continue;

        SkinnedSubMesh sub;
        sub.Mesh = std::make_shared<SkinnedMesh>(verts, indices);
        sub.Tint = glm::vec3(1.0f);
        if (mesh->materials.count > 0) {
            const ufbx_material* mat = mesh->materials.data[0];
            ufbx_vec3 c = mat->fbx.diffuse_color.value_vec3;
            if (mat->pbr.base_color.has_value) c = mat->pbr.base_color.value_vec3;
            sub.Tint = {(float)c.x, (float)c.y, (float)c.z};
            if (mat->pbr.metalness.has_value) sub.Metallic = (float)mat->pbr.metalness.value_real;
            if (mat->pbr.roughness.has_value) sub.Roughness = (float)mat->pbr.roughness.value_real;
        }
        model->m_subMeshes.push_back(std::move(sub));
    }
    if (model->m_subMeshes.empty())
        throw std::runtime_error("SkinnedModel: в FBX нет скиновых мешей: " + path);

    // --- Анимации: каждый anim-stack -> клип; кости сэмплируем в мировой позе. ---
    const int kBones = sk.Count();
    for (size_t si = 0; si < scene->anim_stacks.count; ++si) {
        const ufbx_anim_stack* stack = scene->anim_stacks.data[si];
        double t0 = stack->time_begin, t1 = stack->time_end;
        if (t1 <= t0) continue;
        const ufbx_anim* anim = stack->anim;

        AnimationClip clip;
        clip.Name = stack->name.length ? std::string(stack->name.data, stack->name.length)
                                       : ("clip" + std::to_string(model->m_clips.size()));
        clip.Duration = (float)(t1 - t0);

        // Равномерно 30 к/с — ufbx-каналы могут иметь разные разбиения ключей,
        // единая сетка даёт ровные линейно-интерполируемые клипы под наш Animator.
        const double fps = 30.0;
        int frames = std::max(2, (int)std::lround((t1 - t0) * fps) + 1);

        for (int bi = 0; bi < kBones; ++bi) {
            AnimChannel tc; tc.Joint = bi; tc.Target = AnimPath::Translation;
            AnimChannel rc; rc.Joint = bi; rc.Target = AnimPath::Rotation;
            AnimChannel sc; sc.Joint = bi; sc.Target = AnimPath::Scale;
            for (int f = 0; f < frames; ++f) {
                double tt = t0 + (t1 - t0) * (double)f / (double)(frames - 1);
                glm::mat4 world = FbxEvalWorld(anim, boneNodes[bi], tt);
                glm::vec3 T, S; glm::quat R;
                FbxDecompose(world, T, R, S);
                float lt = (float)(tt - t0);
                tc.Times.push_back(lt); tc.Values.push_back(glm::vec4(T, 0.0f));
                rc.Times.push_back(lt); rc.Values.push_back(glm::vec4(R.x, R.y, R.z, R.w));
                sc.Times.push_back(lt); sc.Values.push_back(glm::vec4(S, 0.0f));
            }
            clip.Channels.push_back(std::move(tc));
            clip.Channels.push_back(std::move(rc));
            clip.Channels.push_back(std::move(sc));
        }
        model->m_clips.push_back(std::move(clip));
    }

    LOG_INFO("Anim") << "SkinnedModel загружен (FBX): " << path << " (костей " << kBones
                     << ", submesh " << model->m_subMeshes.size()
                     << ", клипов " << model->m_clips.size() << ")";
    return model;
}

} // namespace sage::render
