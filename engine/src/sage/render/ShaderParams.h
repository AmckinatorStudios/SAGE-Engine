#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/Material.h"

// ---------------------------------------------------------------------------
// Параметры своего шейдера — прямо из его текста.
//
// Шейдер объявляет то, что можно настраивать, обычной строкой uniform, а
// подсказку для редактора пишет комментарием в той же строке:
//
//     uniform float uWaveHeight;   // 0.35 [0, 2] Высота волн
//     uniform vec3  uDeepColor;    // color 0.02 0.16 0.28 Цвет глубины
//     uniform vec2  uFlow;         // 1 0.3 Направление течения
//
// Порядок в комментарии: необязательное слово color (вектор — цвет, рисуется
// пипеткой), числа умолчания (сколько компонент у типа), необязательный
// диапазон [min, max] (ползунок), остальное — подпись. Всё необязательно:
// голый uniform float — тоже параметр, с умолчанием 0 и диапазоном 0..1.
//
// ТЕКСТУРА — uniform sampler2D: в инспекторе это слот картинки. Слово linear
// в подсказке — данные (нормали, маски), без него — цвет в sRGB:
//
//     uniform sampler2D uFoam;     // Пена
//     uniform sampler2D uFlowMap;  // linear Карта течения
//
// ЗАГОЛОВОК — строка-комментарий «// @header Название»: параметры ниже неё до
// следующего заголовка идут в инспекторе одной группой. Шейдер сам решает,
// как разложить свои настройки, — у движка своего списка полей нет.
//
// Без этого параметры правились только в .sagemat руками: редактор показывал
// «параметров: 3 (правьте в файле)», и нужно было помнить и имена, и типы.
//
// Служебные юниформы движка (uView, uTime, uOpacity, освещение, тени…) в
// список не попадают: их заливает движок, и правка из инспектора ничего бы не
// дала. Сэмплеры тоже: текстуры материалу задаются картами.
// ---------------------------------------------------------------------------
namespace sage::render {

struct ShaderUniformInfo {
    std::string Name;
    ShaderParam::Type Type = ShaderParam::Type::Float;
    glm::vec4 Default{0.0f};
    float Min = 0.0f;
    float Max = 1.0f;
    bool HasRange = false;
    bool Color = false;
    std::string Label; // подпись из комментария; пусто — имя без префикса «u»
    bool Texture = false;   // sampler2D: слот картинки, а не число
    bool Linear = false;    // текстура-данные (без перевода из sRGB)
    std::string Header;     // группа из «// @header …» выше; пусто — без группы
};

// Параметры из текста шейдера в порядке объявления, без повторов.
std::vector<ShaderUniformInfo> ParseShaderUniforms(const std::string& source);

// Параметры программы из нескольких исходников (вершинный + пиксельный): одно
// имя — один параметр. Подсказка — из первого объявления, но заголовок и
// подпись дополняются из других (волна объявлена в .vert без группы и в .frag
// под «@header Волны» — она в группе «Волны»). Группы собраны вместе в порядке
// первого появления, параметры без группы — первыми.
std::vector<ShaderUniformInfo> MergeShaderUniforms(const std::vector<std::vector<ShaderUniformInfo>>& sources);

// Юниформ заливает сам движок (контракт docs/custom_shaders.md, блок PBR).
bool IsEngineUniform(const std::string& name);

// Подпись для человека: «uWaveHeight» -> «Wave Height».
std::string UniformLabel(const std::string& name);

} // namespace sage::render
