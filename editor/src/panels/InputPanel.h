#pragma once
#include <string>
#include <vector>

#include "sage/input/Binding.h"
#include "sage/input/InputSystem.h"

class EditorHost;

// ---------------------------------------------------------------------------
// Панель «Управление» — раскладка проекта: какие у игры есть ДЕЙСТВИЯ и чем
// они вызываются.
//
// Зачем она нужна отдельно от кода и скриптов. Раскладку можно объявить из
// Lua (BindAction), и до сих пор это был единственный способ. Значит, чтобы
// посмотреть, что и на чём висит, приходилось читать скрипты; чтобы поменять
// клавишу — править код; а увидеть, что два действия дерутся за одну клавишу,
// было нельзя вообще ничем, кроме как поиграв и удивившись.
//
// Что панель РЕДАКТИРУЕТ: документ (host.ProjectInput()), а не работающий
// ввод. Сохраняется он в <проект>/input.sageinput и применяется к настоящему
// вводу при запуске игры — одинаково в Play-режиме и в собранной игре.
//
// ---------------------------------------------------------------------------
// ЧТО ЗДЕСЬ ВИДНО И В КАКОМ ПОРЯДКЕ
//
// Панель отвечает на один вопрос — «что и на какой клавише», — и всё остальное
// прячет до того момента, когда его спросят.
//
//   • СВЕРХУ — поиск, «+ Действие» и «+ Контекст». Сохранение и откат — не
//     здесь: они общие для всего окна настроек (Cancel / Apply).
//   • НИЖЕ — ОДИН список на всю ширину, сгруппированный по контекстам. Строка:
//     имя, назначенное «кейкапами», вид значения, меню «⋯». Группа
//     сворачивается; её приоритет, отбор устройств и удаление — в меню у
//     заголовка. Выделения, перетаскивания и правой кнопки нет: всё, что можно
//     сделать со строкой, написано в её меню.
//   • ОКНО ДЕЙСТВИЯ — одно и для создания, и для правки: имя, контекст, вид
//     значения, клавиши. Редкое (режим срабатывания, мёртвая зона, вклад осей)
//     — в свёрнутой секции «Дополнительно». Правится ЧЕРНОВИК: «Отмена»
//     отбрасывает всё, «Готово» применяет разом.
//
// Назначение клавиши — ЛОВЛЯ ВНУТРИ ЭТОГО ЖЕ окна, а не вторая модалка поверх
// первой: нажатие на «кейкап» или «+ Control» переводит его в режим «нажмите
// клавишу». Список известных источников — под сотню пунктов, и найти в нём
// «правый Alt» дольше, чем нажать его.
// ---------------------------------------------------------------------------
class InputPanel {
public:
    // Содержимое раздела «Управление» окна настроек (своего окна больше нет:
    // управление — такая же настройка игры, как экран и качество).
    void DrawBody(EditorHost& host);

private:
    // Действие адресуется ПАРОЙ «контекст + имя»: имена уникальны внутри
    // контекста, а не во всей раскладке.
    struct Ref {
        std::string Context;
        std::string Name;
    };

    // Шапка: поиск, «+ Действие», «+ Контекст», меню типовой раскладки.
    void DrawHeader(EditorHost& host, sage::input::InputSystem& input);
    // Список действий всех контекстов. Контекст — заголовок группы.
    void DrawActionList(EditorHost& host, sage::input::InputSystem& input);
    // Одна строка действия; меню строки (изменить / перенести / удалить).
    void DrawActionRow(EditorHost& host, sage::input::InputSystem& input, sage::input::Context& ctx,
                       const sage::input::Action& action);
    // Меню настроек контекста у его заголовка.
    void DrawContextMenu(EditorHost& host, sage::input::InputSystem& input,
                         sage::input::Context& ctx);

    // --- ОКНО ДЕЙСТВИЯ ------------------------------------------------------
    // Открыть окно: новое действие в контексте / правка существующего.
    void OpenNewAction(const std::string& context);
    void OpenEditAction(sage::input::InputSystem& input, const Ref& ref);
    void DrawActionModal(EditorHost& host, sage::input::InputSystem& input);
    // Привязки черновика: кейкапы, добавление, ловля.
    void DrawDraftControls(EditorHost& host, sage::input::InputSystem& input);
    // «Дополнительно»: зависит от вида значения.
    void DrawAdvanced(EditorHost& host);
    // Применить черновик к раскладке. false — имя занято или пусто.
    bool ApplyDraft(EditorHost& host, sage::input::InputSystem& input);
    // Модалка «новый контекст»: имя и приоритет.
    void DrawNewContextModal(EditorHost& host, sage::input::InputSystem& input);

    // Кто ещё занял этот источник (пусто, если никто).
    std::string ConflictWith(sage::input::InputSystem& input, const sage::input::Binding& binding,
                             const sage::input::Action* self) const;
    // Есть ли конфликт хоть у одной привязки действия — для метки в списке.
    bool HasAnyConflict(sage::input::InputSystem& input, const sage::input::Action& action) const;

    // ЧЕРНОВИК ДЕЙСТВИЯ. Окно правит копию, а не живое действие: так «Отмена»
    // честна, а переименование и перенос между контекстами — одна операция.
    struct Draft {
        bool Open = false;
        bool IsNew = true;
        Ref Original;                     // что правим (для новых пусто)
        char Name[64] = {0};
        std::string Context;
        int Type = 0;                     // ActionType
        std::vector<sage::input::Binding> Bindings;
        sage::input::ActionSettings Settings;
        // Ловля: kNoCapture — нет; −1 — добавить новую; i — заменить привязку i.
        int Capture = kNoCapture;
        static constexpr int kNoCapture = -2;
    };
    Draft m_draft;

    // Поиск по всем действиям сразу.
    char m_search[64] = {0};

    // Модалка создания контекста.
    bool m_newContextOpen = false;
    char m_newContextName[64] = {0};
    int m_newContextPriority = 10;

    // Свёрнутые группы — по имени контекста. Состояние живёт в панели, а не в
    // ImGui: имя заголовка переводится, и при смене языка ImGui считал бы его
    // другим заголовком, разворачивая всё заново.
    std::vector<std::string> m_collapsed;
    bool IsCollapsed(const std::string& context) const;
    void ToggleCollapsed(const std::string& context);
};
