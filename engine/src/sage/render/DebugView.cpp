#include "sage/render/DebugView.h"

#include <cctype>
#include <cstring>
#include <string>

namespace sage::render {
namespace {

struct Entry {
    DebugView View;
    const char* Id;      // имя для настроек и командной строки
    const char* Name;    // имя для интерфейса
    const char* Hint;
};

// Порядок совпадает с порядком в меню редактора; Id — то, что пишут в
// SAGE_DEBUG_VIEW и в настройках.
const Entry kEntries[] = {
    {DebugView::None, "none", "Обычный", "Кадр как есть"},
    {DebugView::Unlit, "unlit", "Без света", "Цвет и свечение без освещения — видно саму раскраску"},
    {DebugView::Albedo, "albedo", "Базовый цвет", "Только albedo: сюда не примешаны ни свет, ни свечение"},
    {DebugView::Normals, "normals", "Нормали", "Мировые нормали. Пятна и швы здесь — причина странных бликов"},
    {DebugView::Roughness, "roughness", "Шероховатость", "Чёрное — зеркало, белое — матовое"},
    {DebugView::Metallic, "metallic", "Металличность", "Белое — металл. Серое между — почти всегда ошибка материала"},
    {DebugView::Emissive, "emissive", "Свечение", "Что светит само. Чёрный кадр — свечения в сцене нет"},
    {DebugView::AmbientOcclusion, "ao", "Затенение", "Карта AO материала (не экранный SSAO)"},
    {DebugView::Shadow, "shadow", "Тень солнца", "Белое — освещено, чёрное — в тени"},
    {DebugView::Depth, "depth", "Глубина", "Расстояние от камеры, шкала до ста метров"},
    {DebugView::Cascades, "cascades", "Каскады теней", "Красный/зелёный/синий — ближний/средний/дальний"},
    {DebugView::WorldGrid, "grid", "Метровая клетка", "Клетка по мировым координатам — масштаб и перекос"},
};

} // namespace

const char* DebugViewName(DebugView view) {
    for (const Entry& e : kEntries)
        if (e.View == view) return e.Name;
    return "?";
}

const char* DebugViewHint(DebugView view) {
    for (const Entry& e : kEntries)
        if (e.View == view) return e.Hint;
    return "";
}

bool ParseDebugView(const char* text, DebugView& out) {
    if (!text || !*text) return false;
    std::string want;
    for (const char* p = text; *p; ++p) want += (char)std::tolower((unsigned char)*p);
    for (const Entry& e : kEntries) {
        if (want == e.Id) {
            out = e.View;
            return true;
        }
    }
    return false;
}

} // namespace sage::render
