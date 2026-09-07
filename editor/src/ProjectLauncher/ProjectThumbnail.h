#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "imgui.h"

class Texture;

namespace Sage::Launcher {

struct ProjectEntry;

// ---------------------------------------------------------------------------
// ОБЛОЖКИ ПРОЕКТОВ: асинхронно, маленькими, с кэшем на диске.
//
// ПОЧЕМУ НЕ «ПРОСТО ПОКАЗАТЬ КАРТИНКУ ПРОЕКТА». Обложка проекта — это снимок
// экрана: 1920x1080 PNG, три-четыре мегабайта. Показать двенадцать таких в
// сетке значит на открытии окна разобрать сорок мегабайт JPEG/PNG в главном
// потоке и залить в видеопамять сто мегабайт текстур ради картинок размером с
// спичечный коробок. Стартовое окно от этого замирает на секунды — а оно
// первое, что человек видит, и по нему судит о скорости всей программы.
//
// КАК ЗДЕСЬ. Три уровня:
//   1. КЭШ НА ДИСКЕ. Уменьшенная копия (не больше 320x180) лежит рядом с
//      настройками редактора. Второй запуск разбирает килобайты вместо
//      мегабайтов.
//   2. ФОНОВЫЙ ПОТОК. Разбор и уменьшение идут не в кадре. Кадр берёт готовые
//      пиксели и заливает их в текстуру — только это и обязано быть в главном
//      потоке (графический контекст один).
//   3. РИСУНОК ВМЕСТО ОЖИДАНИЯ. Пока картинки нет (ещё грузится, её нет вовсе,
//      проект пропал), карточка показывает СВОЮ обложку — она рисуется
//      примитивами по имени проекта, поэтому у каждого проекта она своя и
//      узнаваемая. Пустой серый прямоугольник читался бы как «сломалось».
//
// ЛИМИТ ТЕКСТУР. Их не больше kMaxTextures: база на пятьсот проектов не имеет
// права держать пятьсот текстур в видеопамяти. Вытесняется та, которую дольше
// всех не рисовали, — она за кадром, и её пропажи не видно.
// ---------------------------------------------------------------------------
class ProjectThumbnail {
public:
    ~ProjectThumbnail();

    // Отпустить текстуры и остановить поток, ПОКА ГРАФИЧЕСКИЙ КОНТЕКСТ ЖИВ.
    // Деструктор для этого не годится: он срабатывает уже после разрушения
    // контекста — ровно так редактор падал при выходе на превью материалов.
    void Shutdown();

    // Забрать готовые пиксели и залить их в текстуры. Зовётся раз за кадр из
    // стартового окна, ДО отрисовки карточек.
    void Pump();

    // Обложка записи в прямоугольник экрана. Если картинки ещё (или уже) нет —
    // рисует свою. Ничего не подаёт как элемент ImGui: это чистый рисунок,
    // годный внутри кнопки-карточки.
    void Draw(const ProjectEntry& entry, const ImVec2& min, const ImVec2& max, float rounding);

    // Сколько картинок загружено и сколько ещё в очереди — для самопроверки и
    // отладки. Иначе «асинхронность» проверяется только на глаз.
    int LoadedCount() const;
    int PendingCount() const;

    // Куда кладутся уменьшенные копии.
    static std::filesystem::path CacheDir();
    static std::filesystem::path CachePathFor(const std::string& source);

    // Предельный размер уменьшенной копии.
    static constexpr int kThumbWidth = 320;
    static constexpr int kThumbHeight = 180;
    // Сколько текстур держим в видеопамяти одновременно.
    static constexpr int kMaxTextures = 64;

private:
    // Готовые пиксели из фонового потока.
    struct Decoded {
        std::string Source;
        int W = 0, H = 0;
        std::vector<unsigned char> Pixels;   // RGBA8, сверху вниз
        bool Failed = false;
    };

    struct Item {
        std::shared_ptr<Texture> Tex;
        bool Requested = false;   // уже отдан фоновому потоку
        bool Failed = false;      // файл битый: второй раз не просим
        uint64_t UsedFrame = 0;   // когда последний раз рисовали (для вытеснения)
    };

    void Request(const std::string& source);
    void EnsureWorker();
    void EvictIfNeeded();
    // Обложка, нарисованная примитивами: своя у каждого проекта.
    void DrawGenerated(const ProjectEntry& entry, const ImVec2& min, const ImVec2& max,
                       float rounding);

    std::unordered_map<std::string, Item> m_items;
    uint64_t m_frame = 0;

    // --- фоновый поток ---
    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::string> m_queue;      // что разобрать
    std::vector<Decoded> m_ready;         // что готово забрать в кадре
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_pending{0};
};

} // namespace Sage::Launcher
