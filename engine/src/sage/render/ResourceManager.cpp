#include <array>
#include <filesystem>
#include "ResourceManager.h"
#include "sage/core/EngineContext.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Quarantine.h"
#include "sage/assets/import/ModelProbe.h"
#include "sage/render/AlphaBleed.h"
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
        bool Bleed = false;   // вырез по альфе: цвет под прозрачностью (AlphaBleed.h)
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
            if (d.Ok && job.Bleed) sage::render::BleedTransparentColor(d.Pixels, d.W, d.H);
            {
                std::lock_guard<std::mutex> lk(ResMx);
                Results.push_back(std::move(d));
            }
        }
    }
};

namespace {
// Путь из проекта -> путь, который откроется ИЗ ТЕКУЩЕГО каталога процесса.
std::string Locate(const std::string& path) {
    return sage::AssetDatabase::Instance().LocatePath(path);
}

// КЛЮЧ КЭША: один файл — одна запись, как бы путь ни написали.
//
// Ключом раньше был путь «как дали», и это ломало редактор материалов
// НАСМЕРТЬ. Сущность держит ссылку относительно проекта («assets/wall.sagemat»
// — так пишет Project::AssetRef, так она уезжает в .sage), а панель ассетов
// работает настоящим путём в файловой системе («<проект>/assets/wall.sagemat»).
// Два написания — две записи в кэше — ДВА РАЗНЫХ объекта Material на один файл:
// редактор правил свой, рендер рисовал свой. Со стороны это выглядело как
// «правки материала не применяются» — и не применялись они никогда, ни к
// одному полю. Тем же путём один и тот же меш и одна и та же текстура ложились
// в память по два раза.
//
// НАСТРОЙКИ ВЫБОРКИ — ЧАСТЬ КЛЮЧА.
//
// Раньше ключом был только путь, то есть одна картинка на файл. Два
// потребителя с разными требованиями (материал просит анизотропную с
// мипмапами, элемент интерфейса — Nearest без них) перекидывали её туда-сюда
// каждый кадр; чтобы не заливать текстуру на видеокарту дважды за кадр, после
// двух перекидываний настройка ЗАМОРАЖИВАЛАСЬ на той, что уже на карте.
//
// Снаружи это и было «фильтрация не работает»: набор спрайтов 16x16 остаётся
// сглаженным, что ни поставь, — потому что ту же картинку рядом показывает
// слот ассета, и он просит сглаживание. Причём замирало оно молча (кроме одной
// строки в логе), а от порядка обращений зависело, чья настройка победит.
//
// Теперь у каждой пары «фильтр + мипмапы» своя запись: никто ни у кого ничего
// не отнимает, пересоздавать нечего, и обе картинки на видеокарте существуют
// ровно столько, сколько ими действительно пользуются (GarbageCollectUnused
// выгружает ту, на которую не осталось ссылок).
//
// Сначала Locate (ссылка проекта -> открывающийся путь), затем
// weakly_canonical: он убирает «.», «..», разницу разделителей и символические
// ссылки, и НЕ требует существования файла — несуществующий путь тоже получает
// одно каноническое написание, поэтому негативный кэш работает так же.
//
// Загрузка при этом идёт по-прежнему через Locate(исходный путь): ключ — дело
// кэша, а чем открыть файл, решает база ассетов.
//
// Цена — около двух микросекунд на вызов (Locate плюс weakly_canonical, обе
// ходят в файловую систему), и её НЕ НАДО прятать за таблицей запомненных
// ответов: ключ считается только при загрузке ассета и при назначении его
// объекту, а не в кадре — в кадре материалы и текстуры уже разобраны в
// shared_ptr. Таблица же добавила бы устаревание, привязанное к текущему
// каталогу процесса и к корню проекта: файл при этом существует, просто не тот.
std::string SamplingSuffix(TextureFilter filter, bool mipmaps, bool bleed = false,
                           bool srgb = false) {
    return std::string("|f") + std::to_string((int)filter) + (mipmaps ? "m" : "") +
           (bleed ? "b" : "") + (srgb ? "s" : "");
}

std::string CacheKey(const std::string& path) {
    if (path.empty()) return path;
    std::error_code ec;
    const std::filesystem::path full = std::filesystem::weakly_canonical(Locate(path), ec);
    if (ec || full.empty()) return std::filesystem::path(path).lexically_normal().generic_string();
    return full.generic_string();
}
} // namespace

// --- GL-независимое декодирование (используется и воркером, и юнит-тестом) ---
ResourceManager::ResourceManager() = default;

ResourceManager& ResourceManager::Instance() {
    return sage::EngineContext::Current().Resources();
}

bool ResourceManager::DecodeImageFile(const std::string& path, std::vector<unsigned char>& outRGBA,
                                      int& outW, int& outH) {
    // Тот же флип, что и синхронный путь Texture(path): (0,0) внизу-слева для GL,
    // чтобы async- и sync-текстуры выглядели одинаково.
    //
    // ФЛАГ — ПОТОЧНЫЙ (_thread), и это не перестраховка. Обычный
    // stbi_set_flip_vertically_on_load ГЛОБАЛЕН на весь процесс, а эта функция
    // работает в фоновом потоке загрузки текстур. Рядом, на главном потоке,
    // идут разборы, которым нужно ОБРАТНОЕ значение: картинки внутри glTF
    // (у них v=0 — верхняя строка), грани скайбокса, запечённые карты света.
    // Выставит воркер свой true между «поставил false» и «декодирую» — и
    // текстура модели приезжает перевёрнутой: у палитровых моделей цвета лежат
    // в одном углу, после переворота развёртка попадает в пустоту, и персонаж
    // выходит чёрным. Плавающе, «через раз», в зависимости от того, успела ли
    // рядом грузиться чужая картинка, — то есть ровно так, как выглядит
    // «модель не всегда загружается нормально».
    //
    // Поточный флаг снимает гонку целиком: у каждого потока своё значение, а
    // ставят его ВСЕ, кто декодирует (см. остальные вызовы
    // stbi_set_flip_vertically_on_load_thread в движке) — смешивать поточный и
    // глобальный нельзя, поточный всегда старше.
    stbi_set_flip_vertically_on_load_thread(true);
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

int ResourceManager::ReloadChangedAssets(size_t budget) {
    int reloaded = 0;

    // ОКНО ОПРОСА. Все записи четырёх кэшей пронумерованы подряд (модели,
    // материалы, скелетные модели, текстуры), и за вызов опрашиваются только
    // номера из окна [курсор, курсор + budget) по кругу. Обход самих
    // контейнеров дёшев — дорог системный вызов на каждый файл, и именно их
    // число бюджет ограничивает.
    const size_t total =
        m_modelStamps.size() + m_materialStamps.size() + m_skinnedStamps.size() + m_textures.size();
    if (total == 0) return 0;
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
        if (inWindow() && FileStamp(Locate(path)) != stamp) staleModels.push_back(path);
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
        if (rec.Generated || rec.Pending || !rec.Tex) continue;
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
    return m_async ? std::max(0, m_async->Pending.load()) : 0;
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
    if (path.empty()) return nullptr;
    // Тот же ключ, что у GetTexture с теми же настройками, — иначе асинхронно
    // загруженная картинка легла бы в ДРУГУЮ запись кэша, чем ту, которую
    // потом спросят обычным путём, и файл прочитался бы дважды.
    const std::string key = CacheKey(path) + SamplingSuffix(filter, mipmaps, bleed, srgb);
    auto it = m_textures.find(key);
    if (it != m_textures.end()) {
        it->second.Tick = NextTick();
        return it->second.Tex; // готовая или ещё грузящийся плейсхолдер
    }

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
        m_textures[key] = TextureRecord{}; // негативный кэш
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
    rec.Stamp = FileStamp(Locate(path));
    std::shared_ptr<Texture> result = rec.Tex;
    m_textureBytes += rec.Bytes;
    m_textures[key] = std::move(rec);

    StartWorker();
    {
        // Ключ задачи — тот же канонический: результат ищется в кэше по нему,
        // и разойдись они, готовые пиксели не нашли бы своей записи.
        std::lock_guard<std::mutex> lk(m_async->JobMx);
        m_async->Jobs.push_back({key, Locate(path), bleed});
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
            // С ТЕМИ настройками выборки, с какими заказывали: здесь стояли
            // значения по умолчанию, и картинка, заказанная резкой или без
            // мипмапов, приезжала сглаженной.
            rec.Tex->ReplacePixels(d.Pixels.data(), d.W, d.H, rec.Filter, rec.Mipmaps);
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

std::shared_ptr<Material> ResourceManager::GetMaterial(const std::string& path) {
    const std::string key = CacheKey(path);
    auto it = m_materials.find(key);
    if (it != m_materials.end()) {
        // ПУТИ МОГЛИ ПОМЕНЯТЬСЯ ПОСЛЕ ЗАГРУЗКИ — и меняет их не одно место (см.
        // Material::ResolvedFrom). Раньше кэш отдавал материал как есть, с
        // указателями от прежних путей: назначенная кнопкой «Обзор…» текстура
        // не появлялась НИКОГДА, потому что перерезолвить её было некому.
        // Сравнение шести строк дешевле любой попытки помнить это правило
        // руками — и, в отличие от правила, не забывается.
        RefreshMaterialTextures(*it->second);
        return it->second;
    }
    auto material = std::make_shared<Material>();
    try {
        *material = Material::LoadFromFile(Locate(path));
    } catch (const std::exception& e) {
        LOG_ERROR("Resources") << "Материал не загрузился, использую дефолт: " << e.what();
    }
    ResolveMaterialTextures(*material);
    if (material->HasCustomShader())
        material->ShaderPtr = GetShader(material->VertexShaderPath, material->FragmentShaderPath);
    m_materials[key] = material;
    m_materialStamps[key] = FileStamp(Locate(path));
    return material;
}

std::shared_ptr<Material> ResourceManager::MakeMaterial(const std::string& name) {
    auto it = m_materials.find(name);
    if (it != m_materials.end()) return it->second;
    auto material = std::make_shared<Material>();
    m_materials[name] = material;
    // Штамп НЕ ставим: у материала нет файла, и ReloadChangedAssets, увидев
    // штамп 0 против несуществующего файла, перечитывал бы его каждый кадр,
    // затирая всё, что скрипт в нём настроил.
    return material;
}


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
    auto it = m_materials.find(CacheKey(path));
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

// Карты материала — АНИЗОТРОПНЫЕ, а не трилинейные.
//
// Поддержка анизотропии в движке была с самого начала: и режим фильтрации, и
// запрос лимита у драйвера, и применение в бэкенде. Не было одного — её никто
// не просил: каждая карта грузилась трилинейной, потому что таково значение
// параметра по умолчанию. То есть код работал вхолостую, а пол под острым
// углом мылился на любой видеокарте — включая те, где резкость досталась бы
// даром.
//
// Почему именно карты материала, а не все текстуры подряд: анизотропия имеет
// смысл там, где поверхность видна ПОД УГЛОМ и сжата сильнее по одной оси, —
// это пол, стены, дорога. Картинки интерфейса всегда фронтальны, и платить за
// них нечем и незачем; пиксель-арт и вовсе грузится Nearest без мипмапов.
//
// Карта без расширения получает трилинейную фильтрацию сама (см.
// EffectiveFilter в Texture.cpp) — запрос не отказывает, а опускается.
void ResourceManager::ResolveMaterialTextures(Material& m) {
    constexpr TextureFilter kSurface = TextureFilter::Anisotropic;
    // Цвет под прозрачностью правится только у карты, которую материал режет
    // по альфе (см. AlphaBleed.h): у остальных альфа значит что-то своё.
    const bool cutout = m.Render.AlphaCutoff > 0.0f;
    auto get = [&](const std::string& path, bool bleed, bool colour) {
        return m_streamMaterialTextures ? GetTextureAsync(path, kSurface, true, bleed, colour)
                                        : GetTexture(path, kSurface, true, bleed, colour);
    };
    // ЦВЕТ — в sRGB, ДАННЫЕ — как есть. Альбедо и свечение — это картинки,
    // нарисованные для глаза (sRGB); нормали, металличность, шероховатость и
    // затенение — числа, и переводить их нельзя: нормаль «исказилась бы».
    m.AlbedoTex = get(m.TexturePath, cutout, true);
    m.NormalTex = get(m.NormalMapPath, false, false);
    m.MetallicTex = get(m.MetallicMapPath, false, false);
    m.RoughnessTex = get(m.RoughnessMapPath, false, false);
    m.AOTex = get(m.AOMapPath, false, false);
    m.EmissiveTex = get(m.EmissiveMap, false, true);
    m.TexturesFrom = PathsOf(m);
    m.TexturesCutout = cutout;
}

// Слепок путей к картам — по нему видно, что указатели устарели.
Material::ResolvedFrom ResourceManager::PathsOf(const Material& m) {
    Material::ResolvedFrom f;
    f.Albedo = m.TexturePath;
    f.Normal = m.NormalMapPath;
    f.Metallic = m.MetallicMapPath;
    f.Roughness = m.RoughnessMapPath;
    f.AO = m.AOMapPath;
    f.Emissive = m.EmissiveMap;
    return f;
}

bool ResourceManager::RefreshMaterialTextures(Material& m) {
    // Включили или сняли вырез — карта альбедо нужна другая (с цветом под
    // прозрачностью или без), хотя пути не менялись.
    if (m.TexturesFrom == PathsOf(m) && m.TexturesCutout == (m.Render.AlphaCutoff > 0.0f))
        return false;
    ResolveMaterialTextures(m);
    return true;
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
        c.Evictable = rec.Tex && !rec.Pending && !rec.Generated && rec.Tex.use_count() == 1;
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
        d.Streamable = !rec.Pending && !rec.Generated;
        dcands.push_back(d);
    }
    for (size_t idx : SelectDowngrades(dcands, m_textureBytes, m_textureBudget)) {
        DowngradeTexture(keys[idx]);
    }
}

void ResourceManager::DowngradeTexture(const std::string& key) {
    auto it = m_textures.find(key);
    if (it == m_textures.end() || !it->second.Tex) return;
    TextureRecord& rec = it->second;
    // Пиксели читаются ПО ПУТИ записи: ключ теперь несёт ещё и настройки
    // выборки, и открыть его как файл нельзя.
    const std::string path = rec.Source;

    // Пиксели берём заново с диска: держать их копию в оперативной памяти ради
    // возможного понижения значило бы экономить видеопамять за счёт обычной,
    // то есть менять шило на мыло. Чтение синхронное — понижение случается
    // редко (только при исчерпанном бюджете) и стоит дешевле, чем срыв кадра
    // из-за нехватки видеопамяти.
    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
    if (!DecodeImageFile(path, pixels, w, h)) return;
    if (rec.Bleed) sage::render::BleedTransparentColor(pixels, w, h);

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
    rec.Tex->ReplacePixels(pixels.data(), sw, sh, rec.Filter, rec.Mipmaps);
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
        // Посчитанная текстура остаётся даже неиспользуемой: перечитать её
        // неоткуда, а собрана она была ровно затем, чтобы ею вот-вот покрасили
        // объект (материал заводят и назначают разными вызовами).
        const bool unused = !rec.Generated &&
                            (!rec.Tex || (!rec.Pending && rec.Tex.use_count() == 1));
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
    // ВСЕ формы разом: забыть одну нельзя, а забытая освобождала бы свой буфер
    // видеокарты после смерти GL-контекста — то есть падением при выходе.
    for (std::shared_ptr<Mesh>& primitive : m_primitives) primitive.reset();
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
    if (m_async) {
        m_async->Stop.store(true);
        m_async->JobCv.notify_all();
        if (m_async->Worker.joinable()) m_async->Worker.join();
    }
}
