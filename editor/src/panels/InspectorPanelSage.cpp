#include "InspectorPanelSage.h"

#include <algorithm>
#include <functional>

#include "EditorHost.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "ui/PropertyRows.h"
#include "../Localization.h"

namespace sui = sage::ui::sui;
using sage::scene::ComponentRegistry;
using sage::scene::ComponentType;

// ============================================================================
//  Сборка
// ============================================================================

void InspectorPanelSage::Build(sui::UIContext& ui, sui::UIElement* root) {
    m_ui = &ui;
    root->Vertical(0.0f);

    // --- Шапка: имя и номер объекта ---
    //
    // Не «ещё одна строка свойств»: имя и id отвечают на вопрос «на что я
    // смотрю», и они обязаны стоять выше всего остального и не уезжать при
    // прокрутке длинного инспектора.
    m_header = ui.CreateIn<sui::UIElement>(root);
    m_header->SetName("Header");
    m_header->Vertical(2.0f);
    m_header->SetStretch(true, false);
    // Высота с запасом под поле ввода темы (28) и подпись под ним: «по
    // содержимому» здесь не годится — вложенный ряд считается позже, и подпись
    // «Id» получала нулевую высоту, то есть исчезала.
    m_header->SetHeight(64.0f);
    m_header->Padding(sage::ui::UIEdges(8.0f, 6.0f, 8.0f, 6.0f));

    sui::UIElement* nameRow = ui.CreateIn<sui::UIElement>(m_header);
    nameRow->Horizontal(6.0f);
    nameRow->SetStretch(true, false);
    nameRow->SetHeight(28.0f);   // высота поля ввода в теме
    m_name = ui.CreateIn<sui::TextInput>(nameRow, std::string(), std::string(T("Name")));
    m_name->SetStretch(true, false);
    m_name->OnChanged([this](const std::string& text) {
        if (!m_host) return;
        GameObject obj = m_host->SelectedObject();
        if (obj.Valid() && obj.Name() != text) {
            // Точка отмены на каждую нажатую клавишу — это сорок снимков на
            // одно имя. Снимок берётся Capture/Commit: «до» запоминается один
            // раз, в историю уходит по факту правки.
            m_host->CapturePendingSnapshot();
            obj.SetName(text);
            m_host->CommitPendingSnapshot();
        }
    });

    m_id = ui.CreateIn<sui::Label>(m_header, std::string());
    m_id->SetStyle("Caption");
    m_id->SetStretch(true, false);
    m_id->SetHeight(18.0f);

    // --- Таблица компонентов ---
    m_grid = ui.CreateIn<sui::PropertyGrid>(root);
    m_grid->SetStretch(true, true);
    // Колонка подписей шире стандартной: у компонентов движка есть поля вроде
    // «Emissive Strength», и в 88 точек они обрубаются на середине слова.
    m_grid->SetLabelWidth(104.0f);

    // Подсказка «ничего не выбрано». Пустая панель без объяснения читается как
    // сломанная: человек не знает, выбрать ему объект или чинить редактор.
    m_empty = ui.CreateIn<sui::Label>(root, std::string(T("Select an object")));
    m_empty->SetStyle("Caption");
    m_empty->SetStretch(true, false);
    m_empty->SetHeight(24.0f);
    m_empty->Padding(sage::ui::UIEdges(10.0f, 4.0f, 8.0f, 4.0f));

    // --- Добавить компонент ---
    sui::UIElement* footer = ui.CreateIn<sui::UIElement>(root);
    footer->SetName("Footer");
    footer->Horizontal(0.0f);
    footer->SetStretch(true, false);
    footer->SetHeight(32.0f);
    footer->Padding(sage::ui::UIEdges(8.0f, 4.0f, 8.0f, 6.0f));
    m_add = ui.CreateIn<sui::Button>(footer, std::string(T("Add Component")));
    m_add->SetStretch(true, false);
    m_add->OnClick([this] {
        if (!m_addMenu) return;
        BuildAddMenu();
        m_addMenu->OpenUnder(m_add);
    });

    // Меню собирается ЗАНОВО на каждое открытие: список того, чего у объекта
    // ещё нет, меняется вместе с объектом.
    m_addMenu = ui.Create<sui::Popup>();

    Rebuild();
}

void InspectorPanelSage::BuildAddMenu() {
    if (!m_addMenu || !m_host || !m_ui) return;
    // Пункты пересоздаются: держать в меню компонент, который уже добавлен, —
    // значит предлагать действие, которое ничего не сделает.
    const std::vector<sui::UIElement*> old = m_addMenu->Children();
    for (sui::UIElement* child : old) child->RemoveFromParent();

    GameObject obj = m_host->SelectedObject();
    if (!obj.Valid()) return;
    entt::registry& reg = m_host->CurrentScene().Registry();

    for (const ComponentType& type : ComponentRegistry::Instance().Types()) {
        if (type.Essential) continue;                     // Transform не добавляют
        if (type.Has && type.Has(reg, obj.Entity())) continue;
        const ComponentType* ptr = &type;
        m_addMenu->AddItem(T(type.Label), [this, ptr] {
            if (!m_host) return;
            GameObject target = m_host->SelectedObject();
            if (!target.Valid()) return;
            m_host->PushUndoSnapshot();   // он же помечает сцену изменённой
            ptr->Add(m_host->CurrentScene().Registry(), target.Entity());
            m_stamp = 0;
        });
    }
}

void InspectorPanelSage::BuildComponent(const ComponentType& type) {
    if (!m_ui || !m_grid || !m_host) return;
    sui::UIElement* section = m_grid->AddSection(T(type.Label), true);
    m_sections.push_back(type.Id);

    // Кнопка «убрать» — в шапке секции, справа. У обязательного компонента её
    // нет вовсе: объект без положения — не объект, и предлагать это действие
    // значит предлагать сломать сцену.
    if (!type.Essential) {
        if (sui::UIElement* head = m_grid->SectionHead(section)) {
            const ComponentType* ptr = &type;
            sui::IconButton* remove = m_ui->CreateIn<sui::IconButton>(
                head, sui::Icon::Delete, std::string(T("Remove the component")));
            remove->SetSize({18.0f, 18.0f});
            remove->OnPress([this, ptr] {
                if (!m_host) return;
                GameObject obj = m_host->SelectedObject();
                if (!obj.Valid()) return;
                m_host->PushUndoSnapshot();
                ptr->Remove(m_host->CurrentScene().Registry(), obj.Entity());
                m_stamp = 0;
            });
        }
    }

    // Точка отмены — ОДНА на серию правок подряд. Иначе перетаскивание
    // ползунка кладёт в историю сорок снимков, и «отменить» откатывает на
    // полпикселя.
    editor::PropertyHooks hooks;
    hooks.BeginEdit = [this] {
        if (m_host) m_host->CapturePendingSnapshot();
    };
    hooks.Changed = [this] {
        if (m_host) m_host->CommitPendingSnapshot();
    };

    // Подгруппы внутри компонента: поля с одинаковым Group идут своей секцией.
    // Компонент из десяти полей без подгрупп нечитаем, а дробить его на
    // компоненты ради вида — врать про модель данных.
    sui::UIElement* current = section;
    const char* currentGroup = nullptr;
    for (int i = 0; i < type.PropCount; ++i) {
        const sage::scene::Property& prop = type.Props[i];
        const bool sameGroup = (prop.Group == nullptr && currentGroup == nullptr) ||
                               (prop.Group && currentGroup &&
                                std::string(prop.Group) == currentGroup);
        if (!sameGroup) {
            currentGroup = prop.Group;
            current = prop.Group ? m_grid->AddSection(T(prop.Group), true, /*depth=*/1) : section;
        }
        // Адрес компонента спрашивается ЗАНОВО на каждое обращение: удаление
        // компонента у другой сущности переставляет оставшиеся в пуле ECS, и
        // сохранённый указатель после этого показывает на чужие байты.
        const ComponentType* ptr = &type;
        editor::DataFn data = [this, ptr]() -> void* {
            if (!m_host) return nullptr;
            GameObject obj = m_host->SelectedObject();
            if (!obj.Valid()) return nullptr;
            entt::registry& reg = m_host->CurrentScene().Registry();
            if (!ptr->Has(reg, obj.Entity())) return nullptr;
            return ptr->Data(reg, obj.Entity());
        };
        editor::BuildPropertyRow(*m_ui, m_grid, current, prop, data, hooks);
    }
}

// ============================================================================
//  Обновление
// ============================================================================

void InspectorPanelSage::Rebuild() {
    if (!m_host || !m_grid) return;
    m_grid->Clear();
    m_sections.clear();

    GameObject obj = m_host->SelectedObject();
    const bool has = obj.Valid();
    if (m_header) m_header->SetVisible(has);
    if (m_empty) m_empty->SetVisible(!has);
    if (m_add) m_add->SetVisible(has);
    if (!has) {
        m_shownId = 0;
        return;
    }

    m_shownId = obj.Id();
    if (m_name) m_name->SetValue(obj.Name());
    if (m_id) m_id->SetText(std::string(T("Id")) + ": " + std::to_string(obj.Id()));

    entt::registry& reg = m_host->CurrentScene().Registry();
    for (const ComponentType* type : ComponentRegistry::Instance().Of(reg, obj.Entity()))
        BuildComponent(*type);
}

void InspectorPanelSage::Sync(float dt) {
    (void)dt;
    if (!m_host || !m_grid) return;

    // Слепок ловит ровно то, от чего зависит СОСТАВ панели: кто выбран и какие
    // компоненты у него есть. Значения полей в слепок не входят намеренно —
    // они правятся прямо в компоненте, и пересобирать панель на каждое движение
    // ползунка значило бы терять фокус в поле под курсором.
    GameObject obj = m_host->SelectedObject();
    std::size_t stamp = 1469598103934665603ull;
    auto mix = [&stamp](std::size_t v) {
        stamp ^= v + 0x9e3779b9ull + (stamp << 6) + (stamp >> 2);
    };
    if (obj.Valid()) {
        mix((std::size_t)obj.Id());
        entt::registry& reg = m_host->CurrentScene().Registry();
        for (const ComponentType* t : ComponentRegistry::Instance().Of(reg, obj.Entity()))
            mix(std::hash<std::string>{}(t->Id));
    }
    if (stamp == m_stamp) {
        // Имя правит и иерархия, и сам инспектор. Обновляем поле, только пока
        // в нём не печатают: иначе каретка прыгала бы на каждый кадр.
        if (obj.Valid() && m_name && m_name->Value() != obj.Name() &&
            !m_name->Get<sage::ui::UIInteraction>()->Is(sage::ui::UIState_Focused))
            m_name->SetValue(obj.Name());
        return;
    }
    m_stamp = stamp;
    Rebuild();
}

void InspectorPanelSage::OnAssetDropped(const std::string& path, glm::vec2 at) {
    (void)at;
    if (!m_host || path.empty()) return;
    GameObject obj = m_host->SelectedObject();
    if (!obj.Valid()) return;
    // Точка броска здесь не важна: инспектор целиком принадлежит одному
    // объекту, и «куда именно попали» ничего не уточняет.
    if (!m_host->ApplyAssetToEntity(obj.Id(), path))
        m_host->SetStatusMessage(T("This file cannot be applied to an object"));
    m_stamp = 0;
}

std::string InspectorPanelSage::SectionNames() const {
    std::string out;
    for (const std::string& s : m_sections) {
        if (!out.empty()) out += ",";
        out += s;
    }
    return out;
}
