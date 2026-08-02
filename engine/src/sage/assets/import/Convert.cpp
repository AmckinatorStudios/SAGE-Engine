#include "sage/assets/import/Convert.h"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include <stb_image.h>

#include "sage/assets/import/Importer.h"
#include "sage/core/Log.h"

namespace sage::assets {

namespace fs = std::filesystem;

namespace {

std::string LowerExt(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

size_t FileSize(const std::string& path) {
    std::error_code ec;
    const auto n = fs::file_size(path, ec);
    return ec ? 0 : (size_t)n;
}

std::string DefaultOutput(const std::string& source, const char* newExt) {
    fs::path p(source);
    p.replace_extension(newExt);
    return p.string();
}

// Назначение текстуры по имени файла: «_normal», «_rough», «_ao» — соглашение,
// которого держатся все библиотеки PBR-материалов. Угадывание здесь уместно
// именно потому, что ошибка видна сразу и переопределяется явной опцией, а
// заставлять человека размечать сотню файлов руками — не уместно.
TextureIntent GuessIntent(const std::string& path) {
    std::string name = fs::path(path).stem().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    if (has("normal") || has("_nrm") || has("_norm") || has("_n")) return TextureIntent::NormalMap;
    if (has("rough") || has("metal") || has("_ao") || has("occlusion") || has("mask") ||
        has("specular") || has("gloss"))
        return TextureIntent::DataMap;
    return TextureIntent::Albedo;
}

} // namespace

bool IsConvertibleModel(const std::string& path) {
    const std::string ext = LowerExt(path);
    if (ext == ".sagemesh") return false;   // уже наш формат
    return ImporterRegistry::Instance().CanImport(ext);
}

bool IsConvertibleTexture(const std::string& path) {
    const std::string ext = LowerExt(path);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" ||
           ext == ".psd" || ext == ".gif" || ext == ".hdr";
}

ConvertResult ConvertModelToNative(const std::string& sourcePath, const std::string& outPath,
                                   const ConvertOptions& opts) {
    ConvertResult r;
    r.SourcePath = sourcePath;
    r.OutputPath = outPath.empty() ? DefaultOutput(sourcePath, ".sagemesh") : outPath;
    r.SourceBytes = FileSize(sourcePath);

    if (!opts.Overwrite && fs::exists(r.OutputPath)) {
        r.Error = "файл уже есть: " + r.OutputPath + " (перезапись выключена)";
        return r;
    }

    ImportedScene scene;
    if (!ImporterRegistry::Instance().Import(sourcePath, scene, r.Error)) return r;
    r.Warnings = scene.Warnings;

    sage::render::MeshData mesh =
        opts.FlattenToSingleMesh ? scene.Flatten() : (scene.Nodes.empty() ? sage::render::MeshData{}
                                                                          : scene.Nodes[0].Mesh);
    if (mesh.Empty()) {
        r.Error = "после импорта не осталось геометрии";
        return r;
    }
    r.Vertices = mesh.Vertices.size();
    r.Triangles = mesh.Indices.size() / 3;

    std::error_code ec;
    fs::create_directories(fs::path(r.OutputPath).parent_path(), ec);

    MeshStats stats;
    if (!WriteMeshFile(r.OutputPath, mesh, opts.Mesh, r.Error, &stats)) return r;
    r.OutputBytes = stats.PackedBytes;
    r.Ok = true;
    return r;
}

ConvertResult ConvertTextureToNative(const std::string& sourcePath, const std::string& outPath,
                                     const ConvertOptions& opts) {
    ConvertResult r;
    r.SourcePath = sourcePath;
    r.OutputPath = outPath.empty() ? DefaultOutput(sourcePath, ".sagetex") : outPath;
    r.SourceBytes = FileSize(sourcePath);

    if (!opts.Overwrite && fs::exists(r.OutputPath)) {
        r.Error = "файл уже есть: " + r.OutputPath + " (перезапись выключена)";
        return r;
    }

    int w = 0, h = 0, channels = 0;
    // Без переворота: .sagetex хранит строки сверху вниз, как и исходник, а за
    // ориентацию отвечает загрузчик текстуры — иначе флип пришлось бы помнить
    // в двух местах.
    stbi_set_flip_vertically_on_load(false);
    unsigned char* pixels = stbi_load(sourcePath.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        r.Error = std::string("картинка не читается: ") +
                  (stbi_failure_reason() ? stbi_failure_reason() : "неизвестная причина");
        return r;
    }

    TextureWriteOptions texOpts = opts.Texture;
    // Назначение угадываем ТОЛЬКО если его не задали явно: пакетная конвертация
    // с одним назначением на всю папку испортила бы карты нормалей.
    if (texOpts.Intent == TextureIntent::Albedo && !texOpts.ForceFormat)
        texOpts.Intent = GuessIntent(sourcePath);

    std::error_code ec;
    fs::create_directories(fs::path(r.OutputPath).parent_path(), ec);

    TextureStats stats;
    const bool ok = WriteTextureFile(r.OutputPath, pixels, w, h, texOpts, r.Error, &stats);
    stbi_image_free(pixels);
    if (!ok) return r;

    r.OutputBytes = stats.PackedBytes;
    r.Ok = true;
    return r;
}

ConvertResult ConvertAnyToNative(const std::string& sourcePath, const std::string& outPath,
                                 const ConvertOptions& opts) {
    if (IsConvertibleTexture(sourcePath)) return ConvertTextureToNative(sourcePath, outPath, opts);
    if (IsConvertibleModel(sourcePath)) return ConvertModelToNative(sourcePath, outPath, opts);

    ConvertResult r;
    r.SourcePath = sourcePath;
    r.SourceBytes = FileSize(sourcePath);
    r.Error = "нечего конвертировать: " + LowerExt(sourcePath) +
              " не модель и не картинка, известные движку";
    return r;
}

std::vector<ConvertResult> ConvertFolder(const std::string& folder, const ConvertOptions& opts) {
    std::vector<ConvertResult> results;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        ConvertResult r;
        r.SourcePath = folder;
        r.Error = "это не папка: " + folder;
        results.push_back(std::move(r));
        return results;
    }
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(folder, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string path = entry.path().string();
        if (!IsConvertibleModel(path) && !IsConvertibleTexture(path)) continue;
        // Пофайловый отчёт и продолжение после ошибки: конвертация набора
        // ассетов не должна упираться в первый же битый файл.
        results.push_back(ConvertAnyToNative(path, {}, opts));
    }
    return results;
}

} // namespace sage::assets
