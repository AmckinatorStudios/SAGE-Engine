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

    // --- Копия геометрии на стороне процессора --------------------------------
    //
    // Включает сохранение вершин/индексов у всех мешей, созданных ПОСЛЕ вызова.
    // Нужна там, где по геометрии надо что-то посчитать, а не только нарисовать:
    // точный выбор объекта мышью, подгонка камеры, замеры. Это ровно то, чем
    // занят редактор — и ровно то, чего не делает игра, поэтому флаг, а не
    // всегда: копия большой модели весит мегабайты, и платить за неё в
    // собранной игре не за что.
    //
    // Влияет только на новые меши: уже лежащие в кэше не перестраиваются, иначе
    // держатели shared_ptr получили бы другой объект под тем же путём.
    void SetKeepMeshCpuData(bool keep) { m_keepMeshCpu = keep; }
    bool KeepMeshCpuData() const { return m_keepMeshCpu; }

    // --- Примитивы (лениво создаются, живут до Clear) ---
    //
    // МАССИВОМ, А НЕ ПОЛЕМ НА КАЖДУЮ ФОРМУ. Полей было пять, и каждая новая
    // форма требовала правки в трёх местах сразу: поле, ветка в GetPrimitive и
    // строка в Clear(). Забытая строка в Clear() — не мелочь: меш остаётся жив
    // до конца программы и освобождает свой буфер видеокарты ПОСЛЕ того, как
    // умер GL-контекст, то есть редактор падает при выходе. Падает при этом не
    // там, где ошиблись, и не всегда — ровно тот вид поломки, который ищут
    // сутками. Ровно так и случилось с капсулой.
    //
    // С массивом забыть нечего: Clear() сбрасывает весь массив одной строкой, а
    // новая форма — это одна ветка в Build ниже.
    std::shared_ptr<Mesh> GetPrimitive(MeshRef::Type type) {
        const size_t index = (size_t)type;
        if (index >= kPrimitiveСount) return nullptr;
        std::shared_ptr<Mesh>& slot = m_primitives[index];
        if (!slot) {
            switch (type) {
                case MeshRef::Type::Cube:
                    slot = std::make_shared<Mesh>(Mesh::CreateCube(m_keepMeshCpu)); break;
                case MeshRef::Type::Sphere:
                    slot = std::make_shared<Mesh>(Mesh::CreateSphere(24, 32, m_keepMeshCpu)); break;
                case MeshRef::Type::Plane:
                    slot = std::make_shared<Mesh>(Mesh::CreatePlane(1, m_keepMeshCpu)); break;
                case MeshRef::Type::Cylinder:
                    slot = std::make_shared<Mesh>(Mesh::CreateCylinder(32, m_keepMeshCpu)); break;
                case MeshRef::Type::Cone:
                    slot = std::make_shared<Mesh>(Mesh::CreateCone(32, m_keepMeshCpu)); break;
                case MeshRef::Type::Capsule:
                    slot = std::make_shared<Mesh>(Mesh::CreateCapsule(12, 24, m_keepMeshCpu)); break;
                // None не рисуется, Model грузится через GetModel.
                default: return nullptr;
            }
        }
        return slot;
    }

    // Короткие имена — их зовут десятки мест; все ведут в GetPrimitive.
    std::shared_ptr<Mesh> GetCube()     { return GetPrimitive(MeshRef::Type::Cube); }
    std::shared_ptr<Mesh> GetSphere()   { return GetPrimitive(MeshRef::Type::Sphere); }
    std::shared_ptr<Mesh> GetPlane()    { return GetPrimitive(MeshRef::Type::Plane); }
    std::shared_ptr<Mesh> GetCylinder() { return GetPrimitive(MeshRef::Type::Cylinder); }
    std::shared_ptr<Mesh> GetCone()     { return GetPrimitive(MeshRef::Type::Cone); }
    std::shared_ptr<Mesh> GetCapsule()  { return GetPrimitive(MeshRef::Type::Capsule); }

    // Модель по пути. nullptr при ошибке (файл удалён/бит) — вызывающий просто
    // не рисует сущность, а сцена с одной битой моделью грузится ЦЕЛИКОМ.
    std::shared_ptr<Mesh> GetModel(const std::string& path);

    // Анимированная (скиннинг) модель — ОДНА на путь. Раньше её грузила каждая
    // сущность отдельно: дюжина одинаковых NPC означала дюжину разборов файла,
    // дюжину загрузок геометрии на видеокарту и дюжину копий всех клипов. Для
    // библиотеки анимаций это десятки мегабайт ключей .
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

    // Кладёт в кэш ГОТОВУЮ текстуру под именем, у которого нет файла на диске
    // (процедурная — см. render/TextureGen.h, снятая в рендер-текстуру, собранная
    // игрой). Дальше она ничем не отличается от загруженной: материал ссылается
    // на неё по имени, GetTexture отдаёт её из кэша, бюджет VRAM её учитывает.
    //
    // Имя, уже занятое, ПЕРЕЗАПИСЫВАЕТСЯ: процедурную текстуру пересчитывают
    // ровно затем, чтобы заменить прежнюю (сменился размер клетки, цвет,
    // разрешение), и молча оставить старую значило бы «правка не применилась».
    void RegisterTexture(const std::string& name, std::shared_ptr<Texture> texture);

    std::shared_ptr<Material> GetMaterial(const std::string& path);

    // Материал, СОБРАННЫЙ В ПАМЯТИ, без файла на диске. Кладётся в тот же кэш
    // под указанным именем, поэтому дальше он ничем не отличается от
    // загруженного: SetMaterial(obj, name) его найдёт, инстансный батчинг
    // сгруппирует по нему, редактор покажет его поля.
    //
    // ЗАЧЕМ ОТДЕЛЬНО ОТ GetMaterial. GetMaterial по несуществующему пути
    // честно пытается открыть файл, ругается в лог и отдаёт дефолт — это
    // правильное поведение для СЦЕНЫ, где пустой материал означает потерянный
    // ассет. Но скрипту, который строит материалы сам (сетка metallic/roughness,
    // палитра под цвет команды, материал, собранный из настроек игрока), файла
    // не существует по замыслу, и ошибка в логе на каждый такой материал
    // означает, что настоящую ошибку в этом логе уже не найти.
    //
    // Имя, уже занятое загруженным материалом, возвращается как есть — второй
    // вызов с тем же именем не сбрасывает правки первого.
    std::shared_ptr<Material> MakeMaterial(const std::string& name);

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

    // Перечитывает модели и материалы, файлы которых изменились на диске.
    //
    // ЗАЧЕМ ОТДЕЛЬНЫМ ВЫЗОВОМ. Кэш ресурсов хранит уже загруженный объект по
    // пути и НЕ смотрит на файл. Пока правки шли только через редактор, это
    // сходило: он сам звал ReloadModel. Но модель или материал правят и
    // снаружи — в Blender, в другом редакторе, скриптом, — и тогда движок
    // продолжал показывать ПРЕЖНИЕ данные до перезапуска. Выглядело это как
    // «правка не применилась», и искали её где угодно, кроме кэша.
    //
    // Возвращает, сколько ресурсов перечитано. Зовётся редактором раз в кадр
    // рядом с ReloadChangedShaders.
    int ReloadChangedAssets();

    // Небо из каталога с гранями px/nx/py/ny/pz/nz. Кэшируется по пути: сцена
    // спрашивает его КАЖДЫЙ кадр, а сборка cubemap — это шесть декодирований
    // картинок и загрузка в VRAM. nullptr при ошибке кэшируется тоже, иначе
    // битый путь пытался бы грузиться в каждом кадре.
    std::shared_ptr<Skybox> GetSkybox(const std::string& directory);
    // Небо из ШЕСТИ ОТДЕЛЬНЫХ ФАЙЛОВ в порядке +X,-X,+Y,-Y,+Z,-Z.
    //
    // Отдельный вход, а не «положите файлы в папку с нашими именами»: наборы
    // неба приходят россыпью и с чужими именами (right/left/top, sky_ft/sky_bk),
    // и переименовывать чужие файлы под наше соглашение — работа, которой можно
    // не быть. Кэшируется по самим путям: один и тот же набор, выбранный дважды,
    // это одна кубическая текстура.
    std::shared_ptr<Skybox> GetSkyboxFaces(const std::string faces[6]);

    // Небо из ОДНОГО файла (крест, полоса, столбец, панорама). layout — 0
    // «определить по соотношению сторон», дальше по порядку Skybox::Layout.
    std::shared_ptr<Skybox> GetSkyboxImage(const std::string& file, int layout);

    // Перечитать материал с диска В ТОТ ЖЕ разделяемый экземпляр (все
    // держатели видят обновление). Если не кэширован — просто загрузит.
    std::shared_ptr<Material> ReloadMaterial(const std::string& path);

    // Подгружает albedo/normal/… текстуры материала по его путям (для PBR-пути).
    void ResolveMaterialTextures(Material& m);

    // Пересобирает карты, ЕСЛИ пути к ним разошлись с теми, по которым собраны
    // текущие указатели. Возвращает true, если пересобрал. Зовётся из
    // GetMaterial, поэтому обычному коду вызывать её не нужно: достаточно
    // записать новый путь в поле материала.
    bool RefreshMaterialTextures(Material& m);

    // Слепок путей к картам материала (для сравнения «устарели ли указатели»).
    static Material::ResolvedFrom PathsOf(const Material& m);

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

    // Кэш форм по MeshRef::Type. Размер — по последнему значению перечисления,
    // чтобы новая форма не требовала править ещё и число.
    static constexpr size_t kPrimitiveСount = (size_t)MeshRef::Type::Model + 1;
    std::shared_ptr<Mesh> m_primitives[kPrimitiveСount];
    bool m_keepMeshCpu = false;   // см. SetKeepMeshCpuData
    std::unordered_map<std::string, std::shared_ptr<Mesh>> m_models;
    std::unordered_map<std::string, std::shared_ptr<Material>> m_materials;
    // Время правки файла на момент загрузки — по нему ReloadChangedAssets
    // понимает, что ресурс устарел. Ноль означает «файла не было»: такой
    // ресурс перечитается, как только файл появится.
    std::unordered_map<std::string, long long> m_modelStamps;
    std::unordered_map<std::string, long long> m_materialStamps;

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
        // Сколько раз один и тот же файл уже пересоздавали из-за РАЗНОЙ
        // фильтрации. Кэш держит одну картинку на путь, и два потребителя с
        // разными требованиями (материал просит анизотропную, пиксель-арт
        // интерфейса — Nearest без мипмапов) перекидывали бы её туда-обратно
        // КАЖДЫЙ КАДР, то есть заново заливали бы текстуру на видеокарту по
        // два раза за кадр. Снаружи это выглядит как необъяснимое проседание,
        // которое появляется от одной картинки, использованной дважды.
        int FilterFlips = 0;
        bool FilterWarned = false;
        // Ограничение стороны при загрузке. 0 — полное разрешение. Ставится
        // понижением под нехватку памяти (см. StreamToBudget) и снимается,
        // когда память освободилась.
        int MaxSide = 0;
        // У картинки НЕТ файла: она посчитана (процедурная) или снята движком.
        // Такую нельзя ни вытеснить, ни понизить, ни собрать мусорщиком — всё
        // это делается в расчёте «понадобится — перечитаем с диска», а читать
        // здесь неоткуда, и вместо экономии памяти получилась бы пропавшая
        // текстура.
        bool Generated = false;
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
