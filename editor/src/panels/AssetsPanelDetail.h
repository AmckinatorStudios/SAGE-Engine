#pragma once
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

class EditorHost;

// Общее для файлов панели Assets (AssetsPanel*.cpp). Только для них.
namespace sage::editor::assetspanel_detail {

inline std::string ToUpper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

inline std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Габариты карточки-тайла. ВАЖНО: высота тайла включает и плашку, и подпись —
// раньше подпись рисовалась DrawList'ом НИЖЕ InvisibleButton'а (высотой лишь в
// плашку), выходила за границы item'а, и строки грида налезали друг на друга
// («неровность»). Теперь весь тайл — единый item, всё внутри его границ.
constexpr float kTileW = 104.0f;
constexpr float kTileInset = 8.0f; // внутренний отступ карточки
// Обложка КВАДРАТНАЯ у всех типов ассетов.
//
// Раньше она была 88x54 — вытянутый прямоугольник. Для трёхбуквенного тега это
// было неважно, но превью в него вписывается по меньшей стороне, то есть по
// высоте: от карточки шириной 88 под картинку работали 54 пикселя, а остальное
// оставалось пустым. Квадрат отдаёт превью всю ширину и заодно выравнивает
// сетку — карточки разных типов перестают отличаться пропорциями.
//
// Высота плашки подобрана так, чтобы её ВИДИМАЯ область (kTileW - 2*inset по
// ширине, kSwatchH - inset по высоте) была квадратом.
constexpr float kSwatchH = kTileW - kTileInset;
// Подпись — РОВНО ОДНА строка. Раньше под неё отводилось 34 пикселя «на одну-две
// строки», но рисовалась всегда одна: каждая карточка несла полторы строки
// пустоты, и сетка выглядела рыхлой.
constexpr float kLabelH = 22.0f;
// Под подписью ДВЕ строки: имя и тип. Тип — это второй вопрос к файлу («что это
// вообще такое»), и раньше на него отвечали только трёхбуквенная метка в углу
// обложки и значок. Для .obj, .gltf и .sagemesh метка разная, а предмет один —
// модель; словом это сказано один раз и одинаково.
constexpr float kTileH = kSwatchH + kLabelH * 2.0f + 8.0f;
constexpr float kTileSpacing = 12.0f;

// Спутники моделей и регистрация в базе ассетов (AssetsPanel_Files.cpp).
std::vector<std::string> MtlTextureNames(const std::filesystem::path& mtl);
std::vector<std::string> GltfExternalUris(const std::filesystem::path& gltf);
void RegisterInDatabase(const std::filesystem::path& file);
// Объект сцены, брошенный на папку, становится префабом в ней (AssetsPanel.cpp).
void AcceptObjectAsPrefab(EditorHost& host, const std::filesystem::path& folder);

} // namespace sage::editor::assetspanel_detail
