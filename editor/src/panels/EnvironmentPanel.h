#pragma once
#include <string>
#include <vector>

#include "sage/scene/EnvironmentSchema.h"

#include "../FileBrowser.h"

class EditorHost;
class Scene;

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
// ВЫПЕЧКИ GI ЗДЕСЬ БОЛЬШЕ НЕТ — временно. Раздел обещал готовую возможность:
// кнопка «Запечь GI», полоса, «пометить статичную геометрию». Возможность же
// сырая — результат зависит от разметки статикой, которую человек делает
// вслепую, и запечённый свет сплошь и рядом выходит хуже незапечённого.
// Кнопка, которую жмут и получают «стало хуже», дороже отсутствующей кнопки.
// Код движка (engine/src/sage/gi) и состояние сцены на месте: вернуть раздел —
// значит вернуть сюда его UI, а не писать GI заново.
// ПАНЕЛЬ РИСУЕТ ПО СХЕМЕ (sage/scene/EnvironmentSchema.h). Она не знает ни
// одного поля неба или тумана по имени: система (небо, цикл суток,
// окружающий свет, туман) даёт включатель, выбор типа и группы свойств своего
// типа, а панель показывает ровно их. Новый тип неба — новая запись в схеме;
// сюда при этом не добавляется ни строчки. Особые виджеты (готовый вид неба,
// «что сейчас на небе», вычисленный цвет) схема отмечает как Custom, и панель
// рисует их по ключу.
class EnvironmentPanel {
public:
    void Draw(EditorHost& host, bool* open);

    // Самопроверка редактора: сколько строк свойств панель нарисовала в
    // последнем кадре и какие ключи — «показываем только нужное» проверяется
    // по факту отрисовки, а не по схеме.
    const std::vector<std::string>& LastDrawnKeys() const { return m_drawn; }

private:
    void DrawSystem(EditorHost& host, const sage::env::System& system, LightingEnvironment& env);
    void DrawGroup(EditorHost& host, const sage::env::Group& group, LightingEnvironment& env,
                   bool& changed);
    void DrawProps(EditorHost& host, const std::vector<sage::env::Prop>& props,
                   LightingEnvironment& env, bool& changed);
    bool DrawProp(EditorHost& host, const sage::env::Prop& prop, LightingEnvironment& env);
    void DrawCustom(EditorHost& host, const std::string& key, LightingEnvironment& env);
    void DrawFaceMap(EditorHost& host, LightingEnvironment& env);
    // Строка про объект-солнце: время суток задаётся его поворотом или циклом,
    // и добраться до него надо отсюда одним нажатием.
    void DrawSunLink(EditorHost& host, Scene& scene, LightingEnvironment& env);

    FileBrowser m_browser;
    // Ключ свойства, для которого открыт файловый диалог: ответ приходит через
    // кадр, и указатель на поле за это время мог бы повиснуть (откат, другая сцена).
    std::string m_pickKey;
    bool m_convertFailed = false;   // перевод папки неба не нашёл всех граней
    std::vector<std::string> m_drawn;
};
