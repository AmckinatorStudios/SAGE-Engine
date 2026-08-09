// Реализация tinygltf развёрнута в Model.cpp — здесь только объявления
// (второе разворачивание дало бы дублирующиеся символы на линковке).
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

#include "sage/render/ModelMaterial.h"

#include "sage/assets/import/Importer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <stb_image.h>
#include <stb_image_write.h>   // реализация развёрнута в render/Screenshot.cpp
#include <tiny_obj_loader.h>

#include "sage/core/Log.h"

namespace fs = std::filesystem;

namespace ModelLoader {
namespace {

// Раскодировать картинку tinygltf сам не умеет — реализация собрана с
// TINYGLTF_NO_STB_IMAGE (см. Model.cpp), и БЕЗ этого колбэка любой файл с
// картинками разбирается с ошибкой «No LoadImageData callback specified», то
// есть материал не читается вообще. Такой же колбэк стоит у загрузчика
// геометрии; свой здесь, а не общий, потому что делить статическую функцию
// через границу единиц трансляции пришлось бы отдельным заголовком ради
// девяти строк.
bool DecodeImage(tinygltf::Image* image, const int, std::string* err, std::string*, int, int,
                 const unsigned char* bytes, int size, void*) {
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load_from_memory(bytes, size, &w, &h, &comp, 4); // всегда RGBA
    if (!data) {
        if (err) *err += "не удалось раскодировать изображение glTF\n";
        return false;
    }
    image->width = w;
    image->height = h;
    image->component = 4;
    image->bits = 8;
    image->pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
    image->image.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    return true;
}

std::string ExtLower(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

// Имя, годное для файла: пробелы и разделители из имени материала в Blender
// («Suit Teeth», «body/skin») в путь класть нельзя.
std::string FileSafe(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c >= 0x80;
        out.push_back(ok ? (char)c : '_');
    }
    return out;
}

// Куда класть создаваемые картинки и как их называть. Имя привязано к модели, а
// не к номеру текстуры: две модели в одной папке не должны затирать карты друг
// друга, а по имени файла должно быть видно, чьё оно.
//
// Slot — различитель МАТЕРИАЛА внутри модели. Пуст у одноматериальной модели
// (имена остаются прежними — <модель>_albedo.png), а у многоматериальной
// обязателен: без него четырнадцать albedo-карт Springbonnie написали бы друг
// поверх друга в один файл, и модель получила бы одну случайную текстуру на
// всё — то есть ровно тот симптом, за которым сюда и пришли.
struct TextureSink {
    fs::path Dir;
    std::string Stem;
    std::string Slot;

    fs::path PathFor(const char* usage) const {
        return Dir / (Stem + (Slot.empty() ? "" : "_" + Slot) + "_" + usage + ".png");
    }
};

// Записывает картинку, если её ещё нет. Возвращает путь или пустую строку.
//
// Существующий файл НЕ перезаписывается: карты могли быть поправлены руками
// после первого импорта, и повторный «загрузить модель» не повод стирать эту
// работу. Перезапись — отдельное осознанное действие (удалить файл).
std::string WritePng(const TextureSink& sink, const char* usage, const unsigned char* pixels,
                     int w, int h, int channels, std::vector<std::string>& warnings) {
    if (!pixels || w <= 0 || h <= 0) return {};
    const fs::path out = sink.PathFor(usage);
    std::error_code ec;
    if (fs::exists(out, ec)) return out.generic_string();
    fs::create_directories(out.parent_path(), ec);
    if (!stbi_write_png(out.string().c_str(), w, h, channels, pixels, w * channels)) {
        warnings.push_back(std::string("не удалось записать карту ") + usage + ": " +
                           out.string());
        return {};
    }
    return out.generic_string();
}

// Достаёт ОДИН канал из RGBA в одноканальную картинку. Нужно потому, что glTF
// пакует несколько карт в одну текстуру (occlusion в R, roughness в G, metallic
// в B), а движок читает свои карты из R-канала — отдав ему упакованную
// текстуру как есть, мы взяли бы металличность оттуда, где лежит AO.
std::vector<unsigned char> ExtractChannel(const std::vector<unsigned char>& rgba, int w, int h,
                                          int comp, int channel) {
    std::vector<unsigned char> out;
    if (comp <= channel || (size_t)w * h * comp > rgba.size()) return out;
    out.resize((size_t)w * h);
    for (size_t i = 0; i < out.size(); ++i) out[i] = rgba[i * comp + channel];
    return out;
}

// --- glTF / GLB --------------------------------------------------------------

struct GltfImageAccess {
    const tinygltf::Model* Model = nullptr;
    fs::path ModelDir;

    // Индекс изображения по индексу элемента textures[].
    int ImageOf(int textureIndex) const {
        if (!Model || textureIndex < 0 || textureIndex >= (int)Model->textures.size()) return -1;
        return Model->textures[textureIndex].source;
    }

    // Картинка лежит отдельным файлом рядом с моделью? Тогда её не надо ни
    // раскодировать, ни переписывать — просто сослаться.
    std::string ExternalPath(int imageIndex) const {
        if (!Model || imageIndex < 0 || imageIndex >= (int)Model->images.size()) return {};
        const std::string& uri = Model->images[imageIndex].uri;
        // data:... — картинка встроена в сам файл, файла на диске нет.
        if (uri.empty() || uri.rfind("data:", 0) == 0) return {};
        std::error_code ec;
        const fs::path p = ModelDir / uri;
        if (!fs::exists(p, ec)) return {};
        return p.generic_string();
    }
};

void ExtractGltfMaterial(const tinygltf::Model& model, const std::string& path,
                         const tinygltf::Material& m, const TextureSink& sink,
                         ExtractedMaterial& out) {
    out.Found = true;
    out.Name = m.name;

    const tinygltf::PbrMetallicRoughness& pbr = m.pbrMetallicRoughness;
    if (pbr.baseColorFactor.size() >= 4) {
        out.Albedo = glm::vec3((float)pbr.baseColorFactor[0], (float)pbr.baseColorFactor[1],
                               (float)pbr.baseColorFactor[2]);
        out.Opacity = (float)pbr.baseColorFactor[3];
    }
    out.Metallic = (float)pbr.metallicFactor;
    out.Roughness = (float)pbr.roughnessFactor;
    if (m.emissiveFactor.size() >= 3) {
        out.Emissive = glm::vec3((float)m.emissiveFactor[0], (float)m.emissiveFactor[1],
                                 (float)m.emissiveFactor[2]);
    }

    GltfImageAccess images{&model, fs::path(path).parent_path()};

    // Карта, которая идёт в движок ЦЕЛИКОМ (albedo, normal, emissive): либо
    // ссылка на соседний файл, либо выложенная рядом копия встроенной.
    auto wholeMap = [&](int textureIndex, const char* usage) -> std::string {
        const int img = images.ImageOf(textureIndex);
        if (img < 0) return {};
        if (std::string external = images.ExternalPath(img); !external.empty()) return external;
        const tinygltf::Image& src = model.images[img];
        if (src.image.empty() || src.component < 1) {
            out.Warnings.push_back(std::string("карта ") + usage + " не раскодирована");
            return {};
        }
        return WritePng(sink, usage, src.image.data(), src.width, src.height, src.component,
                        out.Warnings);
    };

    // Карта, которую надо РАСПАКОВАТЬ по каналу (metallic/roughness/AO).
    auto channelMap = [&](int textureIndex, int channel, const char* usage) -> std::string {
        const int img = images.ImageOf(textureIndex);
        if (img < 0) return {};
        const tinygltf::Image& src = model.images[img];

        std::vector<unsigned char> decoded;
        const unsigned char* pixels = nullptr;
        int w = src.width, h = src.height, comp = src.component;
        if (!src.image.empty() && comp >= 1) {
            pixels = src.image.data();
        } else if (std::string external = images.ExternalPath(img); !external.empty()) {
            // Внешний файл: тут его всё же приходится раскодировать — канал
            // из сжатого png не достать.
            int c = 0;
            unsigned char* raw = stbi_load(external.c_str(), &w, &h, &c, 4);
            if (!raw) {
                out.Warnings.push_back(std::string("карта ") + usage + ": не читается " + external);
                return {};
            }
            decoded.assign(raw, raw + (size_t)w * h * 4);
            stbi_image_free(raw);
            comp = 4;
            pixels = decoded.data();
        } else {
            return {};
        }

        std::vector<unsigned char> single =
            ExtractChannel(std::vector<unsigned char>(pixels, pixels + (size_t)w * h * comp), w, h,
                           comp, channel);
        if (single.empty()) {
            out.Warnings.push_back(std::string("карта ") + usage + ": нет канала " +
                                   std::to_string(channel));
            return {};
        }
        return WritePng(sink, usage, single.data(), w, h, 1, out.Warnings);
    };

    out.AlbedoMap = wholeMap(pbr.baseColorTexture.index, "albedo");
    out.NormalMap = wholeMap(m.normalTexture.index, "normal");
    out.EmissiveMap = wholeMap(m.emissiveTexture.index, "emissive");
    // glTF: metallic в B, roughness в G одной текстуры; occlusion — R своей
    // (часто той же самой, ORM).
    out.MetallicMap = channelMap(pbr.metallicRoughnessTexture.index, 2, "metallic");
    out.RoughnessMap = channelMap(pbr.metallicRoughnessTexture.index, 1, "roughness");
    out.AOMap = channelMap(m.occlusionTexture.index, 0, "ao");

    // Карта задана — значит, вид определяет она, а фактор её лишь домножает
    // (семантика glTF). Фактор 0 при наличии карты обнулил бы карту целиком,
    // и «металл приехал без металличности» выглядело бы как потеря текстуры.
    if (!out.MetallicMap.empty() && out.Metallic <= 0.0f) out.Metallic = 1.0f;
    if (!out.RoughnessMap.empty() && out.Roughness <= 0.0f) out.Roughness = 1.0f;
    if (!out.EmissiveMap.empty() && out.Emissive == glm::vec3(0.0f)) out.Emissive = glm::vec3(1.0f);
}

// Различитель материала в именах создаваемых картинок. Имя, а не номер: по
// <модель>_SuitTeeth_albedo.png видно, чья это карта, а по <модель>_7_albedo.png
// — нет. Номер идёт в ход, только если имени нет или оно повторяется (в .glb
// одинаковые имена материалов встречаются).
std::string SlotName(const std::vector<std::string>& taken, const std::string& name, size_t index) {
    std::string slot = FileSafe(name);
    if (slot.empty() || std::find(taken.begin(), taken.end(), slot) != taken.end())
        slot = "mat" + std::to_string(index);
    return slot;
}

void ExtractGltf(const std::string& path, bool binary, const TextureSink& base,
                 ExtractedMaterialSet& out) {
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(&DecodeImage, nullptr);
    tinygltf::Model model;
    std::string err, warn;
    const bool ok = binary ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
                           : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!ok) {
        out.Warnings.push_back("материалы не прочитаны: " + (err.empty() ? "разбор glTF" : err));
        return;
    }
    if (model.materials.empty()) return;   // геометрия без материала — законно

    // ВСЕ материалы и СТРОГО В ПОРЯДКЕ ФАЙЛА: индекс здесь — тот же индекс, что
    // стоит в разметке меша (sage::render::Submesh::Material).
    const bool many = model.materials.size() > 1;
    std::vector<std::string> slots;
    for (size_t i = 0; i < model.materials.size(); ++i) {
        const tinygltf::Material& m = model.materials[i];
        TextureSink sink = base;
        if (many) {
            sink.Slot = SlotName(slots, m.name, i);
            slots.push_back(sink.Slot);
        }
        ExtractedMaterial extracted;
        ExtractGltfMaterial(model, path, m, sink, extracted);
        out.Materials.push_back(std::move(extracted));
    }
}

// --- OBJ + MTL ---------------------------------------------------------------

void ExtractObjMaterial(const fs::path& dir, const tinyobj::material_t& m, ExtractedMaterial& out) {
    out.Found = true;
    out.Name = m.name;
    out.Albedo = glm::vec3(m.diffuse[0], m.diffuse[1], m.diffuse[2]);
    out.Emissive = glm::vec3(m.emission[0], m.emission[1], m.emission[2]);
    out.Metallic = m.metallic;
    // Roughness в .mtl (map_Pr/Pr) есть далеко не всегда, и ноль по умолчанию
    // означал бы зеркало на каждой модели без PBR-полей. Отсутствие — это «не
    // задано», а не «идеально гладкая».
    out.Roughness = m.roughness > 0.0f ? m.roughness : 0.5f;
    out.Opacity = m.dissolve;

    // Пути в .mtl — относительно самого .mtl, то есть папки модели.
    auto resolve = [&](const std::string& name) -> std::string {
        if (name.empty()) return {};
        std::error_code ec;
        const fs::path p = fs::path(name).is_absolute() ? fs::path(name) : dir / name;
        if (!fs::exists(p, ec)) {
            out.Warnings.push_back("текстура не найдена: " + name);
            return {};
        }
        return p.generic_string();
    };

    out.AlbedoMap = resolve(m.diffuse_texname);
    // norm (PBR-расширение) точнее, чем map_bump: последний бывает и картой
    // высот, но отличить их по файлу нельзя, а нормаль-как-высота хотя бы не
    // ломает освещение целиком.
    out.NormalMap = resolve(!m.normal_texname.empty() ? m.normal_texname : m.bump_texname);
    out.MetallicMap = resolve(m.metallic_texname);
    out.RoughnessMap = resolve(m.roughness_texname);
    out.AOMap = resolve(m.ambient_texname);
    out.EmissiveMap = resolve(m.emissive_texname);

    if (!out.MetallicMap.empty() && out.Metallic <= 0.0f) out.Metallic = 1.0f;
    if (!out.EmissiveMap.empty() && out.Emissive == glm::vec3(0.0f)) out.Emissive = glm::vec3(1.0f);
}

void ExtractObj(const std::string& path, ExtractedMaterialSet& out) {
    const fs::path dir = fs::path(path).parent_path();

    tinyobj::ObjReaderConfig config;
    config.mtl_search_path = dir.empty() ? "." : dir.string();
    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, config)) {
        out.Warnings.push_back("материалы не прочитаны: " +
                               (reader.Error().empty() ? std::string("разбор OBJ") : reader.Error()));
        return;
    }
    const std::vector<tinyobj::material_t>& mats = reader.GetMaterials();
    if (mats.empty()) return;   // нет .mtl — законно

    // Порядок tinyobj — порядок .mtl, он же индекс в разметке меша
    // (LoadObjData ставит в Submesh::Material тот же material_id).
    for (const tinyobj::material_t& m : mats) {
        ExtractedMaterial extracted;
        ExtractObjMaterial(dir, m, extracted);
        out.Materials.push_back(std::move(extracted));
    }
}

} // namespace

// Материал FBX берётся у ИМПОРТЁРА: он уже разбирает блоки Material, Texture и
// связи между ними (Texture --OP--> Material --OO--> Model). Второй разборщик
// того же формата здесь означал бы две реализации одной задачи, которые
// разойдутся на первом же экспортёре с непривычными именами слотов.
//
// Пути к картам в FBX почти всегда чужие и абсолютные («C:\\Users\\...»),
// поэтому картинка ищется по ИМЕНИ ФАЙЛА рядом с моделью и в её подпапках:
// набор, скачанный одной папкой (source/ + textures/), собирается сам.
void ExtractFbx(const std::string& modelPath, ExtractedMaterialSet& set) {
    sage::assets::ImportedScene scene;
    std::string err;
    if (!sage::assets::ImporterRegistry::Instance().Import(modelPath, scene, err)) {
        set.Warnings.push_back("материалы FBX не прочитаны: " + err);
        return;
    }
    if (scene.Materials.empty()) return;

    const fs::path modelDir = fs::path(modelPath).parent_path();

    // Порядок импортёра — порядок материалов в файле, он же индекс в разметке
    // меша: FBX едет в движок через тот же ImportedScene::Flatten().
    for (const sage::assets::ImportedMaterial& m : scene.Materials) {
        ExtractedMaterial out;

        auto locate = [&](const std::string& ref) -> std::string {
            if (ref.empty()) return {};
            std::error_code ec;
            const fs::path direct = modelDir / ref;
            if (fs::exists(direct, ec) && !fs::is_directory(direct, ec))
                return direct.generic_string();
            const std::string name = fs::path(ref).filename().string();
            // Пустая ссылка вида «.» — не картинка. Экспортёры оставляют такие
            // заглушки у материалов без текстур, и без этой проверки путь
            // разрешался бы в САМУ ПАПКУ модели: слот «есть, но не грузится».
            if (name.empty() || name == "." || name == "..") return {};
            // Соседние папки: у наборов из сети текстуры лежат в textures/, а
            // модель — в source/, то есть на уровень выше и вбок.
            const fs::path roots[] = {modelDir, modelDir / "textures", modelDir.parent_path(),
                                      modelDir.parent_path() / "textures"};
            for (const fs::path& root : roots) {
                if (root.empty()) continue;
                const fs::path candidate = root / name;
                if (fs::exists(candidate, ec)) return candidate.generic_string();
            }
            out.Warnings.push_back("текстура не найдена рядом с моделью: " + name);
            return {};
        };

        out.Found = true;
        out.Name = m.Name;
        out.Albedo = m.Albedo;
        out.Emissive = m.Emissive;
        out.Metallic = m.Metallic;
        out.Roughness = m.Roughness;
        out.Opacity = m.Opacity;
        out.AlbedoMap = locate(m.AlbedoTexture);
        out.NormalMap = locate(m.NormalTexture);
        out.MetallicMap = locate(m.MetallicTexture);
        out.RoughnessMap = locate(m.RoughnessTexture);
        out.AOMap = locate(m.AOTexture);
        out.EmissiveMap = locate(m.EmissiveTexture);
        set.Materials.push_back(std::move(out));
    }
}

ExtractedMaterialSet ExtractMaterials(const std::string& modelPath, const std::string& textureDir) {
    ExtractedMaterialSet out;

    std::error_code ec;
    if (!fs::exists(modelPath, ec)) {
        out.Warnings.push_back("файл модели не найден: " + modelPath);
        LOG_WARN("Model") << modelPath << ": " << out.Warnings.back();
        return out;
    }

    const fs::path model(modelPath);
    TextureSink sink;
    sink.Dir = textureDir.empty() ? model.parent_path() : fs::path(textureDir);
    sink.Stem = model.stem().string();

    const std::string ext = ExtLower(modelPath);
    try {
        if (ext == "gltf") ExtractGltf(modelPath, false, sink, out);
        else if (ext == "glb") ExtractGltf(modelPath, true, sink, out);
        else if (ext == "obj") ExtractObj(modelPath, out);
        else if (ext == "fbx") ExtractFbx(modelPath, out);
        // Прочие форматы (свой .sagemesh, .bbmodel, что зарегистрирует игра)
        // материал не несут — молчание здесь правильное, предупреждать не о чем.
    } catch (const std::exception& e) {
        // Импорт материалов не должен ронять уже удавшуюся загрузку геометрии.
        out.Warnings.push_back(std::string("материалы не прочитаны: ") + e.what());
        out.Materials.clear();
    }

    for (const std::string& w : out.Warnings) LOG_WARN("Model") << modelPath << ": " << w;
    for (const ExtractedMaterial& m : out.Materials)
        for (const std::string& w : m.Warnings)
            LOG_WARN("Model") << modelPath << " [" << m.Name << "]: " << w;
    return out;
}

ExtractedMaterial ExtractMaterial(const std::string& modelPath, const std::string& textureDir) {
    ExtractedMaterialSet set = ExtractMaterials(modelPath, textureDir);
    if (set.Materials.empty()) {
        // Материалов нет — отдаём пустой результат, но с причиной: «модель
        // белая» без объяснения это тупик, в котором человек ищет ошибку у себя.
        ExtractedMaterial empty;
        empty.Warnings = std::move(set.Warnings);
        return empty;
    }
    ExtractedMaterial first = std::move(set.Materials.front());
    // Предупреждения разбора (не привязанные к материалу) остаются видны и
    // здесь: иначе они пропадали бы ровно у того вызова, который чаще всего и
    // делают.
    first.Warnings.insert(first.Warnings.begin(), set.Warnings.begin(), set.Warnings.end());
    return first;
}

} // namespace ModelLoader
