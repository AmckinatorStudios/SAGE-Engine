// ---------------------------------------------------------------------------
// ИНСПЕКТОР ЭЛЕМЕНТА: порядок разделов.
//
// Частое — сверху, редкое — внизу: Identity (тип, имя, активность) → Arrange
// (инструменты выделения, компактно) → Placement → Appearance → свойства
// типа → Interaction → Advanced (закрыт) → Events. Разделы, которым нечего
// показать, не рисуются вовсе. Поля берутся из описания частей (UIPart.h) —
// второго списка свойств у инспектора нет.
// ---------------------------------------------------------------------------
#include "UIElementPropertiesDetail.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorTheme.h"
#include "InspectorControls.h"
#include "Localization.h"
#include "SignalLinksEditor.h"

#include "sage/scene/Components.h"
#include "sage/ui/UIButton.h"
#include "sage/ui/UISceneSystem.h"

namespace sage::editor {
using namespace uiprops_detail;

// Кнопка «открыть в режиме интерфейса» — одна на оба компонента.
//
// ЗАЧЕМ ОНА. Объект интерфейса и его элементы видны и в сцене, и найдя их
// там, человек идёт верстать: до сих пор это значило переключить пространство
// вручную, а потом ещё выбрать в списке нужный интерфейс из десятка похожих
// имён — при том, что объект уже был под рукой.
static void DrawOpenInInterfaceButton(EditorHost& host, GameObject obj) {
    if (host.Workspace() == EditorWorkspace::Interface) return;   // уже там
    // Элемент без интерфейса (его собрал код или скрипт) открывать некуда.
    if (sage::ui::InterfaceOf(host.CurrentScene(), obj.Entity()) == entt::null) return;
    if (EditorIcons::Button("ui-panel", T("Open in interface mode"),
                            T("Switch to layout and open this interface")))
        OpenInInterfaceMode(host, obj);
    ImGui::Separator();
}

void DrawUIElementProperties(EditorHost& host, GameObject obj,
                             const UIPropsContext& ctx) {
    entt::registry& reg = host.CurrentScene().Registry();
    const entt::entity e = obj.Entity();
    namespace ui = sage::ui;

    ui::Element* xf = reg.try_get<ui::Element>(e);
    if (!xf) return;
    g_engineSkin = xf->Skin == ui::Element::SkinMode::Engine;

    DrawOpenInInterfaceButton(host, obj);

    // СКОЛЬКО ВЫБРАНО — сказано сразу: инспектор показывает поля одного
    // элемента, а правит их у всех.
    const int selected = SelectedElements(host);
    if (selected > 1) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Accent));
        ImGui::Text(T("Selected: %d — an edit goes to all of them"), selected);
        ImGui::PopStyleColor();
    }

    // === IDENTITY: что это, как называется, включён ли ======================
    if (xf->Type.empty()) {
        const std::string guessed = ui::InferType(reg, e);
        if (!guessed.empty()) xf->Type = guessed;
    }
    const ui::Preset* type = ui::FindPreset(xf->Type);
    const bool isContainer = type && type->Container;

    // === ELEMENT: что это, как называется, включён ли =======================
    if (ictl::BeginSection("identity", T("Element"))) {
        EditorIcons::Inline(type ? type->Icon : "ui-empty");
        ImGui::SameLine(0.0f, EditorIcons::TextGap());
        ImGui::SetNextItemWidth(-1.0f);
        // Имя типа ПЕРЕВОДИТСЯ; само значение хранится английским ключом.
        // В СПИСКЕ — ТОЛЬКО СВОЙ РОД: элемент меняется на элемент, контейнер —
        // на контейнер.
        if (ImGui::BeginCombo("##Type", xf->Type.empty() ? T("Custom") : T(xf->Type.c_str()))) {
            std::string category;
            for (const ui::Preset& preset : ui::Presets()) {
                if (type && preset.Container != type->Container) continue;
                if (preset.Category != category) {
                    category = preset.Category;
                    ImGui::SeparatorText(T(category.c_str()));
                }
                // СМЕНА ТИПА ПЕРЕСОБИРАЕТ ЧАСТИ, но не трогает детей и место.
                if (ImGui::Selectable(T(preset.Name.c_str()), preset.Name == xf->Type)) {
                    host.PushUndoSnapshot();
                    const ui::Element keep = *xf;
                    ui::ApplyPreset(host.CurrentScene(), e, preset.Name, /*replaceChildren=*/false);
                    if (ui::Element* now = reg.try_get<ui::Element>(e)) {
                        const std::string newType = now->Type;
                        const glm::vec2 presetSize = now->Size;
                        *now = keep;
                        now->Type = newType;
                        if (now->Size.x <= 0.0f || now->Size.y <= 0.0f) now->Size = presetSize;
                        now->StyleVersion = -1;
                    }
                    xf = reg.try_get<ui::Element>(e);
                }
                if (preset.Hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(preset.Hint));
            }
            ImGui::EndCombo();
        }
        if (xf) {
            type = ui::FindPreset(xf->Type);
            if (type && type->Hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(type->Hint));
            if (!type)
                HintWrapped("%s", T("An element built by code: its parts are shown as they are. Pick a type "
                                    "above to rebuild it."));
            if (ctx.ShowIdentity) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
                ictl::Label(T("Name"));
                if (ImGui::InputText("##name", buf, sizeof(buf))) obj.SetName(buf);
                host.TrackLastImGuiItem();
                ictl::Switch(host, T("Active"), &xf->Active);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", T("Off — the element and everything inside are left out: not drawn, no room taken"));
            }
            // Инструменты выделения — компактной строкой под именем.
            if (ctx.Arrange) ctx.Arrange();
        }
        ictl::EndSection();
    }
    if (!xf) return;

    // === PLACEMENT ==========================================================
    if (ictl::BeginSection("placement", T("Placement"))) {
        DrawLayoutSection(host, e, xf, selected);
        ictl::EndSection();
    }

    // === APPEARANCE: источник оформления и вид ==============================
    //
    // Вид элемента — подложка (Fill) и форма (Shape): это его оформление, а
    // не назначение. Назначение (текст, значение, режим) — своим разделом ниже.
    auto belongs = [&](const ui::PartType& p) { return !type || TypeHasPart(*type, p.Id); };
    auto isLook = [](const ui::PartType& p) {
        return p.Id && (std::strcmp(p.Id, "fill") == 0 || std::strcmp(p.Id, "shape") == 0);
    };
    // Вид переключателя меняет ширину значка — подпись встаёт правее него.
    const ui::Range* toggle = reg.try_get<ui::Range>(e);
    const int variantBefore = toggle && toggle->Toggle ? (int)toggle->ToggleVariant : -1;
    struct ToggleWatch {
        EditorHost& Host;
        entt::entity E;
        int Before;
        ~ToggleWatch() {
            const ui::Range* r = Host.CurrentScene().Registry().try_get<ui::Range>(E);
            if (Before >= 0 && r && (int)r->ToggleVariant != Before) ui::PlaceToggleCaption(Host.CurrentScene(), E);
        }
    } toggleWatch{host, e, variantBefore};
    auto drawPart = [&](const ui::PartType& p) {
        void* data = p.GetMutable(reg, e);
        if (!data) return;
        ImGui::PushID(p.Id);
        DrawPartSection(host, obj, ctx, p, data, FieldView{}, selected);
        if (std::string(p.Id) == "shape") DrawCustomShapeTools(host, e);
        ImGui::PopID();
    };
    // Части типа (кроме вида и реакции на мышь): их поля раскладываются по
    // разделам из описания полей (PartField::Section), а не по частям.
    std::vector<const ui::PartType*> own;
    for (const ui::PartType& p : ui::Parts()) {
        if (!p.Fields || p.Container || p.Hidden || isLook(p) || !p.Has(reg, e) || !belongs(p)) continue;
        if (p.Id && std::strcmp(p.Id, "interactable") == 0) continue;   // — в Interaction
        own.push_back(&p);
    }
    // Часть, давшая типу имя (Text Input у поля ввода, Dropdown у списка), —
    // первой: в главном разделе её поля важнее полей надписи внутри.
    std::stable_partition(own.begin(), own.end(),
                          [&](const ui::PartType* p) { return p->Title && xf->Type == p->Title; });
    auto ownWith = [&](const char* section) {
        std::vector<const ui::PartType*> with;
        for (const ui::PartType* p : own) {
            void* data = p->GetMutable(reg, e);
            if (!data) continue;
            FieldView v;
            v.BySection = true;
            v.Section = section;
            v.Advanced = false;
            int n = DrawFieldsOf(host, obj, ctx, *p, data, v, selected, /*dryRun=*/true);
            v.Advanced = true;
            n += DrawFieldsOf(host, obj, ctx, *p, data, v, selected, /*dryRun=*/true);
            if (n > 0) with.push_back(p);
        }
        return with;
    };
    auto drawOwn = [&](const std::vector<const ui::PartType*>& with, const char* section, bool titled) {
        for (const ui::PartType* p : with) {
            // Несколько частей в одном разделе — подписью группы каждая.
            if (titled || with.size() > 1) ictl::GroupTitle(T(p->Title));
            FieldView v;
            v.BySection = true;
            v.Section = section;
            DrawPartSection(host, obj, ctx, *p, p->GetMutable(reg, e), v, selected);
        }
    };

    // APPEARANCE: источник оформления, подложка и форма, оформление частей
    // типа (заполнение шкалы, дорожка и ручка ползунка, цвет надписи).
    auto appearance = [&] {
        const std::vector<const ui::PartType*> looks = ownWith(ui::kSectionLook);
        if (isContainer && !(type && AppearanceSwitchable(*type)) && looks.empty()) return;
        if (!ictl::BeginSection("appearance", T("Appearance"))) return;
        if (!isContainer) {
            DrawAppearanceSource(host, obj, ctx, *xf);
            g_engineSkin = xf->Skin == ui::Element::SkinMode::Engine;
        }
        if (type && AppearanceSwitchable(*type)) DrawAppearanceSwitch(host, e);
        // ПОДПИСИ ГРУПП: если рядом с подложкой стоит цвет другой части
        // (текст поля ввода), без подписей это две строки «Colour» подряд.
        bool background = false;
        for (const ui::PartType& p : ui::Parts())
            background |= p.Fields && !p.Hidden && isLook(p) && p.Has(reg, e) && belongs(p);
        bool titled = false;
        for (const ui::PartType* p : looks)
            for (const ui::PartField& f : *p->Fields)
                titled |= background && !f.Hidden && f.Type == ui::PartField::Kind::Color && f.Section &&
                          std::strcmp(f.Section, ui::kSectionLook) == 0;
        const char* bgTitle = type && type->BackgroundTitle ? type->BackgroundTitle : "Background";
        for (const ui::PartType& p : ui::Parts()) {
            if (!p.Fields || p.Hidden || !isLook(p) || !p.Has(reg, e) || !belongs(p)) continue;
            const bool fill = std::strcmp(p.Id, "fill") == 0;
            if (fill && (titled || (type && type->BackgroundTitle && !g_engineSkin))) {
                ictl::GroupTitle(T(bgTitle));
            } else if (fill && type && type->BackgroundTitle) {
                // Оформление движка: у подложки один цвет — строкой с её ролью
                // («Track»), а не заголовком над строкой «Colour».
                static std::vector<ui::PartField> renamed;
                renamed = *p.Fields;
                for (ui::PartField& f : renamed)
                    if (f.Key && std::strcmp(f.Key, "color") == 0) f.Label = type->BackgroundTitle;
                // Постоянная копия: значения «по умолчанию» для сброса
                // запоминаются по адресу части (DefaultPartData).
                static ui::PartType copy;
                copy = p;
                copy.Fields = &renamed;
                drawPart(copy);
                continue;
            }
            drawPart(p);
        }
        drawOwn(looks, ui::kSectionLook, titled);
        ictl::EndSection();
    };
    // ГЛАВНЫЙ РАЗДЕЛ ТИПА: Progress, Range, Value, Content, Input, Options.
    auto mainSection = [&] {
        const std::vector<const ui::PartType*> with = ownWith(nullptr);
        if (with.empty()) return;
        const std::string title = type && type->Section ? std::string(T(type->Section))
                                  : xf->Type.empty()    ? std::string(T("Properties"))
                                                        : std::string(T(xf->Type.c_str()));
        if (!ictl::BeginSection("type", title.c_str())) return;
        drawOwn(with, nullptr, false);
        ictl::EndSection();
    };
    auto shared = [&](const char* id, const char* title) {
        const std::vector<const ui::PartType*> with = ownWith(id);
        if (with.empty() || !ictl::BeginSection(id, title)) return;
        drawOwn(with, id, false);
        ictl::EndSection();
    };
    // Порядок: у надписи и картинки содержимое и есть вид — оно первым;
    // у остальных сначала оформление, затем назначение.
    if (type && type->ContentFirst) {
        mainSection();
        shared(ui::kSectionTypography, T("Typography"));
        appearance();
    } else {
        appearance();
        mainSection();
        shared(ui::kSectionTypography, T("Typography"));
    }
    shared(ui::kSectionTransition, T("Transition"));

    // === ARRANGEMENT И CONTENT контейнера ===================================
    if (isContainer && ictl::BeginSection("content", T("Arrangement and content"))) {
        DrawContentSection(host, obj, ctx, selected);
        ictl::EndSection();
    }

    // === INTERACTION: только у тех, кто взаимодействует =====================
    //
    // Включено ли, курсор, навигация, звуки — и СОСТОЯНИЯ (наведение,
    // нажатие, фокус, недоступно) вкладками: это отклик на пользователя.
    const ui::PartType* interaction = ui::FindPart("interactable");
    if (interaction && interaction->Has(reg, e) && ictl::BeginSection("interaction", T("Interaction"))) {
        if (void* data = interaction->GetMutable(reg, e)) {
            ImGui::PushID("behaviour");
            DrawPartFields(host, obj, ctx, *interaction, data, FieldFilter::Behaviour, true, selected);
            const std::vector<const char*> tabs = StateTabs(host, obj, ctx, *interaction, data);
            if (!tabs.empty()) {
                ImGui::SeparatorText(T("States"));
                if (ImGui::BeginTabBar("##ui_states", ImGuiTabBarFlags_FittingPolicyScroll)) {
                    for (const char* tab : tabs) {
                        if (!ImGui::BeginTabItem(T(tab))) continue;
                        FieldView v;
                        v.SplitContent = true;
                        v.Tab = tab;
                        DrawPartSection(host, obj, ctx, *interaction, data, v, selected);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::PopID();
        }
        ictl::EndSection();
    }

    // === ADVANCED: редкое, закрыто по умолчанию =============================
    if (ictl::BeginSection("advanced", T("Advanced"), /*defaultOpen=*/false)) {
        DrawElementAdvanced(host, e, xf);
        // ЧАСТИ СВЕРХ ТИПА: у элемента из старой сцены (или собранного кодом)
        // бывают части, которых в его типе нет. Они работают и показаны
        // честно — здесь, с кнопкой «убрать».
        if (type) {
            std::vector<const ui::PartType*> extra;
            for (const ui::PartType& p : ui::Parts())
                if (p.Fields && !p.Hidden && p.Has(reg, e) && !TypeHasPart(*type, p.Id)) extra.push_back(&p);
            if (!extra.empty()) {
                ImGui::SeparatorText(T("Parts beyond the type"));
                for (const ui::PartType* p : extra) {
                    ImGui::PushID(p->Id);
                    ImGui::SeparatorText(T(p->Title));
                    if (ImGui::Button(T("Remove"))) {
                        host.PushUndoSnapshot();
                        p->Remove(reg, e);
                        ImGui::PopID();
                        break;
                    }
                    if (void* data = p->GetMutable(reg, e))
                        DrawPartFields(host, obj, ctx, *p, data, FieldFilter::Look, false, selected);
                    ImGui::PopID();
                }
            }
        }
        ictl::EndSection();
    }

    // --- События: та же секция, что у любого объекта (SignalLinksEditor.h).
    if (ctx.ShowEvents && ictl::BeginSection("events", T("Events"))) {
        DrawSignalLinks(host, obj);
        ictl::EndSection();
    }
}

} // namespace sage::editor
