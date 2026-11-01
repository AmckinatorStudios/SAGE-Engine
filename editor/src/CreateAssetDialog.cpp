#include "CreateAssetDialog.h"

#include <algorithm>
#include <cctype>
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
#include "NewShader.h"
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
    bool NameEdited = false;     // человек правил имя — смена вида его не трогает
    std::string Logic = "slua";  // id вида логики (LogicKinds.h)
    bool ScreenShader = false;   // вид нового шейдера: экран или материал (NewShader.h)
    int Color = -1;         // номер цвета палитры; -1 — без цвета
    std::string Error;      // ошибка создания (диск); ошибка имени считается на лету
    bool CloseRequested = false;   // закрыть снаружи (самопроверка)
    float Width = 0.0f;            // ширина окна в прошлом кадре
    float Height = 0.0f;
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
        case What::Shader: return T("New shader");
        case What::Script:
        default: return T("New script");
    }
}

// ИМЯ ФАЙЛА — проверяется до нажатия, а не ошибкой после: кнопка «Создать»
// неактивна, пока имя не годится, и под полем сказано почему. Правила — те, что
// не пропустит хоть одна из систем, где проект откроют (Windows строже всех).
std::string NameProblem(const std::string& name) {
    if (name.empty()) return T("Enter a name");
    if (name.size() > 64) return T("The name is too long (64 characters at most)");
    for (char c : name)
        if (std::strchr("\\/:*?\"<>|", c) || (unsigned char)c < 32) return T("The name cannot contain \\ / : * ? \" < > |");
    if (name.front() == ' ' || name.back() == ' ' || name.front() == '.' || name.back() == '.')
        return T("The name cannot start or end with a space or a dot");
    std::string upper;
    for (char c : name.substr(0, name.find('.'))) upper += (char)std::toupper((unsigned char)c);
    static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                      "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                      "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char* r : kReserved)
        if (upper == r) return T("This name is reserved by the system");
    return {};
}

// Строка вида: значок, название и несколько слов о назначении — в ОДНУ строку.
// NoAutoClosePopups ОБЯЗАТЕЛЕН: Selectable прямо в модальном окне по
// умолчанию закрывает это окно — щелчок по типу захлопывал бы «Новый скрипт».
bool KindRow(const logic::Kind& k, bool selected) {
    ImGui::PushID(k.Id.c_str());
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y;
    const bool pressed = ImGui::Selectable("##kind", selected, ImGuiSelectableFlags_NoAutoClosePopups,
                                           ImVec2(ImGui::GetContentRegionAvail().x, h));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(k.Note.c_str()));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& st = ImGui::GetStyle();
    const float textY = at.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    const float icon = ImGui::GetTextLineHeight();
    const float labelW = EditorIcons::DrawLabeled(
        dl, ImVec2(at.x + st.FramePadding.x, textY), icon, k.Icon.c_str(),
        ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled), T(k.Title.c_str()),
        ImGui::GetColorU32(ImGuiCol_Text));
    // Пояснение — после названия, обрезано по краю строки: длинный текст не
    // раздувает окно и не переносится на вторую строку.
    const float noteX = at.x + st.FramePadding.x + labelW + st.ItemSpacing.x * 2.0f;
    const float right = at.x + ImGui::GetContentRegionAvail().x;
    if (noteX < right - icon) {
        dl->PushClipRect(ImVec2(noteX, at.y), ImVec2(right - st.FramePadding.x, at.y + h), true);
        dl->AddText(ImVec2(noteX, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), T(k.Note.c_str()));
        dl->PopClipRect();
    }
    ImGui::PopID();
    return pressed;
}

namespace ns = sage::editor::newshader;

// Виды нового шейдера — строками того же вида, что виды логики.
const logic::Kind& ShaderKindRow(bool screen) {
    static const logic::Kind material = [] {
        logic::Kind k;
        k.Id = "shader.material";
        k.Title = "Shader material";
        k.Note = "For objects: its own .vert/.frag and settings";
        k.Icon = "material";
        return k;
    }();
    static const logic::Kind screenKind = [] {
        logic::Kind k;
        k.Id = "shader.screen";
        k.Title = "Screen shader";
        k.Note = "For the camera: processes the finished frame";
        k.Icon = "camera";
        return k;
    }();
    return screen ? screenKind : material;
}

const char* GroupTitle(const std::string& group) {
    return group == "ai" ? T("Artificial intelligence") : T("Scripts");
}

} // namespace

void Open(const Request& request) {
    State& s = S();
    s.Req = request;
    s.Pending = true;
    s.CloseRequested = false;   // просьба закрыть прошлое окно к новому не относится
    s.Error.clear();
    s.Color = -1;
    s.NameEdited = false;
    std::snprintf(s.Name, sizeof(s.Name), "%s", request.Name.c_str());
    s.ScreenShader = request.ScreenOnly;
    // Вид — привычка человека, а не свойство проекта: запоминается.
    s.Logic = sage::editor::prefs::GetString("create.script_language", "slua");
    // Имя по умолчанию — по виду («NewAIGraph»), если слот не предложил своё.
    if (request.Kind == What::Script)
        if (const logic::Kind* k = LogicOf(s); k && (request.Name.empty() || request.Name == "NewScript"))
            std::snprintf(s.Name, sizeof(s.Name), "%s", k->DefaultName.c_str());
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
    // РАЗМЕР — ПОСТОЯННЫЙ И В ПРЕДЕЛАХ ЭКРАНА. Ширина задана, а не «по
    // содержимому» (поле имени «до края» и окно «по полю» сжимали друг друга
    // до полоски); высота — не больше 85% окна редактора: лишнее прокручивается
    // в списке, а имя и кнопки остаются на месте при любом разрешении.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float width = std::min(ImGui::GetFontSize() * 26.0f, vp->WorkSize.x * 0.92f);
    const float maxHeight = vp->WorkSize.y * 0.85f;
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, maxHeight));
    char title[128];
    std::snprintf(title, sizeof(title), "%s%s", TitleOf(s.Req.Kind), kPopup);
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
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

    // --- ТИП — список с разделами, прокрутка внутри при нехватке места --------
    if (s.Req.Kind == What::Script) {
        const std::vector<const logic::Kind*> kinds = logic::ForSlot(s.Req.Slot);
        if (kinds.empty()) ImGui::TextDisabled("%s", T("No kinds of logic for this slot"));
        const float row = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y + ImGui::GetStyle().ItemSpacing.y;
        int groups = 0;
        std::string last;
        for (const logic::Kind* k : kinds)
            if (k->Group != last) {
                ++groups;
                last = k->Group;
            }
        const float want = row * (float)kinds.size() + ImGui::GetTextLineHeightWithSpacing() * 1.4f * (float)groups;
        // Место под имя, подсказку и кнопки — всегда; список — сколько останется.
        const float reserved = ImGui::GetFrameHeightWithSpacing() * 5.0f + ImGui::GetTextLineHeightWithSpacing() * 4.0f;
        const float listH = std::max(row * 2.0f, std::min(want, maxHeight - reserved));
        ImGui::BeginChild("##create_kinds", ImVec2(0.0f, listH), ImGuiChildFlags_None);
        last.clear();
        for (const logic::Kind* k : kinds) {
            // Скрипты и ИИ — раздельно: инструменты разного назначения.
            if (k->Group != last) {
                if (!last.empty()) ImGui::Spacing();
                ImGui::TextDisabled("%s", GroupTitle(k->Group));
                ImGui::Separator();
                last = k->Group;
            }
            if (KindRow(*k, lk == k)) {
                // Имя по умолчанию следует за видом, пока человек его не трогал.
                if (!s.NameEdited) std::snprintf(s.Name, sizeof(s.Name), "%s", k->DefaultName.c_str());
                s.Logic = k->Id;
                lk = k;
                s.Error.clear();
            }
        }
        ImGui::EndChild();
        ImGui::Spacing();
    }

    // --- ШЕЙДЕР: пустой, вид — материал или экран --------------------------------
    const bool shader = s.Req.Kind == What::Shader;
    if (shader && !s.Req.ScreenOnly) {
        // Пустой шейдер: контракт движка соблюдён, рисует он то, что напишут.
        ImGui::TextDisabled("%s", T("Kind"));
        ImGui::Separator();
        if (KindRow(ShaderKindRow(false), !s.ScreenShader)) s.ScreenShader = false;
        if (KindRow(ShaderKindRow(true), s.ScreenShader)) s.ScreenShader = true;
        ImGui::Spacing();
    }
    if (shader && host.CurrentProject().Loaded())
        dir = host.CurrentProject().AssetsDir() / (s.ScreenShader ? "shaders" : "materials");

    // --- ИМЯ -----------------------------------------------------------------
    ImGui::TextDisabled("%s", T("Name"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter =
        ImGui::InputText("##create_name", s.Name, sizeof(s.Name), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited()) {
        s.NameEdited = true;
        s.Error.clear();
    }

    // Проверка имени — на лету: в том числе «такой файл уже есть».
    // Шейдер материала кладёт три файла; показан главный — материал.
    const std::string ext = shader ? (s.ScreenShader ? ".frag" : ".sagemat") : lk ? lk->Ext : std::string();
    const std::string file = FileName(s.Req.Kind, ext, s.Name[0] ? s.Name : "…");
    std::string problem = NameProblem(s.Name);
    if (problem.empty()) {
        std::error_code ec;
        if (fs::exists(dir / sage::PathFromUtf8(file), ec)) problem = T("A file with this name already exists");
    }
    if (s.Req.Kind == What::Script && !lk) problem = T("Choose a language");

    if (s.Req.Kind == What::Folder) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T("Colour"));
        const std::vector<sage::editor::foldercolors::Tint> palette = sage::editor::foldercolors::Palette();
        const float box = ImGui::GetFrameHeight();
        // «Без цвета» — первым и выбран по умолчанию: цвет — пометка, а не
        // обязанность.
        if (EditorIcons::IconOnlyButton("folder", T("No colour"), s.Color < 0)) s.Color = -1;
        for (int i = 0; i < (int)palette.size(); ++i) {
            if (ImGui::GetContentRegionAvail().x > box + 4.0f) ImGui::SameLine(0.0f, 4.0f);
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

    // Ошибка имени или создания — красным под полем; иначе — куда ляжет файл.
    const std::string shown = !s.Error.empty() ? s.Error : problem;
    if (!shown.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", shown.c_str());
        ImGui::PopTextWrapPos();
    } else {
        std::string where = sage::PathToUtf8(dir.filename());
        if (host.CurrentProject().Loaded()) {
            std::error_code ec;
            const fs::path rel = fs::relative(dir, host.CurrentProject().Dir(), ec);
            if (!ec && !rel.empty()) where = rel.generic_string();
        }
        const std::string full = where + "/" + file;
        ImGui::TextDisabled("%s", full.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("Will be created: %s"), full.c_str());
    }

    // --- КНОПКИ — внизу, справа, всегда видны --------------------------------
    ImGui::Separator();
    const bool valid = problem.empty();
    const float cancelW = ImGui::CalcTextSize(T("Cancel")).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float createW = ImGui::CalcTextSize(T("Create")).x + ImGui::GetStyle().FramePadding.x * 2.0f +
                          ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - cancelW - createW -
                                                                      ImGui::GetStyle().ItemSpacing.x));
    ImGui::BeginDisabled(!valid);
    bool create = EditorIcons::Button("plus", T("Create"));
    ImGui::EndDisabled();
    if (!valid && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", problem.c_str());
    create = (create || enter) && valid;
    ImGui::SameLine();
    const bool cancel = ImGui::Button(T("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape);

    if (create) {
        std::error_code mk;
        fs::create_directories(dir, mk);
        fs::path created;
        bool made = false;
        if (s.Req.Kind == What::Script) made = lk && lk->Create && lk->Create(s.Name, dir, created, s.Error);
        else if (shader) {
            if (!host.CurrentProject().Loaded()) s.Error = T("Open a project first");
            else made = ns::Create(s.ScreenShader ? ns::Kind::Screen : ns::Kind::Material, s.Name,
                                   host.CurrentProject().Dir(), host.CurrentProject().AssetsDir(), created, s.Error);
        } else made = AssetsPanel::CreateAsset(KindFor(s.Req.Kind), s.Name, dir, created, s.Error);
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
            // Граф — сразу в его редактор: Script Graph — в Script Graph, AI
            // Graph — в AI Graph; выбор типа однозначно задаёт и редактор.
            if (lk && lk->Open && s.Req.OpenAfter) lk->Open(host, created);
            host.SetStatusMessage(T("Created: ") + sage::PathToUtf8(created.filename()));
            s.Open = false;
            ImGui::CloseCurrentPopup();
        }
    } else if (cancel || s.CloseRequested) {
        s.Open = false;
        ImGui::CloseCurrentPopup();
    }
    s.CloseRequested = false;
    s.Width = ImGui::GetWindowWidth();
    s.Height = ImGui::GetWindowHeight();
    ImGui::EndPopup();
}

float WindowWidth() { return S().Open ? S().Width : 0.0f; }
float WindowHeight() { return S().Open ? S().Height : 0.0f; }
void RequestClose() { S().CloseRequested = true; }

} // namespace sage::editor::createasset
