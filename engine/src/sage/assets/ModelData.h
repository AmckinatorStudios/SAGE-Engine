#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

// ---------------------------------------------------------------------------
// КУДА ИМПОРТ МОДЕЛИ КЛАДЁТ ТО, ЧТО ДОСТАЛ ИЗ НЕЁ: материалы, текстуры, клипы.
//
// Раньше — плоско рядом с моделью: у персонажа с полусотней клипов папка с
// моделью превращалась в свалку из сотни файлов, а у десятка моделей в одной
// папке их данные перемешивались. Теперь у каталога моделей ОДНИ на всех
// папки по виду данных — Materials/, Textures/, Animations/, — а чьё это,
// говорит имя файла (оно начинается с имени модели). Папок «Materials» по
// одной на модель не появляется: две модели рядом делят одну.
//
// Встроенные текстуры с одинаковым содержимым (одна и та же палитра, атлас,
// «белая» карта у десятка моделей набора) пишутся ОДИН раз: Textures/ ведёт
// индекс «содержимое -> файл», и вторая модель ссылается на уже лежащую
// картинку, а не кладёт её копию.
//
// СТАРЫЕ ПРОЕКТЫ НЕ ЛОМАЮТСЯ: файл, уже лежащий по-старому рядом с моделью,
// остаётся там и продолжает использоваться — на него ссылаются сцены.
// ---------------------------------------------------------------------------
namespace sage::assets {

enum class ModelData { Materials, Textures, Animations };

const char* ModelDataFolder(ModelData kind);

// Каталог данных этого вида для модели: <папка модели>/<вид>/.
std::filesystem::path ModelDataDir(const std::filesystem::path& modelPath, ModelData kind);

// Путь файла данных модели. Файл с тем же именем, лежащий по-старому рядом с
// моделью, — его путь (он уже на месте, и на него ссылаются); иначе — в
// каталоге вида.
std::filesystem::path ModelDataPath(const std::filesystem::path& modelPath, ModelData kind,
                                    const std::string& fileName);

// Отпечаток пикселей картинки (размер, каналы и содержимое).
uint64_t PixelHash(const unsigned char* pixels, int w, int h, int channels);

// Уже записанная в dir картинка с этим отпечатком — её путь, или пусто.
std::filesystem::path FindSharedImage(const std::filesystem::path& dir, uint64_t hash);
// Запомнить, что в dir картинка с этим отпечатком лежит в файле file.
void RememberSharedImage(const std::filesystem::path& dir, uint64_t hash,
                         const std::filesystem::path& file);

// Имя служебного индекса картинок в каталоге (с точки — скрыт в Assets).
inline constexpr const char* kSharedImageIndex = ".shared-images.sageindex";

} // namespace sage::assets
