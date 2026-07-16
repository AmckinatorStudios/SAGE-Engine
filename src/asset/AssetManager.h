#pragma once
#include "Asset.h"
#include "../render/Texture.h"
#include "../render/Mesh.h"
#include "../render/Shader.h"
#include <string>
#include <unordered_map>

// ---------------------------------------------------------------------
// AssetManager — единая точка загрузки и владения ассетами движка: текстуры,
// меши (.obj), шейдеры (позже — любой тип). Часть ЯДРА движка. Заменяет
// разрозненные загрузки «текстуру создаём тут, меш кэшируем там». Даёт:
//
//   • Единый корень ассетов (SetAssetRoot) и разрешение путей (Resolve) —
//     код просит "textures/blocks.png", а не хардкодит "assets/...".
//   • Кэш + дедупликация: один и тот же файл грузится с диска один раз;
//     повторный запрос отдаёт тот же ресурс.
//   • Ссылочный счёт через хендлы Asset<T>: пока жив хоть один хендл, ассет
//     остаётся; CollectGarbage() выгружает те, на кого никто не ссылается.
//   • Синхронная и асинхронная загрузка (async — поверх JobSystem: CPU-декод
//     в фоне, GL-загрузка в главном потоке).
//   • Hot-reload: PollHotReload() перечитывает изменённые на диске файлы и
//     обновляет ресурс НА МЕСТЕ — правишь шейдер/текстуру и видишь результат
//     без перезапуска. Держатели Asset<T> ничего не переполучают.
//   • Реестр/статистика (GetStats) — сколько чего загружено и сколько памяти.
//
// ВНИМАНИЕ: все методы вызываются из ГЛАВНОГО потока (кэш трогает только он).
// В фоне (async-загрузка) выполняется лишь чистая CPU-часть загрузчиков.
//
// Использование:
//   auto& assets = AssetManager::Instance();
//   assets.SetAssetRoot("assets");
//   Asset<Texture> atlas = assets.LoadTexture("textures/atlas.png", TextureFilter::Nearest, false);
//   Asset<Shader>  basic = assets.LoadShader("shaders/basic.vert", "shaders/basic.frag");
//   ... в кадре: atlas->Bind(0);  basic->Use();
//   ... раз в N кадров (dev): assets.PollHotReload();
// ---------------------------------------------------------------------
class AssetManager {
public:
    static AssetManager& Instance();

    // Корень, относительно которого разрешаются пути ассетов (по умолчанию
    // "assets"). Resolve() сначала пробует путь как есть (для уже полных
    // "assets/..."), потом — root + "/" + path. Так работают оба стиля.
    void SetAssetRoot(const std::string& root) { m_root = root; }
    const std::string& AssetRoot() const { return m_root; }
    std::string Resolve(const std::string& path) const;

    // --- Текстуры ---
    Asset<Texture> LoadTexture(const std::string& path, TextureFilter filter = TextureFilter::Trilinear,
                               bool generateMipmaps = true);
    Asset<Texture> LoadTextureAsync(const std::string& path, TextureFilter filter = TextureFilter::Trilinear,
                                    bool generateMipmaps = true);

    // --- Модели (.obj) ---
    Asset<Mesh> LoadModel(const std::string& path);
    Asset<Mesh> LoadModelAsync(const std::string& path);
    // Процедурный единичный куб (кэшируется; не грузится с диска, не hot-reload).
    Asset<Mesh> Cube();

    // --- Шейдеры (vertex + fragment) --- (компиляция — GL, только главный поток)
    Asset<Shader> LoadShader(const std::string& vertexPath, const std::string& fragmentPath);

    // --- Hot-reload ---
    void EnableHotReload(bool on) { m_hotReload = on; }
    bool HotReloadEnabled() const { return m_hotReload; }
    // Проверяет mtime исходных файлов всех перезагружаемых ассетов и обновляет
    // изменённые. Дёшево звать раз в несколько кадров. Возвращает число
    // ассетов, поставленных на перезагрузку. No-op, если hot-reload выключен.
    size_t PollHotReload();

    // Принудительно перезагружает ВСЕ перезагружаемые ассеты, не глядя на mtime
    // (напр. по хоткею «перечитать ассеты» или в тестах). Возвращает число
    // поставленных на перезагрузку. Не требует включённого hot-reload.
    size_t ReloadAll();

    // --- Управление временем жизни / статистика ---
    // Выгружает готовые ассеты, на которые не осталось живых хендлов Asset<T>
    // (в кэше — единственная ссылка). Возвращает число выгруженных.
    size_t CollectGarbage();

    struct Stats {
        size_t Total = 0, Ready = 0, Loading = 0, Failed = 0;
        size_t ApproxBytes = 0;
        size_t ByType[6] = {0, 0, 0, 0, 0, 0}; // индексируется (int)AssetType
    };
    Stats GetStats() const;

    // Полная очистка кэша. Вызывать, пока GL-контекст ещё жив (ресурсы держат
    // GL-объекты, освобождаемые в их деструкторах) — см. main.cpp при выходе.
    void Clear();

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

private:
    AssetManager() = default;

    // Возвращает существующую запись по ключу или nullptr.
    std::shared_ptr<AssetRecord> Find(const std::string& key) const;

    // Общий фоновый путь загрузки/перезагрузки: CPU-декод в JobSystem +
    // GL-финализация в главном потоке. reload=true — обновить ресурс на месте.
    void EnqueueTextureLoad(std::shared_ptr<AssetRecord> rec, std::string resolved,
                            TextureFilter filter, bool generateMipmaps, bool reload);
    void EnqueueMeshLoad(std::shared_ptr<AssetRecord> rec, std::string resolved, bool reload);

    std::string m_root = "assets";
    bool m_hotReload = false;
    std::unordered_map<std::string, std::shared_ptr<AssetRecord>> m_cache;
};
