#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

// ---------------------------------------------------------------------------
// КУДА ИМПОРТ МОДЕЛИ КЛАДЁТ ТО, ЧТО ДОСТАЛ ИЗ НЕЁ: материалы, текстуры, клипы.
//
// У КАЖДОЙ МОДЕЛИ — СВОЯ ПАПКА: <модель>/Materials, <модель>/Textures,
// <модель>/Animations. Сначала всё лежало плоско рядом с моделью, потом — в
// общих на каталог Materials/ и Textures/, где у десятка моделей данные
// перемешивались, а чьё что, приходилось угадывать по началу имени файла.
// Импорт кладёт и саму модель в папку с её именем (Assets -> Import), так что
// всё, что относится к модели, лежит в одном месте и переносится одной папкой.
//
// Встроенные текстуры с одинаковым содержимым внутри одной модели пишутся
// ОДИН раз: Textures/ ведёт индекс «содержимое -> файл».
//
// СТАРЫЕ ПРОЕКТЫ НЕ ЛОМАЮТСЯ: файл, уже лежащий по-старому (рядом с моделью
// или в общей папке каталога), остаётся там и продолжает использоваться — на
// него ссылаются сцены.
// ---------------------------------------------------------------------------
namespace sage::assets {

enum class ModelData { Materials, Textures, Animations };

const char* ModelDataFolder(ModelData kind);

// Своя папка модели: та, где она лежит, если папка названа её именем
// (assets/Steve/Steve.gltf — так кладёт импорт), иначе — папка с её именем
// рядом с файлом (assets/props/crate.gltf -> assets/props/crate/).
std::filesystem::path ModelHomeDir(const std::filesystem::path& modelPath);

// Каталог данных этого вида для модели: <своя папка модели>/<вид>/.
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
