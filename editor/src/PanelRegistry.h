#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "EditorTypes.h"

class EditorHost;
namespace Sage::UI { class CommandRegistry; }

// ---------------------------------------------------------------------------
// РЕЕСТР ПАНЕЛЕЙ — панель редактора описывается ОДИН раз.
//
// ЗАЧЕМ. Панель жила в шести местах сразу: перечисление EditorPanel, поле в
// EditorLayer, строка в реестре команд, пункт меню «Окно», строка в меню
// «Отдельным окном», вызов отрисовки — плюс раскладка доков по умолчанию.
// Забытое место давало панель, которую не открыть из палитры, или которую
// нельзя вытащить своим окном, или которая не возвращается «Сбросом
// раскладки», — и замечалось это только жалобой.
//
// Теперь панель — запись PanelSpec: что это (подпись, значок, раздел меню), где
// живёт (пространства, место в доке) и как рисуется. Меню «Окно», палитра
// команд, «Отдельным окном», раскладка по умолчанию, «Сбросить раскладку» и
// сама отрисовка читают реестр. Новая панель — одна регистрация:
//
//   PanelSpec notes;
//   notes.Id = "Notes";
//   notes.Title = "Notes";
//   notes.Icon = "list";
//   notes.Draw = [](EditorHost& h, bool* open, const std::string& id) { ... };
//   host.Panels().Register(std::move(notes));
//
// Регистрировать можно и во время работы (модуль, плагин): реестр не знает
// заранее ни одной панели, встроенные регистрирует EditorLayer_Panels.cpp.
// ---------------------------------------------------------------------------
namespace sage::editor {

// Раздел меню «Окно». Порядок перечисления — порядок разделов в меню.
enum class PanelSection { Scene, Interface, Shared, Tools, Plugins };

// Место в раскладке доков по умолчанию (своё в каждом пространстве).
enum class PanelDock { None, Left, Right, Bottom, Center };

struct PanelPlace {
    PanelDock Slot = PanelDock::None;
    // Порядок внутри узла: ПЕРВАЯ докнутая вкладка становится выбранной
    // (поэтому вьюпорт сцены — 0, а Game — после него).
    int Order = 100;
};

namespace panelspace {
constexpr unsigned Scene = 1u << (unsigned)EditorWorkspace::Scene;
constexpr unsigned Interface = 1u << (unsigned)EditorWorkspace::Interface;
constexpr unsigned Both = Scene | Interface;
} // namespace panelspace

struct PanelSpec {
    // Имя окна ImGui (часть после «###»). У панели с PerSpaceWindow к нему
    // добавляется суффикс пространства (PanelWindowId.h): «Assets» / «AssetsUI».
    std::string Id;
    // Подпись по-английски — переводится T() при показе.
    std::string Title;
    std::string Icon = "window";
    PanelSection Section = PanelSection::Tools;
    // Команда палитры (Ctrl+K), переключающая панель. Пусто — «panel.<id>».
    std::string Command;

    unsigned Spaces = panelspace::Both;   // в каких пространствах рисуется
    bool PerSpaceWindow = false;          // своё окно в каждом пространстве
    PanelPlace SceneDock;                 // место в раскладке пространства сцены
    PanelPlace InterfaceDock;             // ... и пространства интерфейса

    bool InLayout = false;       // часть постоянной раскладки: её возвращает «Сбросить раскладку»
    bool Detachable = true;      // есть в меню «Отдельным окном»
    // Обёртка panelwindows::Before/After (своё окно системы). Выключают те,
    // кто делает это сам или открывает окно с другим именем.
    bool WrapWindow = true;
    // Звать Draw и у закрытой панели: Tween по закрытию возвращает значения,
    // которые показывал просмотр.
    bool DrawWhenHidden = false;

    // Рисует панель. open — флаг видимости (крестик окна его сбрасывает),
    // windowId — имя окна в текущем пространстве.
    std::function<void(EditorHost& host, bool* open, const std::string& windowId)> Draw;

    // Где лежит флаг видимости. nullptr — его хранит реестр. Встроенные панели
    // указывают на EditorPanelVisibility, чтобы старый доступ по EditorPanel
    // (верхняя панель, самопроверка) видел то же самое.
    bool* Visible = nullptr;
    bool DefaultVisible = false;  // начальное значение флага, который хранит реестр
};

class PanelRegistry {
public:
    PanelRegistry();
    ~PanelRegistry();
    PanelRegistry(const PanelRegistry&) = delete;
    PanelRegistry& operator=(const PanelRegistry&) = delete;

    // Регистрирует панель (панель с тем же Id заменяется). Если передан
    // реестр команд — сразу добавляет команду «показать/скрыть».
    void Register(PanelSpec spec);
    bool Unregister(const std::string& id);
    const PanelSpec* Find(const std::string& id) const;
    // Все панели в порядке меню: по разделам, внутри — по порядку регистрации.
    std::vector<const PanelSpec*> All() const;
    size_t Count() const;

    bool* Visible(const std::string& id);
    bool IsVisible(const std::string& id) const;
    void SetVisible(const std::string& id, bool visible);
    void Toggle(const std::string& id);

    // Имя окна панели в пространстве (PerSpaceWindow — с суффиксом).
    static std::string WindowId(const PanelSpec& spec, EditorWorkspace workspace);
    static bool InSpace(const PanelSpec& spec, EditorWorkspace workspace);

    // Отрисовка всех панелей пространства (видимых; DrawWhenHidden — всех).
    void DrawPanels(EditorHost& host, EditorWorkspace workspace);
    // Пункты меню «Окно» одного раздела (переключатели видимости).
    void DrawMenuSection(PanelSection section);
    // Пункты меню «Отдельным окном».
    void DrawDetachMenu(EditorWorkspace workspace);
    // Раскладка по умолчанию: dock(windowId, slot) для каждой панели
    // пространства, в порядке Order. Сам DockBuilder — у вызывающего.
    void ForEachDocked(EditorWorkspace workspace,
                       const std::function<void(const std::string& windowId, PanelDock slot)>& dock) const;

    // «Сбросить раскладку»: открыть панели постоянной раскладки, ничего не закрывая.
    void Restore();
    // Есть ли на экране хоть одна панель (пустой док — повод подсказать).
    bool AnyVisible() const;
    void CloseAll();

    // Команды «показать/скрыть» для всех панелей, которые уже есть, и для тех,
    // что зарегистрируют потом (реестр запоминает, куда добавлять).
    void AttachCommands(Sage::UI::CommandRegistry& commands, std::string category);

private:
    struct Entry;
    void AddCommand(const Entry& e);
    Entry* FindEntry(const std::string& id);
    const Entry* FindEntry(const std::string& id) const;

    std::vector<std::unique_ptr<Entry>> m_entries;
    Sage::UI::CommandRegistry* m_commands = nullptr;
    std::string m_category;
    int m_nextOrder = 0;
};

} // namespace sage::editor
