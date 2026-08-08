#include <filesystem>
#include "ResourceManager.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/render/SkinnedModel.h"

#include <stb_image.h> // реализация STB_IMAGE_IMPLEMENTATION живёт в Texture.cpp

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

// ---------------------------------------------------------------------------
// Асинхронный конвейер декодирования. Спрятан в .cpp, чтобы <thread>/очереди не
// протекали в каждый TU, включающий ResourceManager.h. Один фоновый поток:
// декодирует файлы изображений в CPU-буферы RGBA8; главный поток забирает
// готовое в PumpAsyncUploads() и заливает в VRAM (GL только с главного потока).
// ---------------------------------------------------------------------------
struct ResourceManager::AsyncImpl {
    struct Decoded {
        std::string Path;
        std::vector<unsigned char> Pixels;
        int W = 0, H = 0;
        bool Ok = false;
    };

    std::thread Worker;
    std::mutex JobMx;
    std::condition_variable JobCv;
    // Ключ кэша и путь, который реально открывается, — разные строки (см.
    // Locate ниже). Разрешение делает главный поток при постановке задачи:
    // воркеру нельзя трогать базу ассетов, её могут пересканировать в этот же
    // момент.
    struct Job {
        std::string Key;
        std::string File;
    };
    std::deque<Job> Jobs;

    std::mutex ResMx;
    std::deque<Decoded> Results;

    std::atomic<bool> Stop{false};
    std::atomic<int> Pending{0}; // задач в полёте (в очереди/декодируются/ждут pump)

    void Run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(JobMx);
                JobCv.wait(lk, [&] { return Stop.load() || !Jobs.empty(); });
                if (Stop.load() && Jobs.empty()) return;
                if (Jobs.empty()) continue;
                job = std::move(Jobs.front());
                Jobs.pop_front();
            }
            Decoded d;
            d.Path = job.Key;
            d.Ok = ResourceManager::DecodeImageFile(job.File, d.Pixels, d.W, d.H);
            {
                std::lock_guard<std::mutex> lk(ResMx);
                Results.push_back(std::move(d));
            }
        }
    }
};

namespace {
// Путь из проекта -> путь, который откроется ИЗ ТЕКУЩЕГО каталога процесса.
//
// Ключи кэша при этом остаются ИСХОДНЫМИ: по ним приходят повторные запросы,
// hot-reload и стриминг, и подменять их на разрешённые значило бы, что один и
// тот же ассет лежит в кэше дважды — под относительным путём и под полным.
std::string Locate(const std::string& path) {
    return sage::AssetDatabase::Instance().LocatePath(path);
}
} // namespace

// --- GL-независимое декодирование (используется и воркером, и юнит-тестом) ---
ResourceManager::ResourceManager() = default;

bool ResourceManager::DecodeImageFile(const std::string& path, std::vector<unsigned char>& outRGBA,
                                      int& outW, int& outH) {
    // Тот же флип, что и синхронный путь Texture(path): (0,0) внизу-слева для GL,
    // чтобы async- и sync-текстуры выглядели одинаково. Флаг stb глобальный, но
    // и синхронный путь, и воркер выставляют его в одно и то же значение (true) —
    // гонки на нём не наблюдаемы (значение всегда одинаковое).
    stbi_set_flip_vertically_on_load(true);
    int channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &outW, &outH, &channels, 4); // форсируем RGBA
    if (!data) return false;
    const size_t bytes = (size_t)outW * (size_t)outH * 4u;
    outRGBA.assign(data, data + bytes);
    stbi_image_free(data);
    return true;
}

// --- Чистая LRU-политика вытеснения (ядро менеджера памяти, юнит-тестируемо) ---
std::vector<size_t> ResourceManager::SelectEvictions(const std::vector<EvictCandidate>& candidates,
                                                     size_t currentBytes, size_t budget) {
    std::vector<size_t> result;
    if (budget == 0 || currentBytes <= budget) return result;

    std::vector<const EvictCandidate*> evictable;
    for (const auto& c : candidates)
        if (c.Evictable) evictable.push_back(&c);
    // Старейшие (наименьший Tick) — первыми на выход.
    std::sort(evictable.begin(), evictable.end(),
              [](const EvictCandidate* a, const EvictCandidate* b) { return a->Tick < b->Tick; });

    size_t bytes = currentBytes;
    for (const EvictCandidate* c : evictable) {
        if (bytes <= budget) break;
        result.push_back(c->Index);
        bytes -= std::min(bytes, c->Bytes);
    }
    return result;
}

namespace {
// Время последней правки файла в виде числа; 0 — файла нет/недоступен.
long long FileStamp(const std::string& path) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return (long long)t.time_since_epoch().count();
}
} // namespace

std::shared_ptr<Mesh> ResourceManager::GetModel(const std::string& path) {
    auto it = m_models.find(path);
    if (it != m_models.end()) return it->second;
    std::shared_ptr<Mesh> mesh;
    try {
        mesh = ModelLoader::LoadMesh(Locate(path), m_keepMeshCpu);
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Модель не загрузилась (" << path << "): " << e.what();
    }
    m_models[path] = mesh; // в т.ч. nullptr — негативный кэш (не перечитывать битый файл)
    m_modelStamps[path] = FileStamp(Locate(path));
    return mesh;
}

std::shared_ptr<sage::render::SkinnedModel> ResourceManager::GetSkinnedModel(
    const std::string& path) {
    auto it = m_skinned.find(path);
    if (it != m_skinned.end()) return it->second;
    std::shared_ptr<sage::render::SkinnedModel> model;
    try {
        model = sage::render::SkinnedModel::Load(Locate(path));
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Скиннинг-модель не загрузилась (" << path << "): " << e.what();
    }
    m_skinned[path] = model; // в т.ч. nullptr — негативный кэш
    return model;
}

std::shared_ptr<Mesh> ResourceManager::ReloadModel(const std::string& path) {
    m_models.erase(path); // сброс кэша -> GetModel перечитает с диска (новый .sageimport)
    m_modelStamps.erase(path);
    return GetModel(path);
}

int ResourceManager::ReloadChangedAssets() {
    int reloaded = 0;

    // Списки путей собираются ЗАРАНЕЕ: перезагрузка меняет те самые
    // контейнеры, по которым идёт обход, и обходить их на ходу — обращение по
    // недействительному итератору.
    std::vector<std::string> staleModels;
    for (const auto& [path, stamp] : m_modelStamps) {
        if (FileStamp(Locate(path)) != stamp) staleModels.push_back(path);
    }
    std::vector<std::string> staleMaterials;
    for (const auto& [path, stamp] : m_materialStamps) {
        if (FileStamp(Locate(path)) != stamp) staleMaterials.push_back(path);
    }

    for (const std::string& path : staleModels) {
        // Меш подменяется НА МЕСТЕ (по тому же shared_ptr), а не заводится
        // заново: на прежний указывают компоненты сцены, и заменив запись в
        // кэше, мы оставили бы их со старой геометрией — то есть починили бы
        // кэш и не починили картинку.
        std::shared_ptr<Mesh> existing = m_models[path];
        m_models.erase(path);
        m_modelStamps.erase(path);
        std::shared_ptr<Mesh> fresh = GetModel(path);
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
    return reloaded;
}

std::shared_ptr<Texture> ResourceManager::GetTexture(const std::string& path,
                                                    TextureFilter filter, bool mipmaps) {
    if (path.empty()) return nullptr;
    auto it = m_textures.find(path);
    if (it != m_textures.end()) {
        it->second.Tick = NextTick(); // обращение -> «свежая» для LRU
        // Тот же файл запросили с другой фильтрацией: пересоздаём картинку НА
        // МЕСТЕ, чтобы все, кто уже держит на неё shared_ptr, увидели правку.
        // Ключ кэша остаётся путём к файлу — по нему же идут перезагрузка и
        // стриминг, и подмешивать в него настройки нельзя.
        if (it->second.Tex && (it->second.Filter != filter || it->second.Mipmaps != mipmaps)) {
            try {
                *it->second.Tex = Texture(Locate(path), filter, mipmaps);
                m_textureBytes -= it->second.Bytes;
                it->second.Bytes = it->second.Tex->GpuBytes();
                m_textureBytes += it->second.Bytes;
                it->second.Filter = filter;
                it->second.Mipmaps = mipmaps;
            } catch (const std::exception& e) {
                LOG_ERROR("Resources") << "Смена фильтрации не удалась (" << path
                                       << "): " << e.what();
            }
        }
        return it->second.Tex;
    }
    TextureRecord rec;
    rec.Filter = filter;
    rec.Mipmaps = mipmaps;
    try {
        rec.Tex = std::make_shared<Texture>(Locate(path), filter, mipmaps);
        rec.Bytes = rec.Tex->GpuBytes();
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Текстура не загрузилась (" << path << "): " << e.what();
        rec.Tex = nullptr; // негативный кэш
        rec.Bytes = 0;
    }
    rec.Tick = NextTick();
    std::shared_ptr<Texture> result = rec.Tex; // держим ссылку -> не вытеснится ниже
    m_textureBytes += rec.Bytes;
    m_textures[path] = std::move(rec);
    EvictToBudget();
    return result;
}

std::shared_ptr<Texture> ResourceManager::GetTextureAsync(const std::string& path) {
    if (path.empty()) return nullptr;
    auto it = m_textures.find(path);
    if (it != m_textures.end()) {
        it->second.Tick = NextTick();
        return it->second.Tex; // готовая или ещё грузящийся плейсхолдер
    }

    // Плейсхолдер 1x1 (нейтральный серый) — виден мгновенно, реальные пиксели
    // заменят его на PumpAsyncUploads(). Без мипмапов -> размер ничтожен.
    const unsigned char kPlaceholder[4] = {128, 128, 128, 255};
    TextureRecord rec;
    try {
        rec.Tex = std::make_shared<Texture>(kPlaceholder, 1, 1, TextureFilter::Bilinear, false);
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Не удалось создать плейсхолдер текстуры (" << path << "): " << e.what();
        m_textures[path] = TextureRecord{}; // негативный кэш
        return nullptr;
    }
    rec.Bytes = rec.Tex->GpuBytes();
    rec.Tick = NextTick();
    rec.Pending = true;
    std::shared_ptr<Texture> result = rec.Tex;
    m_textureBytes += rec.Bytes;
    m_textures[path] = std::move(rec);

    StartWorker();
    {
        std::lock_guard<std::mutex> lk(m_async->JobMx);
        m_async->Jobs.push_back({path, Locate(path)});
    }
    m_async->Pending.fetch_add(1);
    m_async->JobCv.notify_one();
    return result;
}

int ResourceManager::PumpAsyncUploads() {
    if (!m_async) return 0;
    std::deque<AsyncImpl::Decoded> ready;
    {
        std::lock_guard<std::mutex> lk(m_async->ResMx);
        ready.swap(m_async->Results);
    }
    int uploaded = 0;
    for (auto& d : ready) {
        m_async->Pending.fetch_sub(1);
        auto it = m_textures.find(d.Path);
        if (it == m_textures.end()) continue; // запись успели выгрузить/Clear
        TextureRecord& rec = it->second;
        if (d.Ok && rec.Tex && d.W > 0 && d.H > 0) {
            m_textureBytes -= std::min(m_textureBytes, rec.Bytes);
            rec.Tex->ReplacePixels(d.Pixels.data(), d.W, d.H); // GL-заливка на главном потоке
            rec.Bytes = rec.Tex->GpuBytes();
            m_textureBytes += rec.Bytes;
            rec.Tick = NextTick();
            ++uploaded;
        } else if (!d.Ok) {
            LOG_ERROR("Resources") << "Асинхронная текстура не декодировалась: " << d.Path;
        }
        rec.Pending = false;
    }
    if (uploaded > 0) EvictToBudget();
    return uploaded;
}

std::shared_ptr<Skybox> ResourceManager::GetSkybox(const std::string& directory) {
    if (directory.empty()) return nullptr;
    auto it = m_skyboxes.find(directory);
    if (it != m_skyboxes.end()) return it->second; // в т.ч. закэшированный nullptr

    std::shared_ptr<Skybox> sky = Skybox::LoadFromDirectory(Locate(directory));
    m_skyboxes[directory] = sky;
    return sky;
}

std::shared_ptr<Material> ResourceManager::GetMaterial(const std::string& path) {
    auto it = m_materials.find(path);
    if (it != m_materials.end()) return it->second;
    auto material = std::make_shared<Material>();
    try {
        *material = Material::LoadFromFile(Locate(path));
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Материал не загрузился, использую дефолт: " << e.what();
    }
    ResolveMaterialTextures(*material);
    if (material->HasCustomShader())
        material->ShaderPtr = GetShader(material->VertexShaderPath, material->FragmentShaderPath);
    m_materials[path] = material;
    m_materialStamps[path] = FileStamp(Locate(path));
    return material;
}


std::shared_ptr<Shader> ResourceManager::GetShader(const std::string& vertexPath,
                                                   const std::string& fragmentPath) {
    if (vertexPath.empty() || fragmentPath.empty()) return nullptr;
    const std::string key = vertexPath + "|" + fragmentPath;
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
        LOG_INFO("Resources") << "Шейдер собран: " << vertexPath << " + " << fragmentPath;
    } catch (const std::exception& e) {
        // nullptr тоже кэшируем — см. комментарий в заголовке.
        LOG_ERROR("Resources") << "Шейдер не собрался (" << key << "): " << e.what();
    }
    m_shaders[key] = entry;
    return m_shaders[key].Program;
}

int ResourceManager::ReloadChangedShaders() {
    int reloaded = 0;
    for (auto& [key, entry] : m_shaders) {
        long long v = FileStamp(entry.VertPath);
        long long f = FileStamp(entry.FragPath);
        if (v == entry.VertStamp && f == entry.FragStamp) continue;
        entry.VertStamp = v;
        entry.FragStamp = f;
        try {
            // Собираем во ВРЕМЕННЫЙ объект и подменяем содержимое только при
            // успехе: опечатка в шейдере не должна стирать рабочую программу и
            // оставлять сцену без материала посреди правки.
            Shader fresh(entry.VertPath, entry.FragPath);
            if (entry.Program) *entry.Program = std::move(fresh);
            else entry.Program = std::make_shared<Shader>(std::move(fresh));
            ++reloaded;
            LOG_INFO("Resources") << "Шейдер перечитан: " << key;
        } catch (const std::exception& e) {
            LOG_WARN("Resources") << "Правка шейдера не собралась, оставляю прежний: " << e.what();
        }
    }
    return reloaded;
}

std::shared_ptr<Material> ResourceManager::ReloadMaterial(const std::string& path) {
    auto it = m_materials.find(path);
    if (it == m_materials.end()) return GetMaterial(path);
    try {
        *it->second = Material::LoadFromFile(Locate(path));
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Перезагрузка материала не удалась: " << e.what();
    }
    ResolveMaterialTextures(*it->second);
    if (it->second->HasCustomShader())
        it->second->ShaderPtr = GetShader(it->second->VertexShaderPath, it->second->FragmentShaderPath);
    return it->second;
}

void ResourceManager::ResolveMaterialTextures(Material& m) {
    m.AlbedoTex = GetTexture(m.TexturePath);
    m.NormalTex = GetTexture(m.NormalMapPath);
    m.MetallicTex = GetTexture(m.MetallicMapPath);
    m.RoughnessTex = GetTexture(m.RoughnessMapPath);
    m.AOTex = GetTexture(m.AOMapPath);
    m.EmissiveTex = GetTexture(m.EmissiveMap);
}

void ResourceManager::DownscaleRGBA(const std::vector<unsigned char>& src, int w, int h,
                                    std::vector<unsigned char>& dst, int& outW, int& outH) {
    outW = std::max(1, w / 2);
    outH = std::max(1, h / 2);
    dst.assign((size_t)outW * outH * 4u, 0);
    if (w <= 0 || h <= 0 || src.size() < (size_t)w * h * 4u) return;

    // Усреднение 2x2, а не выбрасывание каждого второго пикселя. Прореживание
    // вдвое дешевле и даёт заметный алиасинг: тонкие линии текстуры то
    // появляются, то исчезают при движении. Здесь это особенно важно, потому
    // что понижение применяется к УЖЕ ВИДИМЫМ текстурам — рябь была бы заметна
    // сразу.
    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            const int sx = std::min(x * 2, w - 1);
            const int sy = std::min(y * 2, h - 1);
            const int sx1 = std::min(sx + 1, w - 1);
            const int sy1 = std::min(sy + 1, h - 1);
            for (int c = 0; c < 4; ++c) {
                const int a0 = src[((size_t)sy * w + sx) * 4 + c];
                const int a1 = src[((size_t)sy * w + sx1) * 4 + c];
                const int a2 = src[((size_t)sy1 * w + sx) * 4 + c];
                const int a3 = src[((size_t)sy1 * w + sx1) * 4 + c];
                dst[((size_t)y * outW + x) * 4 + c] = (unsigned char)((a0 + a1 + a2 + a3 + 2) / 4);
            }
        }
    }
}

std::vector<size_t> ResourceManager::SelectDowngrades(
    const std::vector<DowngradeCandidate>& candidates, size_t currentBytes, size_t budget) {
    std::vector<size_t> chosen;
    if (budget == 0 || currentBytes <= budget) return chosen;

    // Сортируем по давности обращения: та, к которой давно не обращались,
    // переживёт понижение незаметнее.
    std::vector<const DowngradeCandidate*> order;
    order.reserve(candidates.size());
    for (const DowngradeCandidate& c : candidates) {
        // Ниже 64 пикселей не опускаемся: дальше экономия копеечная, а текстура
        // превращается в кашу, и это видно уже не «слегка».
        if (c.Streamable && c.Side > 64) order.push_back(&c);
    }
    std::sort(order.begin(), order.end(),
              [](const DowngradeCandidate* a, const DowngradeCandidate* b) { return a->Tick < b->Tick; });

    size_t bytes = currentBytes;
    for (const DowngradeCandidate* c : order) {
        if (bytes <= budget) break;
        // Понижение вдвое по стороне — это вчетверо по площади, то есть
        // освобождается три четверти занятого текстурой.
        bytes -= std::min(bytes, c->Bytes - c->Bytes / 4);
        chosen.push_back(c->Index);
    }
    return chosen;
}

void ResourceManager::EvictToBudget() {
    if (m_textureBudget == 0 || m_textureBytes <= m_textureBudget) return;

    // Кандидаты: резидентные, не в полёте, не ссылаемые извне (use_count()==1 —
    // держит только кэш). Ссылающиеся текстуры не трогаем — это была бы висячая
    // ссылка у материала/UI.
    std::vector<EvictCandidate> cands;
    std::vector<std::string> keys;
    cands.reserve(m_textures.size());
    keys.reserve(m_textures.size());
    for (auto& kv : m_textures) {
        const TextureRecord& rec = kv.second;
        EvictCandidate c;
        c.Index = keys.size();
        c.Bytes = rec.Bytes;
        c.Tick = rec.Tick;
        c.Evictable = rec.Tex && !rec.Pending && rec.Tex.use_count() == 1;
        cands.push_back(c);
        keys.push_back(kv.first);
    }

    std::vector<size_t> toEvict = SelectEvictions(cands, m_textureBytes, m_textureBudget);
    for (size_t idx : toEvict) {
        auto it = m_textures.find(keys[idx]);
        if (it == m_textures.end()) continue;
        m_textureBytes -= std::min(m_textureBytes, it->second.Bytes);
        m_textures.erase(it);
        ++m_evictions;
    }

    // СТРИМИНГ МИПОВ. Вытеснение бессильно, когда все текстуры кому-то нужны:
    // выгрузить их нельзя (это висячая ссылка), и бюджет просто молча
    // превышается — то есть настройка «бюджет VRAM» не работает ровно в том
    // случае, ради которого её заводили, в тяжёлой сцене.
    //
    // Понижение разрешения работает и здесь: объект Texture остаётся жив, у
    // держателей ничего не рвётся, меняется только картинка внутри.
    if (!m_mipStreaming || m_textureBytes <= m_textureBudget) return;

    std::vector<DowngradeCandidate> dcands;
    dcands.reserve(m_textures.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        const TextureRecord& rec = m_textures.count(keys[i]) ? m_textures[keys[i]] : TextureRecord{};
        if (!rec.Tex) continue;
        DowngradeCandidate d;
        d.Index = i;
        d.Bytes = rec.Bytes;
        d.Tick = rec.Tick;
        d.Side = std::max(rec.Tex->Width(), rec.Tex->Height());
        d.Streamable = !rec.Pending;
        dcands.push_back(d);
    }
    for (size_t idx : SelectDowngrades(dcands, m_textureBytes, m_textureBudget)) {
        DowngradeTexture(keys[idx]);
    }
}

void ResourceManager::DowngradeTexture(const std::string& path) {
    auto it = m_textures.find(path);
    if (it == m_textures.end() || !it->second.Tex) return;
    TextureRecord& rec = it->second;

    // Пиксели берём заново с диска: держать их копию в оперативной памяти ради
    // возможного понижения значило бы экономить видеопамять за счёт обычной,
    // то есть менять шило на мыло. Чтение синхронное — понижение случается
    // редко (только при исчерпанном бюджете) и стоит дешевле, чем срыв кадра
    // из-за нехватки видеопамяти.
    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
    if (!DecodeImageFile(path, pixels, w, h)) return;

    // Целевая сторона — вдвое меньше текущей, а не исходной: понижения
    // накапливаются, и второй проход должен уменьшать то, что уже уменьшено.
    int targetSide = std::max(rec.Tex->Width(), rec.Tex->Height()) / 2;
    targetSide = std::max(targetSide, 64);

    std::vector<unsigned char> scaled;
    int sw = w, sh = h;
    while (std::max(sw, sh) > targetSide && std::max(sw, sh) > 1) {
        DownscaleRGBA(pixels, sw, sh, scaled, sw, sh);
        pixels.swap(scaled);
    }

    const size_t before = rec.Bytes;
    rec.Tex->ReplacePixels(pixels.data(), sw, sh);
    rec.Bytes = rec.Tex->GpuBytes();
    rec.MaxSide = std::max(sw, sh);
    m_textureBytes -= std::min(m_textureBytes, before);
    m_textureBytes += rec.Bytes;
    ++m_downgrades;
}

int ResourceManager::GarbageCollectUnused() {
    int freed = 0;

    // Материалы первыми: отпускают ссылки на свои текстуры, после чего те могут
    // стать неиспользуемыми и тоже выгрузиться этим же проходом.
    for (auto it = m_materials.begin(); it != m_materials.end();) {
        if (!it->second || it->second.use_count() == 1) {
            it = m_materials.erase(it);
            ++freed;
        } else {
            ++it;
        }
    }
    for (auto it = m_textures.begin(); it != m_textures.end();) {
        TextureRecord& rec = it->second;
        const bool unused = !rec.Tex || (!rec.Pending && rec.Tex.use_count() == 1);
        if (unused) {
            m_textureBytes -= std::min(m_textureBytes, rec.Bytes);
            it = m_textures.erase(it);
            ++freed;
        } else {
            ++it;
        }
    }
    for (auto it = m_models.begin(); it != m_models.end();) {
        if (!it->second || it->second.use_count() == 1) {
            it = m_models.erase(it);
            ++freed;
        } else {
            ++it;
        }
    }
    return freed;
}

ResourceManager::Stats ResourceManager::GetStats() const {
    Stats s;
    s.Downgrades = m_downgrades;
    s.Textures = m_textures.size();
    s.Models = m_models.size();
    s.Materials = m_materials.size();
    s.TextureBytes = m_textureBytes;
    s.TextureBudget = m_textureBudget;
    s.PendingAsync = m_async ? (size_t)std::max(0, m_async->Pending.load()) : 0;
    s.Evictions = m_evictions;
    return s;
}

void ResourceManager::StartWorker() {
    if (m_async) return;
    m_async = std::make_unique<AsyncImpl>();
    m_async->Worker = std::thread([impl = m_async.get()] { impl->Run(); });
}

void ResourceManager::Clear() {
    // Останавливаем фоновый поток ДО очистки карт: иначе он мог бы дописать
    // результат для только что удалённой записи (безвредно, но чище так).
    if (m_async) {
        m_async->Stop.store(true);
        m_async->JobCv.notify_all();
        if (m_async->Worker.joinable()) m_async->Worker.join();
        m_async.reset();
    }
    m_cube.reset();
    m_sphere.reset();
    m_plane.reset();
    m_cylinder.reset();
    m_cone.reset();
    m_models.clear();
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

ResourceManager::~ResourceManager() {
    if (m_async) {
        m_async->Stop.store(true);
        m_async->JobCv.notify_all();
        if (m_async->Worker.joinable()) m_async->Worker.join();
    }
}
