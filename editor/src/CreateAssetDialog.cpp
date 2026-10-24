#include "CreateAssetDialog.h"

#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorPrefs.h"
#include "FolderColors.h"
#include "Localization.h"
#include "Project.h"
#include "panels/AssetsPanel.h"
#include "sage/assets/AssetDatabase.h"

namespace fs = std::filesystem;

namespace sage::editor::createasset {

namespace {

struct State {
    bool Pending = false;   // попросили открыть: OpenPopup — в Draw, на уровне хоста
    bool Open = false;
    Request Req;
    char Name[128] = {0};
    Language Lang = Language::Lua;
    int Color = -1;         // номер цвета палитры; -1 — без цвета
    std::string Error;
};

State& S() {
    static State s;
    return s;
}

constexpr const char* kPopup = "###create_asset_dialog";

AssetsPanel::CreateKind KindFor(What kind, Language lang) {
    switch (kind) {
        case What::Folder: return AssetsPanel::CreateKind::Folder;
        case What::Scene: return AssetsPanel::CreateKind::Scene;
        case What::Material: return AssetsPanel::CreateKind::Material;
        case What::TextFile: return AssetsPanel::CreateKind::TextFile;
        case What::Script:
        default: return lang == Language::SageLua ? AssetsPanel::CreateKind::SageScript : AssetsPanel::CreateKind::Script;
    }
}

const char* TitleOf(What kind) {
    switch (kind) {
        case What::Folder: return T("New folder");
        case What::Scene: return T("New scene");
        case What::Material: return T("New material");
        case What::TextFile: return T("New text file");
        case What::Script:
        default: return T("New script");
    }
}

const char* IconOf(What kind) {
    switch (kind) {
        case What::Folder: return "folder-plus";
        case What::Scene: return "scene";
        case What::Material: return "material";
        case What::TextFile: return "file";
        case What::Script:
        default: return "script";
    }
}

// Выбор одного из двух — карточкой с объяснением: «Lua» и «SAGE Lua» по одному
// названию не различить, а разница в том, ЧТО проверяется до запуска.
bool Choice(const char* id, const char* title, const char* note, bool selected) {
    ImGui::PushID(id);
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::Selectable("##choice", selected, 0, ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = ImGui::GetStyle().FramePadding.x;
    dl->AddText(ImVec2(at.x + pad, at.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), title);
    dl->AddText(ImVec2(at.x + pad, at.y + ImGui::GetStyle().FramePadding.y + ImGui::GetTextLineHeightWithSpacing()),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), note);
    ImGui::PopID();
    return pressed;
}

} // namespace

void Open(const Request& request) {
    State& s = S();
    s.Req = request;
    s.Pending = true;
    s.Error.clear();
    s.Color = -1;
    std::snprintf(s.Name, sizeof(s.Name), "%s", request.Name.c_str());
    // Язык скрипта — привычка человека, а не свойство проекта: запоминается.
    s.Lang = sage::editor::prefs::GetString("create.script_language", "lua") == "slua" ? Language::SageLua
                                                                                         : Language::Lua;
}

bool IsOpen() { return S().Open || S().Pending; }

void Draw(EditorHost& host) {
    State& s = S();
    if (s.Pending) {
        ImGui::OpenPopup(kPopup);
        s.Pending = false;
        s.Open = true;
    }
    if (!s.Open) return;
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0.0f), ImGuiCond_Appearing);
    char title[128];
    std::snprintf(title, sizeof(title), "%s%s", TitleOf(s.Req.Kind), kPopup);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        s.Open = false;
        return;
    }

    fs::path dir = s.Req.Dir;
    if (dir.empty() && host.CurrentProject().Loaded())
        dir = DefaultDir(host.CurrentProject().AssetsDir(), s.Req.Kind);

    EditorIcons::Inline(IconOf(s.Req.Kind));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(TitleOf(s.Req.Kind));
    ImGui::Spacing();

    // ИМЯ — первым и сразу с курсором: окно открыли, чтобы назвать вещь.
    ImGui::TextDisabled("%s", T("Name"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    bool create = ImGui::InputText("##create_name", s.Name, sizeof(s.Name), ImGuiInputTextFlags_EnterReturnsTrue);

    if (s.Req.Kind == What::Script) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("Language"));
        if (Choice("lua", "Lua (.lua)", T("Plain Lua: quick to write, errors show up while the game runs"),
                   s.Lang == Language::Lua))
            s.Lang = Language::Lua;
        if (Choice("slua", "SAGE Lua (.slua)",
                   T("Lua with types and classes: mistakes are caught before the script runs"),
                   s.Lang == Language::SageLua))
            s.Lang = Language::SageLua;
    }

    if (s.Req.Kind == What::Folder) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("Colour"));
        const std::vector<sage::editor::foldercolors::Tint> palette = sage::editor::foldercolors::Palette();
        const float box = ImGui::GetFrameHeight();
        // «Без цвета» — первым и выбран по умолчанию: цвет — пометка, а не
        // обязанность.
        if (EditorIcons::IconOnlyButton("folder", T("No colour"), s.Color < 0)) s.Color = -1;
        for (int i = 0; i < (int)palette.size(); ++i) {
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::PushID(i);
            const glm::vec3 c = palette[(size_t)i].Color;
            const ImVec4 col(c.r, c.g, c.b, 1.0f);
            if (s.Color == i) {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_Text));
            }
            if (ImGui::ColorButton("##tint", col, ImGuiColorEditFlags_NoTooltip, ImVec2(box, box))) s.Color = i;
            if (s.Color == i) {
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", palette[(size_t)i].Label);
            ImGui::PopID();
        }
    }

    // Что получится и где — до нажатия: расширение и папку видно заранее.
    ImGui::Spacing();
    const std::string file = FileName(s.Req.Kind, s.Lang, s.Name[0] ? s.Name : "…");
    std::string where = dir.filename().string();
    if (host.CurrentProject().Loaded()) {
        std::error_code ec;
        const fs::path rel = fs::relative(dir, host.CurrentProject().Dir(), ec);
        if (!ec && !rel.empty()) where = rel.generic_string();
    }
    ImGui::TextDisabled(T("Will be created: %s/%s"), where.c_str(), file.c_str());
    if (!s.Error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", s.Error.c_str());

    ImGui::Spacing();
    create = EditorIcons::Button("plus", T("Create")) || create;
    ImGui::SameLine();
    const bool cancel = ImGui::Button(T("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape);

    if (create) {
        std::error_code mk;
        fs::create_directories(dir, mk);
        fs::path created;
        if (AssetsPanel::CreateAsset(KindFor(s.Req.Kind, s.Lang), s.Name, dir, created, s.Error)) {
            if (s.Req.Kind == What::Script)
                sage::editor::prefs::SetString("create.script_language", s.Lang == Language::SageLua ? "slua" : "lua");
            if (s.Req.Kind == What::Folder && s.Color >= 0) {
                const auto palette = sage::editor::foldercolors::Palette();
                if (s.Color < (int)palette.size())
                    sage::editor::foldercolors::Set(created, palette[(size_t)s.Color].Color);
            }
            // База ассетов — сразу: созданному нужен GUID (.meta), иначе
            // ссылка на него из слота держалась бы только за путь.
            if (host.CurrentProject().Loaded())
                sage::AssetDatabase::Instance().ScanProject(host.CurrentProject().Dir().string());
            if (s.Req.Done) s.Req.Done(created);
            host.SetStatusMessage(T("Created: ") + created.filename().string());
            s.Open = false;
            ImGui::CloseCurrentPopup();
        }
    } else if (cancel) {
        s.Open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace sage::editor::createasset
