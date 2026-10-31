#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sage/render/RenderGraph.h"

namespace sage::rhi {
class Geometry;
class ShaderProgram;
}

// ---------------------------------------------------------------------------
// Свои эффекты обработки изображения и их цепочки.
//
// Эффект — это материал своего шейдера (ShaderMaterial), нарисованный
// треугольником на весь экран. Цепочка — эффекты по порядку: выход одного —
// вход следующего, промежуточные цели граф заводит сам (две по очереди).
// Выключенный эффект из цепочки выпадает, а не рисуется пустышкой; порядок —
// порядок списка, его и меняют, чтобы переставить эффекты.
//
// Штатная пост-обработка (PostChain, PostFX) от этого не зависит: её можно
// оставить, заменить цепочкой своих эффектов или поставить их до/после неё.
// ---------------------------------------------------------------------------
namespace sage::render {

class ShaderMaterial;

// Треугольник на весь экран без вершинного буфера (gl_VertexID): в
// вершинном шейдере — sage_fullscreen_pos/uv из "sage/common.glsl".
void DrawFullscreenTriangle();

// Вершинный шейдер полноэкранного треугольника: отдаёт vUV.
const char* FullscreenVertexSource();

struct EffectStep {
    std::string Name;
    ShaderMaterial* Material = nullptr;
    bool Enabled = true;
    // Свои параметры кадра (время, сила эффекта). Источник уже в uSource
    // (юнит 0), глубина входа — в uDepth (юнит 1), размер — uTexel.
    std::function<void(ShaderMaterial&, RenderGraphContext&)> Setup;
};

// Объявляет цепочку проходов «prefix: имя шага» от input к output. Возвращает
// число объявленных проходов. Нет ни одного включённого шага — объявляется
// копия input в output (выход кадра обязан быть записан).
int AddEffectChain(RenderGraph& graph, const std::string& prefix, RGHandle input, RGHandle output,
                   const std::vector<EffectStep>& steps);

} // namespace sage::render
