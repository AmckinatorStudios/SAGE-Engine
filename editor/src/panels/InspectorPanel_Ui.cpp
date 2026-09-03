// ---------------------------------------------------------------------------
// Инспектор — элементы интерфейса.
//
// Правка элемента интерфейса: якорь, растяжение, вид, поведение. Отдельно от
// свойств обычной сущности потому, что элемент UI — это НАБОР компонентов
// (раскладка, вид, поведение), и его инспектор устроен иначе, чем список
// компонентов трёхмерного объекта.
//
// Часть класса InspectorPanel: объявления остались в InspectorPanel.h, здесь
// только тела. Разбит потому, что дорос до двух тысяч строк, в которых рядом
// лежали редактор материала, якоря интерфейса и список компонентов.
// ---------------------------------------------------------------------------
#include <cstdarg>
#include "InspectorPanel.h"
#include "../UIElementProperties.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include <algorithm>

#include "AssetSlot.h"
#include "EditorIcons.h"
#include "ModelMaterialImport.h"
#include "Project.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/import/Importer.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/render/ParticlePresets.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "../Localization.h"

namespace fs = std::filesystem;

// Кнопка «Add Component» + попап со списком компонентов, которых у сущности ещё
// нет. Так добавление унифицировано (а удаление — кнопкой Remove в самой секции).
// Серое пояснение, КОТОРОЕ ПЕРЕНОСИТСЯ. Панель инспектора узкая (её ширину
// задаёт раскладка доккинга), а TextDisabled не переносит — поэтому пояснения
// обрезались по краю панели прямо посередине слова: «габарит 1.00 x 1.00 x 1.0(»,
// «задаём вид объекта целик». Пропадал ровно тот текст, ради которого их писали.
void InspectorPanel::HintWrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}


// Свойства элемента интерфейса переехали в общий модуль
// (UIElementProperties.h): их правят и отсюда, и из редактора интерфейса.
// Здесь остался вызов — вместе с тем состоянием панели, которое ему нужно.
void InspectorPanel::DrawUIElement(EditorHost& host, GameObject obj) {
    sage::editor::UIPropsContext ctx;
    ctx.Preview = &m_preview;
    ctx.Browser = &m_browser;
    ctx.BrowseTarget = &m_browseTarget;
    // Выбор пути из диалога применяется в InspectorPanel::Draw, и он должен
    // знать, что это НЕ меш, НЕ материал и НЕ шейдер, а картинка элемента.
    m_browseIsShader = false;
    m_browseIsMesh = false;
    m_browseIsMaterial = false;
    sage::editor::DrawUIElementProperties(host, obj, ctx);
}

bool InspectorPanel::DrawAnchorPicker(UIAnchor& anchor) {
    return sage::editor::DrawAnchorPicker(anchor);
}
