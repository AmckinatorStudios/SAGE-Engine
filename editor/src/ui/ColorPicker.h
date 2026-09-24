#pragma once
#include <string>
#include <vector>

#include "imgui.h"

// ---------------------------------------------------------------------------
// ВЫБОР ЦВЕТА РЕДАКТОРА — своё поле и своя палитра вместо ImGui::ColorEdit.
//
// ЗАЧЕМ СВОЁ. Стандартное поле ImGui — это крошечный квадрат и три-четыре
// числовых окошка «R:255 G:128 …», а палитра открывается по щелчку в квадрат
// размером с букву. Прозрачность там — узкая вертикальная полоса сбоку от
// квадрата насыщенности, без чисел и без шахматки в самом поле: понять, сколько
// сейчас альфы, можно только открыв палитру и присмотревшись. Для материала
// (цвет + «Непрозрачность» отдельным ползунком ниже) картинки «какой получится
// цвет» не было вовсе.
//
// Здесь:
//   • поле во всю ширину: сам цвет, его hex и процент непрозрачности поверх
//     шахматки — видно без щелчка;
//   • палитра: квадрат насыщенность/яркость, полоса тона, КРУПНАЯ полоса
//     прозрачности с числом в процентах рядом, каналы RGB/HSV полосами с
//     градиентом, hex (копируется и вставляется Ctrl+C/Ctrl+V), «было/стало»
//     со щелчком «вернуть как было» и недавние цвета;
//   • цвет перетаскивается с поля на поле (и на стандартные поля ImGui — тот же
//     формат перетаскивания).
//
// Отмена правок работает как у обычного виджета: после поля можно звать
// IsItemActivated / IsItemDeactivatedAfterEdit (EditorHost::TrackLastImGuiItem),
// и они отражают работу в палитре, хоть она и в отдельном окне.
// ---------------------------------------------------------------------------
namespace Sage::UI {

enum ColorFieldFlags_ {
    ColorField_None     = 0,
    // Только квадратный образец без текста: строка таблицы, метка папки.
    ColorField_Compact  = 1 << 0,
    // Показать и не давать менять: вычисленный цвет, который правится не здесь.
    ColorField_ReadOnly = 1 << 1,
};
using ColorFieldFlags = int;

// Цвет без прозрачности (rgb[3]) и с ней (rgba[4]). true — значение изменилось
// в этом кадре. Подпись — как у ImGui: «##» прячет её.
bool ColorField3(const char* label, float rgb[3], ColorFieldFlags flags = 0);
bool ColorField4(const char* label, float rgba[4], ColorFieldFlags flags = 0);
// Цвет и непрозрачность, хранящиеся РАЗДЕЛЬНО (материал: Albedo и Opacity).
// alpha == nullptr — то же, что ColorField3.
bool ColorFieldAlpha(const char* label, float rgb[3], float* alpha, ColorFieldFlags flags = 0);

// Палитра прямо в окне, без поля (подложка холста интерфейса). width <= 0 —
// ширина по умолчанию.
bool ColorPickerInline(const char* id, float rgb[3], float* alpha = nullptr, float width = 0.0f);

// --- чистая математика: проверяется без кадра ImGui ---------------------------
namespace color {

// «#RRGGBB» или «#RRGGBBAA» (alpha != nullptr). Компоненты зажимаются в 0..1.
std::string ToHex(const float rgb[3], const float* alpha = nullptr);
// Понимает «#RGB», «RGB», «RRGGBB», «RRGGBBAA», с решёткой и без, регистр любой.
// Альфа из текста пишется в *alpha, только если она там есть и alpha != nullptr.
// false — строка не цвет; rgb при этом не трогается.
bool ParseHex(const char* text, float rgb[3], float* alpha = nullptr);

// Тон, насыщенность, яркость — все в 0..1.
void RgbToHsv(const float rgb[3], float hsv[3]);
void HsvToRgb(const float hsv[3], float rgb[3]);
// Тон и насыщенность у серого и чёрного не определены: у серого любой тон,
// у чёрного — и любая насыщенность. Потеряй их при движении маркера в угол —
// и полоса тона прыгает в красный, а маркер в квадрате — в левый край: палитра
// «забывает», что выбирали. prevHsv — то, что было до правки.
void RgbToHsvKeep(const float rgb[3], const float prevHsv[3], float hsv[3]);

// Тёмный ли текст ставить поверх этого цвета (по воспринимаемой яркости).
bool WantsDarkText(const float rgb[3]);

// Недавние цвета палитры, новые впереди, без повторов, не больше kRecentMax.
constexpr int kRecentMax = 10;
const std::vector<ImVec4>& Recent();
void Remember(const ImVec4& rgba);
void ClearRecent();

// Где в последнем кадре лежали части открытой палитры — для проверок, которые
// двигают мышь по настоящему кадру (tests/test_colorpicker.cpp).
struct Box {
    ImVec2 Min{0, 0}, Max{0, 0};
    ImVec2 At(float fx, float fy) const {
        return ImVec2(Min.x + (Max.x - Min.x) * fx, Min.y + (Max.y - Min.y) * fy);
    }
};
struct PickerLayout {
    Box SV, Hue, Alpha, Original;
};
const PickerLayout& LastPickerLayout();

} // namespace color
} // namespace Sage::UI
