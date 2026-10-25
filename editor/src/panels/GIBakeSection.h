#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <entt/entt.hpp>

#include "sage/gi/GI.h"

class EditorHost;
class Scene;

// ---------------------------------------------------------------------------
// РАЗДЕЛ «ЗАПЕЧЁННОЕ ОСВЕЩЕНИЕ» панели Environment — запекание GI из редактора.
//
// Движок умеет запекать непрямой свет (sage/gi: лайтмапы статики и объём
// световых проб для всего остального), а редактор этого не показывал: раздел
// был убран, потому что результат выходил хуже незапечённого. Причина была в
// бейкере (развёртка UV2 и растеризация текселей — см. LightmapUV.cpp и
// RasterizeChart), а не в самой идее, и она исправлена.
//
// Чего раздел НЕ требует — разметки вслепую. «Пометить статику» размечает всю
// сцену по одному понятному правилу (IsStaticCandidate): объект с мешем, который
// ничто не двигает. Запекание идёт в фоне (CPU, свои потоки), окно не замирает;
// готовый результат применяется к сцене, только если её статика за это время
// не изменилась.
// ---------------------------------------------------------------------------
class GIBakeSection {
public:
    ~GIBakeSection();

    void Draw(EditorHost& host);

    // Качество: Черновик / Среднее / Высокое — плотность лайтмап, число лучей и
    // отскоков. Поля GISettings по отдельности человеку ничего не говорят.
    enum class Quality { Draft = 0, Medium, High };
    static sage::gi::GISettings SettingsFor(Quality quality, float probeCell);

    // Правило разметки: меш есть, и его ничто не двигает — ни физика, ни
    // персонаж, ни анимация, ни скрипт. Динамику освещают пробы, а не лайтмапа.
    static bool IsStaticCandidate(Scene& scene, entt::entity e);
    // Помечает все подходящие объекты, возвращает, сколько помечено ВСЕГО.
    static int MarkStatic(Scene& scene);
    static int StaticCount(Scene& scene);

    // Запустить запекание текущей сцены (false — уже идёт или нечего печь).
    bool Start(EditorHost& host);
    bool Busy() const { return m_job != nullptr; }
    // Забрать готовый результат (зовётся из Draw; самопроверка зовёт сама).
    // true — результат применён к сцене.
    bool Poll(EditorHost& host);
    // Дождаться конца фонового запекания (самопроверка, закрытие редактора).
    void Wait();

    // Состояние запечённого GI сцены — словами для строки статуса.
    enum class Status { None, Baked, Stale };
    static Status SceneStatus(Scene& scene);

private:
    struct Job {
        std::thread Thread;
        std::atomic<float> Progress{0.0f};
        std::atomic<bool> Done{false};
        std::mutex PhaseMutex;
        std::string Phase;
        std::shared_ptr<sage::gi::GIState> Result;
        Scene* Target = nullptr;
        uint64_t Hash = 0;
        sage::gi::GISettings Settings;
    };
    std::unique_ptr<Job> m_job;
    Quality m_quality = Quality::Medium;
    float m_probeCell = 2.0f;
    std::string m_lastMessage;
};
