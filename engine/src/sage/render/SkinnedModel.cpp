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

#include "sage/assets/AssetCache.h"
#include <filesystem>
#include <cctype>

#include "sage/assets/import/FbxSkin.h"
#include "sage/render/ModelData.h"
#include "sage/anim/Retarget.h"
#include "sage/gi/GIUpload.h"

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

void SkinnedModel::Draw(const glm::mat4& entityModel, const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& viewPos, const LightingEnvironment& env,
                        const std::vector<glm::mat4>& bones,
                        const ShadowBinding& shadows,
                        const sage::render::ReflectionBinding* reflections,
                        const std::vector<float>* morphWeights, int shadingMode) const {
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

    // GI: юниты сэмплеров общего PBR-блока обязательны всегда (конфликт типов
    // сэмплеров на юните 0 — см. SetGISamplerUnits). Объём проб к скиннингу
    // пока не подключён (сюда не проброшена сцена) — непрямой свет остаётся
    // полусферическим ambient.
    sage::gi::SetGISamplerUnits(shader);
    shader.SetInt("uGIVolumeEnabled", 0);
    // Лайтмапы у скина нет по смыслу: она печётся на неподвижную геометрию, а
    // персонаж двигается. Флаг выставляем ЯВНО — общий фрагмент его читает.
    shader.SetInt("uLightmapEnabled", 0);

    // Тени от солнца: каскады на свои юниты + матрицы — как у статики.
    BindAndUploadShadows(shader, shadows);
    // Отражения — тоже как у статики, включая пустую привязку (см. RenderBatch).
    sage::render::UploadReflection(shader, reflections ? *reflections
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
    shader.SetVec2("uUVScale", glm::vec2(1.0f));  // повтор развёртки задаёт файл модели
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

// ============================================================================
//  Загрузка из glTF/GLB (skins + animations)
// ============================================================================
namespace {

bool GltfSkinImageLoader(tinygltf::Image* image, const int, std::string* err, std::string*,
                         int, int, const unsigned char* bytes, int size, void*) {
    // Переворот выключаем ЯВНО и ПОТОЧНО. У glTF соглашение обратное обычным
    // текстурам: v=0 — ВЕРХНЯЯ строка картинки. Унаследовав чужое значение,
    // модель получает текстуру вверх ногами — и это не «картинка зеркальная»,
    // а совсем другие тексели: у палитровых моделей все цвета лежат в одном
    // углу, и после переворота UV попадают в пустоту, персонаж становится
    // чёрным.
    //
    // Именно поточный флаг (_thread), а не глобальный: фоновый загрузчик
    // текстур ставит себе true, и с глобальным флагом он перебивал бы наш
    // false ПОСРЕДИ разбора модели — «через раз», в зависимости от того, что
    // грузилось рядом (см. ResourceManager::DecodeImageFile).
    stbi_set_flip_vertically_on_load_thread(false);
    int w, h, comp;
    unsigned char* data = stbi_load_from_memory(bytes, size, &w, &h, &comp, 4);
    if (!data) { if (err) *err += "Не удалось декодировать изображение glTF\n"; return false; }
    image->width = w; image->height = h; image->component = 4; image->bits = 8;
    image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image->image.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return true;
}

// Аксессоры читает ОБЩИЙ читатель (assets/import/GltfAccessor.h) — тот же, что
// у статического импорта. Здесь раньше лежала своя пара функций БЕЗ единой
// проверки: `m.bufferViews[acc.bufferView]` при bufferView = -1 (а это законный
// разреженный аксессор, которым Blender пишет каждый ключ формы) означало
// обращение за начало вектора и падение редактора на ровном месте.
using sage::assets::gltf::ReadFloats;
using sage::assets::gltf::ReadUInts;

// Разложение матрицы узла в перенос/поворот/масштаб.
//
// Нужно потому, что glTF разрешает задать узел ЛИБО тремя полями, ЛИБО
// матрицей, а скелет движка хранит именно TRS: каналы анимации правят перенос,
// поворот и масштаб по отдельности, и хранить вместо них матрицу значило бы
// раскладывать её обратно на каждом кадре. Кость, заданная матрицей, до этого
// молча получала единичный TRS — скелет складывался в точку.
void DecomposeTRS(const glm::mat4& m, glm::vec3& t, glm::quat& r, glm::vec3& s) {
    t = glm::vec3(m[3]);
    glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
    s = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));
    // Зеркальный масштаб (определитель < 0) поворотом не выражается: отдаём
    // знак одной оси, как это делает всякое разложение, — иначе кватернион
    // вышел бы «вывернутым» и кость смотрела бы в другую сторону.
    if (glm::determinant(glm::mat3(m)) < 0.0f) s.x = -s.x;
    const float ex = 1e-8f;
    c0 /= (std::fabs(s.x) > ex ? s.x : 1.0f);
    c1 /= (std::fabs(s.y) > ex ? s.y : 1.0f);
    c2 /= (std::fabs(s.z) > ex ? s.z : 1.0f);
    r = glm::normalize(glm::quat_cast(glm::mat3(c0, c1, c2)));
}

// Нормали по треугольникам — для частей, в которых их нет в файле.
void RebuildNormals(std::vector<SkinnedVertex>& verts, const std::vector<unsigned int>& indices) {
    for (SkinnedVertex& v : verts) v.Normal = glm::vec3(0.0f);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const unsigned a = indices[i], b = indices[i + 1], c = indices[i + 2];
        if (a >= verts.size() || b >= verts.size() || c >= verts.size()) continue;
        const glm::vec3 n = glm::cross(verts[b].Position - verts[a].Position,
                                       verts[c].Position - verts[a].Position);
        verts[a].Normal += n; verts[b].Normal += n; verts[c].Normal += n;
    }
    for (SkinnedVertex& v : verts) {
        v.Normal = glm::dot(v.Normal, v.Normal) > 1e-12f ? glm::normalize(v.Normal)
                                                   : glm::vec3(0.0f, 1.0f, 0.0f);
    }
}

// Касательные по развёртке — для карты нормалей, когда TANGENT в файле нет.
// Классическое накопление по треугольникам с ортогонализацией Грама-Шмидта:
// та же формула, что у статических мешей (см. render/Mesh.cpp), иначе одна и
// та же модель освещалась бы по-разному скином и статикой.
void RebuildTangents(std::vector<SkinnedVertex>& verts, const std::vector<unsigned int>& indices) {
    std::vector<glm::vec3> tan(verts.size(), glm::vec3(0.0f));
    std::vector<glm::vec3> bit(verts.size(), glm::vec3(0.0f));
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const unsigned ia = indices[i], ib = indices[i + 1], ic = indices[i + 2];
        if (ia >= verts.size() || ib >= verts.size() || ic >= verts.size()) continue;
        const glm::vec3 e1 = verts[ib].Position - verts[ia].Position;
        const glm::vec3 e2 = verts[ic].Position - verts[ia].Position;
        const glm::vec2 d1 = verts[ib].TexCoords - verts[ia].TexCoords;
        const glm::vec2 d2 = verts[ic].TexCoords - verts[ia].TexCoords;
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;   // вырожденная развёртка
        const float f = 1.0f / det;
        const glm::vec3 t = f * (d2.y * e1 - d1.y * e2);
        const glm::vec3 b = f * (d1.x * e2 - d2.x * e1);
        tan[ia] += t; tan[ib] += t; tan[ic] += t;
        bit[ia] += b; bit[ib] += b; bit[ic] += b;
    }
    for (size_t i = 0; i < verts.size(); ++i) {
        const glm::vec3 n = verts[i].Normal;
        glm::vec3 t = tan[i] - n * glm::dot(n, tan[i]);
        if (glm::dot(t, t) < 1e-12f) {
            // Треугольников с годной развёрткой не нашлось — берём любую ось,
            // перпендикулярную нормали: карта нормалей всё равно ляжет криво,
            // но кадр не получит NaN.
            t = std::fabs(n.x) < 0.9f ? glm::cross(n, glm::vec3(1, 0, 0))
                                      : glm::cross(n, glm::vec3(0, 1, 0));
        }
        t = glm::normalize(t);
        const float w = glm::dot(glm::cross(n, t), bit[i]) < 0.0f ? -1.0f : 1.0f;
        verts[i].Tangent = glm::vec4(t, w);
    }
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
//
// ЧТО ЗДЕСЬ ГЛАВНОЕ. Разбор собирает модель ЦЕЛИКОМ, а не только её скиновую
// часть. Раньше отбор шёл по одному признаку — есть ли у примитива JOINTS_0, —
// и всё остальное молча выбрасывалось. Для персонажей это не мелочь: жёсткие
// детали (зубы, глазницы, провода, панели эндоскелета, болты) риггят НЕ весами,
// а привязкой узла к кости — это дешевле и точнее. У проверочной модели
// Springtrap скиновых примитивов 18 из 179: в кадр попадала голова и пара
// кусков костюма, а остальные девять десятых модели не рисовались вовсе.
// Выглядело это как «модель бледная и разваливается».
static ModelData ParseGltf(const std::string& path) {
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(&GltfSkinImageLoader, nullptr);
    tinygltf::Model g;
    std::string err, warn;
    // Двоичный файл или текстовый — решает общий вход по содержимому
    // (assets/import/GltfFile.h): здесь это решалось по последним четырём
    // буквам имени, и «.GLB» уже уходило в текстовый разбор.
    bool ok = sage::assets::LoadGltfFile(loader, g, path, err, warn);
    if (!warn.empty()) LOG_WARN("Anim") << "glTF (" << path << "): " << warn;
    if (!ok) throw std::runtime_error("SkinnedModel: не загрузить glTF " + path + ": " + err);
    if (g.skins.empty()) throw std::runtime_error("SkinnedModel: в файле нет скина: " + path);

    ModelData data;

    // --- Кости ВСЕХ скинов файла — одним скелетом -----------------------------
    //
    // ПОЧЕМУ ВСЕХ, А НЕ ПЕРВОГО. Номер кости в JOINTS_n — это место в списке
    // ТОГО скина, на который ссылается узел с мешем (node.skin), а не общий
    // номер по файлу. Пока читался один g.skins[0], вершины остальных скинов
    // попадали в чужие кости: делить персонажа на скины (тело, одежда, волосы,
    // оружие) — обычная работа экспортёра, и такая модель приезжала с частями,
    // размазанными по случайным костям, либо теряла их вовсе. Сводим кости всех
    // скинов в один скелет и держим для каждого скина перевод «его номер → наш».
    std::vector<int> jointNodes;                 // узел каждой нашей кости
    std::unordered_map<int, int> nodeToJoint;    // узел -> наша кость
    std::vector<glm::mat4> invBind;              // обратные bind-матрицы по нашим костям
    std::vector<char> invBindKnown;
    std::vector<std::vector<int>> skinMaps(g.skins.size());
    for (size_t si = 0; si < g.skins.size(); ++si) {
        const tinygltf::Skin& skin = g.skins[si];
        const std::vector<float> ibm = skin.inverseBindMatrices >= 0
                                           ? ReadFloats(g, skin.inverseBindMatrices, 16)
                                           : std::vector<float>();
        std::vector<int>& map = skinMaps[si];
        map.assign(skin.joints.size(), -1);
        for (size_t j = 0; j < skin.joints.size(); ++j) {
            const int node = skin.joints[j];
            // Кость, ссылающаяся на несуществующий узел, — битый файл. Место в
            // переводной таблице остаётся пустым, и вершина просто теряет это
            // влияние (см. ResolveInfluences) — вместо чтения g.nodes за границей.
            if (node < 0 || node >= (int)g.nodes.size()) continue;
            auto it = nodeToJoint.find(node);
            int joint;
            if (it == nodeToJoint.end()) {
                joint = (int)jointNodes.size();
                jointNodes.push_back(node);
                nodeToJoint[node] = joint;
                invBind.emplace_back(1.0f);
                invBindKnown.push_back(0);
            } else {
                joint = it->second;
            }
            map[j] = joint;
            // Один узел в двух скинах — обычное дело (общий корень скелета), и
            // матрица привязки у него там одна и та же. Берём первую: кость в
            // общем скелете одна, второй записи для неё места нет.
            if (!invBindKnown[(size_t)joint] && (j + 1) * 16 <= ibm.size()) {
                invBind[(size_t)joint] = glm::make_mat4(&ibm[j * 16]);
                invBindKnown[(size_t)joint] = 1;
            }
        }
    }
    const int jointCount = (int)jointNodes.size();
    if (jointCount > kMaxBones) {
        LOG_WARN("Anim") << "SkinnedModel: костей " << jointCount << " > " << kMaxBones
                         << " — лишние не поместятся в палитру";
    }
    auto jointNode = [&](int j) { return (j >= 0 && j < jointCount) ? jointNodes[(size_t)j] : -1; };

    // --- Иерархия узлов: родители и мировые матрицы ---------------------------
    //
    // Нужна целиком, а не только по костям: жёсткая деталь может висеть на
    // кости через несколько промежуточных узлов со своими поворотами, и её
    // место в модели задаёт вся цепочка.
    const int nodeCount = (int)g.nodes.size();
    std::vector<int> parent((size_t)nodeCount, -1);
    for (int ni = 0; ni < nodeCount; ++ni) {
        for (int child : g.nodes[(size_t)ni].children) {
            if (child >= 0 && child < nodeCount) parent[(size_t)child] = ni;
        }
    }
    // Мировая матрица узла — произведение локальных от корня. Считается лениво
    // и запоминается: у модели бывают сотни узлов, и подниматься по цепочке для
    // каждого значило бы квадратичную работу на ровном месте.
    std::vector<glm::mat4> world((size_t)nodeCount, glm::mat4(1.0f));
    std::vector<char> worldReady((size_t)nodeCount, 0);
    std::function<glm::mat4(int)> worldOf = [&](int node) -> glm::mat4 {
        if (node < 0 || node >= nodeCount) return glm::mat4(1.0f);
        if (worldReady[(size_t)node]) return world[(size_t)node];
        // Отметку ставим ДО рекурсии: битый файл с циклом в дереве узлов иначе
        // ушёл бы в бесконечный спуск и повесил загрузку.
        worldReady[(size_t)node] = 1;
        world[(size_t)node] = NodeLocal(g.nodes[(size_t)node]);
        const int p = parent[(size_t)node];
        if (p >= 0) world[(size_t)node] = worldOf(p) * world[(size_t)node];
        return world[(size_t)node];
    };

    // Скелет: TRS из узлов + родитель из иерархии узлов.
    Skeleton& sk = data.Skeleton;
    sk.Joints.resize(jointCount);
    const tinygltf::Node kEmptyNode;
    for (int i = 0; i < jointCount; ++i) {
        const int ni = jointNode(i);
        const tinygltf::Node& n = ni >= 0 ? g.nodes[(size_t)ni] : kEmptyNode;
        Joint& j = sk.Joints[i];
        j.Name = n.name;
        j.InverseBind = invBind[i];
        if (n.matrix.size() == 16) {
            // Узел задан МАТРИЦЕЙ, а не TRS. Так пишут многие экспортёры, и
            // раньше такая кость получала единичный TRS — то есть скелет
            // складывался в точку, а модель сминалась в комок.
            DecomposeTRS(NodeLocal(n), j.Translation, j.Rotation, j.Scale);
        } else {
            j.Translation = n.translation.size() == 3 ? glm::vec3(n.translation[0], n.translation[1], n.translation[2]) : glm::vec3(0.0f);
            j.Rotation = n.rotation.size() == 4 ? glm::quat((float)n.rotation[3], (float)n.rotation[0], (float)n.rotation[1], (float)n.rotation[2]) : glm::quat(1, 0, 0, 0);
            j.Scale = n.scale.size() == 3 ? glm::vec3(n.scale[0], n.scale[1], n.scale[2]) : glm::vec3(1.0f);
        }
        j.Parent = -1;
    }
    // Родитель: у кого этот узел в children.
    for (int ni = 0; ni < nodeCount; ++ni) {
        for (int child : g.nodes[(size_t)ni].children) {
            auto itC = nodeToJoint.find(child);
            auto itP = nodeToJoint.find(ni);
            if (itC != nodeToJoint.end() && itP != nodeToJoint.end())
                sk.Joints[itC->second].Parent = itP->second;
        }
    }

    // Трансформ НАД скелетом (см. Skeleton::Root): мировая матрица родителя
    // корневой кости. Берём у первой кости без родителя — у скина он общий.
    for (int i = 0; i < jointCount; ++i) {
        if (sk.Joints[i].Parent >= 0) continue;
        const int ni = jointNode(i);
        if (ni < 0) continue;
        sk.Root = worldOf(parent[(size_t)ni]);
        break;
    }

    // Матрица кости В ПОЗЕ ПРИВЯЗКИ — ровно то, что окажется в палитре, пока
    // клип не начал двигать кости (см. Animator: Root * цепочка * InverseBind).
    //
    // ЗАЧЕМ ОНА НУЖНА. К этой матрице приводятся жёсткие детали: чтобы деталь
    // встала на своё место, её вершины надо задать так, чтобы палитра вернула
    // их обратно. Считать это «по формуле» нельзя — экспортёры расходятся в
    // том, ОТНОСИТЕЛЬНО ЧЕГО заданы обратные bind-матрицы: спецификация велит
    // от корня сцены, Sketchfab и часть выгрузок Blender пишут от корня
    // скелета. Разница — ровно поворот над скелетом, и угадавший неверно
    // получает половину модели, развёрнутую на 180°. Поэтому не угадываем, а
    // берём то, что реально получится в кадре.
    std::vector<glm::mat4> bindBone((size_t)jointCount, glm::mat4(1.0f));
    {
        std::vector<glm::mat4> bindGlobal((size_t)jointCount, glm::mat4(1.0f));
        for (int i = 0; i < jointCount; ++i) {
            glm::mat4 g = sk.Joints[i].LocalMatrix();
            for (int p = sk.Joints[i].Parent; p >= 0; p = sk.Joints[p].Parent)
                g = sk.Joints[p].LocalMatrix() * g;
            bindGlobal[(size_t)i] = sk.Root * g;
            bindBone[(size_t)i] = bindGlobal[(size_t)i] * sk.Joints[i].InverseBind;
        }
    }

    // --- Изображения и их каналы ---------------------------------------------
    //
    // Картинка забирается РАСПАКОВАННОЙ и запоминается по индексу glTF: одна
    // текстура, использованная десятью материалами, не должна лежать в данных
    // десять раз — ни в памяти, ни в кэше.
    std::unordered_map<int, int> imageSlots;
    auto takeImage = [&](int texIndex) -> int {
        if (texIndex < 0 || texIndex >= (int)g.textures.size()) return -1;
        int img = g.textures[(size_t)texIndex].source;
        auto it = imageSlots.find(img);
        if (it != imageSlots.end()) return it->second;
        if (img < 0 || img >= (int)g.images.size() || g.images[(size_t)img].image.empty()) return -1;
        ModelImage out;
        out.Width = g.images[(size_t)img].width;
        out.Height = g.images[(size_t)img].height;
        out.Pixels = g.images[(size_t)img].image;
        data.Images.push_back(std::move(out));
        const int slot = (int)data.Images.size() - 1;
        imageSlots[img] = slot;
        return slot;
    };

    // --- Материалы ------------------------------------------------------------
    // Разбираются ОДИН раз на материал, а не на каждый примитив: у проверочной
    // модели 179 примитивов на 17 материалов, и раскладывать одну и ту же
    // упакованную карту по каналам 179 раз значило бы 170 лишних проходов по
    // мегапиксельным картинкам.
    std::vector<ModelSubMeshMaterial> materials((size_t)g.materials.size());
    for (size_t mi = 0; mi < g.materials.size(); ++mi) {
        const tinygltf::Material& m = g.materials[mi];
        ModelSubMeshMaterial& out = materials[mi];
        out.Name = m.name;
        const tinygltf::PbrMetallicRoughness& pbr = m.pbrMetallicRoughness;
        if (pbr.baseColorFactor.size() >= 4) {
            out.Tint = glm::vec3((float)pbr.baseColorFactor[0], (float)pbr.baseColorFactor[1],
                                 (float)pbr.baseColorFactor[2]);
            out.Opacity = (float)pbr.baseColorFactor[3];
        }
        out.Metallic = (float)pbr.metallicFactor;    // glTF по умолчанию 1.0
        out.Roughness = (float)pbr.roughnessFactor;  // glTF по умолчанию 1.0
        out.Albedo = takeImage(pbr.baseColorTexture.index);
        out.Normal = takeImage(m.normalTexture.index);
        // Металличность в B, шероховатость в G, затенение в R — так пакует
        // glTF, и это ОДНА текстура. Берём её один раз и говорим шейдеру, из
        // какого канала читать: три копии одной карты 4K не нужны ни памяти,
        // ни кэшу (см. uMetallicMask в PbrShader.h).
        out.MetallicMap = takeImage(pbr.metallicRoughnessTexture.index);
        out.MetallicChannel = 2;
        out.RoughnessMap = takeImage(pbr.metallicRoughnessTexture.index);
        out.RoughnessChannel = 1;
        out.AOMap = takeImage(m.occlusionTexture.index);
        out.AOChannel = 0;
        if (m.emissiveFactor.size() >= 3) {
            out.Emissive = glm::vec3((float)m.emissiveFactor[0], (float)m.emissiveFactor[1],
                                     (float)m.emissiveFactor[2]);
        }
        out.EmissiveMap = takeImage(m.emissiveTexture.index);
        // KHR_materials_emissive_strength: яркость свечения выше единицы. Без
        // неё светящиеся детали (глаза) выходят просто светлыми — bloom их не
        // подхватывает, потому что подхватывать нечего.
        auto ext = m.extensions.find("KHR_materials_emissive_strength");
        if (ext != m.extensions.end() && ext->second.Has("emissiveStrength")) {
            const tinygltf::Value& v = ext->second.Get("emissiveStrength");
            const float strength = v.IsNumber() ? (float)v.GetNumberAsDouble() : 1.0f;
            out.Emissive *= strength;
        }
        if (m.alphaMode == "MASK") out.AlphaMode = 1;
        else if (m.alphaMode == "BLEND") out.AlphaMode = 2;
        out.AlphaCutoff = (float)m.alphaCutoff;
        out.DoubleSided = m.doubleSided;
        out.Unlit = m.extensions.count("KHR_materials_unlit") > 0;
    }

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

    // --- Сбор примитивов ------------------------------------------------------
    //
    // Идём по УЗЛАМ, а не по мешам: у жёсткой детали место в модели задаёт узел,
    // а один и тот же меш может стоять в нескольких узлах (провода, болты).
    // Обход по мешам нарисовал бы такую деталь один раз и не в том месте.
    int skinnedParts = 0, rigidParts = 0, skippedParts = 0;
    for (int ni = 0; ni < nodeCount; ++ni) {
        const tinygltf::Node& node = g.nodes[(size_t)ni];
        if (node.mesh < 0 || node.mesh >= (int)g.meshes.size()) continue;
        const tinygltf::Mesh& mesh = g.meshes[(size_t)node.mesh];

        // Узел со скином — вершины уже в системе координат скелета (так велит
        // спецификация: их место задают кости, а не трансформ узла).
        // Перевод «номер кости в этом скине -> наша кость»: у каждого скина он
        // свой (см. сбор костей выше).
        const std::vector<int>* jointMap =
            (node.skin >= 0 && node.skin < (int)skinMaps.size()) ? &skinMaps[(size_t)node.skin]
                                                                 : nullptr;
        const bool skinned = jointMap != nullptr;

        // Жёсткая деталь: ищем ближайшую кость ВВЕРХ по дереву. Она и будет
        // единственной костью этой детали с весом 1 — тот же скиннинг, просто
        // вырожденный, поэтому дальше по конвейеру разницы нет вообще.
        int rigidJoint = -1;
        if (!skinned) {
            for (int p = ni; p >= 0; p = parent[(size_t)p]) {
                auto it = nodeToJoint.find(p);
                if (it != nodeToJoint.end()) { rigidJoint = it->second; break; }
            }
            // Кость за палитрой шейдера (uBones[kMaxBones]) — это чтение чужой
            // памяти на видеокарте. У модели с сотнями костей такое бывает, и
            // деталь лучше оставить на месте, чем отправить в палитру наугад.
            if (rigidJoint >= kMaxBones) rigidJoint = -1;
        }
        // Вершины жёсткой детали задаём так, чтобы палитра кости вернула их
        // ровно туда, где деталь стоит в файле: v' = bindBone(кость)⁻¹ · W · v.
        // В позе привязки палитра даст обратно W·v, а при движении кости
        // деталь поедет вместе с ней — чего и ждут от привязки к кости.
        const glm::mat4 rigidXf =
            skinned ? glm::mat4(1.0f)
                    : glm::inverse(rigidJoint >= 0 ? bindBone[(size_t)rigidJoint] : sk.Root) *
                          worldOf(ni);
        const glm::mat3 rigidNrm = glm::mat3(glm::transpose(glm::inverse(rigidXf)));

        for (const auto& prim : mesh.primitives) {
            if (prim.mode != TINYGLTF_MODE_TRIANGLES) { ++skippedParts; continue; }
            auto posIt = prim.attributes.find("POSITION");
            if (posIt == prim.attributes.end()) { ++skippedParts; continue; }
            std::vector<float> pos = ReadFloats(g, posIt->second, 3);
            if (pos.size() < 3) { ++skippedParts; continue; }
            auto nIt = prim.attributes.find("NORMAL");
            std::vector<float> nrm = nIt != prim.attributes.end() ? ReadFloats(g, nIt->second, 3) : std::vector<float>();
            auto uvIt = prim.attributes.find("TEXCOORD_0");
            std::vector<float> uv = uvIt != prim.attributes.end() ? ReadFloats(g, uvIt->second, 2) : std::vector<float>();
            auto uv2It = prim.attributes.find("TEXCOORD_1");
            std::vector<float> uv2 = uv2It != prim.attributes.end() ? ReadFloats(g, uv2It->second, 2) : std::vector<float>();
            auto tanIt = prim.attributes.find("TANGENT");
            std::vector<float> tan = tanIt != prim.attributes.end() ? ReadFloats(g, tanIt->second, 4) : std::vector<float>();
            const size_t vc = pos.size() / 3;

            // --- Наборы влияний: JOINTS_0/WEIGHTS_0, JOINTS_1/WEIGHTS_1, ... ---
            //
            // Спецификация разрешает столько наборов, сколько нужно, — по
            // четыре кости в каждом. Пока читался только нулевой, вершина с
            // восемью влияниями теряла четыре, и если главная кость оказалась
            // во втором наборе (волосы, плащ, мягкие части одежды), вершина
            // повисала на второстепенных — рука тянулась не туда.
            std::vector<std::vector<unsigned int>> jointSets;
            std::vector<std::vector<float>> weightSets;
            bool brokenSkin = false;
            if (skinned) {
                for (int set = 0;; ++set) {
                    const std::string suffix = std::to_string(set);
                    auto jIt = prim.attributes.find("JOINTS_" + suffix);
                    auto wIt = prim.attributes.find("WEIGHTS_" + suffix);
                    if (jIt == prim.attributes.end() || wIt == prim.attributes.end()) break;
                    std::vector<unsigned int> js = ReadUInts(g, jIt->second, 4);
                    std::vector<float> ws = ReadFloats(g, wIt->second, 4);
                    // Набор короче меша — битая разметка. На нулевом это значит
                    // непригодную часть целиком, на прочих — просто обрываем.
                    if (js.size() < vc * 4 || ws.size() < vc * 4) {
                        if (set == 0) brokenSkin = true;
                        break;
                    }
                    jointSets.push_back(std::move(js));
                    weightSets.push_back(std::move(ws));
                }
            }
            if (brokenSkin) { ++skippedParts; continue; }
            const bool hasSkin = !jointSets.empty();

            std::vector<SkinnedVertex> verts(vc);
            // Буфер влияний переиспользуется всеми вершинами части: на модели с
            // сотнями тысяч вершин отдельный вектор на вершину — это только
            // работа распределителю памяти.
            std::vector<sage::assets::SkinInfluence> influences;
            for (size_t i = 0; i < vc; ++i) {
                SkinnedVertex& v = verts[i];
                v.Position = {pos[i*3], pos[i*3+1], pos[i*3+2]};
                v.Normal = nrm.size() >= (i + 1) * 3 ? glm::vec3(nrm[i*3], nrm[i*3+1], nrm[i*3+2]) : glm::vec3(0, 1, 0);
                v.TexCoords = uv.size() >= (i + 1) * 2 ? glm::vec2(uv[i*2], uv[i*2+1]) : glm::vec2(0.0f);
                v.TexCoords2 = uv2.size() >= (i + 1) * 2 ? glm::vec2(uv2[i*2], uv2[i*2+1]) : v.TexCoords;
                if (tan.size() >= (i + 1) * 4)
                    v.Tangent = glm::vec4(tan[i*4], tan[i*4+1], tan[i*4+2], tan[i*4+3]);
                if (hasSkin) {
                    // Все влияния всех наборов — в общий список, номера костей
                    // переведены в номера нашего скелета. Что делать дальше
                    // (отбросить негодные, взять четыре самых весомых,
                    // нормировать), решает общий для форматов ResolveInfluences.
                    influences.clear();
                    for (size_t set = 0; set < jointSets.size(); ++set) {
                        for (int c = 0; c < 4; ++c) {
                            const unsigned raw = jointSets[set][i * 4 + (size_t)c];
                            const int mapped =
                                raw < jointMap->size() ? (*jointMap)[(size_t)raw] : -1;
                            influences.push_back({mapped, weightSets[set][i * 4 + (size_t)c]});
                        }
                    }
                    if (!sage::assets::ResolveInfluences(influences, jointCount, v.Joints,
                                                         v.Weights)) {
                        // Годных влияний нет (все веса нулевые или номера
                        // битые). Вес 0 — шейдер возьмёт единичную матрицу, и
                        // вершина останется там, где лежит в файле; отдать её
                        // корню с весом 1 значило бы утянуть к тазу персонажа.
                        v.Joints = glm::vec4(0.0f);
                        v.Weights = glm::vec4(0.0f);
                    }
                } else {
                    // Жёсткая деталь: одна кость с весом 1, вершины заранее
                    // переведены в систему координат скелета.
                    v.Position = glm::vec3(rigidXf * glm::vec4(v.Position, 1.0f));
                    v.Normal = rigidNrm * v.Normal;
                    v.Tangent = glm::vec4(rigidNrm * glm::vec3(v.Tangent), v.Tangent.w);
                    v.Joints = glm::vec4((float)std::max(rigidJoint, 0), 0.0f, 0.0f, 0.0f);
                    // Кости не нашлось (деталь вне скелета) — вес 0: шейдер
                    // возьмёт единичную матрицу, и деталь останется на месте.
                    v.Weights = rigidJoint >= 0 ? glm::vec4(1, 0, 0, 0) : glm::vec4(0.0f);
                }
            }

            std::vector<unsigned int> indices;
            if (prim.indices >= 0) {
                indices = ReadUInts(g, prim.indices, 1);
                // Индекс за пределами вершин ЭТОЙ части — битый файл. Схлопываем
                // в 0 (треугольник выродится) вместо того, чтобы отдать его
                // видеокарте: там он читает вершинный буфер за границей.
                for (unsigned int& idx : indices)
                    if (idx >= (unsigned)vc) idx = 0u;
            } else {
                indices.resize(vc);
                for (size_t i = 0; i < vc; ++i) indices[i] = (unsigned)i;
            }
            if (indices.size() < 3) { ++skippedParts; continue; }

            ModelSubMeshData sub;
            sub.Vertices = std::move(verts);
            sub.Indices = std::move(indices);
            if (prim.material >= 0 && prim.material < (int)materials.size())
                sub.Material = materials[(size_t)prim.material];

            // Нормали, которых в файле нет, считаем по треугольникам: без них
            // деталь освещается как плоскость с нормалью «вверх» — то есть не
            // освещается вовсе.
            if (nrm.size() < vc * 3) RebuildNormals(sub.Vertices, sub.Indices);
            // Касательные нужны ТОЛЬКО карте нормалей: считать их всегда — это
            // лишний проход по каждой модели ради данных, которыми никто не
            // воспользуется.
            if (sub.Material.Normal >= 0 && tan.size() < vc * 4)
                RebuildTangents(sub.Vertices, sub.Indices);

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

            if (hasSkin) ++skinnedParts; else ++rigidParts;
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
            // Номер сэмплера — число из файла: за границей списка это битый
            // файл, а не повод читать чужую память.
            if (ch.sampler < 0 || ch.sampler >= (int)anim.samplers.size()) continue;
            const tinygltf::AnimationSampler& samp = anim.samplers[(size_t)ch.sampler];
            AnimChannel out;
            out.Joint = jIt->second;
            if (ch.target_path == "translation") out.Target = AnimPath::Translation;
            else if (ch.target_path == "rotation") out.Target = AnimPath::Rotation;
            else if (ch.target_path == "scale") out.Target = AnimPath::Scale;
            else continue; // weights (morph) не поддерживаем
            // ВИД ИНТЕРПОЛЯЦИИ РЕШАЕТ, КАК ЧИТАТЬ ВЫХОД СЭМПЛЕРА.
            //
            // У CUBICSPLINE на каждый ключ приходится ТРОЙКА значений:
            // касательная на входе, само значение, касательная на выходе
            // (спецификация glTF, 3.11). Раньше вид сводился к линейному, а
            // тройка читалась как одно значение — то есть позой становилась
            // ВХОДНАЯ КАСАТЕЛЬНАЯ первого ключа, число, к позе отношения не
            // имеющее. Снаружи это «анимация дёргается и едет не туда»,
            // причём только у тех клипов, которые экспортированы кривыми.
            const bool cubic = samp.interpolation == "CUBICSPLINE";
            out.Interp = cubic ? AnimInterp::CubicSpline
                               : (samp.interpolation == "STEP" ? AnimInterp::Step
                                                               : AnimInterp::Linear);

            std::vector<float> times = ReadFloats(g, samp.input, 1);
            const int comps = (out.Target == AnimPath::Rotation) ? 4 : 3;
            const int stride = cubic ? comps * 3 : comps;   // in, value, out
            std::vector<float> vals = ReadFloats(g, samp.output, comps);
            // Ключей столько, на сколько хватает ОБОИХ массивов: у битого файла
            // значений меньше, чем времён, и цикл по временам читал бы за концом.
            size_t keyCount = std::min(times.size(), vals.size() / (size_t)stride);
            if (keyCount == 0) continue;
            times.resize(keyCount);
            out.Times = times;
            out.Values.resize(keyCount, glm::vec4(0.0f));
            if (cubic) {
                out.InTangents.resize(keyCount, glm::vec4(0.0f));
                out.OutTangents.resize(keyCount, glm::vec4(0.0f));
            }
            for (size_t k = 0; k < keyCount; ++k) {
                const size_t base = k * (size_t)stride;
                for (int c = 0; c < comps; ++c) {
                    if (cubic) {
                        out.InTangents[k][c] = vals[base + c];
                        out.Values[k][c] = vals[base + comps + c];
                        out.OutTangents[k][c] = vals[base + 2 * comps + c];
                    } else {
                        out.Values[k][c] = vals[base + c];
                    }
                }
                clip.Duration = std::max(clip.Duration, times[k]);
            }
            clip.Channels.push_back(std::move(out));
        }
        if (!clip.Channels.empty()) data.Clips.push_back(std::move(clip));
    }

    if (data.SubMeshes.empty())
        throw std::runtime_error("SkinnedModel: в файле нет мешей: " + path);

    LOG_INFO("Anim") << "SkinnedModel: частей со скином " << skinnedParts << ", жёстких "
                     << rigidParts << (skippedParts ? ", пропущено " + std::to_string(skippedParts)
                                                    : std::string())
                     << ", материалов " << materials.size() << ", картинок " << data.Images.size();
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
        // не остаётся ничего. Мелкие текстуры (палитры, пиксель-арт) берём
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
    // персонаж из FBX (а это всё, что отдают Blender, Maya, Mixamo и
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
        data = ParseGltf(path);
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
    // персонаж из FBX (а это всё, что отдают Blender, Maya, Mixamo и
    // ассет-сторы по умолчанию) получал «не загрузить glTF … parse error»:
    // сообщение о ЧУЖОМ формате, из которого следовал вывод «моя модель
    // движку не подходит».
    data = ParseSkinnedModelFile(path);
    sage::assets::WriteModelCache(path, data);
    return WithImportSettings(BuildFromData(data), data, path);
}

} // namespace sage::render
