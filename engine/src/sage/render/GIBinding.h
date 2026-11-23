#pragma once

class Shader;
namespace sage::gi { struct GIState; }

// ---------------------------------------------------------------------------
// Точка, через которую рендер заливает в шейдер объём проб GI, не зная
// устройства GI.
//
// ЗАЧЕМ. Модуль gi стоит НАД render: бейк берёт у рендера геометрию
// (MeshData, ModelLoader), а заливка GI — его Mesh и Shader. Скиннинг (render)
// при этом звал sage::gi::UploadGIVolume напрямую, и модули включали друг
// друга по кругу. Теперь render держит здесь раскладку шейдера (юниты
// сэмплеров — это свойство общего PBR-фрагмента, а не GI) и слот заливки;
// заливку подставляет сам gi (см. sage/gi/GIUpload.cpp).
// ---------------------------------------------------------------------------
namespace sage::render {

// Юниты текстур GI в общем PBR-фрагменте (kPbrSharedGlsl). Не пересекаются с
// материальными (0..5): 6 — 3D-объём проб (три канала слоями), 9 — лайтмапа.
constexpr int kGIVolumeUnit = 6;
constexpr int kLightmapUnit = 9;

// Назначает шейдеру юниты сэмплеров GI. ОБЯЗАТЕЛЬНО для каждого шейдера с
// kPbrSharedGlsl, даже когда GI нет: иначе sampler3D-униформы остаются на
// юните 0 вместе с sampler2D материала — конфликт типов на одном юните
// (GL_INVALID_OPERATION на части драйверов).
void SetGISamplerUnits(Shader& sh);

// Заливка объёма проб gi в шейдер. Ставит её модуль gi.
using GIVolumeUploader = void (*)(Shader&, sage::gi::GIState*);
void SetGIVolumeUploader(GIVolumeUploader fn);

// Заливает объём проб (или выключает его) и назначает юниты. gi может быть
// nullptr. Слот пуст только пока не создано ни одного GIState — а без него
// и заливать нечего: тогда объём просто выключается.
void UploadGIVolume(Shader& sh, sage::gi::GIState* gi);

} // namespace sage::render
