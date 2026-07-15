#include "AssetManager.h"
#include "../render/ModelLoader.h"
#include "../core/JobSystem.h"
#include "../core/MainThreadDispatcher.h"
#include "../core/Log.h"
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

const char* AssetTypeName(AssetType type) {
    switch (type) {
        case AssetType::Texture: return "Texture";
        case AssetType::Mesh:    return "Mesh";
        case AssetType::Shader:  return "Shader";
        case AssetType::Audio:   return "Audio";
        case AssetType::Text:    return "Text";
        default:                 return "Other";
    }
}

namespace {

// Максимальный mtime по списку файлов (0, если какого-то нет/ошибка). Служит
// «отпечатком» для hot-reload — изменился => перезагрузить.
int64_t MaxMtime(const std::vector<std::string>& files) {
    int64_t newest = 0;
    for (const std::string& f : files) {
        std::error_code ec;
        auto t = fs::last_write_time(f, ec);
        if (ec) return 0;
        int64_t v = static_cast<int64_t>(t.time_since_epoch().count());
        if (v > newest) newest = v;
    }
    return newest;
}

// Оценщики размера — обычные функции без захвата (см. AssetRecord::ApproxBytes).
size_t TextureBytes(const AssetRecord& r) {
    auto* t = static_cast<Texture*>(r.Resource.get());
    return t ? t->ApproxBytes() : 0;
}
size_t MeshBytes(const AssetRecord& r) {
    auto* m = static_cast<Mesh*>(r.Resource.get());
    return m ? m->IndexCount() * sizeof(Vertex) : 0; // грубая оценка
}

} // namespace

AssetManager& AssetManager::Instance() {
    static AssetManager instance;
    return instance;
}

std::string AssetManager::Resolve(const std::string& path) const {
    if (path.empty()) return path;
    std::error_code ec;
    if (fs::exists(path, ec)) return path;         // уже существующий путь (в т.ч. полный "assets/...")
    return m_root + "/" + path;                    // иначе — относительно корня ассетов
}

std::shared_ptr<AssetRecord> AssetManager::Find(const std::string& key) const {
    auto it = m_cache.find(key);
    return it == m_cache.end() ? nullptr : it->second;
}

// ---------------- Текстуры ----------------

void AssetManager::EnqueueTextureLoad(std::shared_ptr<AssetRecord> rec, std::string resolved,
                                      TextureFilter filter, bool generateMipmaps, bool reload) {
    JobSystem::Instance().Enqueue([rec, resolved, filter, generateMipmaps, reload]() {
        std::string err;
        ImageData img = LoadImageFile(resolved, &err); // фон: декод PNG/JPG
        MainThreadDispatcher::Instance().Post(
            [rec, filter, generateMipmaps, reload, img = std::move(img), err]() mutable {
                if (!img.Ok()) {
                    if (reload) {
                        LOG_WARN("Assets") << "Hot-reload текстуры не удался: " << err;
                    } else {
                        rec->Error = err;
                        rec->State.store(AssetState::Failed, std::memory_order_release);
                    }
                    return;
                }
                if (reload && rec->Resource) {
                    static_cast<Texture*>(rec->Resource.get())->Reload(img);
                } else {
                    rec->Resource = std::make_shared<Texture>(img, filter, generateMipmaps);
                }
                rec->Mtime = MaxMtime(rec->Files);
                rec->State.store(AssetState::Ready, std::memory_order_release);
                rec->Version.fetch_add(1, std::memory_order_acq_rel);
            });
    });
}

Asset<Texture> AssetManager::LoadTexture(const std::string& path, TextureFilter filter, bool generateMipmaps) {
    std::string resolved = Resolve(path);
    std::string key = "tex|" + resolved + "|" + std::to_string((int)filter) + (generateMipmaps ? "|m" : "|n");
    if (auto rec = Find(key)) return Asset<Texture>(rec);

    auto rec = std::make_shared<AssetRecord>();
    rec->Key = key;
    rec->Type = AssetType::Texture;
    rec->Files = {resolved};
    rec->ApproxBytes = &TextureBytes;
    rec->Reload = [this, resolved, filter, generateMipmaps](const std::shared_ptr<AssetRecord>& r) {
        EnqueueTextureLoad(r, resolved, filter, generateMipmaps, /*reload=*/true);
    };

    std::string err;
    ImageData img = LoadImageFile(resolved, &err);
    if (!img.Ok()) {
        rec->Error = err;
        rec->State.store(AssetState::Failed);
        LOG_ERROR("Assets") << "Текстура не загружена: " << err;
    } else {
        rec->Resource = std::make_shared<Texture>(img, filter, generateMipmaps);
        rec->Mtime = MaxMtime(rec->Files);
        rec->State.store(AssetState::Ready);
        rec->Version.store(1);
    }
    m_cache[key] = rec;
    return Asset<Texture>(rec);
}

Asset<Texture> AssetManager::LoadTextureAsync(const std::string& path, TextureFilter filter, bool generateMipmaps) {
    std::string resolved = Resolve(path);
    std::string key = "tex|" + resolved + "|" + std::to_string((int)filter) + (generateMipmaps ? "|m" : "|n");
    if (auto rec = Find(key)) return Asset<Texture>(rec);

    auto rec = std::make_shared<AssetRecord>();
    rec->Key = key;
    rec->Type = AssetType::Texture;
    rec->Files = {resolved};
    rec->ApproxBytes = &TextureBytes;
    rec->Reload = [this, resolved, filter, generateMipmaps](const std::shared_ptr<AssetRecord>& r) {
        EnqueueTextureLoad(r, resolved, filter, generateMipmaps, /*reload=*/true);
    };
    m_cache[key] = rec;
    EnqueueTextureLoad(rec, resolved, filter, generateMipmaps, /*reload=*/false);
    return Asset<Texture>(rec);
}

// ---------------- Модели (.obj) ----------------

void AssetManager::EnqueueMeshLoad(std::shared_ptr<AssetRecord> rec, std::string resolved, bool reload) {
    JobSystem::Instance().Enqueue([rec, resolved, reload]() {
        std::string err;
        bool ok = true;
        MeshData data;
        try {
            data = ModelLoader::LoadObjData(resolved); // фон: чтение+парсинг .obj
        } catch (const std::exception& e) {
            ok = false; err = e.what();
        }
        MainThreadDispatcher::Instance().Post(
            [rec, reload, ok, err, data = std::move(data)]() mutable {
                if (!ok) {
                    if (reload) {
                        LOG_WARN("Assets") << "Hot-reload модели не удался: " << err;
                    } else {
                        rec->Error = err;
                        rec->State.store(AssetState::Failed, std::memory_order_release);
                    }
                    return;
                }
                if (reload && rec->Resource) {
                    static_cast<Mesh*>(rec->Resource.get())->Reload(data);
                } else {
                    rec->Resource = std::make_shared<Mesh>(data);
                }
                rec->Mtime = MaxMtime(rec->Files);
                rec->State.store(AssetState::Ready, std::memory_order_release);
                rec->Version.fetch_add(1, std::memory_order_acq_rel);
            });
    });
}

Asset<Mesh> AssetManager::LoadModel(const std::string& path) {
    std::string resolved = Resolve(path);
    std::string key = "mesh|" + resolved;
    if (auto rec = Find(key)) return Asset<Mesh>(rec);

    auto rec = std::make_shared<AssetRecord>();
    rec->Key = key;
    rec->Type = AssetType::Mesh;
    rec->Files = {resolved};
    rec->ApproxBytes = &MeshBytes;
    rec->Reload = [this, resolved](const std::shared_ptr<AssetRecord>& r) {
        EnqueueMeshLoad(r, resolved, /*reload=*/true);
    };
    try {
        rec->Resource = std::make_shared<Mesh>(ModelLoader::LoadObjData(resolved));
        rec->Mtime = MaxMtime(rec->Files);
        rec->State.store(AssetState::Ready);
        rec->Version.store(1);
    } catch (const std::exception& e) {
        rec->Error = e.what();
        rec->State.store(AssetState::Failed);
        LOG_ERROR("Assets") << "Модель не загружена: " << e.what();
    }
    m_cache[key] = rec;
    return Asset<Mesh>(rec);
}

Asset<Mesh> AssetManager::LoadModelAsync(const std::string& path) {
    std::string resolved = Resolve(path);
    std::string key = "mesh|" + resolved;
    if (auto rec = Find(key)) return Asset<Mesh>(rec);

    auto rec = std::make_shared<AssetRecord>();
    rec->Key = key;
    rec->Type = AssetType::Mesh;
    rec->Files = {resolved};
    rec->ApproxBytes = &MeshBytes;
    rec->Reload = [this, resolved](const std::shared_ptr<AssetRecord>& r) {
        EnqueueMeshLoad(r, resolved, /*reload=*/true);
    };
    m_cache[key] = rec;
    EnqueueMeshLoad(rec, resolved, /*reload=*/false);
    return Asset<Mesh>(rec);
}

Asset<Mesh> AssetManager::Cube() {
    std::string key = "mesh|__cube__";
    if (auto rec = Find(key)) return Asset<Mesh>(rec);

    auto rec = std::make_shared<AssetRecord>();
    rec->Key = key;
    rec->Type = AssetType::Mesh;
    rec->ApproxBytes = &MeshBytes;
    rec->Resource = std::make_shared<Mesh>(Mesh::CreateCube());
    rec->State.store(AssetState::Ready);
    rec->Version.store(1);
    m_cache[key] = rec;
    return Asset<Mesh>(rec);
}

// ---------------- Шейдеры ----------------

Asset<Shader> AssetManager::LoadShader(const std::string& vertexPath, const std::string& fragmentPath) {
    std::string rv = Resolve(vertexPath), rf = Resolve(fragmentPath);
    std::string key = "shader|" + rv + "|" + rf;
    if (auto rec = Find(key)) return Asset<Shader>(rec);

    auto rec = std::make_shared<AssetRecord>();
    rec->Key = key;
    rec->Type = AssetType::Shader;
    rec->Files = {rv, rf};
    // Перекомпиляция шейдера — GL, поэтому строго синхронно в главном потоке.
    rec->Reload = [](const std::shared_ptr<AssetRecord>& r) {
        if (r->Resource && static_cast<Shader*>(r->Resource.get())->Reload()) {
            r->Version.fetch_add(1, std::memory_order_acq_rel);
        }
    };
    try {
        rec->Resource = std::make_shared<Shader>(rv, rf);
        rec->Mtime = MaxMtime(rec->Files);
        rec->State.store(AssetState::Ready);
        rec->Version.store(1);
    } catch (const std::exception& e) {
        rec->Error = e.what();
        rec->State.store(AssetState::Failed);
        LOG_ERROR("Assets") << "Шейдер не загружен: " << e.what();
    }
    m_cache[key] = rec;
    return Asset<Shader>(rec);
}

// ---------------- Hot-reload / GC / статистика ----------------

size_t AssetManager::PollHotReload() {
    if (!m_hotReload) return 0;
    size_t reloaded = 0;
    for (auto& [key, rec] : m_cache) {
        if (!rec->Reload || rec->Files.empty()) continue;
        if (rec->State.load() == AssetState::Loading) continue; // ещё грузится
        int64_t cur = MaxMtime(rec->Files);
        if (cur == 0 || cur == rec->Mtime) continue;            // не изменилось
        rec->Mtime = cur;   // обновляем сразу, чтобы не триггерить каждый poll
        rec->Reload(rec);   // sync (шейдер) или async (текстура/меш)
        ++reloaded;
    }
    return reloaded;
}

size_t AssetManager::ReloadAll() {
    size_t reloaded = 0;
    for (auto& [key, rec] : m_cache) {
        if (!rec->Reload) continue;
        if (rec->State.load() == AssetState::Loading) continue;
        rec->Reload(rec);
        ++reloaded;
    }
    return reloaded;
}

size_t AssetManager::CollectGarbage() {
    size_t freed = 0;
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        const std::shared_ptr<AssetRecord>& rec = it->second;
        bool inFlight = rec->State.load() == AssetState::Loading;
        if (!inFlight && rec.use_count() == 1) { // только кэш держит запись
            it = m_cache.erase(it);
            ++freed;
        } else {
            ++it;
        }
    }
    return freed;
}

AssetManager::Stats AssetManager::GetStats() const {
    Stats s;
    for (const auto& [key, rec] : m_cache) {
        ++s.Total;
        ++s.ByType[(int)rec->Type];
        switch (rec->State.load()) {
            case AssetState::Ready:   ++s.Ready; break;
            case AssetState::Loading: ++s.Loading; break;
            case AssetState::Failed:  ++s.Failed; break;
        }
        if (rec->ApproxBytes && rec->Resource) s.ApproxBytes += rec->ApproxBytes(*rec);
    }
    return s;
}

void AssetManager::Clear() {
    m_cache.clear();
}
