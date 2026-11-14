#pragma once
#include "sage/assets/Pack.h"
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
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include "sage/jobs/JobSystem.h"
#include <utility>

// Внутреннее устройство кэша ресурсов, общее для его файлов: загрузка и
// перезагрузка (ResourceManager.cpp), материалы (ResourceManager_Materials.cpp),
// бюджет памяти (ResourceManager_Memory.cpp).

// ---------------------------------------------------------------------------
// Асинхронный конвейер декодирования — на общей системе задач.
//
// Раньше у кэша был СВОЙ фоновый поток с очередью и условной переменной —
// третий независимый пул рядом с пулом движка и пулом Jolt. Теперь файл
// читается и декодируется задачей полосы ввода-вывода (её потоки для того и
// есть: ожидание диска не держит ядро кадра), а результат забирает главный
// поток в PumpAsyncUploads() и заливает в VRAM (GL только там).
//
// Задача НЕ знает о кэше: она получает копии ключа и пути и возвращает
// пиксели через Future. Поэтому кэш, разрушенный или очищенный раньше, чем
// задача закончила, ничем не рискует — обращаться ей не к кому. Очистка
// отменяет ещё не начатые и дожидается идущих (группа задач).
// ---------------------------------------------------------------------------
struct ResourceManager::AsyncImpl {
    struct Decoded {
        std::string Path;
        std::vector<unsigned char> Pixels;
        int W = 0, H = 0;
        bool Ok = false;
    };
    struct InFlight {
        std::string Key;
        sage::jobs::Future<Decoded> Result;
    };
    // Только главный поток (постановка и разбор).
    std::vector<InFlight> Jobs;
    sage::jobs::TaskGroup Group;

    // Ключ кэша и путь, который реально открывается, — разные строки (см.
    // Locate ниже). Разрешение делает главный поток при постановке задачи:
    // воркеру нельзя трогать базу ассетов, её могут пересканировать в этот же
    // момент.
    void Submit(const std::string& key, const std::string& file, bool bleed) {
        sage::jobs::JobOptions o("texture.decode", sage::jobs::Category::Resources, sage::jobs::Lane::IO);
        o.Group = &Group;
        Jobs.push_back({key, sage::JobSystem::Get().Async([key, file, bleed] {
            Decoded d;
            d.Path = key;
            d.Ok = ResourceManager::DecodeImageFile(file, d.Pixels, d.W, d.H);
            if (d.Ok && bleed) sage::render::BleedTransparentColor(d.Pixels, d.W, d.H);
            return d;
        }, o)});
    }
    int Pending() const { return (int)Jobs.size(); }
    void StopAll() {
        Group.CancelAndWait();
        Jobs.clear();
        Group.Reset();
    }
};

namespace resource_detail {

// Путь из проекта -> путь, который откроется ИЗ ТЕКУЩЕГО каталога процесса.
inline std::string Locate(const std::string& path) {
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
inline std::string SamplingSuffix(TextureFilter filter, bool mipmaps, bool bleed = false,
                           bool srgb = false) {
    return std::string("|f") + std::to_string((int)filter) + (mipmaps ? "m" : "") +
           (bleed ? "b" : "") + (srgb ? "s" : "");
}

inline std::string CacheKey(const std::string& path) {
    if (path.empty()) return path;
    std::error_code ec;
    const std::filesystem::path full = std::filesystem::weakly_canonical(Locate(path), ec);
    if (ec || full.empty()) return std::filesystem::path(path).lexically_normal().generic_string();
    return full.generic_string();
}
// Время последней правки файла в виде числа; 0 — файла нет/недоступен.
inline long long FileStamp(const std::string& path) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    return (long long)t.time_since_epoch().count();
}
// Ключ части модели: «полный путь␟номер». Разделитель — символ, которого не
// бывает в путях, чтобы ключ разбирался обратно однозначно (перечитывание).
inline constexpr char kPartSep = '\x1f';
inline std::string FileOfKey(const std::string& key) {
    const size_t at = key.find(kPartSep);
    return at == std::string::npos ? key : key.substr(0, at);
}
inline int PartOfKey(const std::string& key) {
    const size_t at = key.find(kPartSep);
    return at == std::string::npos ? -1 : std::atoi(key.c_str() + at + 1);
}

} // namespace resource_detail
