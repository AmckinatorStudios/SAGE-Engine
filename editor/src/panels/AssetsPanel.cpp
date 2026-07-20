#include "AssetsPanel.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <system_error>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "Project.h"
#include "sage/core/Log.h"

namespace fs = std::filesystem;

namespace {

// Цветная плашка + короткий тег по типу файла — свой минимализм панели Assets
// вместо иконочного шрифта: ImDrawList::AddRectFilled с закруглением плюс
// 2-4 символа расширения по центру.
struct AssetStyle { ImVec4 Color; std::string Tag; };

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

AssetStyle StyleForPath(const fs::path& path, bool isDir) {
    if (isDir) return { ImVec4(0.80f, 0.60f, 0.20f, 1.0f), "DIR" };
    std::string ext = ToLower(path.extension().string());
    if (ext == ".sage") return { ImVec4(0.25f, 0.45f, 0.85f, 1.0f), "SCENE" };
    if (ext == ".sageprefab") return { ImVec4(0.35f, 0.55f, 0.95f, 1.0f), "PREFAB" };
    if (ext == ".sagemat") return { ImVec4(0.75f, 0.45f, 0.20f, 1.0f), "MAT" };
    if (ext == ".sageimport") return { ImVec4(0.45f, 0.40f, 0.30f, 1.0f), "IMPORT" };
    if (ext == ".lua") return { ImVec4(0.25f, 0.70f, 0.30f, 1.0f), "LUA" };
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb") return { ImVec4(0.55f, 0.35f, 0.80f, 1.0f), "MESH" };
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
        return { ImVec4(0.20f, 0.65f, 0.65f, 1.0f), ext.substr(1) };
    if (ext == ".wav" || ext == ".ogg" || ext == ".mp3") return { ImVec4(0.80f, 0.30f, 0.55f, 1.0f), "AUDIO" };
    if (ext == ".vert" || ext == ".frag" || ext == ".glsl") return { ImVec4(0.50f, 0.50f, 0.55f, 1.0f), "GLSL" };
    std::string tag = ext.empty() ? "FILE" : ToLower(ext.substr(1));
    return { ImVec4(0.45f, 0.45f, 0.48f, 1.0f), tag };
}

// Обрезает строку многоточием справа, чтобы уместиться в maxWidth.
std::string TruncateToWidth(const std::string& s, float maxWidth) {
    if (ImGui::CalcTextSize(s.c_str()).x <= maxWidth) return s;
    const char* ellipsis = "...";
    float ellipsisW = ImGui::CalcTextSize(ellipsis).x;
    std::string out;
    for (size_t n = 1; n <= s.size(); ++n) {
        std::string candidate = s.substr(0, n);
        if (ImGui::CalcTextSize(candidate.c_str()).x + ellipsisW > maxWidth) {
            out = s.substr(0, n > 1 ? n - 1 : 1);
            break;
        }
        out = candidate;
    }
    return out + ellipsis;
}

// Габариты карточки-тайла. ВАЖНО: высота тайла включает и плашку, и подпись —
// раньше подпись рисовалась DrawList'ом НИЖЕ InvisibleButton'а (высотой лишь в
// плашку), выходила за границы item'а, и строки грида налезали друг на друга
// («неровность»). Теперь весь тайл — единый item, всё внутри его границ.
constexpr float kTileW = 104.0f;
constexpr float kSwatchH = 62.0f;  // цветная плашка (верх карточки)
constexpr float kLabelH = 34.0f;   // область подписи (1-2 строки)
constexpr float kTileH = kSwatchH + kLabelH + 6.0f; // + внутренний отступ
constexpr float kTileSpacing = 12.0f;

} // namespace

void AssetsPanel::DrawBreadcrumb(EditorHost& host) {
    Project& project = host.CurrentProject();
    fs::path& cwd = host.AssetsCwd();
    fs::path root = project.Loaded() ? project.Dir().parent_path() : cwd.root_path();

    // Собираем цепочку сегментов от текущей папки вверх до корня.
    std::vector<fs::path> chain;
    for (fs::path p = cwd; p != root && p.has_parent_path(); p = p.parent_path()) {
        chain.push_back(p);
        if (p.parent_path() == p) break; // достигли корня файловой системы
    }
    std::reverse(chain.begin(), chain.end());

    for (size_t i = 0; i < chain.size(); ++i) {
        std::string seg = chain[i].filename().string();
        if (seg.empty()) seg = chain[i].string();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton(seg.c_str())) cwd = chain[i];
        ImGui::PopID();
        if (i + 1 < chain.size()) { ImGui::SameLine(0, 2); ImGui::TextDisabled("/"); ImGui::SameLine(0, 2); }
    }
}

void AssetsPanel::DrawTile(EditorHost& host, const fs::path& path, bool isDir) {
    AssetStyle style = StyleForPath(path, isDir);
    std::string filename = path.filename().string();

    ImGui::PushID(filename.c_str());

    // Весь тайл — ОДИН item (InvisibleButton на полную высоту карточки): подпись
    // теперь внутри его границ, строки грида больше не налезают друг на друга.
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tile", ImVec2(kTileW, kTileH));
    bool hovered = ImGui::IsItemHovered();
    bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool isSelected = m_selected == path;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 tileMax(cursor.x + kTileW, cursor.y + kTileH);

    // Подложка карточки — по теме редактора; подсвечивается при наведении/выборе.
    ImU32 cardBg = 0;
    if (isSelected)      cardBg = ImGui::GetColorU32(ImGuiCol_Header);
    else if (hovered)    cardBg = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
    else                 cardBg = ImGui::GetColorU32(ImVec4(1, 1, 1, 0.035f));
    dl->AddRectFilled(cursor, tileMax, cardBg, 8.0f);
    if (isSelected) {
        dl->AddRect(cursor, tileMax, ImGui::GetColorU32(ImGuiCol_NavHighlight), 8.0f, 0, 1.6f);
    }

    // Цветная плашка типа с внутренним отступом (верх карточки).
    constexpr float kInset = 8.0f;
    ImVec2 sw0(cursor.x + kInset, cursor.y + kInset);
    ImVec2 sw1(tileMax.x - kInset, cursor.y + kInset + kSwatchH - kInset);
    ImVec4 fill = style.Color;
    if (!hovered && !isSelected) { fill.x *= 0.9f; fill.y *= 0.9f; fill.z *= 0.9f; }
    dl->AddRectFilled(sw0, sw1, ImGui::ColorConvertFloat4ToU32(fill), 6.0f);

    // Тег типа по центру плашки.
    std::string tagUpper = style.Tag;
    for (char& c : tagUpper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    ImVec2 tagSize = ImGui::CalcTextSize(tagUpper.c_str());
    ImVec2 tagPos(sw0.x + (sw1.x - sw0.x - tagSize.x) * 0.5f, sw0.y + (sw1.y - sw0.y - tagSize.y) * 0.5f);
    dl->AddText(tagPos, ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 0.96f)), tagUpper.c_str());

    // Имя файла по центру области подписи (внутри границ тайла, с усечением).
    std::string label = TruncateToWidth(filename, kTileW - 8.0f);
    ImVec2 labelSize = ImGui::CalcTextSize(label.c_str());
    ImVec2 labelPos(cursor.x + (kTileW - labelSize.x) * 0.5f, sw1.y + 7.0f);
    ImU32 textCol = ImGui::GetColorU32(isSelected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    if (hovered) textCol = ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddText(labelPos, textCol, label.c_str());

    if (clicked) m_selected = path;
    if (doubleClicked) {
        if (isDir) host.AssetsCwd() = path;
        else if (path.extension() == ".sage") host.LoadSceneFromFile(path);
        else if (path.extension() == ".sageprefab") host.InstantiatePrefab(path); // инстанс в сцену
    }
    if (ImGui::BeginPopupContextItem("##tile_ctx")) {
        m_selected = path;
        if (ImGui::MenuItem("Rename")) { m_renameTarget = path; m_error.clear(); }
        if (ImGui::MenuItem("Delete")) { m_deleteTarget = path; }
        ImGui::EndPopup();
    }
    if (hovered && !filename.empty() && label != filename) ImGui::SetTooltip("%s", filename.c_str());

    ImGui::PopID();
}

bool AssetsPanel::CreateAsset(CreateKind kind, const std::string& rawName, const fs::path& dir,
                              fs::path& outCreated, std::string& err) {
    std::string name = rawName;
    if (name.empty()) {
        err = "Name must not be empty";
        return false;
    }

    const char* wantExt = nullptr;
    switch (kind) {
        case CreateKind::Script:   wantExt = ".lua"; break;
        case CreateKind::TextFile: wantExt = ".txt"; break;
        case CreateKind::Material: wantExt = ".sagemat"; break;
        default: break;
    }
    if (wantExt && fs::path(name).extension() != wantExt) name += wantExt;

    fs::path target = dir / name;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        err = "Already exists: " + name;
        return false;
    }

    if (kind == CreateKind::Folder) {
        if (!fs::create_directory(target, ec) || ec) {
            err = "Create folder failed: " + ec.message();
            return false;
        }
    } else {
        std::string content;
        if (kind == CreateKind::Script) {
            content =
                "-- " + name + " — скрипт сущности SAGE.\n"
                "-- OnStart(entity) вызывается один раз при привязке скрипта,\n"
                "-- OnUpdate(entity, dt) — каждый кадр Play-режима/игры.\n"
                "\n"
                "function OnStart(entity)\n"
                "end\n"
                "\n"
                "function OnUpdate(entity, dt)\n"
                "    -- entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 45.0\n"
                "end\n";
        } else if (kind == CreateKind::Material) {
            content =
                "{\n"
                "    \"albedo\": [1.0, 1.0, 1.0],\n"
                "    \"emissive\": [0.0, 0.0, 0.0],\n"
                "    \"shininess\": 32.0,\n"
                "    \"texture\": \"\"\n"
                "}\n";
        }
        std::ofstream out(target);
        if (!out) {
            err = "Create file failed: " + target.string();
            return false;
        }
        out << content;
    }

    LOG_INFO("Editor") << "Asset created: " << target.string();
    outCreated = target;
    return true;
}

// Модалки панели. Деферред-паттерн: контекстные меню только выставляют
// m_createKind/m_renameTarget/m_deleteTarget, OpenPopup зовётся здесь, на
// уровне окна панели (OpenPopup изнутри BeginMenu не находит модалку —
// другой ID-стек, документированная ловушка ImGui).
void AssetsPanel::DrawModals(EditorHost& host) {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();

    if (m_createKind != CreateKind::None && !ImGui::IsPopupOpen("Create Asset")) {
        ImGui::OpenPopup("Create Asset");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Create Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const char* kindLabel = "";
        switch (m_createKind) {
            case CreateKind::Folder:   kindLabel = "Folder"; break;
            case CreateKind::Script:   kindLabel = "Lua script"; break;
            case CreateKind::TextFile: kindLabel = "Text file"; break;
            case CreateKind::Material: kindLabel = "Material"; break;
            default: break;
        }
        ImGui::TextDisabled("%s in %s", kindLabel, host.AssetsCwd().filename().string().c_str());
        bool enterPressed = ImGui::InputText("Name", m_createName, sizeof(m_createName),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (enterPressed || ImGui::Button("Create", ImVec2(120, 0))) {
            fs::path created;
            if (CreateAsset(m_createKind, m_createName, host.AssetsCwd(), created, m_error)) {
                m_selected = created;
                m_createKind = CreateKind::None;
                m_error.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_createKind = CreateKind::None;
            m_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!m_renameTarget.empty() && !ImGui::IsPopupOpen("Rename Asset")) {
        ImGui::OpenPopup("Rename Asset");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", m_renameTarget.filename().string().c_str());
        if (ImGui::IsWindowAppearing()) {
            std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s",
                          m_renameTarget.filename().string().c_str());
        }
        bool enterPressed = ImGui::InputText("New name", m_renameBuf, sizeof(m_renameBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if (!m_error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", m_error.c_str());
        if (enterPressed || ImGui::Button("Rename", ImVec2(120, 0))) {
            fs::path target = m_renameTarget.parent_path() / m_renameBuf;
            std::error_code ec;
            fs::rename(m_renameTarget, target, ec);
            if (ec) {
                m_error = "Rename failed: " + ec.message();
                LOG_ERROR("Editor") << "Asset rename failed: " << ec.message();
            } else {
                m_renameTarget.clear();
                m_error.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_renameTarget.clear();
            m_error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!m_deleteTarget.empty() && !ImGui::IsPopupOpen("Delete Asset")) {
        ImGui::OpenPopup("Delete Asset");
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete \"%s\"?", m_deleteTarget.filename().string().c_str());
        ImGui::TextDisabled("This cannot be undone.");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
        if (ImGui::Button("Delete", ImVec2(120, 0))) {
            std::error_code ec;
            fs::remove_all(m_deleteTarget, ec);
            if (ec) LOG_ERROR("Editor") << "Asset delete failed: " << ec.message();
            if (m_selected == m_deleteTarget) m_selected.clear();
            m_deleteTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_deleteTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void AssetsPanel::Draw(EditorHost& host) {
    Project& project = host.CurrentProject();
    fs::path& cwd = host.AssetsCwd();

    ImGui::Begin("Assets");

    if (!project.Loaded()) {
        ImGui::TextDisabled("No project open.");
        ImGui::TextDisabled("File > New Project... to create one; browsing current dir:");
    }

    fs::path root = project.Loaded() ? project.Dir() : fs::path("/");
    bool canGoUp = cwd.has_parent_path() && cwd != root;
    if (canGoUp && ImGui::SmallButton("Up")) cwd = cwd.parent_path();
    ImGui::SameLine();
    DrawBreadcrumb(host);

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##assets_search", "Search...", m_search, sizeof(m_search));
    ImGui::Separator();

    ImGui::BeginChild("##assets_scroll");
    std::error_code ec;
    std::vector<fs::directory_entry> dirs, files;
    for (const auto& entry : fs::directory_iterator(cwd, ec)) {
        (entry.is_directory(ec) ? dirs : files).push_back(entry);
    }
    auto byName = [](const fs::directory_entry& a, const fs::directory_entry& b) {
        return a.path().filename() < b.path().filename();
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    std::string filter = ToLower(m_search);
    auto matches = [&](const fs::path& p) {
        if (filter.empty()) return true;
        return ToLower(p.filename().string()).find(filter) != std::string::npos;
    };

    // Единый ритм по вертикали между строками грида — как горизонтальный зазор.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kTileSpacing, kTileSpacing));
    float availWidth = ImGui::GetContentRegionAvail().x;
    int columns = std::max(1, static_cast<int>((availWidth + kTileSpacing) / (kTileW + kTileSpacing)));
    int col = 0;
    bool any = false;
    auto placeTile = [&](const fs::path& p, bool isDir) {
        any = true;
        if (col > 0) ImGui::SameLine(0, kTileSpacing);
        DrawTile(host, p, isDir);
        col = (col + 1) % columns;
    };
    for (const auto& d : dirs) if (matches(d.path())) placeTile(d.path(), true);
    for (const auto& f : files) if (matches(f.path())) placeTile(f.path(), false);
    ImGui::PopStyleVar();
    if (!any) {
        ImGui::Spacing();
        ImGui::TextDisabled(m_search[0] ? "Nothing matches the search." : "This folder is empty.");
        ImGui::TextDisabled("Right-click to create a folder, script, material or text file.");
    }

    // Создание ассетов — ПКМ по пустому месту (не по тайлу: у тайлов своё меню).
    if (ImGui::BeginPopupContextWindow("##assets_create_ctx",
                                       ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        auto startCreate = [this](CreateKind kind, const char* defaultName) {
            m_createKind = kind;
            std::snprintf(m_createName, sizeof(m_createName), "%s", defaultName);
            m_error.clear();
        };
        if (ImGui::MenuItem("New Folder")) startCreate(CreateKind::Folder, "NewFolder");
        if (ImGui::MenuItem("New Script (.lua)")) startCreate(CreateKind::Script, "new_script");
        if (ImGui::MenuItem("New Text File (.txt)")) startCreate(CreateKind::TextFile, "notes");
        if (ImGui::MenuItem("New Material (.sagemat)")) startCreate(CreateKind::Material, "NewMaterial");
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    DrawModals(host);

    ImGui::End();
}
