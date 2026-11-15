#include "SpriteEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder: раскладка окна редактора по умолчанию
#include <stb_image.h>

#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorPrefs.h"
#include "../EditorTheme.h"
#include "../HotkeyScope.h"
#include "../Localization.h"
#include "SpriteEditorDetail.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Pack.h"
#include "sage/render/Texture.h"

using sage::editor::T;
namespace sp = sage::editor::sprite;

namespace {
constexpr const char* kPrefInspector = "spriteEditor.inspector";
} // namespace

// --- Помощники вида ---------------------------------------------------------

namespace sage::editor::sprite {

bool Segmented(const char* id, const char* const* labels, int count, int* current, float width) {
    ImGui::PushID(id);
    const float full = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + full, p.y + h), EditorTheme::Color32(EditorTheme::Role::Input), h * 0.5f);
    const float seg = full / (float)std::max(1, count);
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + seg * (float)i, p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(seg, h)) && *current != i) {
            *current = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 a(p.x + seg * (float)i + 2.0f, p.y + 2.0f), b(a.x + seg - 4.0f, p.y + h - 2.0f);
        if (*current == i)
            dl->AddRectFilled(a, b, EditorTheme::Color32(EditorTheme::Role::Accent), (h - 4.0f) * 0.5f);
        else if (hovered)
            dl->AddRectFilled(a, b, EditorTheme::Color32(EditorTheme::Role::Hover), (h - 4.0f) * 0.5f);
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2(std::floor(a.x + (b.x - a.x - ts.x) * 0.5f), std::floor(p.y + (h - ts.y) * 0.5f)),
                    EditorTheme::Color32(*current == i ? EditorTheme::Role::TextOnAccent
                                                       : hovered ? EditorTheme::Role::Text
                                                                 : EditorTheme::Role::TextDim),
                    labels[i]);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(full, h));
    ImGui::PopID();
    return changed;
}

void ParamTip(const char* name, const char* what, const char* example) {
    // Своя проверка прямоугольника, а не IsItemHovered: после EndGroup курсор
    // над полем внутри группы «занят» полем, и подсказка группы не всплыла бы.
    if (!ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()) ||
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) || ImGui::IsAnyItemActive())
        return;
    // Задержка — чтобы подсказка не мигала, пока мышь просто идёт мимо.
    if (ImGui::GetCurrentContext()->HoveredIdTimer < 0.45f && GImGui->HoveredId != 0) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
    ImGui::TextUnformatted(name);
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextDim));
    ImGui::TextUnformatted(what);
    if (example && *example) {
        ImGui::Spacing();
        ImGui::Text("%s %s", T("Example:"), example);
    }
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void Nearest(ImDrawList* dl) {
    if (ImDrawCallback cb = ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest) dl->AddCallback(cb, nullptr);
}

void Linear(ImDrawList* dl) {
    if (ImDrawCallback cb = ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear) dl->AddCallback(cb, nullptr);
}

void Checkers(ImDrawList* dl, ImVec2 a, ImVec2 b, float cell) {
    dl->AddRectFilled(a, b, IM_COL32(58, 58, 64, 255));
    cell = std::max(4.0f, cell);
    // Клеток не больше нескольких тысяч: на крупном масштабе шахматка — фон,
    // а не по клетке на пиксель.
    const int nx = (int)std::ceil((b.x - a.x) / cell), ny = (int)std::ceil((b.y - a.y) / cell);
    if ((long)nx * ny > 40000) return;
    for (int y = 0; y < ny; ++y)
        for (int x = (y & 1); x < nx; x += 2) {
            const ImVec2 p(a.x + x * cell, a.y + y * cell);
            dl->AddRectFilled(p, ImVec2(std::min(b.x, p.x + cell), std::min(b.y, p.y + cell)),
                              IM_COL32(84, 84, 92, 255));
        }
}

ImU32 FrameColor() { return IM_COL32(120, 200, 255, 170); }
ImU32 HoverColor() { return IM_COL32(200, 235, 255, 255); }
ImU32 SelectedColor() { return EditorTheme::Color32(EditorTheme::Role::Accent); }
ImU32 InvalidColor() { return IM_COL32(255, 86, 86, 210); }
ImU32 PreviewColor() { return IM_COL32(150, 255, 170, 150); }

} // namespace sage::editor::sprite

// --- Документ ----------------------------------------------------------------

void SpriteEditor::ForgetProject() {
    if (m_autoJob.valid()) m_autoJob.wait();
    m_path.clear();
    m_disk.clear();
    m_tex.reset();
    m_pixels.reset();
    m_w = m_h = 0;
    m_doc = sp::SpriteDoc();
    m_history.Clear();
    m_savedPrint = sp::Fingerprint(m_doc);
    m_dirty = false;
    m_sel.clear();
    m_gridValid = m_autoValid = false;
    m_autoRects.clear();
}

void SpriteEditor::Open(const std::string& path, int region, int sequence) {
    Load(path);
    if (region >= 0 && region < (int)m_doc.Sheet.Regions.size()) SelectOnly(region);
    if (sequence >= 0 && sequence < (int)m_doc.Sheet.Sequences.size()) m_seqSelected = sequence;
}

void SpriteEditor::Load(const std::string& path) {
    if (m_autoJob.valid()) m_autoJob.wait();
    m_path = path;
    m_doc = sp::SpriteDoc();
    m_history.Clear();
    m_sel.clear();
    m_hover = -1;
    m_zoom = 0.0f;
    m_nineZoom = 0.0f;
    m_userView = false;
    m_pan = glm::vec2(0.0f);
    m_pixels.reset();
    m_w = m_h = 0;
    m_gridValid = m_autoValid = false;
    m_autoRects.clear();
    m_status.clear();
    m_tex.reset();
    m_disk = path.empty() ? std::string() : sage::AssetDatabase::Instance().LocatePath(path);
    if (!path.empty()) {
        // Пиксели — для пустых клеток, островов и догадки о девятине.
        int ch = 0, w = 0, h = 0;
        stbi_set_flip_vertically_on_load_thread(false);
        if (unsigned char* data = sage::assets::vfs::LoadImage(m_disk, &w, &h, &ch, 4)) {
            m_pixels = std::make_shared<const std::vector<unsigned char>>(data, data + (size_t)w * h * 4);
            m_w = w;
            m_h = h;
            stbi_image_free(data);
            // Своя текстура, а не из общего кэша: там картинка могла быть уже
            // загружена со сглаживанием (миниатюра, сцена), а на увеличенном
            // холсте нужен каждый пиксель без размытия. Строки — снизу вверх,
            // как у текстур движка.
            std::vector<unsigned char> flipped(m_pixels->size());
            const size_t row = (size_t)w * 4;
            for (int y = 0; y < h; ++y)
                std::copy_n(m_pixels->data() + (size_t)(h - 1 - y) * row, row, flipped.data() + (size_t)y * row);
            m_tex = std::make_shared<Texture>(flipped.data(), w, h, TextureFilter::Nearest, false);
        } else {
            SetStatus(T("Could not read the texture: ") + path, true);
        }
        std::string err;
        if (!sp::LoadDoc(m_disk, m_doc, err)) SetStatus(T("Could not read the sprite sheet: ") + err, true);
    }
    m_savedPrint = sp::Fingerprint(m_doc);
    m_dirty = false;
    m_touched = false;
}

void SpriteEditor::Snapshot() {
    m_history.Push(m_doc);
    m_touched = true;
}

void SpriteEditor::Undo() {
    if (!m_history.Undo(m_doc)) return;
    m_sel.erase(std::remove_if(m_sel.begin(), m_sel.end(),
                               [&](int i) { return i >= (int)m_doc.Sheet.Regions.size(); }),
                m_sel.end());
    m_nameFor = -1;
    m_touched = true;
}

void SpriteEditor::Redo() {
    if (!m_history.Redo(m_doc)) return;
    m_sel.erase(std::remove_if(m_sel.begin(), m_sel.end(),
                               [&](int i) { return i >= (int)m_doc.Sheet.Regions.size(); }),
                m_sel.end());
    m_nameFor = -1;
    m_touched = true;
}

bool SpriteEditor::Save() {
    if (m_disk.empty()) return false;
    std::string err;
    if (!sp::SaveDoc(m_disk, m_doc, err)) {
        SetStatus(T("Could not save: ") + err, true);
        return false;
    }
    m_savedPrint = sp::Fingerprint(m_doc);
    m_dirty = false;
    SetStatus(T("Saved next to the texture"));
    return true;
}

void SpriteEditor::SetStatus(const std::string& text, bool error) {
    m_status = text;
    m_statusError = error;
}

bool SpriteEditor::Selected(int i) const { return std::find(m_sel.begin(), m_sel.end(), i) != m_sel.end(); }

void SpriteEditor::SelectOnly(int i) {
    m_sel.clear();
    if (i >= 0) m_sel.push_back(i);
}

void SpriteEditor::DeleteSelected() {
    if (m_sel.empty()) return;
    Snapshot();
    std::vector<int> order = m_sel;
    std::sort(order.rbegin(), order.rend());
    for (int i : order)
        if (i >= 0 && i < (int)m_doc.Sheet.Regions.size())
            m_doc.Sheet.Regions.erase(m_doc.Sheet.Regions.begin() + i);
    m_sel.clear();
    m_nameFor = -1;
}

// --- Окно --------------------------------------------------------------------

void SpriteEditor::Draw(EditorHost& host, bool* open) {
    if (!m_inspectorLoaded) {
        m_inspectorVisible = sage::editor::prefs::GetBool(kPrefInspector, true);
        m_inspectorLoaded = true;
    }
    // Первое открытие — крупным окном посередине: это рабочее место, а не
    // справочная панелька.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.82f, vp->WorkSize.y * 0.84f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    bool keep = true;
    const bool visible = ImGui::Begin(T("Sprite Editor###SpriteSheet"), &keep,
                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    // Закрыть с несохранённым — сначала спросить: потерянная нарезка — это
    // час работы с увеличенной картинкой.
    if (!keep) {
        if (m_dirty) m_askClose = true;
        else if (open) *open = false;
    }
    if (visible) {
        DrawTopBar(host, open);
        DrawModeTabs();
        DrawDock();
    }
    ImGui::End();
    if (visible) {
        DrawCanvasWindow(host);
        if (m_inspectorVisible) DrawInspectorWindow(host);
        HandleShortcuts();
    }
    PumpAuto();
    DrawCloseModal(open);
    DrawReplaceModal();
    // «Несохранено» — сравнением с сохранённым текстом, а не счётчиком правок:
    // правка и её отмена возвращают документ к сохранённому виду.
    if (m_touched || ImGui::GetFrameCount() % 20 == 0) {
        m_dirty = sp::Fingerprint(m_doc) != m_savedPrint;
        m_touched = false;
    }
}

void SpriteEditor::DrawTopBar(EditorHost& host, bool* open) {
    (void)host;
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::AlignTextToFramePadding();
    EditorIcons::Inline("grid");
    ImGui::SameLine();
    ImGui::TextUnformatted(T("Sprite Editor"));
    if (!m_path.empty()) {
        const size_t slash = m_path.find_last_of('/');
        const std::string file = slash == std::string::npos ? m_path : m_path.substr(slash + 1);
        ImGui::SameLine(0.0f, 14.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Text));
        ImGui::TextUnformatted(file.c_str());
        ImGui::PopStyleColor();
        if (m_dirty) {
            // Несохранённое — точкой акцента у имени файла, как у вкладки.
            ImGui::SameLine(0.0f, 6.0f);
            const float d = ImGui::GetFontSize() * 0.55f;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(d, ImGui::GetFrameHeight()));
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + d * 0.5f, at.y + ImGui::GetFrameHeight() * 0.5f),
                                                        d * 0.5f, EditorTheme::Color32(EditorTheme::Role::Accent));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Unsaved changes"));
        }
        ImGui::SameLine(0.0f, 10.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::TextFaint));
        ImGui::TextUnformatted(m_path.c_str());
        ImGui::PopStyleColor();
    }
    // Справа: отмена, повтор, сохранение, инспектор, раскладка, закрыть.
    const float btn = ImGui::GetFrameHeight();
    const float saveW = ImGui::CalcTextSize(T("Save")).x + st.FramePadding.x * 2.0f + btn;
    const float right = btn * 5.0f + saveW + st.ItemSpacing.x * 6.0f + 12.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, ImGui::GetWindowContentRegionMax().x - right));
    ImGui::BeginDisabled(!m_history.CanUndo());
    if (EditorIcons::IconOnlyButton("undo", T("Undo (Ctrl+Z)"))) Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_history.CanRedo());
    if (EditorIcons::IconOnlyButton("redo", T("Redo (Ctrl+Y)"))) Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_disk.empty());
    if (EditorIcons::Button("save", T("Save"), T("Save the sprite sheet next to the texture (Ctrl+S)"), m_dirty))
        Save();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 12.0f);
    if (EditorIcons::IconOnlyButton("inspector", m_inspectorVisible ? T("Hide the inspector") : T("Show the inspector"),
                                    m_inspectorVisible)) {
        m_inspectorVisible = !m_inspectorVisible;
        sage::editor::prefs::SetBool(kPrefInspector, m_inspectorVisible);
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("layout", T("Restore the default layout"))) {
        m_resetLayout = true;
        m_inspectorVisible = true;
        sage::editor::prefs::SetBool(kPrefInspector, true);
    }
    ImGui::SameLine();
    if (EditorIcons::IconOnlyButton("exit", T("Close the Sprite Editor"))) {
        if (m_dirty) m_askClose = true;
        else if (open) *open = false;
    }
}

void SpriteEditor::DrawModeTabs() {
    const char* modes[] = {T("Sprite Sheet"), T("Nine-Slice")};
    int mode = m_mode == Mode::Sheet ? 0 : 1;
    if (sp::Segmented("##mode", modes, 2, &mode, 300.0f)) m_mode = mode == 0 ? Mode::Sheet : Mode::NineSlice;
    if (!m_status.empty()) {
        ImGui::SameLine(0.0f, 16.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(m_statusError ? EditorTheme::Role::Danger
                                                                              : EditorTheme::Role::TextDim));
        ImGui::TextUnformatted(m_status.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

void SpriteEditor::BuildDefaultLayout(ImGuiID dock) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, ImGui::GetContentRegionAvail());
    ImGuiID center = dock;
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.32f, nullptr, &center);
    ImGui::DockBuilderDockWindow("###SpriteEditorCanvas", center);
    ImGui::DockBuilderDockWindow("###SpriteEditorInspector", right);
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(center))
        node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;   // холст — один, вкладка ему не нужна
    ImGui::DockBuilderFinish(dock);
}

void SpriteEditor::DrawDock() {
    const ImGuiID dock = ImGui::GetID("SpriteEditorDock");
    // Раскладка по умолчанию — при первом открытии и по кнопке; дальше её
    // помнит imgui.ini вместе с остальной раскладкой редактора.
    if (m_resetLayout || ImGui::DockBuilderGetNode(dock) == nullptr) {
        BuildDefaultLayout(dock);
        m_resetLayout = false;
    }
    ImGui::DockSpace(dock, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
}

void SpriteEditor::HandleShortcuts() {
    // Клавиши — пока фокус в окнах редактора спрайтов (хост, холст или
    // инспектор): Ctrl+Z здесь не откатывает сцену, Ctrl+S не сохраняет её.
    const ImGuiWindow* focused = GImGui->NavWindow;
    bool mine = false;
    for (const ImGuiWindow* w = focused; w; w = w->ParentWindow) {
        const char* name = w->Name;
        if (std::strstr(name, "###SpriteSheet") || std::strstr(name, "###SpriteEditorCanvas") ||
            std::strstr(name, "###SpriteEditorInspector")) {
            mine = true;
            break;
        }
    }
    if (!mine) return;
    namespace hk = sage::editor::hotkeys;
    hk::Claim(hk::Key::Undo);
    hk::Claim(hk::Key::Save);
    hk::Claim(hk::Key::Delete);
    hk::Claim(hk::Key::Duplicate);
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save();
    if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
    if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) || (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))
        Redo();
    if (m_mode == Mode::Sheet && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) DeleteSelected();
    if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        m_zoom = 0.0f;
        m_userView = false;
        m_nineZoom = 0.0f;
    }
    if (m_mode == Mode::Sheet && !io.KeyCtrl) {
        // Инструменты — буквами, как подписаны в подсказках панели.
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) m_tool = Tool::Select;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false) && m_doc.Sheet.Slicing.Method == sage::ui::SpriteSlicing::Mode::Manual)
            m_tool = Tool::Rectangle;
        if (ImGui::IsKeyPressed(ImGuiKey_M, false)) m_tool = Tool::Move;
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) m_tool = Tool::Resize;
        if (ImGui::IsKeyPressed(ImGuiKey_P, false)) m_tool = Tool::Pivot;
    }
    // Стрелки — сдвиг выбранных на пиксель (с Shift — на десять).
    if (m_mode == Mode::Sheet && !m_sel.empty()) {
        glm::ivec2 d(0);
        const int step = io.KeyShift ? 10 : 1;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) d.x -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) d.x += step;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) d.y -= step;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) d.y += step;
        if (d != glm::ivec2(0)) {
            Snapshot();
            for (int i : m_sel)
                if (i >= 0 && i < (int)m_doc.Sheet.Regions.size())
                    m_doc.Sheet.Regions[(size_t)i].Rect = sp::MoveRect(m_doc.Sheet.Regions[(size_t)i].Rect, d, m_w, m_h);
        }
    }
}

// --- Вопросы -----------------------------------------------------------------

void SpriteEditor::DrawCloseModal(bool* open) {
    if (m_askClose && !ImGui::IsPopupOpen(T("Unsaved sprite sheet###SpriteEditorClose")))
        ImGui::OpenPopup(T("Unsaved sprite sheet###SpriteEditorClose"));
    if (!ImGui::BeginPopupModal(T("Unsaved sprite sheet###SpriteEditorClose"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextUnformatted(T("The sprite sheet has unsaved changes."));
    ImGui::TextDisabled("%s", T("Save them before closing?"));
    ImGui::Spacing();
    // Тот же вопрос — и при закрытии, и при открытии другой картинки
    // (m_pendingPath): ответ решает судьбу правок, а дальше — своё действие.
    auto proceed = [&] {
        if (!m_pendingPath.empty()) {
            Load(m_pendingPath);
            m_pendingPath.clear();
        } else if (open) {
            *open = false;
        }
    };
    if (ImGui::Button(T("Save"), ImVec2(110, 0))) {
        m_askClose = false;
        if (Save()) proceed();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Don't save"), ImVec2(110, 0))) {
        // Отказ от правок — вернуть документ к сохранённому: при следующем
        // открытии окна не должно всплыть брошенное.
        m_askClose = false;
        Load(m_path);
        proceed();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_askClose = false;
        m_pendingPath.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void SpriteEditor::DrawReplaceModal() {
    // Вопрос держится, пока на него не ответили: окно, закрытое чужой сменой
    // фокуса (раскладка, другое окно), открывается снова, а не теряет вопрос.
    if (m_askReplace && !ImGui::IsPopupOpen(T("Sprites already exist###SpriteEditorReplace")))
        ImGui::OpenPopup(T("Sprites already exist###SpriteEditorReplace"));
    if (!ImGui::BeginPopupModal(T("Sprites already exist###SpriteEditorReplace"), nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text(T("The texture already has %d sprites."), (int)m_doc.Sheet.Regions.size());
    ImGui::TextDisabled("%s", T("Add the new ones next to them, or replace them all?"));
    ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
    ImGui::TextUnformatted(T("Replace removes the current sprites with their names, pivots and nine-slices."));
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (ImGui::Button(T("Add"), ImVec2(110, 0))) {
        CommitSlices(m_pendingRects, m_pendingPrefix, sp::SliceApply::Add);
        m_askReplace = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Replace"), ImVec2(110, 0))) {
        CommitSlices(m_pendingRects, m_pendingPrefix, sp::SliceApply::Replace);
        m_askReplace = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        m_askReplace = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
