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

#include <cstdio>
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
    EditorIcons::Inline(type ? type->Icon : "ui-empty");
    ImGui::SameLine(0.0f, EditorIcons::TextGap());
    ImGui::SetNextItemWidth(-90.0f);
    // Имя типа ПЕРЕВОДИТСЯ; само значение хранится английским ключом.
    //
    // В СПИСКЕ — ТОЛЬКО СВОЙ РОД. Элемент меняется на элемент, контейнер — на
    // контейнер: «сделать из кнопки столбец» значило бы снять с неё всё, что
    // делает её видимой, и это уже не смена типа, а другой объект.
    if (ImGui::BeginCombo(T("Type"), xf->Type.empty() ? T("Custom") : T(xf->Type.c_str()))) {
        std::string category;
        for (const ui::Preset& preset : ui::Presets()) {
            if (type && preset.Container != type->Container) continue;
            if (preset.Category != category) {
                category = preset.Category;
                ImGui::SeparatorText(T(category.c_str()));
            }
            // СМЕНА ТИПА ПЕРЕСОБИРАЕТ ЧАСТИ, но не трогает детей и место:
            // человек просил сменить тип, а не переставить элемент и не
            // потерять то, что внутри.
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
    if (!xf) return;
    type = ui::FindPreset(xf->Type);
    if (type && type->Hint && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(type->Hint));
    if (!type)
        HintWrapped("%s", T("An element built by code: its parts are shown as they are. Pick a type "
                            "above to rebuild it."));
    if (ctx.ShowIdentity) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
        if (ImGui::InputText(T("Name"), buf, sizeof(buf))) obj.SetName(buf);
        host.TrackLastImGuiItem();
        if (ImGui::Checkbox(T("Active"), &xf->Active)) host.PushUndoSnapshot();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Off — the element and everything inside are left out: not drawn, no room taken"));
    }

    // === ARRANGE: инструменты выделения — одной компактной строкой ==========
    if (ctx.Arrange) ctx.Arrange();

    // === PLACEMENT ==========================================================
    if (ictl::Section("placement", T("Placement"))) DrawLayoutSection(host, e, xf, selected);

    // === APPEARANCE: источник оформления ===================================
    auto belongs = [&](const ui::PartType& p) { return !type || TypeHasPart(*type, p.Id); };
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
    if (!isContainer || (type && AppearanceSwitchable(*type))) {
        if (ictl::Section("appearance", T("Appearance"))) {
            if (type && AppearanceSwitchable(*type)) DrawAppearanceSwitch(host, e);
            if (!isContainer) {
                DrawSkinSwitch(host, e, *xf);
                g_engineSkin = xf->Skin == ui::Element::SkinMode::Engine;
                DrawStyleRow(host, obj, ctx, *xf);
            }
        }
    }

    // === СВОЙСТВА ТИПА: то, ради чего элемент заводили ======================
    // Вид есть у всего, кроме невидимого Container: у Panel — её оформление.
    bool hasLook = false;
    for (const ui::PartType& p : ui::Parts())
        if (p.Fields && !p.Container && !p.Hidden && p.Has(reg, e) && belongs(p)) hasLook = true;
    if (hasLook) {
        const std::string typeTitle =
            xf->Type.empty() ? std::string(T("Look")) : std::string(T(xf->Type.c_str()));
        if (ictl::Section("type", typeTitle.c_str())) {
            // СОСТОЯНИЯ КНОПКИ — ВКЛАДКАМИ: «обычная», «при наведении»,
            // «при нажатии»... Каждая вкладка — вид в одном состоянии, а не
            // двадцать полей всех состояний подряд.
            const ui::PartType* act = ui::FindPart("interactable");
            void* actData = (act && act->Has(reg, e) && belongs(*act)) ? act->GetMutable(reg, e) : nullptr;
            const std::vector<const char*> tabs =
                actData ? StateTabs(host, obj, ctx, *act, actData) : std::vector<const char*>{};
            const ui::PartType* fillPart = ui::FindPart("fill");
            const bool fillInTabs = !tabs.empty() && fillPart && fillPart->Has(reg, e) && belongs(*fillPart);

            auto stateTabs = [&](bool withNormal) {
                if (!ImGui::BeginTabBar("##ui_states", ImGuiTabBarFlags_FittingPolicyScroll)) return;
                if (withNormal && ImGui::BeginTabItem(T("Normal"))) {
                    if (void* fd = fillPart->GetMutable(reg, e))
                        DrawPartSection(host, obj, ctx, *fillPart, fd, FieldView{}, selected);
                    ImGui::EndTabItem();
                }
                for (const char* tab : tabs) {
                    if (!ImGui::BeginTabItem(T(tab))) continue;
                    FieldView v;
                    v.SplitContent = true;
                    v.Tab = tab;
                    DrawPartSection(host, obj, ctx, *act, actData, v, selected);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            };

            bool statesDrawn = false;
            for (const ui::PartType& p : ui::Parts()) {
                if (!p.Fields || p.Container || p.Hidden || !p.Has(reg, e) || !belongs(p)) continue;
                if (act && &p == act) continue;   // реакция — вкладками и «Поведением»
                void* data = p.GetMutable(reg, e);
                if (!data) continue;
                if (fillInTabs && &p == fillPart) {
                    stateTabs(true);
                    statesDrawn = true;
                    continue;
                }
                ImGui::SeparatorText(T(p.Title));
                ImGui::PushID(p.Id);
                DrawPartSection(host, obj, ctx, p, data, FieldView{}, selected);
                if (std::string(p.Id) == "shape") DrawCustomShapeTools(host, e);
                ImGui::PopID();
            }
            // Без подложки (ползунок, галка) у состояний только подкраска —
            // она и показывается своими вкладками после вида.
            if (!statesDrawn && !tabs.empty()) {
                ImGui::SeparatorText(T("States"));
                stateTabs(false);
            }
        }
    }

    // === ARRANGEMENT И CONTENT контейнера ===================================
    if (isContainer && ictl::Section("content", T("Arrangement and content")))
        DrawContentSection(host, obj, ctx, selected);

    // === INTERACTION: только у тех, кто взаимодействует =====================
    const ui::PartType* interaction = ui::FindPart("interactable");
    if (interaction && interaction->Has(reg, e) && ictl::Section("interaction", T("Interaction"))) {
        if (void* data = interaction->GetMutable(reg, e)) {
            ImGui::PushID("behaviour");
            DrawPartFields(host, obj, ctx, *interaction, data, FieldFilter::Behaviour, true, selected);
            ImGui::PopID();
        }
    }

    // === ADVANCED: редкое, закрыто по умолчанию =============================
    if (ictl::Section("advanced", T("Advanced"), /*defaultOpen=*/false)) {
        DrawElementAdvanced(host, e, xf);
    // --- ЧАСТИ СВЕРХ ТИПА ------------------------------------------------------
    //
    // У элемента из старой сцены (или собранного кодом) бывают части, которых в
    // его типе нет: картинка на кнопке, раскладка на панели. Они работают и
    // показаны честно — здесь, отдельно от вида типа, с кнопкой «убрать».
    if (type) {
        std::vector<const ui::PartType*> extra;
        for (const ui::PartType& p : ui::Parts())
            if (p.Fields && !p.Hidden && p.Has(reg, e) && !TypeHasPart(*type, p.Id)) extra.push_back(&p);
        if (!extra.empty()) {
            ImGui::SeparatorText(T("Parts beyond the type"));
            HintWrapped("%s", T("These parts are not part of this type. They still work; remove them "
                                "to keep the element simple."));
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

    }

    // --- События: та же секция, что у любого объекта (SignalLinksEditor.h).
    if (ctx.ShowEvents && ictl::Section("events", T("Events"))) DrawSignalLinks(host, obj);
}

} // namespace sage::editor
