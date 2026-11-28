#include "sage/assets/Pack.h"
#include <array>
#include <filesystem>
#include "ResourceManager.h"
#include "sage/render/MeshData.h"
#include "sage/core/EngineContext.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Quarantine.h"
#include "sage/assets/import/ModelProbe.h"
#include "sage/render/AlphaBleed.h"
#include "sage/render/EffectChain.h"
#include "sage/render/SkinnedModel.h"

#include <stb_image.h> // реализация STB_IMAGE_IMPLEMENTATION живёт в Texture.cpp

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <cmath>
#include <stdexcept>
#include <chrono>
#include "sage/render/ResourceManagerDetail.h"
#include "sage/rhi/GraphicsDevice.h"

using namespace resource_detail;



// --- GL-независимое декодирование (используется и воркером, и юнит-тестом) ---
ResourceManager::ResourceManager()
    : m_models(m_cache.Models),
      m_materials(m_cache.Materials),
      m_modelStamps(m_cache.ModelStamps),
      m_materialStamps(m_cache.MaterialStamps),
      m_skinnedStamps(m_cache.SkinnedStamps),
      m_skinned(m_cache.Skinned),
      m_shaders(m_cache.Shaders),
      m_skyboxes(m_cache.Skyboxes),
      m_textures(m_cache.Textures),
      m_reloadCursor(m_cache.ReloadCursor),
      m_assetsGeneration(m_cache.AssetsGeneration),
      m_textureBudget(m_vram.BudgetRef()),
      m_textureBytes(m_vram.ResidentRef()),
      m_evictions(m_vram.EvictionsRef()),
      m_downgrades(m_vram.DowngradesRef()) {}

ResourceManager::ResourceManager(sage::AssetDatabase& assets) : ResourceManager() {
    m_assets = &assets;
}

std::string ResourceManager::Locate(const std::string& path) const {
    // Только default-конструктор сохраняет прежнюю позднюю маршрутизацию.
    // Owned-кэш контекста не зависит от текущего thread binding.
    auto& assets = m_assets ? *m_assets : sage::AssetDatabase::Instance();
    return assets.LocatePath(path);
}

std::string ResourceManager::CacheKey(const std::string& path) const {
    if (path.empty()) return path;
    std::error_code ec;
    const auto full = std::filesystem::weakly_canonical(Locate(path), ec);
    if (ec || full.empty()) return std::filesystem::path(path).lexically_normal().generic_string();
    return full.generic_string();
}

ResourceManager& ResourceManager::Instance() {
    return sage::EngineContext::Current().Resources();
}

bool ResourceManager::DecodeImageFile(const std::string& path, std::vector<unsigned char>& outRGBA,
                                      int& outW, int& outH) {
    return sage::render::TextureStreamer::DecodeImageFile(path, outRGBA, outW, outH);
}

// --- Чистая LRU-политика вытеснения (ядро менеджера памяти, юнит-тестируемо) ---
std::vector<size_t> ResourceManager::SelectEvictions(const std::vector<EvictCandidate>& candidates,
                                                     size_t currentBytes, size_t budget) {
    return sage::render::VRAMBudgetManager::SelectEvictions(candidates, currentBytes, budget);
}



std::shared_ptr<Mesh> ResourceManager::GetModel(const std::string& path, int part) {
    if (part < 0) return GetModel(path);
    const std::string key = CacheKey(path) + kPartSep + std::to_string(part);
    auto it = m_models.find(key);
    if (it != m_models.end()) return it->second;
    std::shared_ptr<Mesh> mesh;
    if (sage::assets::quarantine::Blocked(path)) {
        LOG_ERROR("Resources") << "Модель «" << path << "» в карантине: на ней оборвался прошлый запуск.";
    } else {
        sage::assets::quarantine::Begin(path);
        try {
            mesh = ModelLoader::LoadMeshPart(Locate(path), part, m_keepMeshCpu);
        } catch (const std::exception& e) {
            LOG_ERROR("Resources") << "Часть модели не загрузилась (" << path << ", №" << part
                                   << "): " << e.what();
        }
        sage::assets::quarantine::End();
    }
    m_models[key] = mesh;
    m_modelStamps[key] = FileStamp(Locate(path));
    return mesh;
}

std::shared_ptr<Mesh> ResourceManager::GetModel(const std::string& path) {
    const std::string key = CacheKey(path);
    auto it = m_models.find(key);
    if (it != m_models.end()) return it->second;
    std::shared_ptr<Mesh> mesh;
    // КАРАНТИН: на этом файле прошлый запуск умер (см. assets/Quarantine.h).
    // Загружать его снова значит повторить падение — и запереть редактор в
    // кольце «открыл проект — упал».
    if (sage::assets::quarantine::Blocked(path)) {
        LOG_ERROR("Resources") << "Модель «" << path
                               << "» в карантине: на ней оборвался прошлый запуск. "
                               << "Перезапишите файл — карантин снимется сам.";
    } else {
        sage::assets::quarantine::Begin(path);
        try {
            mesh = ModelLoader::LoadMesh(Locate(path), m_keepMeshCpu);
        } catch (const std::exception& e) {
            LOG_ERROR("Resources") << "Модель не загрузилась (" << path << "): " << e.what();
        }
        sage::assets::quarantine::End();
    }
    m_models[key] = mesh; // в т.ч. nullptr — негативный кэш (не перечитывать битый файл)
    m_modelStamps[key] = FileStamp(Locate(path));
    return mesh;
}

std::shared_ptr<sage::render::SkinnedModel> ResourceManager::GetSkinnedModel(
    const std::string& path) {
    const std::string key = CacheKey(path);
    auto it = m_skinned.find(key);
    if (it != m_skinned.end()) return it->second;
    std::shared_ptr<sage::render::SkinnedModel> model;
    // СНАЧАЛА — ЕСТЬ ЛИ ВООБЩЕ СКЕЛЕТ. Редактор спрашивает скелетную версию у
    // каждой поставленной в сцену модели, и без этой проверки .obj уходил в
    // разбор glTF («Скиннинг-модель не загрузилась … parse error» — ошибкой на
    // обычной декорации), а статический .gltf разбирался целиком, с декодом
    // всех текстур, только чтобы узнать, что костей нет. Проверка читает одно
    // оглавление файла (assets/import/ModelProbe.h); «нет скелета» — не
    // ошибка, а обычное состояние модели, и в лог оно не пишется.
    if (!sage::assets::ModelHasSkeleton(Locate(path))) {
        m_skinned[key] = nullptr;
        m_skinnedStamps[key] = FileStamp(Locate(path));
        return nullptr;
    }
    if (sage::assets::quarantine::Blocked(path)) {
        LOG_ERROR("Resources") << "Скелетная модель «" << path
                               << "» в карантине: на ней оборвался прошлый запуск. "
                               << "Перезапишите файл — карантин снимется сам.";
    } else {
        sage::assets::quarantine::Begin(path);
        try {
            model = sage::render::SkinnedModel::Load(Locate(path));
        } catch (const std::exception& e) {
            LOG_ERROR("Resources") << "Скиннинг-модель не загрузилась (" << path << "): "
                                   << e.what();
        }
        sage::assets::quarantine::End();
    }
    m_skinned[key] = model; // в т.ч. nullptr — негативный кэш
    m_skinnedStamps[key] = FileStamp(Locate(path));
    return model;
}

std::shared_ptr<Mesh> ResourceManager::ReloadModel(const std::string& path) {
    const std::string key = CacheKey(path);
    m_models.erase(key); // сброс кэша -> GetModel перечитает с диска (новый .sageimport)
    m_modelStamps.erase(key);
    return GetModel(path);
}

sage::assets::ResolvedSprite ResourceManager::ResolveSprite(sage::assets::SpriteRef ref, sage::AssetGuid dependent) {
    auto& database = m_assets ? *m_assets : sage::AssetDatabase::Instance();
    return m_sprites.Resolve(database, ref, dependent);
}

ResourceManager::SpriteTexture ResourceManager::ResolveSpriteFrames(const std::vector<std::string>& frames,
                                                                       TextureFilter filter) {
    auto& database = m_assets ? *m_assets : sage::AssetDatabase::Instance();
    const int maxSide = sage::rhi::GraphicsDevice::Get().Capabilities().MaxTexture2DSize;
    return SpriteTextureFor(m_sprites.ResolveFrames(database, frames, maxSide), filter, false, false, true);
}

sage::assets::ResolvedSprite ResourceManager::ResolveSpriteSheet(sage::AssetGuid sheet) {
    auto& database = m_assets ? *m_assets : sage::AssetDatabase::Instance();
    return m_sprites.ResolveSheet(database, sheet);
}

sage::assets::ResolvedSprite ResourceManager::ResolveSpriteImage(const std::string& path, bool allowMissingSheet) {
    auto& database = m_assets ? *m_assets : sage::AssetDatabase::Instance();
    return m_sprites.ResolveImage(database, path, allowMissingSheet);
}

ResourceManager::SpriteTexture ResourceManager::ResolveSpriteTexture(sage::assets::SpriteRef ref,
                                                                     TextureFilter filter, bool mipmaps,
                                                                     bool bleed, bool srgb, bool refresh) {
    auto& database = m_assets ? *m_assets : sage::AssetDatabase::Instance();
    return SpriteTextureFor(m_sprites.Resolve(database, ref, {}, refresh), filter, mipmaps, bleed, srgb);
}

ResourceManager::SpriteTexture ResourceManager::SpriteTextureFor(sage::assets::ResolvedSprite data,
                                                                 TextureFilter filter, bool mipmaps,
                                                                 bool bleed, bool srgb) {
    SpriteTexture result{std::move(data), {}};
    if (!result.Data || !result.Data.Resource->Image) return result;
    auto upload = [&](const sage::assets::ResolvedSprite& data) -> std::shared_ptr<const Texture> {
        const auto& image = *data.Resource->Image;
        const auto key = "@sprite/" + std::to_string(image.Generation) + SamplingSuffix(filter, mipmaps, bleed, srgb);
        if (auto found = m_textures.find(key); found != m_textures.end()) {
            found->second.Tick = NextTick();
            return found->second.Tex;
        }
        const auto limit = sage::rhi::GraphicsDevice::Get().Capabilities().MaxTexture2DSize;
        if (limit > 0 && (image.Width > limit || image.Height > limit))
            throw std::runtime_error("sprite exceeds maximum GPU texture dimensions");
        const unsigned char* pixels = image.Pixels.data();
        std::vector<unsigned char> processed;
        if (bleed) {
            processed = image.Pixels;
            sage::render::BleedTransparentColor(processed, image.Width, image.Height);
            pixels = processed.data();
        }
        TextureRecord record;
        record.Tex = std::make_shared<Texture>(pixels, image.Width, image.Height, filter, mipmaps, srgb);
        record.Tex->SetFlippedRows(false);
        record.SpriteSnapshot = true;
        record.Tick = NextTick();
        record.Bytes = record.Tex->GpuBytes();
        auto texture = record.Tex;
        m_textureBytes += record.Bytes;
        m_textures.emplace(key, std::move(record));
        EvictToBudget();
        return texture;
    };
    try {
        result.Image = upload(result.Data);
    } catch (const std::exception& error) {
        result.Data.Diagnostic = error.what();
        LOG_WARN("Sprites") << result.Data.Diagnostic;
        auto previous = m_sprites.Previous(result.Data.Ref);
        if (previous) {
            try {
                result.Image = upload(previous);
                previous.Diagnostic = std::move(result.Data.Diagnostic);
                result.Data = std::move(previous);
            } catch (const std::exception& fallback) {
                result.Data.Diagnostic += "; previous image: ";
                result.Data.Diagnostic += fallback.what();
            }
        }
    }
    return result;
}

ResourceManager::SpriteFrame ResourceManager::SpriteTexture::Frame(const sage::assets::SpriteRegion& region) const {
    auto uv = Data.UVFor(region);
    // Keep linear filtering within the selected region, even on an unpadded grid.
    const glm::vec2 halfPixel(0.5f / Image->Width(), 0.5f / Image->Height());
    uv += glm::vec4(halfPixel.x, halfPixel.y, -halfPixel.x, -halfPixel.y);
    return {{uv.x, Image->FlippedRows() ? 1.0f - uv.w : uv.w,
             uv.z, Image->FlippedRows() ? 1.0f - uv.y : uv.y},
            {0.5f - region.Pivot.x, region.Pivot.y - 0.5f}};
}

std::shared_ptr<const Texture> ResourceManager::SpriteFrameTable(const SpriteTexture& sprite, int frameCount) {
    constexpr int width = 128;
    constexpr size_t byteLimit = 64u << 20;
    if (!sprite || frameCount <= 0) return {};
    const auto& data = sprite.Data;
    const auto key = "@sprite-frames/" + std::to_string(data.Resource->Number) + "/" +
                     data.Ref.Item.ToString() + "/" + std::to_string(static_cast<int>(data.Ref.Type)) +
                     "/" + std::to_string(frameCount);
    if (auto found = m_textures.find(key); found != m_textures.end()) {
        found->second.Tick = NextTick();
        return found->second.Tex;
    }
    // Two RGBA32F texels per frame: UV rectangle and pivot. Keep the instance stream
    // the same size for legacy grids and arbitrary atlas regions.
    const auto* sequence = data.Sequence();
    if (size_t(frameCount) > (sequence ? sequence->FrameIds().size() : 1)) return {};
    const size_t height = (size_t(frameCount) + width / 2 - 1) / (width / 2);
    const auto maxSide = sage::rhi::GraphicsDevice::Get().Capabilities().MaxTexture2DSize;
    if (maxSide < width || height > size_t(maxSide) || height > byteLimit / (width * 4 * sizeof(float)))
        return {};
    std::vector<float> pixels(width * height * 4, 0.0f);
    for (int i = 0; i < frameCount; ++i) {
        const auto* region = sequence ? data.Resource->Sheet.Find(sequence->FrameIds()[i]) : data.Region();
        if (!region) return {};
        const auto frame = sprite.Frame(*region);
        auto* pixel = pixels.data() + size_t(i) * 8;
        std::copy_n(&frame.UV.x, 4, pixel);
        pixel[4] = frame.Pivot.x;
        pixel[5] = frame.Pivot.y;
    }
    try {
        TextureRecord record;
        record.Tex = Texture::CreateFloat32Data(pixels.data(), width, static_cast<int>(height));
        record.SpriteSnapshot = true;
        record.Tick = NextTick();
        record.Bytes = record.Tex->GpuBytes();
        const auto texture = record.Tex;
        m_textureBytes += record.Bytes;
        m_textures.emplace(key, std::move(record));
        EvictToBudget();
        return texture;
    } catch (const std::exception& error) {
        LOG_WARN("Sprites") << error.what();
        return {};
    }
}

int ResourceManager::ReloadChangedAssets(size_t budget) {
    int reloaded = 0;
    if (!m_sprites.Empty()) {
        auto& database = m_assets ? *m_assets : sage::AssetDatabase::Instance();
        reloaded = static_cast<int>(m_sprites.Poll(database, budget));
    }

    // ОКНО ОПРОСА. Все записи четырёх кэшей пронумерованы подряд (модели,
    // материалы, скелетные модели, текстуры), и за вызов опрашиваются только
    // номера из окна [курсор, курсор + budget) по кругу. Обход самих
    // контейнеров дёшев — дорог системный вызов на каждый файл, и именно их
    // число бюджет ограничивает.
    const size_t total =
        m_modelStamps.size() + m_materialStamps.size() + m_skinnedStamps.size() + m_textures.size();
    if (total == 0) {
        if (reloaded > 0) ++m_assetsGeneration;
        return reloaded;
    }
    const bool all = budget == 0 || budget >= total;
    const size_t start = all ? 0 : m_reloadCursor % total;
    const size_t window = all ? total : budget;
    if (!all) m_reloadCursor = (start + window) % total;
    size_t index = 0;
    auto inWindow = [&]() {
        const size_t i = index++;
        return all || (i + total - start) % total < window;
    };

    // Списки путей собираются ЗАРАНЕЕ: перезагрузка меняет те самые
    // контейнеры, по которым идёт обход, и обходить их на ходу — обращение по
    // недействительному итератору.
    std::vector<std::string> staleModels;
    for (const auto& [path, stamp] : m_modelStamps) {
        if (inWindow() && FileStamp(Locate(FileOfKey(path))) != stamp) staleModels.push_back(path);
    }
    std::vector<std::string> staleMaterials;
    for (const auto& [path, stamp] : m_materialStamps) {
        if (inWindow() && FileStamp(Locate(path)) != stamp) staleMaterials.push_back(path);
    }
    std::vector<std::string> staleSkinned;
    for (const auto& [path, stamp] : m_skinnedStamps) {
        if (inWindow() && FileStamp(Locate(path)) != stamp) staleSkinned.push_back(path);
    }
    // КЛЮЧ, А НЕ ПУТЬ: у одного файла в кэше столько записей, сколько у него
    // разных настроек выборки, и перечитать надо КАЖДУЮ — иначе сглаженная
    // копия обновится, а резкая останется со вчерашними пикселями.
    std::vector<std::string> staleTextures;
    for (const auto& [key, rec] : m_textures) {
        if (!inWindow()) continue;
        // Процедурную (Generated) перечитывать неоткуда, а грузящуюся (Pending)
        // рано: её пиксели ещё едут из фонового потока, и подмена сейчас
        // означала бы гонку с ними.
        if (rec.Generated || rec.SpriteSnapshot || rec.Pending || !rec.Tex) continue;
        if (FileStamp(Locate(rec.Source)) != rec.Stamp) staleTextures.push_back(key);
    }

    for (const std::string& path : staleModels) {
        // Меш подменяется НА МЕСТЕ (по тому же shared_ptr), а не заводится
        // заново: на прежний указывают компоненты сцены, и заменив запись в
        // кэше, мы оставили бы их со старой геометрией — то есть починили бы
        // кэш и не починили картинку.
        std::shared_ptr<Mesh> existing = m_models[path];
        m_models.erase(path);
        m_modelStamps.erase(path);
        std::shared_ptr<Mesh> fresh = GetModel(FileOfKey(path), PartOfKey(path));
        if (existing && fresh && existing != fresh) {
            *existing = std::move(*fresh);
            m_models[path] = existing;
        }
        ++reloaded;
        LOG_INFO("Resources") << "Модель перечитана: " << path;
    }

    for (const std::string& path : staleMaterials) {
        std::shared_ptr<Material> existing = m_materials[path];
        m_materials.erase(path);
        m_materialStamps.erase(path);
        std::shared_ptr<Material> fresh = GetMaterial(path);
        if (existing && fresh && existing != fresh) {
            *existing = *fresh;
            m_materials[path] = existing;
        }
        ++reloaded;
        LOG_INFO("Resources") << "Материал перечитан: " << path;
    }

    // СКЕЛЕТНАЯ МОДЕЛЬ — ЗАМЕНОЙ ЗАПИСИ, А НЕ ПОДМЕНОЙ СОДЕРЖИМОГО.
    //
    // На обычный меш смотрят только как на геометрию, и подмена на месте
    // безопасна. У скелетной модели наружу торчат УКАЗАТЕЛИ ВНУТРЬ: аниматор
    // держит адреса скелета и списка клипов (см. AnimationSystem), и подменить
    // содержимое под ним значит оставить его с индексами костей, которых в
    // новом скелете может не быть. Поэтому запись в кэше заменяется целиком, а
    // тот, кто держит старую модель через shared_ptr, доживает с ней до того,
    // как заметит смену поколения (AssetsGeneration) и переспросит.
    for (const std::string& path : staleSkinned) {
        m_skinned.erase(path);
        m_skinnedStamps.erase(path);
        GetSkinnedModel(path);
        ++reloaded;
        LOG_INFO("Resources") << "Скелетная модель перечитана: " << path;
    }

    // ТЕКСТУРА — НА МЕСТЕ: на неё ссылаются материалы и интерфейс, и заменить
    // запись в кэше значило бы оставить их со старой картинкой.
    for (const std::string& key : staleTextures) {
        TextureRecord& rec = m_textures[key];
        const std::string path = rec.Source;
        try {
            *rec.Tex = std::move(
                *LoadTextureFile(Locate(path), rec.Filter, rec.Mipmaps, rec.Bleed, rec.Srgb));
            m_textureBytes -= std::min(m_textureBytes, rec.Bytes);
            rec.Bytes = rec.Tex->GpuBytes();
            m_textureBytes += rec.Bytes;
            rec.Stamp = FileStamp(Locate(path));
            ++reloaded;
            LOG_INFO("Resources") << "Текстура перечитана: " << path;
        } catch (const std::exception& e) {
            // Не получилось — оставляем прежнюю картинку и ЗАПОМИНАЕМ штамп:
            // иначе битый файл перечитывался бы каждый кадр, заваливая лог.
            rec.Stamp = FileStamp(Locate(path));
            LOG_ERROR("Resources") << "Текстура не перечиталась (" << path << "): " << e.what();
        }
    }

    // Поколение растёт ОДИН раз на пачку: кэши, которые по нему сбрасываются,
    // должны сброситься от факта перезагрузки, а не по числу файлов.
    if (reloaded > 0) ++m_assetsGeneration;
    return reloaded;
}

// Картинка с диска в текстуру. Обычная — прямо конструктором Texture (он
// знает и свой формат .sagetex, и число каналов файла); с bleed — через
// декод в память, где цвет под прозрачностью правится до заливки.
std::shared_ptr<Texture> ResourceManager::LoadTextureFile(const std::string& file,
                                                          TextureFilter filter, bool mipmaps,
                                                          bool bleed, bool srgb) {
    if (!bleed) return std::make_shared<Texture>(file, filter, mipmaps, srgb);
    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
    if (!DecodeImageFile(file, pixels, w, h))
        return std::make_shared<Texture>(file, filter, mipmaps, srgb);   // .sagetex и прочее
    sage::render::BleedTransparentColor(pixels, w, h);
    return std::make_shared<Texture>(pixels.data(), w, h, filter, mipmaps, srgb);
}

int ResourceManager::PendingAsyncTextures() const {
    return m_streamer.Pending();
}

std::shared_ptr<Texture> ResourceManager::GetTexture(const std::string& path,
                                                    TextureFilter filter, bool mipmaps,
                                                    bool bleed, bool srgb) {
    if (path.empty()) return nullptr;
    // Ключ = файл + настройки выборки: две фильтрации одного файла — это две
    // картинки, а не одна, которую отнимают друг у друга (см. CacheKey выше).
    const std::string key = CacheKey(path) + SamplingSuffix(filter, mipmaps, bleed, srgb);
    auto it = m_textures.find(key);
    if (it != m_textures.end()) {
        it->second.Tick = NextTick(); // обращение -> «свежая» для LRU
        return it->second.Tex;
    }
    // Процедурная картинка зарегистрирована ПОД ИМЕНЕМ, без настроек выборки:
    // её пиксели собраны движком, перечитывать их неоткуда, и просить её с
    // другой фильтрацией — значит просить ту же самую.
    auto generated = m_textures.find(CacheKey(path));
    if (generated != m_textures.end() && generated->second.Generated) {
        generated->second.Tick = NextTick();
        return generated->second.Tex;
    }
    TextureRecord rec;
    rec.Source = path;
    rec.Filter = filter;
    rec.Mipmaps = mipmaps;
    rec.Bleed = bleed;
    rec.Srgb = srgb;
    try {
        rec.Tex = LoadTextureFile(Locate(path), filter, mipmaps, bleed, srgb);
        rec.Bytes = rec.Tex->GpuBytes();
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Текстура не загрузилась (" << path << "): " << e.what();
        rec.Tex = nullptr; // негативный кэш
        rec.Bytes = 0;
    }
    rec.Tick = NextTick();
    rec.Stamp = FileStamp(Locate(path));
    std::shared_ptr<Texture> result = rec.Tex; // держим ссылку -> не вытеснится ниже
    m_textureBytes += rec.Bytes;
    m_textures[key] = std::move(rec);
    EvictToBudget();
    return result;
}

std::shared_ptr<Texture> ResourceManager::GetTextureAsync(const std::string& path,
                                                         TextureFilter filter, bool mipmaps,
                                                         bool bleed, bool srgb) {
    if (!m_acceptingAsyncWork) return nullptr;
    if (path.empty() || path.size() > m_streamer.GetLimits().PathBytes) return nullptr;
    // Тот же ключ, что у GetTexture с теми же настройками, — иначе асинхронно
    // загруженная картинка легла бы в ДРУГУЮ запись кэша, чем ту, которую
    // потом спросят обычным путём, и файл прочитался бы дважды.
    const std::string key = CacheKey(path) + SamplingSuffix(filter, mipmaps, bleed, srgb);
    auto it = m_textures.find(key);
    if (it != m_textures.end()) {
        it->second.Tick = NextTick();
        return it->second.Tex; // готовая или ещё грузящийся плейсхолдер
    }
    // Процедурная картинка (RegisterTexture) — та же, что у GetTexture: файла
    // за ней нет. Без этой ветки модель или материал, заказавшие её с другой
    // выборкой, получали серую заглушку навсегда, а фоновый поток — ошибку
    // «не декодировалась» (так пропадали скин персонажа и иконки предметов).
    auto generated = m_textures.find(CacheKey(path));
    if (generated != m_textures.end() && generated->second.Generated) {
        generated->second.Tick = NextTick();
        return generated->second.Tex;
    }

    // Only the owner thread changes the pending count. Check capacity before
    // touching the GPU, then publish the placeholder before starting decode.
    const auto& limits = m_streamer.GetLimits();
    if (m_streamer.GetStats().Pending >= limits.Requests) return nullptr;
    const std::string source = Locate(path);
    if (key.size() > limits.PathBytes || source.size() > limits.PathBytes) return nullptr;

    // Плейсхолдер 1x1 (нейтральный серый) — виден мгновенно, реальные пиксели
    // заменят его на PumpAsyncUploads(). Без мипмапов -> размер ничтожен.
    // У выреза по альфе заглушка прозрачная: серый квадрат на месте листа на
    // пару кадров — это то самое «листва квадратами», только мигающее.
    const unsigned char kPlaceholder[4] = {128, 128, 128, (unsigned char)(bleed ? 0 : 255)};
    TextureRecord rec;
    try {
        // Цветовое пространство — сразу настоящее: ReplacePixels его сохраняет,
        // и картинка, доехавшая фоном, окажется в том же формате.
        rec.Tex = std::make_shared<Texture>(kPlaceholder, 1, 1, TextureFilter::Bilinear, false, srgb);
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Не удалось создать плейсхолдер текстуры (" << path << "): " << e.what();
        return nullptr;
    } catch (...) {
        LOG_ERROR("Resources") << "Не удалось создать плейсхолдер текстуры: " << path;
        return nullptr;
    }
    rec.Bytes = rec.Tex->GpuBytes();
    rec.Source = path;
    rec.Filter = filter;
    rec.Mipmaps = mipmaps;
    rec.Bleed = bleed;
    rec.Srgb = srgb;
    rec.Tick = NextTick();
    rec.Pending = true;
    rec.Stamp = FileStamp(source);
    std::shared_ptr<Texture> result = rec.Tex;
    auto entry = m_textures.emplace(key, std::move(rec)).first;
    try {
        if (m_streamer.Enqueue(key, source, bleed)) {
            m_textureBytes += entry->second.Bytes;
            return result;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Texture streaming enqueue failed: " << e.what();
    } catch (...) {
        LOG_ERROR("Resources") << "Texture streaming enqueue failed: " << path;
    }
    m_textures.erase(entry);

    return nullptr;
}

void ResourceManager::ConfigureTextureStreaming(const sage::render::TextureStreamer::Limits& limits,
        const sage::render::TextureStreamer::PumpBudget& pump) {
    if (pump.Bytes < limits.DecodedBytes || !pump.Results ||
        !std::isfinite(pump.Milliseconds) || pump.Milliseconds <= 0)
        throw std::invalid_argument("ResourceManager: invalid texture upload budget");
    m_streamer.Configure(limits);
    m_uploadBudget = pump;
}

int ResourceManager::PumpAsyncUploads() {
    if (!m_acceptingAsyncWork) return 0;
    const auto start = std::chrono::steady_clock::now();
    int uploaded = 0;
    m_streamer.PumpReady(m_uploadBudget, [&](const sage::render::TextureStreamer::Decoded& d) {
        auto it = m_textures.find(d.Key);
        if (it == m_textures.end() || !it->second.Pending) return;
        TextureRecord& rec = it->second;
        rec.Pending = false;
        try {
            if (d.Ok && rec.Tex && d.Width > 0 && d.Height > 0) {
                // С ТЕМИ настройками выборки, с какими заказывали: здесь стояли
                // значения по умолчанию, и картинка, заказанная резкой или без
                // мипмапов, приезжала сглаженной.
                rec.Tex->ReplacePixels(d.Pixels.data(), d.Width, d.Height, rec.Filter, rec.Mipmaps);
                m_textureBytes -= std::min(m_textureBytes, rec.Bytes);
                rec.Bytes = rec.Tex->GpuBytes();
                m_textureBytes += rec.Bytes;
                rec.Tick = NextTick();
                ++uploaded;
            } else if (!d.Ok) {
                LOG_ERROR("Resources") << "Асинхронная текстура не декодировалась: " << d.Key;
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Resources") << "Texture streaming upload failed: " << d.Key << ": " << e.what();
        } catch (...) {
            LOG_ERROR("Resources") << "Texture streaming upload failed: " << d.Key;
        }
    });
    if (uploaded > 0) EvictToBudget();
    // Materials loaded once by a scene also need to retry rejected admission.
    // Round-robin over existing cache entries: no deferred queue, no full scan.
    if (m_streamMaterialTextures && !m_materials.empty()) {
        auto it = m_materials.find(m_streamMaterialCursor);
        if (it == m_materials.end()) it = m_materials.begin();
        const size_t checks = std::min(m_uploadBudget.Results, m_materials.size());
        for (size_t i = 0; i < checks; ++i) {
            if (std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count() >= m_uploadBudget.Milliseconds) break;
            if (it->second) RefreshMaterialTextures(*it->second);
            if (++it == m_materials.end()) it = m_materials.begin();
        }
        m_streamMaterialCursor = it->first;
    }
    return uploaded;
}

std::shared_ptr<Skybox> ResourceManager::GetSkybox(const std::string& directory) {
    if (directory.empty()) return nullptr;
    const std::string key = CacheKey(directory);
    auto it = m_skyboxes.find(key);
    if (it != m_skyboxes.end()) return it->second; // в т.ч. закэшированный nullptr

    std::shared_ptr<Skybox> sky = Skybox::LoadFromDirectory(Locate(directory));
    m_skyboxes[key] = sky;
    return sky;
}

std::shared_ptr<Skybox> ResourceManager::GetSkyboxFaces(const std::string faces[6]) {
    std::array<std::string, 6> resolved;
    std::string key;
    for (int i = 0; i < 6; ++i) {
        if (faces[i].empty()) return nullptr;   // набор неполон — небо не собрать
        resolved[(size_t)i] = Locate(faces[i]);
        key += CacheKey(faces[i]);
        key += '|';
    }
    auto it = m_skyboxes.find(key);
    if (it != m_skyboxes.end()) return it->second; // в т.ч. закэшированный nullptr

    std::shared_ptr<Skybox> sky;
    try {
        sky = std::make_shared<Skybox>(resolved);
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Небо из отдельных граней не собралось: " << e.what();
    }
    m_skyboxes[key] = sky;
    return sky;
}

std::shared_ptr<Skybox> ResourceManager::GetSkyboxImage(const std::string& file, int layout) {
    if (file.empty()) return nullptr;
    // Раскладка входит в ключ: одну и ту же картинку можно прочитать и крестом,
    // и панорамой, и это разные небеса.
    const std::string key = "img:" + std::to_string(layout) + "|" + CacheKey(file);
    auto it = m_skyboxes.find(key);
    if (it != m_skyboxes.end()) return it->second;   // в т.ч. закэшированный nullptr

    // LoadFromImage не бросает — отдаёт nullptr и пишет причину сама.
    std::shared_ptr<Skybox> sky =
        Skybox::LoadFromImage(Locate(file), (Skybox::Layout)layout);
    m_skyboxes[key] = sky;
    return sky;
}

bool ResourceManager::UpdateGeneratedTexture(const std::string& name, const unsigned char* pixelsRGBA, int width,
                                             int height, TextureFilter filter, bool generateMipmaps) {
    auto it = m_textures.find(CacheKey(name));
    if (it == m_textures.end() || !it->second.Generated || !it->second.Tex) return false;
    TextureRecord& rec = it->second;
    rec.Tex->ReplacePixels(pixelsRGBA, width, height, filter, generateMipmaps);
    m_textureBytes -= std::min(m_textureBytes, rec.Bytes);
    rec.Bytes = rec.Tex->GpuBytes();
    m_textureBytes += rec.Bytes;
    rec.Tick = NextTick();
    return true;
}

void ResourceManager::RegisterTexture(const std::string& name, std::shared_ptr<Texture> texture) {
    if (name.empty() || !texture) return;
    // Имя приводится тем же CacheKey, что и путь файла: спрашивать
    // посчитанную картинку будут через GetTexture, и там ключ строится так же.
    // Настройки выборки в ключ НЕ идут: пиксели собраны движком, перечитать их
    // неоткуда, и «та же картинка с другой фильтрацией» для неё не существует.
    const std::string key = CacheKey(name);
    auto it = m_textures.find(key);
    if (it != m_textures.end()) {
        m_textureBytes -= std::min(m_textureBytes, it->second.Bytes);
        m_textures.erase(it);
    }
    TextureRecord rec;
    rec.Bytes = texture->GpuBytes();
    rec.Source = name;
    rec.Tick = NextTick();
    rec.Generated = true;
    rec.Tex = std::move(texture);
    m_textureBytes += rec.Bytes;
    m_textures[key] = std::move(rec);
}


namespace {
// Шейдер экрана: встроенный полноэкранный вершинный + фрагментный файл.
Shader BuildScreenShader(const std::string& fragPath) {
    std::vector<std::string> inc;
    const std::string text = Shader::LoadExpanded(fragPath, &inc);
    Shader sh = Shader::FromSource(sage::render::FullscreenVertexSource(), text, fragPath);
    sh.Included = std::move(inc);
    return sh;
}

// Правка включённого файла («#include "noise.glsl"») — тоже повод пересобрать:
// иначе общий код шейдеров правился бы, а сцена рисовалась старым.
long long IncludesStamp(const std::shared_ptr<Shader>& sh) {
    long long sum = 0;
    if (sh)
        for (const std::string& p : sh->Included) sum += FileStamp(p);
    return sum;
}
} // namespace

std::shared_ptr<Shader> ResourceManager::GetShader(const std::string& vertexPath,
                                                   const std::string& fragmentPath) {
    if (vertexPath.empty() || fragmentPath.empty()) return nullptr;
    // Ключ — из КАНОНИЧЕСКИХ путей: одна и та же пара файлов, названная
    // по-разному, не должна собираться дважды.
    const std::string key = CacheKey(vertexPath) + "|" + CacheKey(fragmentPath);
    auto it = m_shaders.find(key);
    if (it != m_shaders.end()) return it->second.Program;

    // Ключ кэша — путь как его дали, а хранимые пути — разрешённые: по ним
    // идут FileStamp и пересборка, и оба обязаны смотреть на ТОТ ЖЕ файл, что
    // и первая сборка. Иначе в редакторе hot-reload сравнивал бы штампы
    // несуществующего файла (0 == 0) и молча никогда не срабатывал.
    ShaderEntry entry;
    entry.VertPath = Locate(vertexPath);
    entry.FragPath = Locate(fragmentPath);
    entry.VertStamp = FileStamp(entry.VertPath);
    entry.FragStamp = FileStamp(entry.FragPath);
    try {
        entry.Program = std::make_shared<Shader>(entry.VertPath, entry.FragPath);
        entry.IncludeStamp = IncludesStamp(entry.Program);
        LOG_INFO("Resources") << "Шейдер собран: " << vertexPath << " + " << fragmentPath;
    } catch (const std::exception& e) {
        // nullptr тоже кэшируем — см. комментарий в заголовке.
        LOG_ERROR("Resources") << "Шейдер не собрался (" << key << "): " << e.what();
    }
    m_shaders[key] = entry;
    return m_shaders[key].Program;
}


std::shared_ptr<Shader> ResourceManager::GetScreenShader(const std::string& fragmentPath) {
    if (fragmentPath.empty()) return nullptr;
    const std::string key = "screen|" + CacheKey(fragmentPath);
    auto it = m_shaders.find(key);
    if (it != m_shaders.end()) return it->second.Program;
    ShaderEntry entry;
    entry.Screen = true;
    entry.FragPath = Locate(fragmentPath);
    entry.FragStamp = FileStamp(entry.FragPath);
    try {
        entry.Program = std::make_shared<Shader>(BuildScreenShader(entry.FragPath));
        entry.IncludeStamp = IncludesStamp(entry.Program);
        LOG_INFO("Resources") << "Шейдер экрана собран: " << fragmentPath;
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Шейдер экрана не собрался (" << fragmentPath << "): " << e.what();
    }
    m_shaders[key] = entry;
    return m_shaders[key].Program;
}

int ResourceManager::ReloadChangedShaders() {
    int reloaded = 0;
    for (auto& [key, entry] : m_shaders) {
        if (entry.Screen) {
            const long long f = FileStamp(entry.FragPath);
            const long long inc = IncludesStamp(entry.Program);
            if (f == entry.FragStamp && inc == entry.IncludeStamp) continue;
            entry.FragStamp = f;
            try {
                Shader fresh = BuildScreenShader(entry.FragPath);
                if (entry.Program) *entry.Program = std::move(fresh);
                else entry.Program = std::make_shared<Shader>(std::move(fresh));
                entry.IncludeStamp = IncludesStamp(entry.Program);
                ++reloaded;
                LOG_INFO("Resources") << "Шейдер экрана перечитан: " << key;
            } catch (const std::exception& e) {
                LOG_WARN("Resources") << "Правка шейдера экрана не собралась, оставляю прежний: " << e.what();
            }
            continue;
        }
        long long v = FileStamp(entry.VertPath);
        long long f = FileStamp(entry.FragPath);
        const long long inc = IncludesStamp(entry.Program);
        if (v == entry.VertStamp && f == entry.FragStamp && inc == entry.IncludeStamp) continue;
        entry.IncludeStamp = inc;
        entry.VertStamp = v;
        entry.FragStamp = f;
        try {
            // Собираем во ВРЕМЕННЫЙ объект и подменяем содержимое только при
            // успехе: опечатка в шейдере не должна стирать рабочую программу и
            // оставлять сцену без материала посреди правки.
            Shader fresh(entry.VertPath, entry.FragPath);
            if (entry.Program) *entry.Program = std::move(fresh);
            else entry.Program = std::make_shared<Shader>(std::move(fresh));
            entry.IncludeStamp = IncludesStamp(entry.Program);
            ++reloaded;
            LOG_INFO("Resources") << "Шейдер перечитан: " << key;
        } catch (const std::exception& e) {
            LOG_WARN("Resources") << "Правка шейдера не собралась, оставляю прежний: " << e.what();
        }
    }
    return reloaded;
}

void ResourceManager::DownscaleRGBA(const std::vector<unsigned char>& src, int w, int h,
                                    std::vector<unsigned char>& dst, int& outW, int& outH) {
    sage::render::VRAMBudgetManager::DownscaleRGBA(src, w, h, dst, outW, outH);
}

std::vector<size_t> ResourceManager::SelectDowngrades(
    const std::vector<DowngradeCandidate>& candidates, size_t currentBytes, size_t budget) {
    return sage::render::VRAMBudgetManager::SelectDowngrades(candidates, currentBytes, budget);
}

std::shared_ptr<Mesh> ResourceManager::GetPrimitive(MeshRef::Type type) {
    return m_primitives.Get(type, m_keepMeshCpu);
}

std::shared_ptr<Mesh> ResourceManager::GetPrimitive(MeshRef::Type type, const sage::render::PrimitiveParams& params) {
    return m_primitives.Get(type, params, m_keepMeshCpu);
}

void ResourceManager::Quiesce() {
    m_acceptingAsyncWork = false;
    m_streamer.Stop();
    for (auto& entry : m_textures) entry.second.Pending = false;
}

void ResourceManager::Clear() {
    m_sprites.Clear();
    // Останавливаем фоновый поток ДО очистки карт: иначе он мог бы дописать
    // результат для только что удалённой записи (безвредно, но чище так).
    m_streamer.Stop();
    m_streamMaterialCursor.clear();
    // ВСЕ формы разом: забыть одну нельзя, а забытая освобождала бы свой буфер
    // видеокарты после смерти GL-контекста — то есть падением при выходе.
    m_primitives.Clear();
    m_models.clear();
    // Штампы — вместе с записями: оставшись без своих ресурсов, они заставляли
    // ReloadChangedAssets «перечитывать» давно выгруженные файлы (а удалённые —
    // с ошибкой в лог).
    m_modelStamps.clear();
    m_materialStamps.clear();
    m_skinnedStamps.clear();
    m_reloadCursor = 0;
    m_skinned.clear(); // GPU-ресурс: чистится, пока GL-контекст ещё жив
    // Материал держит свою программу через ShaderPtr, а копии shared_ptr на сам
    // материал живут в сущностях сцены — она переживает Clear(). Снимаем ссылку
    // у САМОГО материала: тогда программа умрёт здесь, при живом контексте, а
    // не после main вместе со сценой.
    for (auto& kv : m_materials)
        if (kv.second) kv.second->ShaderPtr.reset();
    m_materials.clear();
    m_skyboxes.clear(); // GPU-ресурс: чистится, пока GL-контекст ещё жив
    // Программы своих шейдеров — тоже GPU-ресурс, и удалить их можно ТОЛЬКО
    // здесь: кэш живёт в синглтоне, а синглтон умирает после main, когда
    // контекста уже нет и glDeleteProgram падает.
    m_shaders.clear();
    m_textures.clear();
    m_textureBytes = 0;
    // m_evictions/m_tick намеренно НЕ сбрасываем — это счётчики за жизнь процесса.
}

void ResourceManager::ForgetProjectAssets() {
    m_sprites.Clear();
    // Old-project completions must not reach a new entry with the same key.
    // Also join the reader before its caller remounts the global VFS.
    m_streamer.Stop();
    m_streamMaterialCursor.clear();
    // Только ЗАПИСИ КЭША. Ни один живой объект здесь не разрушается насильно:
    // уходящая сцена ещё держит свои shared_ptr, и отобрать у неё материал
    // посреди кадра значило бы поменять падение «чужая модель» на падение
    // «материала нет».
    m_models.clear();
    m_modelStamps.clear();
    m_skinned.clear();
    m_skinnedStamps.clear();
    m_materials.clear();
    m_materialStamps.clear();
    m_textures.clear();
    m_textureBytes = 0;
    m_skyboxes.clear();
    // Шейдеры — тоже проектные: игра вправе положить свои .vert/.frag рядом со
    // сценой (см. docs/custom_shaders.md), и программа, собранная из файлов
    // прошлого проекта, в новом означает чужую картинку.
    m_shaders.clear();
    // Примитивы (куб, сфера, плоскость) НЕ трогаем: они посчитаны движком, к
    // проекту отношения не имеют, а пересоздавать их — лишняя работа на ровном
    // месте.
    //
    // Поколение ассетов двигаем: по нему те, кто держит РАЗОБРАННЫЕ данные, а не
    // shared_ptr, понимают, что их копия устарела.
    ++m_assetsGeneration;
}

ResourceManager::~ResourceManager() {
}
