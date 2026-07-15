#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------
// Asset<T> — типизированный ссылочно-считаемый ХЕНДЛ на ассет (текстуру, меш,
// шейдер...), выдаваемый AssetManager'ом. Копируется свободно; пока жив хоть
// один хендл, ассет не выгружается (см. AssetManager::CollectGarbage). Чтение
// значения — через Get()/оператор->; пока грузится (async) — nullptr.
//
// Под капотом все хендлы одного ассета делят общий AssetRecord (стёртый по
// типу), поэтому hot-reload меняет ресурс НА МЕСТЕ, и все хендлы сразу видят
// новую версию — им ничего переполучать не надо. Version() растёт при каждой
// (пере)загрузке, если потребителю нужно на неё среагировать.
// Часть ЯДРА движка.
// ---------------------------------------------------------------------
enum class AssetState { Loading, Ready, Failed };
enum class AssetType { Texture, Mesh, Shader, Audio, Text, Other };

const char* AssetTypeName(AssetType type);

// Стёртая по типу запись об ассете. Живёт в кэше AssetManager и во всех
// хендлах Asset<T>. Поля-значения (Resource/Error/Mtime) пишет и читает только
// главный поток; межпоточные — атомарные State/Version (их выставляет
// финализация загрузки, тоже в главном потоке, но читать состояние можно
// откуда угодно).
struct AssetRecord {
    std::string Key;                 // ключ кэша
    AssetType Type = AssetType::Other;
    std::vector<std::string> Files;  // исходные файлы на диске (для hot-reload)
    std::atomic<AssetState> State{AssetState::Loading};
    std::shared_ptr<void> Resource;  // конкретный T (Texture/Mesh/Shader/...)
    std::string Error;
    std::atomic<uint64_t> Version{0};
    int64_t Mtime = 0;               // последний известный max mtime по Files

    // Перезагрузить ресурс НА МЕСТЕ (hot-reload). null, если ассет не
    // перезагружаемый (напр. процедурный куб). Выполняется в главном потоке.
    // Получает запись аргументом (НЕ захватывает её) — иначе shared_ptr на
    // самого себя внутри записи образовал бы цикл и ассет никогда не выгрузился.
    std::function<void(const std::shared_ptr<AssetRecord>&)> Reload;
    // Оценка размера ресурса в байтах — для статистики. Указатель на функцию
    // (а не лямбда с захватом) — по той же причине: никаких ссылок на запись.
    size_t (*ApproxBytes)(const AssetRecord&) = nullptr;
};

template <class T>
class Asset {
public:
    Asset() = default;
    explicit Asset(std::shared_ptr<AssetRecord> rec) : m_rec(std::move(rec)) {}

    bool Valid() const { return static_cast<bool>(m_rec); }
    AssetState State() const {
        return m_rec ? m_rec->State.load(std::memory_order_acquire) : AssetState::Failed;
    }
    bool IsReady() const { return State() == AssetState::Ready; }
    bool IsLoading() const { return State() == AssetState::Loading; }
    bool IsFailed() const { return State() == AssetState::Failed; }

    // Растёт при каждой (пере)загрузке — потребитель может сравнить со своим
    // сохранённым значением, чтобы узнать про hot-reload.
    uint64_t Version() const { return m_rec ? m_rec->Version.load(std::memory_order_acquire) : 0; }

    // Готовый ресурс или nullptr (пока грузится / при ошибке). Для ресурсов,
    // используемых каждый кадр, вызывать Get() каждый кадр — дёшево, и тогда
    // hot-reload подхватывается автоматически.
    T* Get() const { return IsReady() ? static_cast<T*>(m_rec->Resource.get()) : nullptr; }
    std::shared_ptr<T> Shared() const {
        return IsReady() ? std::static_pointer_cast<T>(m_rec->Resource) : nullptr;
    }
    T* operator->() const { return Get(); }
    T& operator*() const { return *Get(); }
    explicit operator bool() const { return IsReady(); }

    const std::string& Error() const { static std::string none; return m_rec ? m_rec->Error : none; }
    const std::shared_ptr<AssetRecord>& Record() const { return m_rec; }

private:
    std::shared_ptr<AssetRecord> m_rec;
};
