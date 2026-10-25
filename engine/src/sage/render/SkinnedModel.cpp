// tinygltf-реализация развёрнута в Model.cpp — здесь только объявления.
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "sage/assets/import/GltfAccessor.h"
#include "sage/assets/import/GltfFile.h"
#include "sage/assets/import/SkinInfluences.h"
#include "sage/render/ModelLoader.h"
#include <limits>

#include "SkinnedModel.h"
#include "sage/render/SkinnedModelGltf.h"

#include "sage/assets/AssetCache.h"
#include <filesystem>
#include <cctype>

#include "sage/assets/import/FbxSkin.h"
#include "sage/render/ModelData.h"
#include "sage/anim/Retarget.h"
#include "sage/gi/GIUpload.h"
#include "sage/render/LightClusters.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
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

namespace fs = std::filesystem;

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
        {5, 4, AttribType::Float, (int)offsetof(SkinnedVertex, Tangent)},
        {6, 2, AttribType::Float, (int)offsetof(SkinnedVertex, TexCoords2)},
    };
    m_geometry = GraphicsDevice::Get().CreateGeometry(layout);
    m_geometry->SetVertexData(vertices.data(), vertices.size() * sizeof(SkinnedVertex), false);
    m_geometry->SetIndexData(indices.data(), indices.size(), false);
}

void SkinnedMesh::Draw() const { m_geometry->DrawIndexed(m_indexCount); }

// ============================================================================
//  Скиннинг-шейдер. СВОЯ здесь только вершинная стадия — скиннинг палитрой
//  костей; фрагментная берётся целиком у статического текстурного прохода
//  (sage::render::TexturedPbrFragSource из PbrShader.h). Не «такая же», а
//  БУКВАЛЬНО ТА ЖЕ функция: две редакции одного шейдера расходятся на первой
//  правке, и ровно так скин остался без карт материала, пока у него был свой
//  сокращённый фрагмент.
//
//  Uniform'ы освещения названы как в UploadLighting/UploadShadowUniforms и
//  заливаются без изменений; юниты текстур совпадают с раскладкой RenderBatch.
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
layout (location = 5) in vec4 aTangent;
layout (location = 6) in vec2 aUV2;

// ВЫХОД ТОТ ЖЕ, ЧТО У СТАТИЧЕСКОГО ТЕКСТУРНОГО МЕША (см. kTexVert в
// ecs/RenderBatch.cpp) — иначе общий фрагментный шейдер сюда не подошёл бы, а
// именно он и делает освещение скина и статики одинаковым.
out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoords;
out mat3 TBN;
out vec2 vUV2;

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
    // Нормаль и касательную поворачивает та же матрица скиннинга: кость гнёт
    // поверхность целиком, и рельеф обязан гнуться вместе с ней.
    mat3 skin3 = mat3(skin);
    vec3 skinnedNormal = skin3 * morphedNrm;
    vec3 skinnedTangent = skin3 * aTangent.xyz;

    // ЕДИНСТВЕННОЕ ОТЛИЧИЕ ОТ ОБЫЧНОГО МЕША — строчки выше. Дальше всё ровно
    // то же самое: модельная матрица, мировая позиция, нормальная матрица.
    vec4 worldPos = uModel * skinnedPos;
    FragPos = worldPos.xyz;
    // Обратно-транспонированная нормальная матрица — верные нормали при
    // неравномерном/отрицательном масштабе модели.
    mat3 nm = transpose(inverse(mat3(uModel)));
    vec3 N = normalize(nm * skinnedNormal);
    vec3 T = normalize(nm * skinnedTangent);
    T = normalize(T - N * dot(N, T));      // Gram-Schmidt, как у статики
    vec3 B = cross(N, T) * aTangent.w;     // знак развёртки
    TBN = mat3(T, B, N);
    Normal = N;
    TexCoords = aUV;
    vUV2 = aUV2;
    gl_Position = uProjection * uView * worldPos;
}
)";

// Фрагментная стадия — ОБЩАЯ со статическим текстурным мешем
// (sage::render::TexturedPbrFragSource, см. PbrShader.h). Здесь нет ни строчки
// своего освещения, и это главное: пока у скина был свой сокращённый фрагмент,
// персонаж не знал ни карты металличности (а значит, металличность бралась
// множителем на всю поверхность и гасила рассеянный свет), ни карты нормалей,
// ни затенения, ни свечения, ни прозрачности.
std::string SkinFragSource() { return sage::render::TexturedPbrFragSource(); }

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
layout (location = 2) in vec2 aUV;
layout (location = 3) in vec4 aJoints;
layout (location = 4) in vec4 aWeights;
out vec2 TexCoords;   // для выреза по альфе в тени (волосы, листва, бахрома)

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
    TexCoords = aUV;
    gl_Position = uLightSpace * uModel * skin * vec4(morphedPos, 1.0);
}
)";

// Исходник depth-стадии: версия + блок морфинга + тело (см. SkinVertSource).
std::string SkinDepthVertSource() {
    return std::string("#version 330 core\n") + kMorphGlsl + kSkinDepthVertBody;
}

// Вырез по альфе — тот же, что у цветного прохода: пряди волос и бахрома
// сделаны карточками с прозрачностью, и без выреза тень от причёски была
// сплошным колпаком. Ноль порога — режима нет, альфа не смотрится.
const char* kSkinDepthFrag = R"(#version 330 core
in vec2 TexCoords;
uniform sampler2D uAlbedoMap;
uniform float uAlphaCutoff;
void main() {
    if (uAlphaCutoff > 0.0 && texture(uAlbedoMap, TexCoords).a < uAlphaCutoff) discard;
}
)";

Shader& SkinDepthShader() {
    static Shader* shader = new Shader(Shader::FromSource(SkinDepthVertSource(), kSkinDepthFrag, "SkinnedModelDepth"));
    return *shader;
}

// Силуэт: та же вершинная часть (скиннинг + морфы), но с записью цвета.
// Вершинный шейдер общий с depth-проходом намеренно: силуэт обязан совпадать
// с геометрией пиксель в пиксель, а две копии одной формулы расходятся.
const char* kSkinSilhouetteFrag = R"(#version 330 core
out vec4 FragColor;
uniform vec3 uColor;
void main() { FragColor = vec4(uColor, 1.0); }
)";

Shader& SkinSilhouetteShader() {
    static Shader* shader = new Shader(
        Shader::FromSource(SkinDepthVertSource(), kSkinSilhouetteFrag, "SkinnedModelSilhouette"));
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
// Не 10/11: там каскады теней, и фрагмент читал бы морф вместо тени (см.
// раскладку юнитов в render/ShadowMap.h).
constexpr int kMorphPosUnit = 16;
constexpr int kMorphNrmUnit = 17;

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

void SkinnedModel::Draw(const glm::mat4& entityModel, const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& viewPos, const LightingEnvironment& env,
                        const std::vector<glm::mat4>& bones,
                        const ShadowBinding& shadows,
                        const sage::render::ReflectionBinding* reflections,
                        const std::vector<float>* morphWeights, int shadingMode,
                        const sage::render::LightClusters* clusters, sage::gi::GIState* gi) const {
    const glm::mat4 model = entityModel * m_import;   // настройки импорта (см. SetImportTransform)
    Shader& shader = SkinShader();
    shader.Use();
    shader.SetMat4("uModel", model);
    shader.SetMat4("uView", view);
    shader.SetMat4("uProjection", proj);
    shader.SetVec3("uViewPos", viewPos);
    shader.SetInt("uShadingMode", shadingMode);

    // Полное освещение сцены (ambient из скайбокса, солнце, точечные, прожекторы,
    // туман) — теми же uniform'ами, что и статический lit-проход.
    UploadLighting(shader, env);
    // Лампы — тем же кластерным списком, что у статики этого вида.
    if (clusters) clusters->Upload(shader);

    // GI-объём проб сцены — тот же, что освещает статику: персонаж, вошедший в
    // тёмную комнату с красными стенами, обязан потемнеть и покраснеть вместе
    // с ящиком рядом. Раньше объём сюда не проброшен был вовсе, и скелетная
    // модель светилась полусферическим ambient, как будто запекания нет.
    sage::gi::UploadGIVolume(shader, gi);
    // Лайтмапы у скина нет по смыслу: она печётся на неподвижную геометрию, а
    // персонаж двигается. Флаг выставляем ЯВНО — общий фрагмент его читает.
    shader.SetInt("uLightmapEnabled", 0);

    // Тени от солнца: каскады на свои юниты + матрицы — как у статики.
    BindAndUploadShadows(shader, shadows);
    // Отражения — тоже как у статики, включая пустую привязку (см. RenderBatch),
    // и зонд — по положению персонажа, а не камеры (см. ReflectionProbeSet).
    sage::render::UploadReflection(shader, reflections ? reflections->At(glm::vec3(entityModel[3]))
                                                       : sage::render::ReflectionBinding{});

    int boneCount = std::min((int)bones.size(), kMaxBones);
    if (boneCount > 0) {
        shader.SetInt("uSkinned", 1);
        shader.SetMat4Array("uBones", bones.data(), boneCount);
    } else {
        shader.SetInt("uSkinned", 0);
    }

    // Юниты — ТЕ ЖЕ, что у текстурного прохода статики (см. RenderBatch):
    // albedo=0, тень=1, нормали=2, металл=3, шероховатость=4, AO=5, свечение=12.
    // Совпадение не косметическое: фрагментный шейдер у них общий, и разойдись
    // раскладка — один и тот же код читал бы чужие текстуры.
    shader.SetInt("uAlbedoMap", 0);
    shader.SetInt("uNormalMap", 2);
    shader.SetInt("uMetallicMap", 3);
    shader.SetInt("uRoughnessMap", 4);
    shader.SetInt("uAOMap", 5);
    shader.SetInt("uEmissiveMap", kEmissiveUnit);
    // Сдвиг ставится ЯВНО нулём: uniform живёт в программе, а не в вызове, и
    // оставленный от прошлого материала сдвиг уехал бы на персонажа.
    shader.SetVec2("uUVOffset", glm::vec2(0.0f));

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();

    // Один подмеш: материал на юниты, состояние отсечения — и рисуем.
    auto drawSub = [&](const SkinnedSubMesh& sub) {
        const SkinnedMaterial& m = sub.Material;
        UploadMorphs(shader, sub, morphWeights);

        // Двусторонний материал рисуется без отсечения задних граней. У
        // персонажей это почти всегда так: ремни, провода, ткань и «плоские»
        // детали смоделированы одной поверхностью, и с отсечением половина из
        // них пропадает — модель выглядит дырявой.
        device.SetCullMode(m.DoubleSided ? sage::rhi::CullMode::Off
                                         : sage::rhi::CullMode::Back);

        shader.SetVec3("uAlbedoFactor", m.Tint);
        shader.SetFloat("uMetallic", m.Metallic);
        shader.SetFloat("uRoughness", m.Roughness);
        shader.SetFloat("uOpacity", m.Opacity);
        // Просвечивание — у выреза с двумя сторонами (пряди волос, бахрома,
        // листва на скелете): тот же признак тонкой карточки, что у импорта.
        shader.SetFloat("uTranslucency",
                        m.Mode == SkinnedMaterial::Alpha::Mask && m.DoubleSided ? 0.5f : 0.0f);
        shader.SetFloat("uAlphaCutoff", m.Mode == SkinnedMaterial::Alpha::Mask ? m.AlphaCutoff
                                                                               : 0.0f);
        shader.SetInt("uUnlit", m.Unlit ? 1 : 0);
        shader.SetVec3("uEmissive", m.Emissive);
        shader.SetInt("uHasAlbedo", m.Albedo ? 1 : 0);
        shader.SetInt("uHasNormal", m.Normal ? 1 : 0);
        shader.SetInt("uHasMetallic", m.MetallicMap ? 1 : 0);
        shader.SetInt("uHasRoughness", m.RoughnessMap ? 1 : 0);
        shader.SetInt("uHasAO", m.AOMap ? 1 : 0);
        shader.SetVec4("uMetallicMask", m.MetallicMask);
        shader.SetVec4("uRoughnessMask", m.RoughnessMask);
        shader.SetVec4("uAOMask", m.AOMask);
        shader.SetInt("uHasEmissive", m.EmissiveMap ? 1 : 0);
        if (m.Albedo) m.Albedo->Bind(0);
        if (m.Normal) m.Normal->Bind(2);
        if (m.MetallicMap) m.MetallicMap->Bind(3);
        if (m.RoughnessMap) m.RoughnessMap->Bind(4);
        if (m.AOMap) m.AOMap->Bind(5);
        if (m.EmissiveMap) m.EmissiveMap->Bind(kEmissiveUnit);
        sub.Mesh->Draw();
    };

    // ДВА ПРОХОДА: сначала непрозрачное, потом полупрозрачное.
    //
    // Смешивание не коммутативно: стекло очков, нарисованное раньше глаз за
    // ним, просто закрасит их собой. Тот же порядок, что у статики (см.
    // RenderBatch): непрозрачное пишет глубину, полупрозрачное её только
    // читает — иначе прозрачная деталь загородила бы всё, что за ней.
    for (const auto& sub : m_subMeshes) {
        if (sub.Material.Transparent()) continue;
        drawSub(sub);
    }

    bool blending = false;
    for (const auto& sub : m_subMeshes) {
        if (!sub.Material.Transparent()) continue;
        if (!blending) {
            device.SetBlend(true);
            device.SetBlendMode(sage::rhi::GraphicsDevice::BlendMode::Alpha);
            device.SetDepthWrite(false);
            blending = true;
        }
        drawSub(sub);
    }
    if (blending) {
        device.SetBlend(false);
        device.SetDepthWrite(true);
    }
    // Состояние отсечения возвращаем общему проходу: следующий, кто рисует,
    // вправе считать, что его никто не менял (то же правило, что в RenderBatch).
    device.SetCullMode(sage::rhi::CullMode::Back);
}


int SkinnedModel::BorrowClipsFrom(const SkinnedModel& source) {
    if (source.m_clips.empty() || m_skeleton.Count() == 0) return 0;
    const sage::anim::BoneMap map = sage::anim::MapByName(source.m_skeleton, m_skeleton);
    if (map.Empty()) {
        LOG_WARN("Anim") << "Ретаргет: ни одна кость не совпала по имени ("
                         << source.m_skeleton.Count() << " -> " << m_skeleton.Count() << ")";
        return 0;
    }
    int added = 0;
    for (const sage::anim::AnimationClip& clip : source.m_clips) {
        sage::anim::AnimationClip out = sage::anim::Retarget(clip, source.m_skeleton, m_skeleton, map);
        if (out.Channels.empty()) continue; // переносить нечего — не плодим пустышки
        m_clips.push_back(std::move(out));
        ++added;
    }
    LOG_INFO("Anim") << "Ретаргет: перенесено клипов " << added << " из "
                     << source.m_clips.size() << ", костей сопоставлено " << map.Mapped()
                     << " из " << source.m_skeleton.Count();
    return added;
}

void SkinnedModel::DrawDepth(const glm::mat4& entityModel, const glm::mat4& lightMatrix,
                             const std::vector<glm::mat4>& bones,
                             const std::vector<float>* morphWeights) const {
    const glm::mat4 model = entityModel * m_import;   // настройки импорта (см. SetImportTransform)
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

    shader.SetInt("uAlbedoMap", 0);
    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    for (const auto& sub : m_subMeshes) {
        // Тень обязана повторять ту же форму, что и сам меш, иначе лицо и его
        // тень «разъедутся» при любом выражении.
        UploadMorphs(shader, sub, morphWeights);
        const SkinnedMaterial& m = sub.Material;
        const bool cut = m.Mode == SkinnedMaterial::Alpha::Mask && m.Albedo;
        shader.SetFloat("uAlphaCutoff", cut ? m.AlphaCutoff : 0.0f);
        if (cut) m.Albedo->Bind(0);
        // Двустороннее отбрасывает тень обеими сторонами — как и рисуется.
        device.SetCullMode(m.DoubleSided ? sage::rhi::CullMode::Off : sage::rhi::CullMode::Back);
        sub.Mesh->Draw();
    }
    device.SetCullMode(sage::rhi::CullMode::Back);
}

void SkinnedModel::DrawSilhouette(const glm::mat4& entityModel, const glm::mat4& viewProj,
                                  const std::vector<glm::mat4>& bones,
                                  const std::vector<float>* morphWeights) const {
    const glm::mat4 model = entityModel * m_import;   // настройки импорта (см. SetImportTransform)
    Shader& shader = SkinSilhouetteShader();
    shader.Use();
    // uLightSpace — имя из общего с depth-проходом вершинного шейдера; здесь в
    // него едет обычная матрица вида-проекции камеры.
    shader.SetMat4("uLightSpace", viewProj);
    shader.SetMat4("uModel", model);
    shader.SetVec3("uColor", glm::vec3(1.0f));

    int boneCount = std::min((int)bones.size(), kMaxBones);
    if (boneCount > 0) {
        shader.SetInt("uSkinned", 1);
        shader.SetMat4Array("uBones", bones.data(), boneCount);
    } else {
        shader.SetInt("uSkinned", 0);
    }

    for (const auto& sub : m_subMeshes) {
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
    // Демо-щупальцу — обычный материал: свой цвет и матовая поверхность.
    sub.Material.Tint = {0.35f, 0.75f, 0.55f};
    sub.Material.Roughness = 0.6f;

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

// Сборка на видеокарте: вершины в буферы, пиксели в текстуры, дельты морфов в
// float-текстуры. Отсюда и только отсюда трогается GPU.
std::unique_ptr<SkinnedModel> SkinnedModel::BuildFromData(ModelData& data) {
    auto model = std::unique_ptr<SkinnedModel>(new SkinnedModel());
    model->m_skeleton = std::move(data.Skeleton);
    model->m_clips = std::move(data.Clips);
    model->m_morphNames = std::move(data.MorphNames);
    model->m_morphDefaults = std::move(data.MorphDefaults);

    // ЦВЕТ И ДАННЫЕ — разными текстурами. Картинка, которая служит альбедо или
    // свечением, хранится в sRGB и на выборке отдаёт линейный цвет (см.
    // rhi::Texture2DDesc::Srgb); карты нормалей и ORM — как есть. Одна и та
    // же картинка в обеих ролях (бывает у палитр) получает две текстуры.
    std::vector<bool> usedAsColour(data.Images.size(), false), usedAsData(data.Images.size(), false);
    for (const ModelSubMeshData& src : data.SubMeshes) {
        const ModelSubMeshMaterial& m = src.Material;
        for (int c : {m.Albedo, m.EmissiveMap})
            if (c >= 0 && c < (int)data.Images.size()) usedAsColour[(size_t)c] = true;
        for (int d : {m.Normal, m.MetallicMap, m.RoughnessMap, m.AOMap})
            if (d >= 0 && d < (int)data.Images.size()) usedAsData[(size_t)d] = true;
    }
    auto makeTexture = [](const ModelImage& img, bool srgb) {
        // Палитра — тот же атлас: мип-уровни усредняют её целиком, а у
        // палитровой модели 90% текстуры пусто, и на дальних уровнях от цвета
        // не остаётся ничего. Мелкие текстуры (палитры, рисунки по пикселям) берём
        // ближайшим соседом и без мипов; крупные — как раньше.
        const bool palette = img.Width <= 128 && img.Height <= 128;
        return std::make_shared<Texture>(img.Pixels.data(), img.Width, img.Height,
                                         palette ? TextureFilter::Nearest : TextureFilter::Trilinear,
                                         !palette, srgb);
    };
    std::vector<std::shared_ptr<Texture>> textures(data.Images.size());       // данные
    std::vector<std::shared_ptr<Texture>> colourTextures(data.Images.size()); // цвет (sRGB)
    for (size_t i = 0; i < data.Images.size(); ++i) {
        if (usedAsColour[i]) colourTextures[i] = makeTexture(data.Images[i], true);
        if (usedAsData[i] || !usedAsColour[i]) textures[i] = makeTexture(data.Images[i], false);
    }

    sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
    for (ModelSubMeshData& src : data.SubMeshes) {
        SkinnedSubMesh sub;
        sub.Mesh = std::make_shared<SkinnedMesh>(src.Vertices, src.Indices);

        // Материал переносится ЦЕЛИКОМ, карта за картой. Пока сюда доезжала
        // одна текстура из шести, у скина не могло быть ни рельефа, ни
        // затенения, ни свечения — и это было не решение, а потеря данных.
        const ModelSubMeshMaterial& m = src.Material;
        auto pick = [&](int index) -> std::shared_ptr<Texture> {
            return index >= 0 && index < (int)textures.size() ? textures[(size_t)index] : nullptr;
        };
        auto pickColour = [&](int index) -> std::shared_ptr<Texture> {
            return index >= 0 && index < (int)colourTextures.size() ? colourTextures[(size_t)index]
                                                                    : nullptr;
        };
        sub.Material.Name = m.Name;
        sub.Material.Albedo = pickColour(m.Albedo);
        sub.Material.Normal = pick(m.Normal);
        sub.Material.MetallicMap = pick(m.MetallicMap);
        sub.Material.RoughnessMap = pick(m.RoughnessMap);
        sub.Material.AOMap = pick(m.AOMap);
        sub.Material.EmissiveMap = pickColour(m.EmissiveMap);
        auto mask = [](int channel) {
            glm::vec4 v(0.0f);
            v[channel >= 0 && channel < 4 ? channel : 0] = 1.0f;
            return v;
        };
        sub.Material.MetallicMask = mask(m.MetallicChannel);
        sub.Material.RoughnessMask = mask(m.RoughnessChannel);
        sub.Material.AOMask = mask(m.AOChannel);
        sub.Material.Tint = m.Tint;
        sub.Material.Opacity = m.Opacity;
        sub.Material.Metallic = m.Metallic;
        sub.Material.Roughness = m.Roughness;
        sub.Material.Emissive = m.Emissive;
        sub.Material.Mode = m.AlphaMode == 2   ? SkinnedMaterial::Alpha::Blend
                            : m.AlphaMode == 1 ? SkinnedMaterial::Alpha::Mask
                                               : SkinnedMaterial::Alpha::Opaque;
        sub.Material.AlphaCutoff = m.AlphaCutoff;
        sub.Material.DoubleSided = m.DoubleSided;
        sub.Material.Unlit = m.Unlit;

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

ModelData ParseSkinnedModelFile(const std::string& path) {
    // ФОРМАТ ВЫБИРАЕТ ПУТЬ РАЗБОРА. glTF читает tinygltf, FBX — свой разбор
    // (assets/import/FbxSkin.h): у FBX скин, кости и клипы лежат совсем иначе,
    // и делать вид, что это один и тот же файл, нельзя. Пока пути не было,
    // персонаж из FBX (а это всё, что отдают 3D-редакторы, сервисы риггинга и
    // ассет-сторы по умолчанию) получал «не загрузить glTF … parse error»:
    // сообщение о ЧУЖОМ формате, из которого следовал вывод «моя модель
    // движку не подходит».
    //
    // БЕЗ ВИДЕОКАРТЫ — и это главное, зачем разбор отделён от загрузки. Так его
    // можно прогнать в тесте и в конвертере: проверять скелет, веса и клипы по
    // картинке — то же самое, что не проверять.
    ModelData data;
    std::string ext = fs::path(path).extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".fbx") {
        std::string err;
        if (!sage::assets::ImportFbxSkinned(path, data, err))
            throw std::runtime_error("SkinnedModel: " + err);
    } else {
        data = ParseGltfSkinned(path);
    }
    LOG_INFO("Anim") << "SkinnedModel разобран: " << path << " (костей "
                     << data.Skeleton.Count() << ", submesh " << data.SubMeshes.size()
                     << ", клипов " << data.Clips.size() << ")";
    return data;
}

namespace {

// Настройки импорта у скелетной модели — матрицей поверх её вершин, а не
// правкой самих вершин: кости и клипы остались бы в прежнем размере, и
// персонаж разъехался бы на первом же кадре анимации. Границы — по вершинам
// привязки: ими же решается и «модель в сантиметрах» (см. ModelLoader.h).
std::unique_ptr<SkinnedModel> WithImportSettings(std::unique_ptr<SkinnedModel> model,
                                                 const ModelData& data, const std::string& path) {
    if (!model) return model;
    glm::vec3 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    for (const ModelSubMeshData& sub : data.SubMeshes)
        for (const SkinnedVertex& v : sub.Vertices) {
            lo = glm::min(lo, v.Position);
            hi = glm::max(hi, v.Position);
        }
    const ::ModelLoader::ImportSettings s = ::ModelLoader::ResolveImportSettings(path, lo, hi);
    model->SetImportTransform(::ModelLoader::ImportMatrix(s, lo, hi));
    return model;
}

} // namespace

std::unique_ptr<SkinnedModel> SkinnedModel::Load(const std::string& path) {
    // Кэш пробуется ПЕРВЫМ. Промах, устаревший или битый кэш — не ошибка:
    // молча разбираем исходник и перезаписываем кэш. Неверный кэш никогда не
    // должен приводить к неверной картинке, а его отсутствие — к отказу.
    ModelData data;
    if (sage::assets::ReadModelCache(path, data) && !data.Empty()) {
        LOG_INFO("Anim") << "SkinnedModel из кэша: " << path << " (костей "
                         << data.Skeleton.Count() << ", submesh " << data.SubMeshes.size()
                         << ", клипов " << data.Clips.size() << ")";
        return WithImportSettings(BuildFromData(data), data, path);
    }

    // ФОРМАТ ВЫБИРАЕТ ПУТЬ РАЗБОРА. glTF читает tinygltf, FBX — свой разбор
    // (assets/import/FbxSkin.h): у FBX скин, кости и клипы лежат совсем иначе,
    // и делать вид, что это один и тот же файл, нельзя. Пока пути не было,
    // персонаж из FBX (а это всё, что отдают 3D-редакторы, сервисы риггинга и
    // ассет-сторы по умолчанию) получал «не загрузить glTF … parse error»:
    // сообщение о ЧУЖОМ формате, из которого следовал вывод «моя модель
    // движку не подходит».
    data = ParseSkinnedModelFile(path);
    sage::assets::WriteModelCache(path, data);
    return WithImportSettings(BuildFromData(data), data, path);
}

} // namespace sage::render
