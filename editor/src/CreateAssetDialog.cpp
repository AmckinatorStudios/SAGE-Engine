#include "CreateAssetDialog.h"

#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorIcons.h"
#include "EditorPrefs.h"
#include "FolderColors.h"
#include "LogicKinds.h"
#include "sage/core/Paths.h"
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
    std::string Logic = "lua";   // id вида логики (LogicKinds.h)
    int Color = -1;         // номер цвета палитры; -1 — без цвета
    std::string Error;
    bool CloseRequested = false;   // закрыть снаружи (самопроверка)
    float Width = 0.0f;            // ширина окна в прошлом кадре
};

State& S() {
    static State s;
    return s;
}

constexpr const char* kPopup = "###create_asset_dialog";

AssetsPanel::CreateKind KindFor(What kind) {
    switch (kind) {
        case What::Folder: return AssetsPanel::CreateKind::Folder;
        case What::Scene: return AssetsPanel::CreateKind::Scene;
        case What::Material: return AssetsPanel::CreateKind::Material;
        case What::TextFile: return AssetsPanel::CreateKind::TextFile;
        default: return AssetsPanel::CreateKind::None;   // логика — через свой вид
    }
}

// Выбранный вид логики — из тех, что предлагаются этому запросу; запомненный
// выбор не годится слоту (слоту Script не предложить граф) — первый из своих.
const logic::Kind* LogicOf(const State& s) {
    const std::vector<const logic::Kind*> offered = logic::ForSlot(s.Req.Slot);
    for (const logic::Kind* k : offered)
        if (k->Id == s.Logic) return k;
    return offered.empty() ? nullptr : offered.front();
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

// Выбор вида — карточкой с объяснением: «Lua» и «SAGE Lua» по одному
// названию не различить, а разница в том, ЧТО проверяется до запуска.
//
// NoAutoClosePopups ОБЯЗАТЕЛЕН: Selectable прямо в модальном окне по
// умолчанию закрывает это окно. Щелчок по языку не выбирал его, а захлопывал
// «Новый скрипт» — выбрать язык было нельзя вовсе.
bool Choice(const char* id, const char* title, const char* note, bool selected) {
    ImGui::PushID(id);
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool pressed =
        ImGui::Selectable("##choice", selected, ImGuiSelectableFlags_NoAutoClosePopups, ImVec2(w, h));
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
    s.Logic = sage::editor::prefs::GetString("create.script_language", "lua");
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
    char title[128];
    std::snprintf(title, sizeof(title), "%s%s", TitleOf(s.Req.Kind), kPopup);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        s.Open = false;
        return;
    }

    const logic::Kind* lk = s.Req.Kind == What::Script ? LogicOf(s) : nullptr;
    fs::path dir = s.Req.Dir;
    if (host.CurrentProject().Loaded()) {
        const fs::path assets = host.CurrentProject().AssetsDir();
        if (lk && !lk->Folder.empty()) {
            const fs::path own = assets / sage::PathFromUtf8(lk->Folder);
            // Своя папка у вида — по умолчанию; у графа, которого ищут по
            // имени, — всегда (вне её библиотека ИИ его не найдёт).
            bool inside = false;
            std::error_code ec;
            const fs::path rel = fs::relative(dir, own, ec);
            if (!dir.empty() && !ec && !rel.empty() && *rel.begin() != "..") inside = true;
            if (dir.empty() || (lk->OwnFolderOnly && !inside)) dir = own;
        } else if (dir.empty()) {
            dir = DefaultDir(assets, s.Req.Kind);
        }
    }

    EditorIcons::Inline(IconOf(s.Req.Kind));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(TitleOf(s.Req.Kind));
    ImGui::Spacing();

    // ИМЯ — первым и сразу с курсором: окно открыли, чтобы назвать вещь.
    ImGui::TextDisabled("%s", T("Name"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    // ШИРИНА ПОЛЯ — ЧИСЛОМ, А НЕ -1. В окне AlwaysAutoResize поле «до края»
    // мерит себя по окну, а окно — по полю: каждый кадр оба становились чуть
    // уже, и окно «Новый материал» (где больше нечему держать ширину, в отличие
    // от палитры папки) сжималось до узкой полоски. Заданный ранее размер окна
    // AlwaysAutoResize всё равно отменял.
    ImGui::SetNextItemWidth(-1.0f);
    bool create = ImGui::InputText("##create_name", s.Name, sizeof(s.Name), ImGuiInputTextFlags_EnterReturnsTrue);

    if (s.Req.Kind == What::Script) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("Language"));
        for (const logic::Kind* k : logic::ForSlot(s.Req.Slot))
            if (Choice(k->Id.c_str(), T(k->Title.c_str()), T(k->Note.c_str()), lk == k)) {
                s.Logic = k->Id;
                lk = k;
            }
        if (!lk) ImGui::TextDisabled("%s", T("No kinds of logic for this slot"));
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
    const std::string file = FileName(s.Req.Kind, lk ? lk->Ext : std::string(), s.Name[0] ? s.Name : "…");
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
        bool made = false;
        if (s.Req.Kind == What::Script) {
            if (!lk || !lk->Create) s.Error = T("Choose a language");
            else made = lk->Create(s.Name, dir, created, s.Error);
        } else {
            made = AssetsPanel::CreateAsset(KindFor(s.Req.Kind), s.Name, dir, created, s.Error);
        }
        if (made) {
            if (lk) sage::editor::prefs::SetString("create.script_language", lk->Id);
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
            if (lk && lk->Open && s.Req.OpenAfter) lk->Open(host, created);
            host.SetStatusMessage(T("Created: ") + created.filename().string());
            s.Open = false;
            ImGui::CloseCurrentPopup();
        }
    } else if (cancel || s.CloseRequested) {
        s.Open = false;
        ImGui::CloseCurrentPopup();
    }
    s.CloseRequested = false;
    s.Width = ImGui::GetWindowWidth();
    ImGui::EndPopup();
}

float WindowWidth() { return S().Open ? S().Width : 0.0f; }
void RequestClose() { S().CloseRequested = true; }

} // namespace sage::editor::createasset
