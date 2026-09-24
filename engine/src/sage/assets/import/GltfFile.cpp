// Реализация tinygltf развёрнута в render/Model.cpp — здесь только объявления.
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "sage/assets/import/GltfFile.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "sage/core/Paths.h"

namespace fs = std::filesystem;

namespace sage::assets {

namespace {

constexpr std::uint32_t kChunkJson = 0x4E4F534A; // 'JSON'
constexpr std::uint32_t kChunkBin  = 0x004E4942; // 'BIN\0'

std::uint32_t ReadU32(const std::string& d, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, d.data() + at, 4); // GLB — little-endian, как и все машины, где мы живём
    return v;
}

void WriteU32(std::string& d, std::uint32_t v) {
    char b[4];
    std::memcpy(b, &v, 4);
    d.append(b, 4);
}

struct Chunk {
    std::uint32_t Type = 0;
    std::size_t   At = 0;   // начало ДАННЫХ куска
    std::size_t   Size = 0;
};

} // namespace

bool RepackGlb(const std::string& data, std::string& out, std::string& err, std::string& warn) {
    if (data.size() < 12) {
        err = "файл короче заголовка GLB: в нём " + std::to_string(data.size()) +
              " байт вместо 12 — файл скачался не до конца";
        return false;
    }
    if (std::memcmp(data.data(), "glTF", 4) != 0) {
        err = "это не GLB: в начале файла нет метки glTF";
        return false;
    }

    const std::uint32_t version  = ReadU32(data, 4);
    const std::uint32_t declared = ReadU32(data, 8);
    if (version != 2) {
        // Версию 1 tinygltf не понимает, но отвечать на неё общим «файл битый»
        // нечестно: человеку надо знать, что модель надо пересохранить.
        warn += "GLB версии " + std::to_string(version) + ", движок читает версию 2. ";
    }

    // --- ОБХОД КУСКОВ ------------------------------------------------------
    std::vector<Chunk> chunks;
    std::size_t off = 12;
    while (off + 8 <= data.size()) {
        const std::uint64_t size = ReadU32(data, off);
        const std::uint32_t type = ReadU32(data, off + 4);
        const std::uint64_t have = (std::uint64_t)data.size() - (std::uint64_t)off - 8ull;
        if (size > have) {
            // ОБРЕЗАННЫЙ ФАЙЛ. Чинить нечем: данных просто нет. Молчаливая
            // «починка» отдала бы движку геометрию с дырой, а это выглядит как
            // поломка движка, а не как недокачанный файл.
            err = "файл обрезан: кусок обещает " + std::to_string(size) +
                  " байт, а в файле осталось " + std::to_string(have) +
                  " — скачайте или экспортируйте модель заново";
            return false;
        }
        chunks.push_back({type, off + 8, (std::size_t)size});
        off += 8 + (std::size_t)size;
    }
    if (off != data.size()) {
        warn += "в конце файла " + std::to_string(data.size() - off) +
                " лишних байт — отброшены. ";
    }

    if (chunks.empty() || chunks.front().Type != kChunkJson) {
        err = "в GLB нет куска JSON — описание модели отсутствует";
        return false;
    }
    if (chunks.front().Size == 0) {
        err = "кусок JSON в GLB пустой — описание модели отсутствует";
        return false;
    }

    // Оставляем ровно то, что читает разборщик: JSON и первый BIN. Прочие куски
    // (их пишут расширения) не выбрасывать нельзя: лежащий ПЕРЕД BIN чужой
    // кусок был бы принят за двоичные данные модели.
    const Chunk* json = &chunks.front();
    const Chunk* bin = nullptr;
    int dropped = 0;
    for (std::size_t i = 1; i < chunks.size(); ++i) {
        if (chunks[i].Type == kChunkBin && !bin) bin = &chunks[i];
        else ++dropped;
    }
    if (dropped > 0) {
        warn += "в GLB " + std::to_string(dropped) +
                " кусков неизвестного вида — пропущены. ";
    }

    // --- СБОРКА ЗАНОВО -----------------------------------------------------
    // Длина и выравнивание пишутся тут, а не берутся из файла: именно они в
    // живых файлах и расходятся с содержимым.
    auto append = [&](const Chunk& c, char pad) {
        const std::size_t padded = (c.Size + 3) & ~(std::size_t)3;
        WriteU32(out, (std::uint32_t)padded);
        WriteU32(out, c.Type);
        out.append(data, c.At, c.Size);
        out.append(padded - c.Size, pad);
    };

    out.clear();
    out.append("glTF", 4);
    WriteU32(out, 2);
    WriteU32(out, 0); // длина — допишем, когда узнаем
    append(*json, ' ');            // JSON добивается пробелами
    if (bin) append(*bin, '\0');   // BIN — нулями

    const std::uint32_t total = (std::uint32_t)out.size();
    std::memcpy(&out[8], &total, 4);

    if (declared != total) {
        warn += "заголовок GLB поправлен: в шапке было " + std::to_string(declared) +
                " байт, на деле " + std::to_string(total) + ". ";
    }
    return true;
}

int ConvertSpecularGlossiness(tinygltf::Model& model) {
    int converted = 0;
    for (tinygltf::Material& m : model.materials) {
        auto it = m.extensions.find("KHR_materials_pbrSpecularGlossiness");
        if (it == m.extensions.end() || !it->second.IsObject()) continue;
        const tinygltf::Value& sg = it->second;
        tinygltf::PbrMetallicRoughness& pbr = m.pbrMetallicRoughness;

        // Цвет: diffuse — ровно то, что в металл-шероховатости называется
        // базовым цветом у неметалла. По умолчанию — белый, как в спецификации.
        pbr.baseColorFactor = {1.0, 1.0, 1.0, 1.0};
        if (sg.Has("diffuseFactor") && sg.Get("diffuseFactor").IsArray()) {
            const tinygltf::Value& f = sg.Get("diffuseFactor");
            for (int i = 0; i < 4 && i < (int)f.ArrayLen(); ++i)
                if (f.Get(i).IsNumber()) pbr.baseColorFactor[i] = f.Get(i).GetNumberAsDouble();
        }
        pbr.baseColorTexture = tinygltf::TextureInfo();
        if (sg.Has("diffuseTexture") && sg.Get("diffuseTexture").IsObject()) {
            const tinygltf::Value& t = sg.Get("diffuseTexture");
            if (t.Has("index") && t.Get("index").IsNumber())
                pbr.baseColorTexture.index = (int)t.Get("index").GetNumberAsDouble();
            if (t.Has("texCoord") && t.Get("texCoord").IsNumber())
                pbr.baseColorTexture.texCoord = (int)t.Get("texCoord").GetNumberAsDouble();
        }
        // Блеск — обратная шероховатость. Металличности у этой модели
        // материала нет вовсе: блик задаётся цветом отражения, и для
        // окрашенных поверхностей (а таких почти все) это диэлектрик. Без
        // явного нуля осталась бы металличность 1 по умолчанию glTF — модель
        // выходила бы тёмным зеркалом.
        double gloss = 1.0;
        if (sg.Has("glossinessFactor") && sg.Get("glossinessFactor").IsNumber())
            gloss = sg.Get("glossinessFactor").GetNumberAsDouble();
        pbr.roughnessFactor = std::clamp(1.0 - gloss, 0.0, 1.0);
        pbr.metallicFactor = 0.0;
        // Карта блеска (альфа specularGlossinessTexture) лежит не в том канале,
        // которого ждёт металл-шероховатость (зелёный, и наоборот по смыслу),
        // поэтому она не подставляется: шероховатость берётся числом.
        pbr.metallicRoughnessTexture = tinygltf::TextureInfo();
        ++converted;
    }
    return converted;
}

bool LoadGltfFile(tinygltf::TinyGLTF& loader, tinygltf::Model& model, const std::string& path,
                  std::string& err, std::string& warn) {
    std::ifstream f(sage::PathFromUtf8(path), std::ios::binary);
    if (!f) {
        err = "файл не открывается: " + path;
        return false;
    }
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty()) {
        err = "файл пустой: " + path;
        return false;
    }

    std::error_code ec;
    const std::string baseDir = sage::PathToUtf8(fs::path(sage::PathFromUtf8(path)).parent_path());

    std::string tinyErr, tinyWarn;
    bool ok = false;
    // ФОРМАТ ПО СОДЕРЖИМОМУ, А НЕ ПО ИМЕНИ: .glb с текстом внутри и .gltf с
    // двоичным содержимым встречаются оба, и оба до этого не открывались.
    if (data.size() >= 4 && std::memcmp(data.data(), "glTF", 4) == 0) {
        std::string glb;
        if (!RepackGlb(data, glb, err, warn)) return false;
        ok = loader.LoadBinaryFromMemory(&model, &tinyErr, &tinyWarn,
                                         reinterpret_cast<const unsigned char*>(glb.data()),
                                         (unsigned int)glb.size(), baseDir);
    } else {
        ok = loader.LoadASCIIFromString(&model, &tinyErr, &tinyWarn, data.data(),
                                        (unsigned int)data.size(), baseDir);
    }

    if (!tinyWarn.empty()) warn += tinyWarn;
    if (!ok) err += tinyErr.empty() ? std::string("разбор glTF не удался") : tinyErr;
    // Здесь, в единственной двери, — чтобы все четыре читателя материалов
    // (статика, скелет, извлечение .sagemat, импорт) увидели обычный
    // металл-шероховатость и не знали о старом расширении вовсе.
    if (ok) ConvertSpecularGlossiness(model);
    (void)ec;
    return ok;
}

} // namespace sage::assets
