#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "sage/gi/GI.h"

class EditorHost;

// Панель Lighting — окружение сцены (Scene::Lighting): полусферический
// ambient (небо/земля/сила) и направленное «солнце». Точечные света — это
// СУЩНОСТИ с LightComponent (Inspector > Light, Entity > Create Light), их
// панель только перечисляет. Всё сериализуется со сценой, правки попадают в
// undo через TrackLastImGuiItem.
//
// Здесь же — запечённое глобальное освещение (GI, sage/gi): настройки бейка,
// запуск в ФОНОВОМ потоке (вход собирается на главном потоке — фон сцену не
// трогает) и применение результата к сцене по завершении.
class LightingPanel {
public:
    ~LightingPanel();
    void Draw(EditorHost& host);

private:
    void DrawGISection(EditorHost& host);
    void StartBake(EditorHost& host, const sage::gi::GISettings& settings);

    std::thread m_bakeThread;
    std::atomic<bool> m_bakeRunning{false};
    std::atomic<float> m_bakeProgress{0.0f};
    std::mutex m_bakeMutex;                          // защищает m_bakePhase и m_bakeResult
    std::string m_bakePhase;
    std::shared_ptr<sage::gi::GIState> m_bakeResult; // готов, когда !m_bakeRunning и непуст
};
