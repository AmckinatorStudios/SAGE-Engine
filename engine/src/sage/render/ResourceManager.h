#pragma once
#include "Mesh.h"
#include "Material.h"

namespace sage::render { class SkinnedModel; }
#include "Skybox.h"
#include "ModelLoader.h"
#include "Texture.h"
#include "Shader.h"
#include "sage/core/Log.h"
#include "sage/scene/Transform.h" // подключаем заранее не обязательно, но пусть будет явный порядок
#include "sage/scene/Components.h" // MeshRef::Type для GetPrimitive
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// ResourceManager — кэш ресурсов И менеджер памяти для больших проектов.
//
// Базовый контракт: на вход — описание (тип + путь), на выход — готовый
// Mesh/Material/Texture. Один и тот же файл, запрошенный дважды, не грузится
// повторно (положительный кэш); битый файл кэшируется как nullptr (негативный
// кэш) — не перечитывается с диска на каждый запрос.
//
// Управление памятью (для проектов, которые не помещаются в VRAM целиком):
//   • Бюджет VRAM под текстуры + LRU-вытеснение. Каждый GetTexture учитывает
//     размер; при превышении бюджета выгружаются наименее недавно
//     использованные текстуры, НА КОТОРЫЕ БОЛЬШЕ НИКТО НЕ ССЫЛАЕТСЯ
//     (shared_ptr::use_count()==1 — держит только кэш). Ссылающиеся ресурсы
//     никогда не выгружаются (это была бы висячая ссылка).
//   • GarbageCollectUnused() — разовая чистка всех неиспользуемых
//     текстур/моделей/материалов (напр. при смене сцены).
//   • Асинхронный стриминг: GetTextureAsync() возвращает объект Texture
//     МГНОВЕННО (плейсхолдер 1x1), а файл декодируется в фоновом потоке;
//     PumpAsyncUploads() на главном потоке (у него единственного GL-контекст)
//     заменяет пиксели в том же объекте — держатели shared_ptr видят картинку
//     без повторного запроса. Кадр не встаёт колом на диске/декодировании.
// ---------------------------------------------------------------------------
class ResourceManager {
public:
    static ResourceManager& Instance() {
        static ResourceManager instance;
        return instance;
    }

    // --- Примитивы (лениво создаются, живут до Clear) ---
    std::shared_ptr<Mesh> GetCube() {
        if (!m_cube) m_cube = std::make_shared<Mesh>(Mesh::CreateCube());
        return m_cube;
    }
    std::shared_ptr<Mesh> GetSphere() {
        if (!m_sphere) m_sphere = std::make_shared<Mesh>(Mesh::CreateSphere());
        return m_sphere;
    }
    std::shared_ptr<Mesh> GetPlane() {
        if (!m_plane) m_plane = std::make_shared<Mesh>(Mesh::CreatePlane());
        return m_plane;
    }
    std::shared_ptr<Mesh> GetCylinder() {
        if (!m_cylinder) m_cylinder = std::make_shared<Mesh>(Mesh::CreateCylinder());
        return m_cylinder;
    }
    std::shared_ptr<Mesh> GetCone() {
        if (!m_cone) m_cone = std::make_shared<Mesh>(Mesh::CreateCone());
        return m_cone;
    }

    // Готовый GPU-меш по описанию примитива — единая точка для сцен/сериализатора
    // (nullptr для None/Model: None не рисуется, Model грузится через GetModel).
    std::shared_ptr<Mesh> GetPrimitive(MeshRef::Type type) {
        switch (type) {
            case MeshRef::Type::Cube:     return GetCube();
            case MeshRef::Type::Sphere:   return GetSphere();
            case MeshRef::Type::Plane:    return GetPlane();
            case MeshRef::Type::Cylinder: return GetCylinder();
            case MeshRef::Type::Cone:     return GetCone();
            default:                      return nullptr;
        }
    }

    // Модель по пути. nullptr при ошибке (файл удалён/бит) — вызывающий просто
    // не рисует сущность, а сцена с одной битой моделью грузится ЦЕЛИКОМ.
    std::shared_ptr<Mesh> GetModel(const std::string& path);

    // Анимированная (скиннинг) модель — ОДНА на путь. Раньше её грузила каждая
    // сущность отдельно: дюжина одинаковых NPC означала дюжину разборов файла,
    // дюжину загрузок геометрии на видеокарту и дюжину копий всех клипов. Для
    // библиотеки анимаций это десятки мегабайт ключей на ровном месте.
    std::shared_ptr<sage::render::SkinnedModel> GetSkinnedModel(const std::string& path);

    // Перечитывает модель с диска (после смены её .sageimport-настроек), заменяя
    // кэш-запись. Держатели старого меша сохраняют его; новые запросы — свежий.
    std::shared_ptr<Mesh> ReloadModel(const std::string& path);

    // Текстура по пути (СИНХРОННО — блокирует на декодировании/загрузке).
    // nullptr при ошибке/пустом пути. Учитывается в бюджете VRAM.
    // filter/mipmaps — как сэмплировать. Для пиксель-арта нужен Nearest без
    // мипмапов: сглаживание превращает набор спрайтов в кашу, а мипмапы на
    // ЛИСТЕ спрайтов ещё и подмешивают соседний спрайт по краям. Настройки
    // запоминаются вместе с текстурой; запрос той же картинки с другими
    // пересоздаёт её на месте (все держатели shared_ptr видят изменение).
    std::shared_ptr<Texture> GetTexture(const std::string& path,
                                        TextureFilter filter = TextureFilter::Trilinear,
                                        bool mipmaps = true);

    // Текстура по пути АСИНХРОННО: возвращает объект немедленно (сначала
    // плейсхолдер 1x1), реальные пиксели подгружаются фоновым потоком и
    // заменяются в этом же объекте на PumpAsyncUploads(). Никогда не nullptr
    // при непустом пути. Для стриминга ассетов без фризов кадра.
    std::shared_ptr<Texture> GetTextureAsync(const std::string& path);

    std::shared_ptr<Material> GetMaterial(const std::string& path);

    // Шейдерная программа из пары файлов, кэшируется по этой паре. Возвращает
    // nullptr, если шейдер не собрался (ошибка уже в логе) — вызывающий рисует
    // объект штатным шейдером, а не падает и не оставляет дыру в кадре.
    // Неудачная компиляция тоже кэшируется: иначе битый шейдер пытался бы
    // собраться каждый кадр, заваливая лог и съедая время.
    std::shared_ptr<Shader> GetShader(const std::string& vertexPath,
                                      const std::string& fragmentPath);

    // Перечитать шейдеры, файлы которых изменились с момента загрузки.
    // Дёшево (stat на файл) и зовётся редактором раз в кадр: правка .frag
    // видна в сцене сразу, без перезапуска — иначе авторство шейдера
    // превращается в цикл «правка -> сборка -> перезапуск -> посмотреть».
    // Возвращает число перезагруженных программ.
    int ReloadChangedShaders();

    // Небо из каталога с гранями px/nx/py/ny/pz/nz. Кэшируется по пути: сцена
    // спрашивает его КАЖДЫЙ кадр, а сборка cubemap — это шесть декодирований
    // картинок и загрузка в VRAM. nullptr при ошибке кэшируется тоже, иначе
    // битый путь пытался бы грузиться в каждом кадре.
    std::shared_ptr<Skybox> GetSkybox(const std::string& directory);

    // Перечитать материал с диска В ТОТ ЖЕ разделяемый экземпляр (все
    // держатели видят обновление). Если не кэширован — просто загрузит.
    std::shared_ptr<Material> ReloadMaterial(const std::string& path);

    // Подгружает albedo/normal/… текстуры материала по его путям (для PBR-пути).
    void ResolveMaterialTextures(Material& m);

    // --- Управление памятью ---

    // Забирает готовые декодированные текстуры с фонового потока и создаёт/
    // заменяет их GPU-хранилище. ОБЯЗАН вызываться на главном потоке раз в кадр
    // (Application::Run делает это сам). Возвращает число загруженных за вызов.
    int PumpAsyncUploads();

    // Выгружает все текстуры/модели/материалы, на которые больше никто не
    // ссылается (use_count()==1). Возвращает число выгруженных ресурсов.
    // Вызывать при смене сцены / выгрузке уровня.
    int GarbageCollectUnused();

    // Бюджет VRAM под текстуры. При превышении GetTexture вытесняет LRU
    // НЕИСПОЛЬЗУЕМЫЕ текстуры. 0 — без ограничения (вытеснения не будет).
    void SetTextureBudgetBytes(size_t bytes) { m_textureBudget = bytes; }
    // Стриминг мип-уровней: когда бюджет исчерпан, а вытеснять нечего (все
    // текстуры кому-то нужны), уменьшать разрешение вместо того, чтобы молча
    // превышать бюджет. Слегка размытая текстура лучше вылета по памяти.
    void SetMipStreaming(bool enabled) { m_mipStreaming = enabled; }
    bool MipStreaming() const { return m_mipStreaming; }

    // Кандидат на ПОНИЖЕНИЕ разрешения (в отличие от вытеснения — применимо и к
    // тем, на кого ссылаются: объект Texture остаётся жив, меняется только
    // картинка внутри).
    struct DowngradeCandidate {
        size_t Index = 0;
        size_t Bytes = 0;
        uint64_t Tick = 0;
        int Side = 0;      // текущая длинная сторона в пикселях
        bool Streamable = false; // можно ли понизить (не в полёте, есть файл)
    };

    // Кого понизить, чтобы уложиться в бюджет. Как и SelectEvictions — ЧИСТАЯ
    // функция без GL и без файлов: политику памяти надо уметь проверять
    // тестами, а не наблюдением за игрой.
    //
    // Порядок — по давности обращения (LRU), как у вытеснения: текстура, к
    // которой давно не обращались, переживёт понижение незаметнее.
    static std::vector<size_t> SelectDowngrades(const std::vector<DowngradeCandidate>& candidates,
                                                size_t currentBytes, size_t budget);

    // Уменьшает RGBA8-картинку вдвое по каждой стороне усреднением 2x2.
    // Отдельной функцией — она чистая и проверяется без GL.
    static void DownscaleRGBA(const std::vector<unsigned char>& src, int w, int h,
                              std::vector<unsigned char>& dst, int& outW, int& outH);

    size_t TextureBudgetBytes() const { return m_textureBudget; }
    size_t ResidentTextureBytes() const { return m_textureBytes; }

    // Статистика для оверлея/тестов.
    struct Stats {
        size_t Textures = 0;      // записей в кэше текстур (вкл. плейсхолдеры)
        size_t Models = 0;
        size_t Materials = 0;
        size_t TextureBytes = 0;  // сумма приблизительных размеров текстур
        size_t TextureBudget = 0;
        size_t PendingAsync = 0;  // текстур в очереди фоновой загрузки
        size_t Evictions = 0;     // всего вытеснений за жизнь менеджера
        size_t Downgrades = 0;    // всего понижений разрешения (стриминг мипов)
    };
    Stats GetStats() const;

    void Clear();
    ~ResourceManager();

    // --- Тестируемые (GL-независимые) хелперы ---

    // Декодирует файл изображения в RGBA8 (то же, что делает фоновый поток).
    // Не трогает GL — чистое CPU-декодирование, поэтому проверяется юнит-тестом.
    // false при ошибке (файл битый/отсутствует).
    static bool DecodeImageFile(const std::string& path, std::vector<unsigned char>& outRGBA,
                                int& outW, int& outH);

    // Запись кандидата на вытеснение для чистой (без GL) LRU-политики.
    struct EvictCandidate {
        size_t Index = 0;    // индекс во внешнем списке вызывающего
        size_t Bytes = 0;    // размер ресурса
        uint64_t Tick = 0;   // «использован последний раз» (меньше = старее)
        bool Evictable = false; // можно ли выгрузить (никто не ссылается)
    };
    // Возвращает индексы кандидатов, которые надо выгрузить, чтобы уместиться в
    // бюджет: сначала самые старые (LRU), только Evictable. currentBytes —
    // текущая занятость. Чистая функция — ядро политики, юнит-тестируемо.
    static std::vector<size_t> SelectEvictions(const std::vector<EvictCandidate>& candidates,
                                               size_t currentBytes, size_t budget);

private:
    ResourceManager(); // out-of-line: m_async — неполный тип pimpl (см. .cpp)

    void StartWorker();          // лениво поднимает фоновый поток декодирования
    void EvictToBudget();        // вытеснить LRU-неиспользуемые до бюджета
    void DowngradeTexture(const std::string& path); // понизить разрешение вдвое
    uint64_t NextTick() { return ++m_tick; }

    std::shared_ptr<Mesh> m_cube, m_sphere, m_plane, m_cylinder, m_cone;
    std::unordered_map<std::string, std::shared_ptr<Mesh>> m_models;
    std::unordered_map<std::string, std::shared_ptr<Material>> m_materials;

    // Кэш шейдерных программ. Ключ — "vert|frag"; вместе с программой держим
    // пути и время правки файлов, чтобы ReloadChangedShaders понял, что менять.
    struct ShaderEntry {
        std::shared_ptr<Shader> Program; // nullptr — не собрался
        std::string VertPath, FragPath;
        long long VertStamp = 0, FragStamp = 0;
    };
    std::unordered_map<std::string, ShaderEntry> m_shaders;
    std::unordered_map<std::string, std::shared_ptr<Skybox>> m_skyboxes;

    // Запись кэша текстуры: сам ресурс + учёт для бюджета/LRU + флаг «ещё
    // грузится» (плейсхолдер, реальные байты пока не в VRAM).
    struct TextureRecord {
        std::shared_ptr<Texture> Tex;
        size_t Bytes = 0;
        uint64_t Tick = 0;
        bool Pending = false;
        // С какими настройками картинка сейчас на видеокарте.
        TextureFilter Filter = TextureFilter::Trilinear;
        bool Mipmaps = true;
        // Ограничение стороны при загрузке. 0 — полное разрешение. Ставится
        // понижением под нехватку памяти (см. StreamToBudget) и снимается,
        // когда память освободилась.
        int MaxSide = 0;
    };
    std::unordered_map<std::string, TextureRecord> m_textures;
    std::unordered_map<std::string, std::shared_ptr<sage::render::SkinnedModel>> m_skinned;

    size_t m_textureBudget = 0;      // 0 = без ограничения (по умолчанию)
    size_t m_textureBytes = 0;       // сумма Bytes резидентных текстур
    uint64_t m_tick = 0;             // монотонный счётчик обращений (LRU)
    size_t m_evictions = 0;
    size_t m_downgrades = 0;         // сколько раз понижали разрешение
    // Понижать разрешение, когда вытеснять нечего. Выключается тестами и теми,
    // кому важнее резкость, чем укладывание в бюджет.
    bool m_mipStreaming = true;

    // --- Асинхронный конвейер (реализация в .cpp, чтобы не тащить <thread>
    //     и очереди во все TU, включающие этот заголовок) ---
    struct AsyncImpl;
    std::unique_ptr<AsyncImpl> m_async;
};
