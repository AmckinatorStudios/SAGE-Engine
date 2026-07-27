// tinygltf-реализация развёрнута в Model.cpp — здесь только объявления.
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "SkinnedModel.h"

#include "sage/assets/AssetCache.h"
#include "sage/render/ModelData.h"
#include "sage/gi/GIUpload.h"

#include <algorithm>
#include <cstdint>
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


// Блок морфинга — общий для цветного и depth-прохода: форма обязана совпадать,
// иначе тень персонажа не соответствовала бы его лицу.
const char* kMorphGlsl = R"(
const int MAX_ACTIVE_MORPHS = 8;
uniform sampler2D uMorphPos;
uniform sampler2D uMorphNrm;
uniform int uMorphCount;                        // сколько АКТИВНЫХ целей в кадре
uniform int uMorphTarget[MAX_ACTIVE_MORPHS];    // их индексы в модели
uniform float uMorphWeight[MAX_ACTIVE_MORPHS];  // и веса
uniform int uMorphWidth;                        // ширина текстуры дельт, вершин
uniform int uMorphRows;                         // строк на одну цель

// Смещает позицию и нормаль вершины суммой активных морф-целей. texelFetch, а
// не texture(): нам нужен ТОЧНО этот тексель, без фильтрации и мип-уровней.
void ApplyMorphs(inout vec3 pos, inout vec3 nrm) {
    if (uMorphCount <= 0) return;
    int col = gl_VertexID % uMorphWidth;
    int rowInTarget = gl_VertexID / uMorphWidth;
    for (int i = 0; i < uMorphCount; ++i) {
        int row = uMorphTarget[i] * uMorphRows + rowInTarget;
        pos += uMorphWeight[i] * texelFetch(uMorphPos, ivec2(col, row), 0).xyz;
        nrm += uMorphWeight[i] * texelFetch(uMorphNrm, ivec2(col, row), 0).xyz;
    }
}
)";

const char* kSkinVertBody = R"(
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec4 aJoints;
layout (location = 4) in vec4 aWeights;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoords;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
const int MAX_BONES = 128;
uniform mat4 uBones[MAX_BONES];
uniform int uSkinned; // 0 — bind-поза (без палитры)

void main() {
    // Морфинг ДО скиннинга: блендшейпы заданы в той же bind-позе, что и меш,
    // а кости двигают уже итоговую форму. Обратный порядок деформировал бы
    // дельты вместе с костями — лицо «уезжало» бы при повороте головы.
    vec3 morphedPos = aPos;
    vec3 morphedNrm = aNormal;
    ApplyMorphs(morphedPos, morphedNrm);

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
    vec4 skinnedPos = skin * vec4(morphedPos, 1.0);
    vec3 skinnedNormal = mat3(skin) * morphedNrm;

    vec4 worldPos = uModel * skinnedPos;
    FragPos = worldPos.xyz;
    // Обратно-транспонированная нормальная матрица — верные нормали при
    // неравномерном/отрицательном масштабе модели.
    Normal = transpose(inverse(mat3(uModel))) * skinnedNormal;
    TexCoords = aUV;
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
    FragColor = vec4(ShadePBR(N, FragPos, albedo, uMetallic, uRoughness), 1.0);
}
)";
}

// Готовый исходник вершинной стадии: версия + блок морфинга + тело. Склейка
// здесь, а не в двух местах, гарантирует, что цветной и depth-проход морфят
// вершину ОДИНАКОВО (см. комментарий у kMorphGlsl).
std::string SkinVertSource() {
    return std::string("#version 330 core\n") + kMorphGlsl + kSkinVertBody;
}

Shader& SkinShader() {
    // Намеренно НЕ уничтожаем: function-local static с деструктором Shader снёс
    // бы GL-программу при выходе из процесса — уже ПОСЛЕ разрушения GL-контекста
    // (segfault в glDeleteProgram). Утечка одной программы на выходе безвредна
    // (ОС всё освободит), зато нет обращения к мёртвому контексту.
    static Shader* shader =
        new Shader(Shader::FromSource(SkinVertSource(), SkinFragSource(), "SkinnedModel"));
    return *shader;
}

// Depth-only скиннинг-шейдер для карты теней: скиннинг в вершинной стадии,
// пустой фрагмент (пишется только глубина). Позиция/кости в тех же локациях,
// что и основной скиннинг-шейдер (0/3/4).
const char* kSkinDepthVertBody = R"(
layout (location = 0) in vec3 aPos;
layout (location = 3) in vec4 aJoints;
layout (location = 4) in vec4 aWeights;

uniform mat4 uLightSpace;
uniform mat4 uModel;
const int MAX_BONES = 128;
uniform mat4 uBones[MAX_BONES];
uniform int uSkinned;

void main() {
    vec3 morphedPos = aPos;
    vec3 unusedNrm = vec3(0.0, 1.0, 0.0);
    ApplyMorphs(morphedPos, unusedNrm);

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
    gl_Position = uLightSpace * uModel * skin * vec4(morphedPos, 1.0);
}
)";

// Исходник depth-стадии: версия + блок морфинга + тело (см. SkinVertSource).
std::string SkinDepthVertSource() {
    return std::string("#version 330 core\n") + kMorphGlsl + kSkinDepthVertBody;
}

const char* kSkinDepthFrag = R"(#version 330 core
void main() {}
)";

Shader& SkinDepthShader() {
    static Shader* shader = new Shader(Shader::FromSource(SkinDepthVertSource(), kSkinDepthFrag, "SkinnedModelDepth"));
    return *shader;
}

} // namespace

namespace {

// Отбирает АКТИВНЫЕ морф-цели submesh и заливает их в шейдер.
//
// В кадр идут только цели с ненулевым весом, и не больше восьми: у лица целей
// бывают десятки, но одновременно шевелится единицы, а цикл в вершинном шейдере
// стоит по выборке из текстуры на каждую. Если активных больше восьми — берём
// восемь САМЫХ ВЕСОМЫХ: тихо потерять сильную цель хуже, чем слабую.
// Единицы текстур 10 и 11. Числа не случайные: 0 — albedo, 1 — карта теней,
// 6..8 — GI-объём, 9 — лайтмапа (см. sage/gi/GIUpload.h). Совпадение юнита
// молча ломает морфинг — выборка идёт из чужой текстуры и даёт нули, — поэтому
// берём заведомо свободные и держим их рядом с этим списком.
constexpr int kMaxActiveMorphs = 8;
constexpr int kMorphPosUnit = 10;
constexpr int kMorphNrmUnit = 11;

void UploadMorphs(const Shader& shader, const SkinnedSubMesh& sub,
                  const std::vector<float>* weights) {
    if (!sub.Morphs.Valid() || !weights || weights->empty()) {
        shader.SetInt("uMorphCount", 0);
        return;
    }

    // Пары (вес, индекс) — сортируем по убыванию веса и берём верхушку.
    std::vector<std::pair<float, int>> active;
    const int count = std::min((int)weights->size(), sub.Morphs.Count);
    for (int i = 0; i < count; ++i) {
        const float w = (*weights)[(size_t)i];
        if (std::fabs(w) > 1e-4f) active.emplace_back(std::fabs(w), i);
    }
    if (active.empty()) {
        shader.SetInt("uMorphCount", 0);
        return;
    }
    std::sort(active.begin(), active.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    if ((int)active.size() > kMaxActiveMorphs) active.resize(kMaxActiveMorphs);

    int indices[kMaxActiveMorphs] = {};
    float values[kMaxActiveMorphs] = {};
    for (size_t i = 0; i < active.size(); ++i) {
        indices[i] = active[i].second;
        values[i] = (*weights)[(size_t)active[i].second]; // знак важен: бывают отрицательные веса
    }

    shader.SetInt("uMorphCount", (int)active.size());
    shader.SetIntArray("uMorphTarget", indices, (int)active.size());
    shader.SetFloatArray("uMorphWeight", values, (int)active.size());
    shader.SetInt("uMorphWidth", sub.Morphs.Width);
    shader.SetInt("uMorphRows", sub.Morphs.RowsPerTarget);
    shader.SetInt("uMorphPos", kMorphPosUnit);
    shader.SetInt("uMorphNrm", kMorphNrmUnit);
    sub.Morphs.Positions->Bind(kMorphPosUnit);
    sub.Morphs.Normals->Bind(kMorphNrmUnit);
}

} // namespace

void SkinnedModel::Draw(const glm::mat4& model, const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& viewPos, const LightingEnvironment& env,
                        const std::vector<glm::mat4>& bones,
                        const ShadowBinding& shadows,
                        const std::vector<float>* morphWeights) const {
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

    // Тени от солнца: каскады на свои юниты + матрицы — как у статики.
    BindAndUploadShadows(shader, shadows);

    int boneCount = std::min((int)bones.size(), kMaxBones);
    if (boneCount > 0) {
        shader.SetInt("uSkinned", 1);
        shader.SetMat4Array("uBones", bones.data(), boneCount);
    } else {
        shader.SetInt("uSkinned", 0);
    }

    for (const auto& sub : m_subMeshes) {
        UploadMorphs(shader, sub, morphWeights);
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
                             const std::vector<glm::mat4>& bones,
                             const std::vector<float>* morphWeights) const {
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

    for (const auto& sub : m_subMeshes) {
        // Тень обязана повторять ту же форму, что и сам меш, иначе лицо и его
        // тень «разъедутся» при любом выражении.
        UploadMorphs(shader, sub, morphWeights);
        sub.Mesh->Draw();
    }
}

int SkinnedModel::FindMorph(const std::string& name) const {
    for (size_t i = 0; i < m_morphNames.size(); ++i) {
        if (m_morphNames[i] == name) return (int)i;
    }
    return -1;
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

    // --- Две морф-цели, как у настоящей модели ---
    // Блендшейпы нужно на чём-то показывать и чем-то проверять, а лицевой модели
    // в комплекте движка нет и быть не должно. Цели процедурные и намеренно
    // разные по характеру: одна меняет толщину (равномерно), вторая — только
    // верхушку (локально), чтобы было видно, что веса работают независимо.
    {
        const int targetCount = 2;
        const int width = 1024;
        const int rows = (int)((verts.size() + width - 1) / width);
        std::vector<float> posDeltas((size_t)width * rows * targetCount * 3u, 0.0f);
        std::vector<float> nrmDeltas((size_t)width * rows * targetCount * 3u, 0.0f);

        const float topY = rings * segLen;
        for (size_t v = 0; v < verts.size(); ++v) {
            const glm::vec3& p = verts[v].Position;

            // Цель 0 «Fatten»: раздуть сечение наружу вдоль нормали.
            const glm::vec3 fatten = verts[v].Normal * (halfW * 1.6f);
            const size_t t0 = 0 * (size_t)width * rows + v;
            posDeltas[t0 * 3 + 0] = fatten.x;
            posDeltas[t0 * 3 + 1] = fatten.y;
            posDeltas[t0 * 3 + 2] = fatten.z;

            // Цель 1 «Bend»: отклонить щупалец в сторону тем сильнее, чем выше
            // вершина. Квадрат по высоте — основание остаётся на месте, а
            // силуэт меняется заметно; равномерный сдвиг просто переставил бы
            // модель и ничего не показал бы про деформацию.
            const float t = topY > 0.0001f ? glm::clamp(p.y / topY, 0.0f, 1.0f) : 0.0f;
            const glm::vec3 bend(t * t * segLen * 2.5f, 0.0f, 0.0f);
            const size_t t1 = 1 * (size_t)width * rows + v;
            posDeltas[t1 * 3 + 0] = bend.x;
            posDeltas[t1 * 3 + 1] = bend.y;
            posDeltas[t1 * 3 + 2] = bend.z;
        }

        sage::rhi::Texture2DDesc desc;
        desc.Width = width;
        desc.Height = rows * targetCount;
        desc.Channels = 3;
        desc.FilterMode = sage::rhi::Filter::Nearest;
        desc.WrapMode = sage::rhi::Wrap::ClampEdge;
        desc.GenerateMipmaps = false;
        desc.FloatPixels = true;

        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        sub.Morphs.Positions = device.CreateTexture2D(desc, posDeltas.data());
        sub.Morphs.Normals = device.CreateTexture2D(desc, nrmDeltas.data());
        sub.Morphs.Count = targetCount;
        sub.Morphs.Width = width;
        sub.Morphs.RowsPerTarget = rows;

        m->m_morphNames = {"Fatten", "Bend"};
        m->m_morphDefaults.assign((size_t)targetCount, 0.0f);
    }

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

// Разбор исходного glTF в ModelData. Ничего не создаёт на видеокарте —
// поэтому работает и без контекста OpenGL, и результат можно записать в кэш.
static ModelData ParseGltf(const std::string& path) {
    bool binary = path.size() > 4 && path.substr(path.size() - 4) == ".glb";

    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(&GltfSkinImageLoader, nullptr);
    tinygltf::Model g;
    std::string err, warn;
    bool ok = binary ? loader.LoadBinaryFromFile(&g, &err, &warn, path)
                     : loader.LoadASCIIFromFile(&g, &err, &warn, path);
    if (!warn.empty()) LOG_WARN("Anim") << "glTF (" << path << "): " << warn;
    if (!ok) throw std::runtime_error("SkinnedModel: не загрузить glTF " + path + ": " + err);
    if (g.skins.empty()) throw std::runtime_error("SkinnedModel: в файле нет скина: " + path);

    ModelData data;
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
    Skeleton& sk = data.Skeleton;
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
    // Изображение забирается РАСПАКОВАННЫМ и запоминается по индексу glTF: одна
    // картинка, использованная десятью материалами, не должна лежать в данных
    // десять раз — ни в памяти, ни в кэше.
    std::unordered_map<int, int> imageSlots;
    auto takeImage = [&](int texIndex) -> int {
        if (texIndex < 0 || texIndex >= (int)g.textures.size()) return -1;
        int img = g.textures[texIndex].source;
        auto it = imageSlots.find(img);
        if (it != imageSlots.end()) return it->second;
        if (img < 0 || img >= (int)g.images.size() || g.images[img].image.empty()) return -1;
        ModelImage out;
        out.Width = g.images[img].width;
        out.Height = g.images[img].height;
        out.Pixels = g.images[img].image;
        data.Images.push_back(std::move(out));
        const int slot = (int)data.Images.size() - 1;
        imageSlots[img] = slot;
        return slot;
    };

    // Ширина текстуры дельт. 1024 — заведомо в пределах любого GL 3.3
    // (минимум по спецификации 1024) и достаточно широка, чтобы у обычного
    // лицевого меша получилось несколько десятков строк на цель, а не тысячи.
    constexpr int kMorphTexWidth = 1024;

    // Имена морф-целей: glTF кладёт их в нестандартное, но общепринятое место —
    // mesh.extras.targetNames. Без имён блендшейпы пришлось бы выбирать номером,
    // а художник называет их «улыбка», «моргание», и терять это нельзя.
    auto readTargetNames = [](const tinygltf::Mesh& mesh, size_t count) {
        std::vector<std::string> names;
        const tinygltf::Value& extras = mesh.extras;
        if (extras.IsObject() && extras.Has("targetNames")) {
            const tinygltf::Value& arr = extras.Get("targetNames");
            if (arr.IsArray()) {
                for (size_t i = 0; i < arr.ArrayLen(); ++i) {
                    const tinygltf::Value& v = arr.Get((int)i);
                    names.push_back(v.IsString() ? v.Get<std::string>() : std::string{});
                }
            }
        }
        names.resize(count);
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i].empty()) names[i] = "Morph " + std::to_string(i);
        }
        return names;
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

            ModelSubMeshData sub;
            sub.Vertices = std::move(verts);
            sub.Indices = std::move(indices);
            sub.Tint = glm::vec3(1.0f);

            // --- Морф-цели примитива ---
            if (!prim.targets.empty()) {
                const int targetCount = (int)prim.targets.size();
                const int rows = (int)((vc + kMorphTexWidth - 1) / kMorphTexWidth);
                const size_t texels = (size_t)kMorphTexWidth * rows * targetCount;

                // Три канала (xyz дельты); четвёртый не нужен, но RGB-раскладка
                // float-текстуры проще и совместимее.
                std::vector<float> posDeltas(texels * 3u, 0.0f);
                std::vector<float> nrmDeltas(texels * 3u, 0.0f);

                for (int t = 0; t < targetCount; ++t) {
                    const auto& target = prim.targets[(size_t)t];
                    auto tp = target.find("POSITION");
                    auto tn = target.find("NORMAL");
                    std::vector<float> dp = tp != target.end() ? ReadFloats(g, tp->second, 3)
                                                              : std::vector<float>();
                    std::vector<float> dn = tn != target.end() ? ReadFloats(g, tn->second, 3)
                                                              : std::vector<float>();
                    for (size_t v = 0; v < vc; ++v) {
                        const size_t texel = (size_t)t * kMorphTexWidth * rows + v;
                        if (dp.size() >= (v + 1) * 3) {
                            posDeltas[texel * 3 + 0] = dp[v * 3 + 0];
                            posDeltas[texel * 3 + 1] = dp[v * 3 + 1];
                            posDeltas[texel * 3 + 2] = dp[v * 3 + 2];
                        }
                        if (dn.size() >= (v + 1) * 3) {
                            nrmDeltas[texel * 3 + 0] = dn[v * 3 + 0];
                            nrmDeltas[texel * 3 + 1] = dn[v * 3 + 1];
                            nrmDeltas[texel * 3 + 2] = dn[v * 3 + 2];
                        }
                    }
                }

                sub.MorphPositions = std::move(posDeltas);
                sub.MorphNormals = std::move(nrmDeltas);
                sub.MorphCount = targetCount;
                sub.MorphWidth = kMorphTexWidth;
                sub.MorphRows = rows;

                // Имена и стартовые веса берём у ПЕРВОГО примитива с морфами:
                // в glTF цели общие для всего меша, а модель у нас одна.
                if (data.MorphNames.empty()) {
                    data.MorphNames = readTargetNames(mesh, (size_t)targetCount);
                    data.MorphDefaults.assign((size_t)targetCount, 0.0f);
                    for (size_t i = 0; i < mesh.weights.size() && i < data.MorphDefaults.size(); ++i) {
                        data.MorphDefaults[i] = (float)mesh.weights[i];
                    }
                }
            }
            if (prim.material >= 0 && prim.material < (int)g.materials.size()) {
                const auto& pbr = g.materials[prim.material].pbrMetallicRoughness;
                if (pbr.baseColorFactor.size() == 4)
                    sub.Tint = glm::vec3(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2]);
                if (pbr.baseColorTexture.index >= 0) sub.Image = takeImage(pbr.baseColorTexture.index);
                sub.Metallic = (float)pbr.metallicFactor;   // glTF по умолчанию 1.0
                sub.Roughness = (float)pbr.roughnessFactor; // glTF по умолчанию 1.0
            }
            data.SubMeshes.push_back(std::move(sub));
        }
    }

    // Анимации: каждый channel -> наш AnimChannel (только кости скина).
    for (const auto& anim : g.animations) {
        AnimationClip clip;
        clip.Name = anim.name.empty() ? ("clip" + std::to_string(data.Clips.size())) : anim.name;
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
        if (!clip.Channels.empty()) data.Clips.push_back(std::move(clip));
    }

    if (data.SubMeshes.empty())
        throw std::runtime_error("SkinnedModel: в файле нет скиновых мешей: " + path);
    return data;
}

// Сборка на видеокарте: вершины в буферы, пиксели в текстуры, дельты морфов в
// float-текстуры. Отсюда и только отсюда трогается GPU.
std::unique_ptr<SkinnedModel> SkinnedModel::BuildFromData(ModelData& data) {
    auto model = std::unique_ptr<SkinnedModel>(new SkinnedModel());
    model->m_skeleton = std::move(data.Skeleton);
    model->m_clips = std::move(data.Clips);
    model->m_morphNames = std::move(data.MorphNames);
    model->m_morphDefaults = std::move(data.MorphDefaults);

    std::vector<std::shared_ptr<Texture>> textures;
    textures.reserve(data.Images.size());
    for (const ModelImage& img : data.Images) {
        textures.push_back(std::make_shared<Texture>(img.Pixels.data(), img.Width, img.Height,
                                                     TextureFilter::Trilinear, true));
    }

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    for (ModelSubMeshData& src : data.SubMeshes) {
        SkinnedSubMesh sub;
        sub.Mesh = std::make_shared<SkinnedMesh>(src.Vertices, src.Indices);
        sub.Tint = src.Tint;
        sub.Metallic = src.Metallic;
        sub.Roughness = src.Roughness;
        if (src.Image >= 0 && src.Image < (int)textures.size()) sub.Diffuse = textures[(size_t)src.Image];

        if (src.MorphCount > 0) {
            // Nearest и без мипов: выборка идёт texelFetch по точному индексу
            // вершины, любая фильтрация тут только смешала бы дельты соседних
            // вершин.
            sage::rhi::Texture2DDesc desc;
            desc.Width = src.MorphWidth;
            desc.Height = src.MorphRows * src.MorphCount;
            desc.Channels = 3;
            desc.FilterMode = sage::rhi::Filter::Nearest;
            desc.WrapMode = sage::rhi::Wrap::ClampEdge;
            desc.GenerateMipmaps = false;
            desc.FloatPixels = true;
            sub.Morphs.Positions = device.CreateTexture2D(desc, src.MorphPositions.data());
            sub.Morphs.Normals = device.CreateTexture2D(desc, src.MorphNormals.data());
            sub.Morphs.Count = src.MorphCount;
            sub.Morphs.Width = src.MorphWidth;
            sub.Morphs.RowsPerTarget = src.MorphRows;
        }
        model->m_subMeshes.push_back(std::move(sub));
    }
    return model;
}

std::unique_ptr<SkinnedModel> SkinnedModel::Load(const std::string& path) {
    // Кэш пробуется ПЕРВЫМ. Промах, устаревший или битый кэш — не ошибка:
    // молча разбираем исходник и перезаписываем кэш. Неверный кэш никогда не
    // должен приводить к неверной картинке, а его отсутствие — к отказу.
    ModelData data;
    if (sage::assets::ReadModelCache(path, data) && !data.Empty()) {
        LOG_INFO("Anim") << "SkinnedModel из кэша: " << path << " (костей "
                         << data.Skeleton.Count() << ", submesh " << data.SubMeshes.size()
                         << ", клипов " << data.Clips.size() << ")";
        return BuildFromData(data);
    }

    data = ParseGltf(path);
    LOG_INFO("Anim") << "SkinnedModel разобран: " << path << " (костей "
                     << data.Skeleton.Count() << ", submesh " << data.SubMeshes.size()
                     << ", клипов " << data.Clips.size() << ")";
    sage::assets::WriteModelCache(path, data);
    return BuildFromData(data);
}

} // namespace sage::render
