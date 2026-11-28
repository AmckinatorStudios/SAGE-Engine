#include "sage/render/ResourceManager.h"
#include "SpriteEditor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>

#include "imgui.h"
#include <stb_image_write.h>

#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorPrefs.h"
#include "../EditorTheme.h"
#include "../InspectorControls.h"
#include "../Localization.h"
#include "../ui/UI.h"
#include "SpriteEditorDetail.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Paths.h"
#include "sage/render/Texture.h"

using sage::editor::T;
namespace sp = sage::editor::sprite;
namespace ictl = sage::editor::ictl;
using Method = sage::ui::SpriteSlicing::Mode;

namespace {

bool IntPair(EditorHost& host, const char* id, int* a, int* b, const char* ta, const char* tb, int min, int max) {
    float fa = (float)*a, fb = (float)*b;
    ictl::NumOpts o;
    o.Step = 1.0f;
    o.Min = (float)min;
    o.Max = (float)max;
    if (!ictl::Pair(host, id, &fa, &fb, ta, tb, o)) return false;
    *a = std::clamp((int)std::lround(fa), min, max);
    *b = std::clamp((int)std::lround(fb), min, max);
    return true;
}

bool SameGrid(const sage::ui::SpriteGrid& x, const sage::ui::SpriteGrid& y) {
    return x.CellW == y.CellW && x.CellH == y.CellH && x.PadX == y.PadX && x.PadY == y.PadY && x.OffX == y.OffX &&
           x.OffY == y.OffY && x.Margin == y.Margin && x.SkipEmpty == y.SkipEmpty;
}

bool SameAuto(const sage::ui::SpriteAuto& x, const sage::ui::SpriteAuto& y) {
    return x.AlphaCutoff == y.AlphaCutoff && x.MinW == y.MinW && x.MinH == y.MinH && x.Padding == y.Padding;
}

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Поле текста строкой «подпись слева», со снимком истории в начале правки.
bool TextRow(const char* label, std::string& value, const std::function<void()>& snapshot) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    ictl::Label(label);
    const bool changed = ImGui::InputText((std::string("##") + label).c_str(), buf, sizeof(buf));
    if (ImGui::IsItemActivated()) snapshot();
    if (changed) value = buf;
    return changed;
}

const char* PivotName(int preset) {
    static const char* const names[] = {"Top Left",    "Top Center",    "Top Right",
                                        "Center Left", "Center",        "Center Right",
                                        "Bottom Left", "Bottom Center", "Bottom Right"};
    return preset >= 0 && preset < 9 ? T(names[preset]) : T("Custom");
}

} // namespace

// --- Окно инспектора ------------------------------------------------------------

void SpriteEditor::DrawInspectorWindow(EditorHost& host) {
    bool visible = m_inspectorVisible;
    const bool shown = ImGui::Begin(T("Inspector###SpriteEditorInspector"), &visible);
    if (visible != m_inspectorVisible) {
        m_inspectorVisible = visible;
        sage::editor::prefs::SetBool("spriteEditor.inspector", visible);
    }
    if (!shown) {
        ImGui::End();
        return;
    }
    // Числа инспектора пишут в историю редактора спрайтов, а не сцены.
    ictl::UndoScope undo([this] { Snapshot(); });
    if (m_mode == Mode::NineSlice) {
        DrawNineInspector(host);
        ImGui::End();
        return;
    }
    if (ictl::BeginSection("sprite.texture", T("Texture"))) {
        const assetslot::Result r = assetslot::Draw(host, "##sp_tex", assetslot::Kind::Texture, m_path, nullptr,
                                                    T("Drop a texture here"));
        if (r.Changed) {
            if (m_dirty) {
                m_pendingPath = r.Path;
                m_askClose = true;
            } else {
                Load(r.Path);
            }
        }
        if (m_tex) {
            char size[64];
            std::snprintf(size, sizeof(size), "%d \xc3\x97 %d %s", m_w, m_h, T("pixels"));
            ImGui::TextDisabled("%s", size);
        }
        ictl::EndSection();
    }
    if (!m_tex || !m_pixels) {
        ImGui::End();
        return;
    }
    if (ictl::BeginSection("sprite.slicing", T("Slicing"))) {
        DrawSlicing(host);
        ictl::EndSection();
    }
    if (ictl::BeginSection("sprite.selected", T("Sprite"))) {
        DrawSelected(host);
        ictl::EndSection();
    }
    if (ictl::BeginSection("sprite.sequences", T("Sequences"), false)) {
        DrawSequences(host);
        ictl::EndSection();
    }
    if (ictl::BeginSection("sprite.atlas", T("Atlas from a folder"), false)) {
        DrawAtlas(host);
        ictl::EndSection();
    }
    ImGui::End();
}

// --- Нарезка ---------------------------------------------------------------------

void SpriteEditor::DrawSlicing(EditorHost& host) {
    sage::ui::SpriteSlicing& s = m_doc.Sheet.Slicing;
    const char* methods[] = {T("Grid"), T("Automatic"), T("Manual")};
    int method = (int)s.Method;
    ImGui::BeginGroup();
    if (sp::Segmented("##method", methods, 3, &method)) {
        Snapshot();
        s.Method = (Method)method;
    }
    ImGui::EndGroup();
    sp::ParamTip(T("Slicing method"),
                 T("Grid — equal cells. Automatic — every island of opaque pixels. Manual — draw rectangles yourself."),
                 T("Grid for a tileset, Automatic for scattered icons."));
    ImGui::Spacing();
    if (s.Method == Method::Grid) DrawGridParams(host);
    else if (s.Method == Method::Automatic) DrawAutoParams(host);
    else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", T("Pick the Rectangle tool on the canvas (R) and drag over the texture to add a "
                                    "sprite. Move and resize it with the handles."));
        ImGui::PopTextWrapPos();
        TextRow(T("Name Prefix"), s.Grid.Prefix, [this] { Snapshot(); });
        sp::ParamTip(T("Name Prefix"), T("The start of each new sprite's name; a number is added."),
                     T("coin → coin_0, coin_1, coin_2"));
        return;
    }
    ImGui::Spacing();
    if (Sage::UI::Button(T("Create Sprites"), Sage::UI::ButtonStyle::Primary,
                         ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
        CreateSprites();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Turn the preview into real sprites. Undo with Ctrl+Z."));
}

void SpriteEditor::DrawGridParams(EditorHost& host) {
    sage::ui::SpriteGrid& g = m_doc.Sheet.Slicing.Grid;
    bool changed = false;

    ImGui::BeginGroup();
    ictl::GroupTitle(T("Cell Size"));
    changed |= IntPair(host, "cell", &g.CellW, &g.CellH, "W", "H", 1, std::max(1, std::max(m_w, m_h)));
    ImGui::EndGroup();
    sp::ParamTip(T("Cell Size"), T("Size of one sprite in the grid, in pixels."),
                 T("16 × 16 for a tileset of 16-pixel tiles."));

    ImGui::BeginGroup();
    ictl::GroupTitle(T("Offset"));
    changed |= IntPair(host, "offset", &g.OffX, &g.OffY, "X", "Y", 0, std::max(0, std::max(m_w, m_h)));
    ImGui::EndGroup();
    sp::ParamTip(T("Offset"), T("Shift of the first cell from the top-left corner of the sliced area."),
                 T("Offset X 8 — the grid starts 8 pixels to the right."));

    ImGui::BeginGroup();
    ictl::GroupTitle(T("Spacing"));
    changed |= IntPair(host, "spacing", &g.PadX, &g.PadY, "H", "V", 0, std::max(0, std::max(m_w, m_h)));
    ImGui::EndGroup();
    sp::ParamTip(T("Spacing"), T("Gap between neighbouring cells: H — between columns, V — between rows."),
                 T("2 — sprites are separated by 2-pixel lines."));

    ImGui::BeginGroup();
    glm::vec4 margin(g.Margin);
    ictl::NumOpts mo;
    mo.Min = 0.0f;
    mo.Max = (float)std::max(m_w, m_h);
    if (ictl::Sides(host, T("Margin"), &margin, mo)) {
        g.Margin = glm::ivec4(glm::round(margin));
        changed = true;
    }
    ImGui::EndGroup();
    sp::ParamTip(T("Margin"), T("Border of the texture that is not sliced, for each side."),
                 T("4 on every side — the 4-pixel frame of the sheet is skipped."));

    changed |= TextRow(T("Name Prefix"), g.Prefix, [this] { Snapshot(); });
    sp::ParamTip(T("Name Prefix"), T("The start of each new sprite's name; a number is added."),
                 T("coin → coin_0, coin_1, coin_2"));
    ImGui::BeginGroup();
    changed |= ictl::Switch(host, T("Skip Empty Cells"), &g.SkipEmpty);
    ImGui::EndGroup();
    sp::ParamTip(T("Skip Empty Cells"), T("Fully transparent cells do not become sprites."),
                 T("A row of 8 cells with 7 frames gives 7 sprites."));
    if (changed) Changed();

    // Итог предпросмотра: сколько будет кусков и что не годится.
    int good = 0, empty = 0, outside = 0;
    for (const sage::ui::GridCell& c : GridPreview()) {
        if (!c.Inside) ++outside;
        else if (c.Empty && g.SkipEmpty) ++empty;
        else ++good;
    }
    ImGui::Spacing();
    ImGui::TextDisabled(T("%d sprites will be created"), good);
    if (empty > 0) ImGui::TextDisabled(T("%d empty cells skipped"), empty);
    if (outside > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Danger));
        ImGui::Text(T("%d cells go past the texture and are not created"), outside);
        ImGui::PopStyleColor();
    }
}

void SpriteEditor::DrawAutoParams(EditorHost& host) {
    sage::ui::SpriteAuto& p = m_doc.Sheet.Slicing.Auto;
    bool changed = false;
    ImGui::BeginGroup();
    changed |= ictl::SliderNumber(host, T("Alpha Threshold"), &p.AlphaCutoff, 0.0f, 1.0f, "%.2f", true, 0.05f);
    ImGui::EndGroup();
    sp::ParamTip(T("Alpha Threshold"), T("From what opacity a pixel counts as part of a sprite."),
                 T("0.05 — almost transparent shadows are ignored."));
    ImGui::BeginGroup();
    ictl::GroupTitle(T("Min Size"));
    changed |= IntPair(host, "min", &p.MinW, &p.MinH, "W", "H", 1, std::max(1, std::max(m_w, m_h)));
    ImGui::EndGroup();
    sp::ParamTip(T("Min Size"), T("Islands narrower than W or lower than H are skipped: dust and stray pixels."),
                 T("4 × 4 — specks under 4 pixels are not sprites."));
    ImGui::BeginGroup();
    {
        float pad = (float)p.Padding;
        ictl::NumOpts o;
        o.Min = 0.0f;
        o.Max = 64.0f;
        if (ictl::Number(host, T("Padding"), &pad, o)) {
            p.Padding = (int)std::lround(pad);
            changed = true;
        }
    }
    ImGui::EndGroup();
    sp::ParamTip(T("Padding"), T("Extra pixels kept around each found sprite."),
                 T("1 — a 1-pixel border around every sprite."));
    changed |= TextRow(T("Name Prefix"), p.Prefix, [this] { Snapshot(); });
    sp::ParamTip(T("Name Prefix"), T("The start of each new sprite's name; a number is added."),
                 T("icon → icon_0, icon_1, icon_2"));
    if (changed) Changed();
    ImGui::Spacing();
    if (m_autoRunning) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
        ImGui::TextUnformatted(T("Analyzing the texture..."));
        ImGui::PopStyleColor();
    } else if (m_autoValid) {
        ImGui::TextDisabled(T("%d sprites found"), (int)m_autoRects.size());
    } else {
        ImGui::TextDisabled("%s", T("Waiting for the values to settle..."));
    }
}

const std::vector<sage::ui::GridCell>& SpriteEditor::GridPreview() {
    const sage::ui::SpriteGrid& g = m_doc.Sheet.Slicing.Grid;
    if (!m_gridValid || !SameGrid(g, m_gridKey)) {
        m_gridCells = sage::ui::GridCells(m_w, m_h, g, m_pixels ? m_pixels->data() : nullptr);
        m_gridKey = g;
        m_gridValid = true;
    }
    return m_gridCells;
}

void SpriteEditor::PumpAuto() {
    if (m_autoRunning && m_autoJob.valid() &&
        m_autoJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        m_autoRects = m_autoJob.get();
        m_autoRunning = false;
        m_autoValid = SameAuto(m_autoKey, m_autoWanted);
    }
    if (!m_pixels || m_doc.Sheet.Slicing.Method != Method::Automatic) return;
    const sage::ui::SpriteAuto& now = m_doc.Sheet.Slicing.Auto;
    if (!SameAuto(now, m_autoWanted)) {
        m_autoWanted = now;
        m_autoChangedAt = Now();
        m_autoValid = false;
    }
    // Пауза после правки: пока тянут число, острова не пересчитываются на
    // каждом шаге — счёт начнётся, когда значение замрёт.
    if (m_autoRunning || m_autoValid || Now() - m_autoChangedAt < 0.25) return;
    m_autoKey = m_autoWanted;
    std::shared_ptr<const std::vector<unsigned char>> px = m_pixels;
    const int w = m_w, h = m_h;
    const sage::ui::SpriteAuto params = m_autoKey;
    m_autoJob = std::async(std::launch::async,
                           [px, w, h, params] { return sage::ui::FindIslands(px->data(), w, h, params); });
    m_autoRunning = true;
}

void SpriteEditor::CreateSprites() {
    const sage::ui::SpriteSlicing& s = m_doc.Sheet.Slicing;
    std::vector<glm::ivec4> rects;
    std::string prefix;
    if (s.Method == Method::Grid) {
        rects = sp::GridRects(m_w, m_h, s.Grid, m_pixels ? m_pixels->data() : nullptr);
        prefix = s.Grid.Prefix;
    } else if (s.Method == Method::Automatic) {
        // Ждать фоновый счёт незачем: нажали — значит, нужен ответ сейчас.
        rects = m_autoValid && !m_autoRunning ? m_autoRects
                                              : sage::ui::FindIslands(m_pixels->data(), m_w, m_h, s.Auto);
        prefix = s.Auto.Prefix;
    }
    if (rects.empty()) {
        SetStatus(T("Nothing to create: no cell fits the texture"), true);
        return;
    }
    if (!m_doc.Sheet.Regions().empty()) {
        m_pendingRects = rects;
        m_pendingPrefix = prefix;
        m_askReplace = true;
        return;
    }
    CommitSlices(rects, prefix, sp::SliceApply::Replace);
}

void SpriteEditor::CommitSlices(const std::vector<glm::ivec4>& rects, const std::string& prefix, sp::SliceApply how) {
    Snapshot();
    const int n = sp::ApplySlices(m_doc.Sheet, rects, prefix, how);
    m_sel.clear();
    m_nameFor = -1;
    char buf[96];
    std::snprintf(buf, sizeof(buf), T("Sprites created: %d"), n);
    SetStatus(buf);
}

// --- Выбранный кусок ---------------------------------------------------------------

void SpriteEditor::DrawSelected(EditorHost& host) {
    auto& regions = m_doc.Sheet.Regions();
    m_sel.erase(std::remove_if(m_sel.begin(), m_sel.end(), [&](int i) { return i < 0 || i >= (int)regions.size(); }),
                m_sel.end());
    if (m_sel.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", T("Click a sprite on the canvas to edit it. Ctrl+click or a frame selects several."));
        ImGui::PopTextWrapPos();
        return;
    }
    if (m_sel.size() > 1) {
        ImGui::Text(T("%d sprites selected"), (int)m_sel.size());
        DrawPivot(host, m_sel);
        ImGui::Spacing();
        if (Sage::UI::Button(T("Delete Sprites"), Sage::UI::ButtonStyle::Danger)) DeleteSelected();
        return;
    }
    const int i = Primary();
    const sage::ui::SpriteRegion& reg = regions[(size_t)i];

    // ИМЯ: уникально в листе. Занятое не применяется — поле подсвечивает
    // ошибку, пока имя не исправят (или Esc не вернёт прежнее).
    if (m_nameFor != i) {
        std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", reg.Name.c_str());
        m_nameFor = i;
        m_nameBad = false;
    }
    ImGui::BeginGroup();
    ictl::Label(T("Name"));
    if (m_nameBad) ImGui::PushStyleColor(ImGuiCol_FrameBg, EditorTheme::ColorAlpha(EditorTheme::Role::Danger, 0.35f));
    if (ImGui::InputText("##sp_name", m_nameBuf, sizeof(m_nameBuf)))
        m_nameBad = !sp::NameFree(m_doc.Sheet, m_nameBuf, i);
    if (m_nameBad) ImGui::PopStyleColor();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (!m_nameBad && reg.Name != m_nameBuf) {
            Snapshot();
            m_doc.Sheet.Rename(reg.Name, m_nameBuf);
        } else if (m_nameBad) {
            std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", reg.Name.c_str());
            m_nameBad = false;
        }
    }
    ImGui::EndGroup();
    sp::ParamTip(T("Name"), T("Unique name of the sprite: the game takes the sprite by it."), T("coin_0"));
    if (m_nameBad) {
        ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Danger));
        ImGui::TextUnformatted(m_nameBuf[0] ? T("This name is taken by another sprite") : T("The name cannot be empty"));
        ImGui::PopStyleColor();
    }

    // ПРЯМОУГОЛЬНИК — в пикселях от левого верхнего угла; меняется вживую
    // вместе с рамкой на холсте.
    ImGui::BeginGroup();
    ictl::GroupTitle(T("Rectangle"));
    glm::ivec4 r = reg.Rect;
    bool rc = IntPair(host, "pos", &r.x, &r.y, "X", "Y", 0, std::max(0, std::max(m_w, m_h) - 1));
    rc |= IntPair(host, "size", &r.z, &r.w, "W", "H", 1, std::max(1, std::max(m_w, m_h)));
    ImGui::EndGroup();
    sp::ParamTip(T("Rectangle"), T("Position and size of the sprite in the texture, in pixels from the top-left corner."),
                 T("X 32, Y 0, W 16, H 16 — the third 16-pixel tile of the first row."));
    if (rc) {
        r.x = std::clamp(r.x, 0, std::max(0, m_w - 1));
        r.y = std::clamp(r.y, 0, std::max(0, m_h - 1));
        r.z = std::clamp(r.z, 1, std::max(1, m_w - r.x));
        r.w = std::clamp(r.w, 1, std::max(1, m_h - r.y));
        m_doc.Sheet.SetRect((size_t)i, r);
        Changed();
    }

    DrawPivot(host, m_sel);

    // Девятина куска — краткой строкой и кнопкой в её режим.
    ImGui::Spacing();
    ictl::Label(T("Nine-Slice"));
    if (reg.Slice == glm::vec4(0.0f)) ImGui::TextDisabled("%s", T("none"));
    else {
        char sides[96];
        std::snprintf(sides, sizeof(sides), "L %.0f  R %.0f  T %.0f  B %.0f", reg.Slice.x, reg.Slice.z, reg.Slice.y,
                      reg.Slice.w);
        ImGui::TextDisabled("%s", sides);
    }
    if (EditorIcons::Button("nineslice", T("Edit Nine-Slice"), T("Open this sprite in the Nine-Slice mode")))
        m_mode = Mode::NineSlice;
    ImGui::SameLine();
    if (EditorIcons::Button("trash", T("Delete"), T("Delete the sprite (Delete)"))) DeleteSelected();
}

void SpriteEditor::DrawPivot(EditorHost& host, std::vector<int>& sel) {
    auto& regions = m_doc.Sheet.Regions();
    const sage::ui::SpriteRegion& first = regions[(size_t)sel.back()];
    const int preset = sp::PivotPresetOf(first.Pivot);
    ImGui::Spacing();
    ImGui::BeginGroup();
    ictl::GroupTitle(T("Pivot"));
    // Сетка 3×3: каждое положение — клетка, выбранное залито акцентом.
    const float cell = ImGui::GetFrameHeight() * 0.8f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + cell * 3 + 4, p.y + cell * 3 + 4), EditorTheme::Color32(EditorTheme::Role::Input), 4.0f);
    for (int k = 0; k < 9; ++k) {
        const ImVec2 c(p.x + 2 + cell * (float)(k % 3), p.y + 2 + cell * (float)(k / 3));
        ImGui::SetCursorScreenPos(c);
        ImGui::PushID(k);
        if (ImGui::InvisibleButton("##pv", ImVec2(cell, cell))) {
            Snapshot();
            for (int i : sel) m_doc.Sheet.SetPivot((size_t)i, sp::PivotPreset(k));
        }
        const bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetTooltip("%s", PivotName(k));
        ImGui::PopID();
        const ImVec2 m(c.x + cell * 0.5f, c.y + cell * 0.5f);
        const bool on = preset == k;
        dl->AddCircleFilled(m, on ? cell * 0.28f : cell * 0.14f,
                            on ? EditorTheme::Color32(EditorTheme::Role::Accent)
                               : EditorTheme::Color32(hov ? EditorTheme::Role::Text : EditorTheme::Role::TextFaint));
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + cell * 3 + 14.0f, p.y + 2.0f));
    ImGui::BeginGroup();
    ImGui::TextUnformatted(PivotName(preset));
    glm::vec2 pv = first.Pivot;
    ictl::NumOpts o;
    o.Step = 0.05f;
    o.Min = 0.0f;
    o.Max = 1.0f;
    o.Format = "%.2f";
    o.HasReset = true;
    o.Reset = 0.5f;
    if (ictl::Pair(host, "pivot", &pv.x, &pv.y, "X", "Y", o)) {
        for (int i : sel) m_doc.Sheet.SetPivot((size_t)i, glm::clamp(pv, glm::vec2(0.0f), glm::vec2(1.0f)));
        Changed();
    }
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p.x, std::max(ImGui::GetCursorScreenPos().y, p.y + cell * 3 + 6.0f)));
    ImGui::Dummy(ImVec2(1, 1));
    ImGui::EndGroup();
    sp::ParamTip(T("Pivot"), T("The point the sprite turns around and is placed by. On the canvas the Pivot tool (P) "
                               "drags it."),
                 T("Bottom Center for a character standing on the ground."));
}

// --- Последовательности -------------------------------------------------------------

// Куски по порядку и скорость — анимация из листа. Кадры набираются по началу
// имени (run_0, run_1, …); превью листает их здесь же.
void SpriteEditor::DrawSequences(EditorHost& host) {
    (void)host;
    ictl::Label(T("Name##seq"));
    ImGui::InputText("##seq_name", m_seqName, sizeof(m_seqName));
    ictl::Label(T("Sprites starting with"));
    ImGui::InputText("##seq_prefix", m_seqPrefix, sizeof(m_seqPrefix));
    if (ImGui::Button(T("Make sequence")) && m_seqName[0]) {
        Snapshot();
        const int n = m_doc.Sheet.SequenceFromPrefix(m_seqName, m_seqPrefix);
        SetStatus(T("Frames in the sequence: ") + std::to_string(n));
        for (int i = 0; i < (int)m_doc.Sheet.Sequences().size(); ++i)
            if (m_doc.Sheet.Sequences()[(size_t)i].Name == m_seqName) m_seqSelected = i;
    }
    for (int i = 0; i < (int)m_doc.Sheet.Sequences().size(); ++i) {
        ImGui::PushID(1000 + i);
        const sage::ui::SpriteSequence& q = m_doc.Sheet.Sequences()[(size_t)i];
        const std::string label = q.Name + "  (" + std::to_string(q.Frames.size()) + ")";
        if (ImGui::Selectable(label.c_str(), m_seqSelected == i)) m_seqSelected = i;
        ImGui::PopID();
    }
    if (m_seqSelected < 0 || m_seqSelected >= (int)m_doc.Sheet.Sequences().size()) return;
    const sage::ui::SpriteSequence& q = m_doc.Sheet.Sequences()[(size_t)m_seqSelected];
    ictl::NumOpts fo;
    fo.Step = 1.0f;
    fo.Min = 0.5f;
    fo.Max = 120.0f;
    fo.Format = "%.1f";
    float fps = q.Fps;
    bool loop = q.Loop;
    bool playbackChanged = ictl::Number(host, T("Frames per second"), &fps, fo);
    playbackChanged |= ictl::Switch(host, T("Loop"), &loop);
    if (playbackChanged) {
        m_doc.Sheet.SetPlayback((size_t)m_seqSelected, fps, loop);
        Changed();
    }
    if (ImGui::Button(T("Remove sequence"))) {
        Snapshot();
        m_doc.Sheet.RemoveSequence((size_t)m_seqSelected);
        m_seqSelected = -1;
        return;
    }
    // Превью: текущий кадр в рамке 96 пикселей.
    m_previewTime += ImGui::GetIO().DeltaTime;
    const sage::ui::SpriteRegion* frame = m_doc.Sheet.Find(q.FrameAt(m_previewTime));
    if (!frame || frame->Rect.z <= 0 || frame->Rect.w <= 0) return;
    const float k = 96.0f / (float)std::max(frame->Rect.z, frame->Rect.w);
    const ImVec2 sz((float)frame->Rect.z * k, (float)frame->Rect.w * k);
    const float u0 = (float)frame->Rect.x / (float)m_w, u1 = (float)(frame->Rect.x + frame->Rect.z) / (float)m_w;
    const float v0 = (float)frame->Rect.y / (float)m_h;
    const float v1 = (float)(frame->Rect.y + frame->Rect.w) / (float)m_h;
    sp::Nearest(ImGui::GetWindowDrawList());
    ImGui::Image((ImTextureID)(std::intptr_t)m_tex->NativeHandle(), sz, ImVec2(u0, v0), ImVec2(u1, v1));
    sp::Linear(ImGui::GetWindowDrawList());
}

// АТЛАС: все картинки папки — в одну, с листом кусков по именам файлов.
// Пишется рядом с папкой: «<папка>_atlas.png» и его «.sagesheet»; атлас
// сразу открывается здесь же.
void SpriteEditor::DrawAtlas(EditorHost& host) {
    const assetslot::Result r = assetslot::Draw(host, "##sp_atlas", assetslot::Kind::Folder, m_atlasFolder, nullptr,
                                                T("A folder of pictures to pack into one atlas"));
    if (r.Changed) m_atlasFolder = r.Path;
    if (m_atlasFolder.empty() || !ImGui::Button(T("Pack into an atlas"))) return;
    namespace fs = std::filesystem;
    const fs::path dir = sage::PathFromUtf8(sage::AssetDatabase::Instance().LocatePath(m_atlasFolder));
    std::vector<std::shared_ptr<const sage::assets::SpriteImage>> pixels;
    std::vector<sage::ui::AtlasInput> inputs;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string ext = entry.path().extension().string();
        if (entry.is_regular_file() && (ext == ".png" || ext == ".PNG") &&
            sage::PathToUtf8(entry.path().filename()).rfind(".sage-atlas-", 0) != 0) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    pixels.reserve(files.size());
    for (const fs::path& f : files) {
        auto source = ResourceManager::Instance().ResolveSpriteImage(sage::PathToUtf8(f), true);
        if (!source || !source.Resource->Image) {
            SetStatus(T("Could not pack: ") + source.Diagnostic, true);
            return;
        }
        const auto& image = source.Resource->Image;
        pixels.push_back(image);
        inputs.push_back({sage::PathToUtf8(f.stem()), image->Pixels.data(), image->Width, image->Height});
    }
    std::vector<unsigned char> atlas;
    int aw = 0, ah = 0;
    sage::ui::SpriteSheet sheet;
    std::string err;
    if (!sage::ui::SpriteSheet::PackAtlas(inputs, 2, 4096, atlas, aw, ah, sheet, err)) {
        SetStatus(T("Could not pack: ") + err, true);
        return;
    }
    const fs::path out = dir.parent_path() / sage::PathFromUtf8(sage::PathToUtf8(dir.filename()) + "_atlas.png");
    const std::string outputPath = sage::PathToUtf8(out);
    sage::ui::SpriteSheet document;
    if (!sage::ui::SpriteSheet::LoadForImage(outputPath, document, err)) {
        SetStatus(T("Could not save: ") + err, true);
        return;
    }
    for (size_t i = document.Regions().size(); i-- > 0;)
        if (!sheet.Find(document.Regions()[i].Name))
            document.Remove(document.Regions()[i].Name);
    for (const auto& packed : sheet.Regions()) {
        if (const auto* existing = document.Find(packed.Name)) {
            auto updated = *existing;
            updated.Rect = packed.Rect;
            document.Set(updated);
        } else {
            document.Set(packed);
        }
    }
    std::string png;
    auto append = [](void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data), size);
    };
    if (!document.Validate(err, aw, ah) ||
        !stbi_write_png_to_func(append, &png, aw, ah, 4, atlas.data(), aw * 4) ||
        !sp::PublishAtlas(outputPath, png, document, err)) {
        SetStatus(T("Could not save: ") + err, true);
        return;
    }
    const std::string ref =
        m_atlasFolder.substr(0, m_atlasFolder.find_last_of('/') + 1) + sage::PathToUtf8(out.filename());
    const auto sheetRef = sage::ui::SpriteSheet::SidecarPath(ref);
    auto& database = sage::AssetDatabase::Instance();
    if (!database.ProjectDir().empty() && !sage::PathFromUtf8(ref).is_absolute()) {
        const auto guid = database.Register(sheetRef);
        if (guid.Valid()) ResourceManager::Instance().NotifySpriteChanged(guid);
        else LOG_WARN("Sprites") << "Could not register sprite sheet: " << sheetRef;
    }
    Load(ref);
    SetStatus(T("Atlas packed: ") + std::to_string(inputs.size()) + " / " + std::to_string(aw) + "x" +
              std::to_string(ah));
}
