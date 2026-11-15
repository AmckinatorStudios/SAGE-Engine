#pragma once
// ---------------------------------------------------------------------------
// Помощники инспектора элемента интерфейса — общие для его файлов:
// UIElementProperties.cpp (поля частей), UIElementProperties_Inspector.cpp
// (порядок разделов), UIElementProperties_Layout.cpp (Placement, Advanced).
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

#include <entt/entt.hpp>

#include "UIElementProperties.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UIPresets.h"

namespace sage::editor::uiprops_detail {

namespace ui = sage::ui;

// Какие поля части рисовать: у реакции на мышь поля ВИДА (виды состояний)
// стоят среди оформления, а поля ПОВЕДЕНИЯ (действие, курсор) — своим
// разделом.
enum class FieldFilter { Look, Behaviour };

// КАКИЕ ПОЛЯ ПОКАЗАТЬ ЗА ОДИН ПРОХОД: основные или редкие, поля вида или
// поведения, поля одной вкладки состояния.
struct FieldView {
    FieldFilter Filter = FieldFilter::Look;
    bool SplitContent = false;   // у реакции на мышь поведение — отдельным разделом
    bool Advanced = false;       // false — основные поля, true — редкие
    const char* Tab = nullptr;   // nullptr — поля без вкладки; иначе только эта вкладка
};

// Оформление элемента, чьи поля сейчас рисуются (движка — поля «только для
// своего» скрыты).
extern bool g_engineSkin;

void HintWrapped(const char* fmt, ...);
// Данные части по умолчанию (для сброса поля к исходному) и подпись поля без
// «(x,y,w,h)» — оси подписаны у самих полей.
const void* DefaultPartData(const ui::PartType& p);
std::string DisplayLabel(const char* label);
int SelectedElements(EditorHost& host);
int DrawFieldsOf(EditorHost& host, GameObject obj, const UIPropsContext& ctx, const ui::PartType& p, void* data,
                 const FieldView& view, int selected, bool dryRun = false);
void DrawPartSection(EditorHost& host, GameObject obj, const UIPropsContext& ctx, const ui::PartType& p,
                     void* data, FieldView view, int selected);
void DrawPartFields(EditorHost& host, GameObject obj, const UIPropsContext& ctx, const ui::PartType& p,
                    void* data, FieldFilter filter, bool splitContent, int selected);
std::vector<const char*> StateTabs(EditorHost& host, GameObject obj, const UIPropsContext& ctx,
                                   const ui::PartType& act, void* actData);
bool AppearanceSwitchable(const ui::Preset& t);
bool TypeHasPart(const ui::Preset& t, const char* id);
void DrawAppearanceSwitch(EditorHost& host, entt::entity e);
void DrawCustomShapeTools(EditorHost& host, entt::entity e);
void DrawContentSection(EditorHost& host, GameObject obj, const UIPropsContext& ctx, int selected);

// UIElementProperties_Layout.cpp.
void DrawLayoutSection(EditorHost& host, entt::entity e, ui::Element* xf, int selected);
void DrawElementAdvanced(EditorHost& host, entt::entity e, ui::Element* xf);
void DrawStyleRow(EditorHost& host, GameObject obj, const UIPropsContext& ctx, ui::Element& xf);
void DrawGroupSettings(EditorHost& host, entt::entity e);
void DrawSkinSwitch(EditorHost& host, entt::entity e, ui::Element& xf);

} // namespace sage::editor::uiprops_detail
