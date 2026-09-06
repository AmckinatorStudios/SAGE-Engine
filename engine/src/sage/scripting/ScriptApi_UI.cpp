#include "ScriptEngine.h"

#include <algorithm>
#include <cmath>

#include "sage/core/Log.h"
#include "sage/ui/UIFramework.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/scene/UIScene.h"
#include "sage/ui/visual/UIIcon.h"
#include "sage/ui/visual/UIMaterial.h"

// ---------------------------------------------------------------------------
// ИНТЕРФЕЙС ДЛЯ СКРИПТОВ: sage.ui.*
//
// ЧТО ИЗМЕНИЛОСЬ. Раньше интерфейс был сущностями сцены, и скрипт правил его
// через объект: `obj:GetUI().Text = "..."`. Сущностей больше нет — интерфейс
// живёт в ДОКУМЕНТЕ, и скрипт работает с документом и его узлами.
//
// ГРАНИЦА ТА ЖЕ, ЧТО И ВЕЗДЕ (§103). Скрипт кладёт в интерфейс значения и
// слушает команды. Интерфейс не знает, что значит "menu.play", и не узнает:
// решает это игра.
//
//   local hud = sage.ui.Open("assets/ui/hud.uidoc")
//   hud:Set("Health/Fill.progress.Value", hp / maxHp)
//   hud:SetText("Ammo/Label", tostring(ammo))
//   sage.ui.OnCommand(function(cmd) if cmd == "menu.play" then StartGame() end end)
// ---------------------------------------------------------------------------

namespace {

namespace ui = sage::ui;

// Ссылка на документ для Lua. По ПУТИ, а не по указателю: документ мог быть
// перезагружен редактором, и указатель протух бы молча.
struct UIDocRef {
    std::string Path;

    ui::UIRuntime* Rt() const { return ui::UIDocuments::Instance().Find(Path); }
    ui::UIDocument* Doc() const {
        ui::UIRuntime* rt = Rt();
        return rt ? &rt->Doc() : nullptr;
    }
    ui::UINode* Node(const std::string& path) const {
        ui::UIDocument* d = Doc();
        if (!d) return nullptr;
        ui::UINode* n = d->FindByPath(path);
        return n ? n : d->FindByName(path);
    }
};

// Ссылка на узел. Номер плюс путь к документу: узел могли удалить, и держать
// указатель между кадрами нельзя.
struct UINodeRef {
    std::string Path;
    ui::UINodeId Id = ui::kUIInvalidNode;

    ui::UIDocument* Doc() const {
        ui::UIRuntime* rt = ui::UIDocuments::Instance().Find(Path);
        return rt ? &rt->Doc() : nullptr;
    }
    ui::UINode* Get() const {
        ui::UIDocument* d = Doc();
        return d ? d->Find(Id) : nullptr;
    }
    bool Alive() const { return Get() != nullptr; }
};

// Разбор адреса свойства и запись в него. Одна дорога для всех типов: скрипт
// пишет "Health/Fill.progress.Value", и больше ему знать ничего не надо.
bool SetProperty(ui::UIDocument& doc, const std::string& path, float value) {
    ui::UIPropertyBinding b;
    if (!b.Bind(doc, path)) return false;
    b.Set(value);
    return true;
}

bool SetPropertyText(ui::UIDocument& doc, const std::string& path, const std::string& value) {
    ui::UIPropertyBinding b;
    if (!b.Bind(doc, path)) return false;
    return b.SetString(value);
}

bool SetPropertyColor(ui::UIDocument& doc, const std::string& path, const glm::vec4& value) {
    ui::UIPropertyBinding b;
    if (!b.Bind(doc, path)) return false;
    return b.SetVec4(value);
}

// Узел по имени или пути; создание отсутствующего сюда НЕ входит намеренно:
// «положить значение в несуществующий узел» — это опечатка, и она должна быть
// видна, а не заводить молча пустой узел.
ui::UINode* Resolve(ui::UIDocument& doc, const std::string& path) {
    ui::UINode* n = doc.FindByPath(path);
    return n ? n : doc.FindByName(path);
}

} // namespace

void ScriptEngine::RegisterUIApi() {
    // --- Узел ---------------------------------------------------------------
    m_lua.new_usertype<UINodeRef>(
        "UINode",
        "Name",
        sol::property([](UINodeRef& r) { ui::UINode* n = r.Get(); return n ? n->Name : std::string(); },
                      [](UINodeRef& r, const std::string& v) { if (ui::UINode* n = r.Get()) n->Name = v; }),
        "Visible",
        sol::property([](UINodeRef& r) { ui::UINode* n = r.Get(); return n && n->Visible; },
                      [](UINodeRef& r, bool v) {
                          if (ui::UINode* n = r.Get()) {
                              n->Visible = v;
                              if (ui::UIDocument* d = r.Doc()) d->MarkDirty(ui::UIDirty_Visual);
                          }
                      }),
        "Enabled",
        sol::property([](UINodeRef& r) { ui::UINode* n = r.Get(); return n && n->Enabled; },
                      [](UINodeRef& r, bool v) {
                          if (ui::UINode* n = r.Get()) {
                              n->Enabled = v;
                              if (ui::UIDocument* d = r.Doc()) d->MarkDirty(ui::UIDirty_All);
                          }
                      }),
        "Opacity",
        sol::property([](UINodeRef& r) { ui::UINode* n = r.Get(); return n ? n->Opacity : 0.0f; },
                      [](UINodeRef& r, float v) {
                          if (ui::UINode* n = r.Get()) {
                              n->Opacity = v;
                              if (ui::UIDocument* d = r.Doc()) d->MarkDirty(ui::UIDirty_Visual);
                          }
                      }),
        "Layer",
        sol::property([](UINodeRef& r) { ui::UINode* n = r.Get(); return n ? n->Layer : 0; },
                      [](UINodeRef& r, int v) {
                          if (ui::UINode* n = r.Get()) {
                              n->Layer = v;
                              if (ui::UIDocument* d = r.Doc()) d->MarkDirty(ui::UIDirty_Hierarchy);
                          }
                      }),
        "Order",
        sol::property([](UINodeRef& r) { ui::UINode* n = r.Get(); return n ? n->Order : 0; },
                      [](UINodeRef& r, int v) {
                          if (ui::UINode* n = r.Get()) {
                              n->Order = v;
                              if (ui::UIDocument* d = r.Doc()) d->MarkDirty(ui::UIDirty_Hierarchy);
                          }
                      }),
        // Текст, значение и цвет — то, что скрипт меняет чаще всего. Отдельными
        // методами, а не свойствами: у узла может не быть ни надписи, ни шкалы,
        // и молча заводить их при записи — значит скрывать опечатку в адресе.
        "SetText",
        [](UINodeRef& r, const std::string& text) {
            ui::UINode* n = r.Get();
            if (!n) return false;
            ui::UIText* t = n->Get<ui::UIText>();
            if (!t) return false;
            t->Text = text;
            t->Key.clear();
            if (ui::UIDocument* d = r.Doc()) d->MarkDirty(ui::UIDirty_Text | ui::UIDirty_Layout);
            return true;
        },
        "GetText",
        [](UINodeRef& r) {
            ui::UINode* n = r.Get();
            const ui::UIText* t = n ? n->Get<ui::UIText>() : nullptr;
            return t ? t->Text : std::string();
        },
        "SetValue",
        [](UINodeRef& r, float v) {
            ui::UINode* n = r.Get();
            if (!n) return false;
            if (ui::UIProgress* p = n->Get<ui::UIProgress>()) { p->Value = v; }
            else if (ui::UIRangeValue* rv = n->Get<ui::UIRangeValue>()) { rv->Value = v; }
            else return false;
            if (ui::UIDocument* d = r.Doc()) d->MarkDirty(ui::UIDirty_Visual);
            return true;
        },
        "GetValue",
        [](UINodeRef& r) {
            ui::UINode* n = r.Get();
            if (!n) return 0.0f;
            if (const ui::UIProgress* p = n->Get<ui::UIProgress>()) return p->Value;
            if (const ui::UIRangeValue* rv = n->Get<ui::UIRangeValue>()) return rv->Value;
            return 0.0f;
        },
        "SetColor",
        [](UINodeRef& r, glm::vec4 c) {
            ui::UINode* n = r.Get();
            if (!n) return false;
            if (ui::UIFill* f = n->Get<ui::UIFill>()) f->Color = c;
            else if (ui::UIText* t = n->Get<ui::UIText>()) t->Color = c;
            else if (ui::UIImage* im = n->Get<ui::UIImage>()) im->Tint = c;
            else if (ui::UIShape* sh = n->Get<ui::UIShape>()) sh->Color = c;
            else return false;
            if (ui::UIDocument* d = r.Doc()) d->MarkDirty(ui::UIDirty_Visual);
            return true;
        },
        "SetImage",
        [](UINodeRef& r, const std::string& path) {
            ui::UINode* n = r.Get();
            if (!n) return false;
            ui::UIImage& im = n->Ensure<ui::UIImage>();
            im.Path = path;
            im.Resolved = nullptr; // путь сменился — прежняя текстура не та
            if (ui::UIDocument* d = r.Doc()) d->MarkDirty(ui::UIDirty_Visual);
            return true;
        },
        // Состояние взаимодействия: нажали ли в этом кадре.
        "Clicked",
        [](UINodeRef& r) {
            ui::UINode* n = r.Get();
            const ui::UIInteraction* ia = n ? n->Get<ui::UIInteraction>() : nullptr;
            return ia && ia->Runtime.Clicked;
        },
        "Hovered",
        [](UINodeRef& r) {
            ui::UINode* n = r.Get();
            const ui::UIInteraction* ia = n ? n->Get<ui::UIInteraction>() : nullptr;
            return ia && ia->Is(ui::UIState_Hovered);
        },
        "Valid", [](UINodeRef& r) { return r.Alive(); });

    // --- Документ -----------------------------------------------------------
    m_lua.new_usertype<UIDocRef>(
        "UIDocument",
        // Отсутствующий узел — nil, а не пустая ссылка: «положил значение в
        // никуда» должно быть видно в скрипте сразу, а не через кадр.
        "Find",
        [](UIDocRef& r, const std::string& path) -> sol::optional<UINodeRef> {
            ui::UINode* n = r.Node(path);
            if (!n) return sol::nullopt;
            return UINodeRef{r.Path, n->Id};
        },
        // Значение по адресу свойства: "Health/Fill.progress.Value".
        "Set",
        [](UIDocRef& r, const std::string& path, float value) {
            ui::UIDocument* d = r.Doc();
            return d && SetProperty(*d, path, value);
        },
        "SetText",
        [](UIDocRef& r, const std::string& node, const std::string& text) {
            ui::UIDocument* d = r.Doc();
            if (!d) return false;
            ui::UINode* n = Resolve(*d, node);
            ui::UIText* t = n ? n->Get<ui::UIText>() : nullptr;
            if (!t) return false;
            t->Text = text;
            t->Key.clear();
            d->MarkDirty(ui::UIDirty_Text | ui::UIDirty_Layout);
            return true;
        },
        "SetString",
        [](UIDocRef& r, const std::string& path, const std::string& value) {
            ui::UIDocument* d = r.Doc();
            return d && SetPropertyText(*d, path, value);
        },
        "SetColor",
        [](UIDocRef& r, const std::string& path, glm::vec4 value) {
            ui::UIDocument* d = r.Doc();
            return d && SetPropertyColor(*d, path, value);
        },
        "Get",
        [](UIDocRef& r, const std::string& path) {
            ui::UIDocument* d = r.Doc();
            if (!d) return 0.0f;
            ui::UIPropertyBinding b;
            if (!b.Bind(*d, path)) return 0.0f;
            float v = 0.0f;
            b.Get(v);
            return v;
        },
        "Show",
        [](UIDocRef& r, const std::string& node, bool visible) {
            ui::UIDocument* d = r.Doc();
            if (!d) return false;
            ui::UINode* n = Resolve(*d, node);
            if (!n) return false;
            n->Visible = visible;
            d->MarkDirty(ui::UIDirty_Visual);
            return true;
        },
        // Создать узел или готовый виджет из реестра — интерфейс, известный
        // только в рантайме (список предметов, строки чата), собирается так.
        "Create",
        [](UIDocRef& r, const std::string& parent, const std::string& name)
            -> sol::optional<UINodeRef> {
            ui::UIDocument* d = r.Doc();
            if (!d) return sol::nullopt;
            ui::UINode* p = parent.empty() ? nullptr : Resolve(*d, parent);
            ui::UINode* n = d->Create(name, p ? p->Id : ui::kUIInvalidNode);
            if (!n) return sol::nullopt;
            return UINodeRef{r.Path, n->Id};
        },
        "Widget",
        [](UIDocRef& r, const std::string& widget, const std::string& parent)
            -> sol::optional<UINodeRef> {
            ui::UIDocument* d = r.Doc();
            if (!d) return sol::nullopt;
            ui::UINode* p = parent.empty() ? nullptr : Resolve(*d, parent);
            const ui::UINodeId id = d->CreateWidget(widget, p ? p->Id : ui::kUIInvalidNode);
            if (id == ui::kUIInvalidNode) {
                // Имени нет в реестре — это опечатка, и она должна быть видна.
                LOG_WARN("Script") << "ui: неизвестный виджет \"" << widget << "\"";
                return sol::nullopt;
            }
            return UINodeRef{r.Path, id};
        },
        "Destroy",
        [](UIDocRef& r, const std::string& node) {
            ui::UIDocument* d = r.Doc();
            if (!d) return false;
            ui::UINode* n = Resolve(*d, node);
            if (!n) return false;
            d->Destroy(n->Id);
            return true;
        },
        "Reload",
        [](UIDocRef& r) { return ui::UIDocuments::Instance().Reload(r.Path); },
        "Path", sol::readonly(&UIDocRef::Path),
        "Valid", [](UIDocRef& r) { return r.Rt() != nullptr; });

    // --- sage.ui.* ----------------------------------------------------------
    Bind("ui", "Open", "OpenUIDocument", [](const std::string& path) {
        ui::UIDocuments::Instance().GetOrLoad(path);
        return UIDocRef{path};
    });
    Bind("ui", "Loaded", nullptr, [](const std::string& path) {
        return ui::UIDocuments::Instance().Loaded(path);
    });
    Bind("ui", "Reload", nullptr, [](const std::string& path) {
        return ui::UIDocuments::Instance().Reload(path);
    });

    // Команды интерфейса за этот кадр. Интерфейс их СООБЩАЕТ; что они значат,
    // решает игра — здесь и сейчас, своим кодом (§102, §103).
    Bind("ui", "Commands", nullptr, [this]() {
        sol::table t = m_lua.create_table();
        int i = 1;
        for (const std::string& cmd : m_uiCommands) t[i++] = cmd;
        return t;
    });
    Bind("ui", "Pressed", nullptr, [this](const std::string& command) {
        return std::find(m_uiCommands.begin(), m_uiCommands.end(), command) != m_uiCommands.end();
    });

    // Векторные значки движка: список имён — чтобы скрипт мог проверить, что
    // значок существует, а не рисовать заглушку молча.
    Bind("ui", "HasIcon", nullptr, [](const std::string& name) { return sage::ui::HasIcon(name); });
}
