#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../FileBrowser.h"

#include "sage/gi/GI.h"

class EditorHost;
class Scene;
struct LightingEnvironment;

// Панель «Среда» — окружение сцены (Scene::Lighting): полусферический ambient
// (небо/земля/сила), туман и небо. Всё сериализуется со сценой, правки попадают
// в undo через TrackLastImGuiItem.
//
// РАНЬШЕ ЭТО БЫЛО ОКНО «LIGHTING», и название обещало больше, чем окно давало.
// Свет в нём был чужой: направление и цвет солнца правились здесь, хотя солнце
// — обычная сущность с LightComponent, стоящая в иерархии; а качество теней и
// объёмный свет жили в третьем месте, в настройках движка. На вопрос «где
// настраивается освещение» честного ответа не было.
//
// Теперь граница проведена по владельцу данных:
//   • СРЕДА (здесь) — свойства мира сцены: небо, воздух, окружающий свет;
//   • ИСТОЧНИКИ — объекты в иерархии, правятся в инспекторе (Entity > Create
//     Light); панель лишь показывает, какой объект работает солнцем, и уводит
//     к нему;
//   • КАЧЕСТВО И ЦЕНА КАДРА — окно настроек движка (Game Settings).
//
// Идентификатор окна остался "###Lighting" НАМЕРЕННО: по нему ImGui находит
// окно в сохранённой раскладке (imgui.ini) и в DockBuilderDockWindow. Сменить
// его — значит у всех, кто уже работает, выкинуть панель из дока в отдельное
// плавающее окно ради строчки в исходнике.
//
// Здесь же — запечённое глобальное освещение (GI, sage/gi): настройки бейка,
// запуск в ФОНОВОМ потоке (вход собирается на главном потоке — фон сцену не
// трогает) и применение результата к сцене по завершении.
class EnvironmentPanel {
public:
    ~EnvironmentPanel();
    void Draw(EditorHost& host, bool* open);

private:
    void DrawGISection(EditorHost& host);
    // Небо: режим (процедурное / кубическая карта / шесть граней) и его
    // настройки. Солнце и луна показываются только у процедурного — у
    // текстурного неба они запечены в самих гранях, и ползунок «размер солнца»
    // там означал бы, что настройка есть, а действия у неё нет.
    void DrawSkySection(EditorHost& host, LightingEnvironment& env);
    // Окружающий свет: от неба или свои значения.
    void DrawAmbientSection(EditorHost& host, LightingEnvironment& env);
    // Строка про объект-солнце внутри настроек процедурного неба: время суток
    // задаётся его поворотом, и добраться до него надо отсюда одним нажатием.
    void DrawSunLink(EditorHost& host, Scene& scene, LightingEnvironment& env);
    void StartBake(EditorHost& host, const sage::gi::GISettings& settings);

    FileBrowser m_browser;
    // Что именно выбирают в открытом диалоге: -1 — каталог неба, 0..5 — грань.
    int m_skyPick = -2;

    std::thread m_bakeThread;
    std::atomic<bool> m_bakeRunning{false};
    std::atomic<float> m_bakeProgress{0.0f};
    std::mutex m_bakeMutex;                          // защищает m_bakePhase и m_bakeResult
    std::string m_bakePhase;
    std::shared_ptr<sage::gi::GIState> m_bakeResult; // готов, когда !m_bakeRunning и непуст
};
