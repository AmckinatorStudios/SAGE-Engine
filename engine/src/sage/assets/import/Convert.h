#pragma once
#include <string>
#include <vector>

#include "sage/assets/format/MeshFormat.h"
#include "sage/assets/format/TextureFormat.h"

// ---------------------------------------------------------------------------
// Конвертация чужих форматов в свои.
//
// ЗАЧЕМ ЭТО ОТДЕЛЬНЫЙ СЛОЙ, А НЕ «сохранить как». Импорт и упаковка — разные
// решения. Импорт отвечает на вопрос «что в этом файле»; упаковка — «как это
// хранить, чтобы игра грузилась быстро и весила мало». Между ними лежит выбор,
// который никто, кроме автора игры, не сделает: квантовать ли позиции, нужны ли
// касательные, пиксель-арт эта текстура или карта нормалей. Слепить их в одну
// кнопку значит зашить эти ответы в код.
//
// ЧТО ВАЖНО В РЕЗУЛЬТАТЕ. Конвертация — операция, которую запускают на всю папку
// ассетов сразу, поэтому она обязана: не падать на одном плохом файле (отчёт
// пофайловый), не терять данные молча (предупреждения импортёра доезжают до
// отчёта) и говорить, что получилось (сколько было, сколько стало) — иначе
// проверить, что упаковка вообще работает, можно только на глаз.
// ---------------------------------------------------------------------------

namespace sage::assets {

struct ConvertOptions {
    MeshWriteOptions Mesh;
    TextureWriteOptions Texture;
    // Склеивать всю сцену в один меш. Для MeshRendererComponent, который держит
    // ровно один Mesh, — да; для импорта сцены целиком — нет.
    bool FlattenToSingleMesh = true;
    // Перезаписывать существующий .sagemesh. По умолчанию нет: конвертация
    // пачкой не должна затирать то, что человек мог поправить руками.
    bool Overwrite = false;
};

struct ConvertResult {
    bool Ok = false;
    std::string SourcePath;
    std::string OutputPath;
    std::string Error;                  // непусто при Ok == false
    std::vector<std::string> Warnings;  // потери и особенности импорта
    size_t SourceBytes = 0;             // размер исходного файла на диске
    size_t OutputBytes = 0;
    size_t Vertices = 0;
    size_t Triangles = 0;

    // Во сколько раз файл стал меньше. 0, если сравнивать не с чем.
    float Ratio() const {
        return (SourceBytes && OutputBytes) ? (float)SourceBytes / (float)OutputBytes : 0.0f;
    }
};

// Модель ЛЮБОГО поддерживаемого формата -> .sagemesh.
// outPath пуст — рядом с исходником, с тем же именем и расширением .sagemesh.
ConvertResult ConvertModelToNative(const std::string& sourcePath, const std::string& outPath,
                                   const ConvertOptions& opts = {});

// Картинка (.png/.jpg/... — всё, что читает stb_image) -> .sagetex.
ConvertResult ConvertTextureToNative(const std::string& sourcePath, const std::string& outPath,
                                     const ConvertOptions& opts = {});

// По расширению выбирает нужное из двух. Для папки — см. ConvertFolder.
ConvertResult ConvertAnyToNative(const std::string& sourcePath, const std::string& outPath,
                                 const ConvertOptions& opts = {});

// Вся папка рекурсивно. Один плохой файл не останавливает остальные — иначе
// конвертация набора ассетов упиралась бы в первый же битый файл.
std::vector<ConvertResult> ConvertFolder(const std::string& folder, const ConvertOptions& opts = {});

// Умеет ли движок сконвертировать такой файл (для панели ассетов и диалогов).
bool IsConvertibleModel(const std::string& path);
bool IsConvertibleTexture(const std::string& path);

} // namespace sage::assets
