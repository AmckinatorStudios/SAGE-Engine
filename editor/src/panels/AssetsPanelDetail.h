#pragma once
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"

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

// Габариты карточки-тайла — ОТ МАСШТАБА (регулятор в шапке). ВАЖНО: высота
// тайла включает и плашку, и подпись — раньше подпись рисовалась DrawList'ом
// НИЖЕ InvisibleButton'а (высотой лишь в плашку), выходила за границы item'а,
// и строки грида налезали друг на друга. Теперь весь тайл — единый item.
//
// Обложка КВАДРАТНАЯ у всех типов: превью вписывается по меньшей стороне, и
// вытянутая плашка отдавала картинке лишь часть карточки.
//
// Под обложкой — имя и, если карточка не мелкая, тип («Модель»): тип — второй
// вопрос к файлу, но в карточке 64 px на него нет места, и там остаётся имя.
struct TileMetrics {
    float W = 104.0f;
    float Inset = 8.0f;
    float SwatchH = 96.0f;   // видимая область обложки — квадрат (W - 2*Inset)
    float LabelH = 20.0f;
    bool KindLine = true;
    float H = 0.0f;
    float Spacing = 10.0f;
};
inline TileMetrics MetricsFor(float tileW, float lineH) {
    TileMetrics m;
    m.W = tileW;
    m.Inset = tileW < 80.0f ? 5.0f : 7.0f;
    m.SwatchH = tileW - m.Inset;
    m.LabelH = lineH + 4.0f;
    m.KindLine = tileW >= 88.0f;
    m.H = m.SwatchH + m.LabelH * (m.KindLine ? 2.0f : 1.0f) + 4.0f;
    m.Spacing = tileW < 80.0f ? 6.0f : 10.0f;
    return m;
}

// Что это за файл одним словом и как он выглядит (AssetsPanel_Tile.cpp).
std::string KindLabel(const std::filesystem::path& path, bool isDir);
struct AssetStyle {
    ImVec4 Color;
    std::string Tag;
    const char* Icon;
};
AssetStyle StyleForPath(const std::filesystem::path& path, bool isDir);

// Обрезка строки многоточием по ширине — ПО ГРАНИЦАМ СИМВОЛОВ UTF-8:
// побайтовая резала кириллическое имя посреди буквы, и на карточке выходил
// мусорный знак. cut — true, если обрезали.
std::string TruncateToWidth(const std::string& s, float maxWidth, bool* cut = nullptr);
// То же, но многоточие В СЕРЕДИНЕ: «item_...017.txt». Имена файлов одной папки
// чаще различаются концом (номер, расширение), и обрезка справа делала сотню
// карточек одинаковыми «item_...».
std::string TruncateMiddle(const std::string& s, float maxWidth, bool* cut = nullptr);

// Время правки (fscache::Stamp) — «2026-11-21 14:05» по местному времени.
std::string FormatStamp(long long stamp);

// Спутники моделей и регистрация в базе ассетов (AssetsPanel_Files.cpp).
std::vector<std::string> MtlTextureNames(const std::filesystem::path& mtl);
std::vector<std::string> GltfExternalUris(const std::filesystem::path& gltf);
void RegisterInDatabase(const std::filesystem::path& file);
// Объект сцены, брошенный на папку, становится префабом в ней (AssetsPanel.cpp).
void AcceptObjectAsPrefab(EditorHost& host, const std::filesystem::path& folder);

} // namespace sage::editor::assetspanel_detail
