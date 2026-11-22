// Пользовательские макеты редактора: сохранить текущую расстановку панелей
// под именем, переключаться между ними, переименовать, удалить.
//
// Макет — это снимок ТРЁХ вещей, потому что расстановка складывается из них:
//   1) ini ImGui в памяти: доки всех трёх пространств (Scene, Interface,
//      Animation), размеры, группы вкладок, активные вкладки, позиции окон;
//   2) видимость панелей (реестр панелей) — в ini её нет;
//   3) «отдельные окна системы» (panelwindows) — тоже не в ini.
// Файл последней сессии sage_editor_imgui.ini при этом не трогается: макеты
// лежат в настройках редактора и живут рядом с ним, а не вместо него.
#include "EditorLayer.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "EditorIcons.h"
#include "EditorPrefs.h"
#include "Localization.h"
#include "PanelWindows.h"
#include "imgui.h"
#include "ui/UI.h"

namespace {

constexpr const char* kListKey = "layouts.list";
constexpr const char* kActiveKey = "layouts.active";
constexpr size_t kNameMax = 63;

} // namespace

void EditorLayer::LoadLayouts() {
    m_layouts.clear();
    using json = nlohmann::json;
    // Испорченная запись — не повод не запуститься: макетов просто нет.
    try {
        const std::string raw = sage::editor::prefs::GetString(kListKey, "");
        const json list = raw.empty() ? json::array() : json::parse(raw);
        if (list.is_array()) {
            for (const json& j : list) {
                if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) continue;
                SavedLayout l;
                l.Name = j["name"].get<std::string>();
                l.Ini = j.value("ini", std::string());
                if (l.Name.empty() || l.Ini.empty()) continue;
                if (j.contains("visible") && j["visible"].is_object())
                    for (auto it = j["visible"].begin(); it != j["visible"].end(); ++it)
                        if (it.value().is_boolean()) l.Visible[it.key()] = it.value().get<bool>();
                if (j.contains("detached") && j["detached"].is_object())
                    for (auto it = j["detached"].begin(); it != j["detached"].end(); ++it)
                        if (it.value().is_boolean()) l.Detached[it.key()] = it.value().get<bool>();
                m_layouts.push_back(std::move(l));
            }
        }
    } catch (const std::exception&) {
        m_layouts.clear();
    }
    // Выбранный макет только ПОМНИТСЯ, а не применяется заново: состояние
    // последней сессии (ini, видимость, окна) уже восстановлено и содержит
    // правки поверх макета — перезапись макетом их бы стёрла.
    m_activeLayout = sage::editor::prefs::GetString(kActiveKey, "");
    const bool known = std::any_of(m_layouts.begin(), m_layouts.end(),
                                   [&](const SavedLayout& l) { return l.Name == m_activeLayout; });
    if (!known) m_activeLayout.clear();
}

void EditorLayer::PersistLayouts() {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& l : m_layouts) {
        nlohmann::json j;
        j["name"] = l.Name;
        j["ini"] = l.Ini;
        j["visible"] = l.Visible;
        j["detached"] = l.Detached;
        list.push_back(std::move(j));
    }
    sage::editor::prefs::SetString(kListKey, list.dump());
    sage::editor::prefs::SetString(kActiveKey, m_activeLayout);
}

void EditorLayer::SaveLayoutAs(const std::string& rawName) {
    std::string name = rawName.substr(0, kNameMax);
    // Пробелы по краям — не часть имени: «Моё » и «Моё» выглядели бы двумя
    // разными пунктами меню.
    const size_t b = name.find_first_not_of(" \t");
    const size_t e = name.find_last_not_of(" \t");
    name = b == std::string::npos ? std::string() : name.substr(b, e - b + 1);
    if (name.empty()) return;

    size_t len = 0;
    const char* ini = ImGui::SaveIniSettingsToMemory(&len);
    SavedLayout layout;
    layout.Name = name;
    layout.Ini.assign(ini ? ini : "", len);
    layout.Visible = m_panelRegistry.VisibilitySnapshot();
    layout.Detached = panelwindows::Snapshot();

    auto it = std::find_if(m_layouts.begin(), m_layouts.end(),
                           [&](const SavedLayout& l) { return l.Name == name; });
    if (it != m_layouts.end()) *it = std::move(layout);
    else m_layouts.push_back(std::move(layout));
    m_activeLayout = name;
    PersistLayouts();
}

void EditorLayer::OverwriteLayout(size_t index) {
    if (index >= m_layouts.size()) return;
    SaveLayoutAs(m_layouts[index].Name);
}

void EditorLayer::RequestDefaultLayout() {
    m_rebuildAllDocks = true;
    m_activeLayout.clear();
    sage::editor::prefs::SetString(kActiveKey, "");
}

void EditorLayer::ApplyPendingLayout() {
    if (m_pendingLayout < 0) return;
    const int index = m_pendingLayout;
    m_pendingLayout = -1;
    if (index >= (int)m_layouts.size()) return;
    const SavedLayout& l = m_layouts[(size_t)index];
    // Ini в память грузится ТОЛЬКО между кадрами: внутри кадра ImGui уже
    // держит указатели на окна и узлы дока, которые загрузка пересоздаёт.
    ImGui::LoadIniSettingsFromMemory(l.Ini.c_str(), l.Ini.size());
    m_panelRegistry.ApplyVisibility(l.Visible);
    panelwindows::Apply(l.Detached);
    m_rebuildDockLayout = false;
    m_activeLayout = l.Name;
    sage::editor::prefs::SetString(kActiveKey, m_activeLayout);
    // Файл сессии обновится сам — расстановка теперь именно эта.
    ImGui::GetIO().WantSaveIniSettings = true;
}

void EditorLayer::DrawLayoutsMenu() {
    if (!EditorIcons::BeginMenu("layout", T("Layouts"))) return;

    if (EditorIcons::MenuItemSelected("grid", T("Default"), m_activeLayout.empty()))
        RequestDefaultLayout();
    for (size_t i = 0; i < m_layouts.size(); ++i) {
        const bool current = m_layouts[i].Name == m_activeLayout;
        // ### — имя пункта может совпасть у двух макетов только если файл
        // правили руками; индекс в id спасает от общего ImGui-идентификатора.
        const std::string label = m_layouts[i].Name + "###layout" + std::to_string(i);
        if (EditorIcons::MenuItemSelected("layout", label.c_str(), current) && !current)
            m_pendingLayout = (int)i;
    }
    ImGui::Separator();

    const auto active = std::find_if(m_layouts.begin(), m_layouts.end(),
                                     [&](const SavedLayout& l) { return l.Name == m_activeLayout; });
    const bool hasActive = active != m_layouts.end();
    const int activeIndex = hasActive ? (int)(active - m_layouts.begin()) : -1;

    // OpenPopup из BeginMenu нельзя (другой стек ID): меню лишь ставит флаг.
    if (EditorIcons::MenuItem("plus", T("Save Current Layout..."))) {
        m_layoutDialog = LayoutDialogMode::SaveAs;
        m_layoutDialogTarget = -1;
        std::snprintf(m_layoutNameBuf, sizeof(m_layoutNameBuf), "%s", T("My Layout"));
        m_layoutDialogOpen = true;
    }
    if (EditorIcons::MenuItem("save", T("Save"), nullptr, hasActive)) OverwriteLayout((size_t)activeIndex);
    if (EditorIcons::MenuItem("pencil", T("Rename..."), nullptr, hasActive)) {
        m_layoutDialog = LayoutDialogMode::Rename;
        m_layoutDialogTarget = activeIndex;
        std::snprintf(m_layoutNameBuf, sizeof(m_layoutNameBuf), "%s", m_activeLayout.c_str());
        m_layoutDialogOpen = true;
    }
    if (EditorIcons::MenuItem("trash", T("Delete"), nullptr, hasActive)) {
        const std::string name = m_activeLayout;
        m_confirm.Ask("layout-delete", T("Deleting a layout"),
                      std::string(T("Delete the layout")) + " \"" + name + "\"?",
                      [this, name] {
                          m_layouts.erase(std::remove_if(m_layouts.begin(), m_layouts.end(),
                                              [&](const SavedLayout& l) { return l.Name == name; }),
                                          m_layouts.end());
                          // Текущая расстановка остаётся как есть: удаляется запись, а не экран.
                          if (m_activeLayout == name) m_activeLayout.clear();
                          PersistLayouts();
                      },
                      T("Delete"));
    }
    ImGui::EndMenu();
}

void EditorLayer::DrawLayoutDialog() {
    // Одно имя и для OpenPopup, и для BeginPopupModal (check_popup_ids).
    const char* const kId = T("Layout name" "###LayoutName");
    if (m_layoutDialogOpen) {
        ImGui::OpenPopup(kId);
        m_layoutDialogOpen = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kId, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(280.0f);
    const bool enter = ImGui::InputText("##layout_name", m_layoutNameBuf, sizeof(m_layoutNameBuf),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string typed = m_layoutNameBuf;
    const bool nonEmpty = typed.find_first_not_of(" \t") != std::string::npos;
    ImGui::Spacing();
    bool ok = false;
    ImGui::BeginDisabled(!nonEmpty);
    ok = ImGui::Button(T("OK"), ImVec2(130, 0)) || (enter && nonEmpty);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button(T("Cancel"), ImVec2(130, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);

    if (ok) {
        if (m_layoutDialog == LayoutDialogMode::SaveAs) {
            SaveLayoutAs(typed);
        } else if (m_layoutDialogTarget >= 0 && m_layoutDialogTarget < (int)m_layouts.size()) {
            SavedLayout& l = m_layouts[(size_t)m_layoutDialogTarget];
            std::string name = typed.substr(0, kNameMax);
            const size_t b = name.find_first_not_of(" \t");
            const size_t e = name.find_last_not_of(" \t");
            name = name.substr(b, e - b + 1);
            // Занятое имя — отказ, а не слияние: молча затереть чужой макет
            // переименованием нельзя.
            const bool taken = std::any_of(m_layouts.begin(), m_layouts.end(), [&](const SavedLayout& o) {
                return &o != &l && o.Name == name;
            });
            if (!taken) {
                if (m_activeLayout == l.Name) m_activeLayout = name;
                l.Name = name;
                PersistLayouts();
            }
        }
        m_layoutDialog = LayoutDialogMode::None;
        ImGui::CloseCurrentPopup();
    } else if (cancel) {
        m_layoutDialog = LayoutDialogMode::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
