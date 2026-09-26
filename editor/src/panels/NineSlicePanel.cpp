#include "NineSlicePanel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>

#include "imgui_internal.h" // MarkItemEdited: перетаскивание линии — одна запись в истории

#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../Localization.h"

#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"
#include "sage/scene/Scene.h"
#include "sage/ui/ImageFit.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPart.h"

namespace fs = std::filesystem;
namespace ui = sage::ui;

using sage::ui::NineSlice;
using sage::ui::SliceFill;
using sage::ui::SliceQuad;
using sage::ui::SliceRequest;

namespace {

// Три роли куска — три цвета, те же в легенде. Углы — самое важное (они не
// тянутся вовсе), поэтому у них самый заметный.
constexpr ImU32 kCornerTint = IM_COL32(255, 196, 64, 70);
constexpr ImU32 kEdgeTint = IM_COL32(96, 176, 255, 55);
constexpr ImU32 kCenterTint = IM_COL32(128, 255, 160, 40);
constexpr ImU32 kGuide = IM_COL32(255, 210, 90, 230);
constexpr ImU32 kGuideHot = IM_COL32(255, 255, 255, 255);

// Полоса захвата линии — в ЭКРАННЫХ пикселях: за линию одинаково легко
// взяться и на увеличении 16x, и на «вписать».
constexpr float kGrabPx = 6.0f;

void DrawCheckers(ImDrawList* dl, ImVec2 a, ImVec2 b, float cell) {
    dl->AddRectFilled(a, b, IM_COL32(58, 58, 62, 255));
    for (float y = a.y; y < b.y; y += cell) {
        const bool odd = std::fmod((y - a.y) / cell, 2.0f) >= 1.0f;
        for (float x = a.x + (odd ? cell : 0.0f); x < b.x; x += cell * 2.0f)
            dl->AddRectFilled({x, y}, {std::min(x + cell, b.x), std::min(y + cell, b.y)},
                              IM_COL32(74, 74, 79, 255));
    }
}

const ui::PartField* FindField(const std::vector<ui::PartField>& fields, const std::string& key) {
    for (const ui::PartField& f : fields)
        if (f.Key && key == f.Key) return &f;
    return nullptr;
}

// «hoverLook.sliceBorder» -> «hoverLook.»; «sliceBorder» -> «».
std::string PrefixOf(const std::string& borderKey) {
    const std::string tail = "sliceBorder";
    if (borderKey.size() >= tail.size() &&
        borderKey.compare(borderKey.size() - tail.size(), tail.size(), tail) == 0)
        return borderKey.substr(0, borderKey.size() - tail.size());
    return {};
}

// Поля одного вида нарезки внутри части: у картинки ключи свои («path»,
// «mode»), у вида — «texture», «fit» с приставкой. Ищутся по таблице части,
// а не списком в редакторе: у части игры с девятиной окно заработает само.
struct SliceFields {
    const ui::PartField* Border = nullptr;
    const ui::PartField* Center = nullptr;
    const ui::PartField* Edge = nullptr;
    const ui::PartField* DrawCenter = nullptr;
    const ui::PartField* Sprite = nullptr;
    const ui::PartField* Path = nullptr;
    const ui::PartField* Fit = nullptr;
    const ui::PartField* PixelScale = nullptr;
    const ui::PartField* Snap = nullptr;
    const ui::PartField* LookHeader = nullptr;
};

SliceFields Locate(const std::vector<ui::PartField>& fields, const std::string& borderKey) {
    const std::string pre = PrefixOf(borderKey);
    SliceFields s;
    s.Border = FindField(fields, borderKey);
    s.Center = FindField(fields, pre + "sliceCenterFill");
    s.Edge = FindField(fields, pre + "sliceEdgeFill");
    s.DrawCenter = FindField(fields, pre + "sliceDrawCenter");
    s.Sprite = FindField(fields, pre + "sprite");
    s.Path = FindField(fields, pre + "texture");
    if (!s.Path) s.Path = FindField(fields, pre + "path");
    s.Fit = FindField(fields, pre + "fit");
    if (!s.Fit) s.Fit = FindField(fields, pre + "mode");
    s.PixelScale = FindField(fields, pre + "pixelScale");
    s.Snap = FindField(fields, pre + "snapPixels");
    if (!pre.empty()) s.LookHeader = FindField(fields, pre.substr(0, pre.size() - 1));
    return s;
}

} // namespace

void NineSlicePanel::OpenFor(const EditorHost::NineSliceTarget& target) {
    m_target = target;
    if (target.ElementId == 0 && target.ImagePath != m_filePath) {
        m_filePath = target.ImagePath;
        m_fileSlice = NineSlice{};
        // Описание рядом с картинкой подхватывается само.
        NineSlice loaded;
        std::string err;
        if (!m_filePath.empty() && NineSlice::LoadFile(NineSlice::SidecarPath(m_filePath), loaded, err))
            m_fileSlice = loaded;
    }
    m_zoom = 0.0f;
    m_pan = ImVec2(0.0f, 0.0f);
    m_previewSize = glm::vec2(0.0f);
    m_status.clear();
}

void NineSlicePanel::ForgetProject() {
    m_target = {};
    m_filePath.clear();
    m_fileSlice = NineSlice{};
    m_texPath.clear();
    m_tex.reset();
    m_status.clear();
}

void NineSlicePanel::EnsureTexture(const std::string& path) {
    if (path == m_texPath && (m_tex || path.empty())) return;
    m_texPath = path;
    // Ближайшим соседом и без мипмапов: считать пиксели угла на размытой
    // картинке нельзя.
    m_tex = path.empty() ? nullptr
                         : ResourceManager::Instance().GetTexture(path, TextureFilter::Nearest, false);
    m_zoom = 0.0f;
}

bool NineSlicePanel::Read(EditorHost& host, Live& out) {
    out = Live{};
    if (m_target.ElementId != 0) {
        Scene& scene = host.CurrentScene();
        entt::registry& reg = scene.Registry();
        GameObject obj = scene.Get(m_target.ElementId);
        const ui::PartType* part = ui::FindPart(m_target.PartId);
        if (obj.Valid() && part && part->Fields && part->Has(reg, obj.Entity())) {
            const void* data = part->Get(reg, obj.Entity());
            const std::vector<ui::PartField> fields = ui::EditableFields(*part->Fields);
            const SliceFields f = Locate(fields, m_target.BorderKey);
            if (data && f.Border) {
                out.Bound = true;
                out.Slice.SetBorder(ui::FieldAs<glm::vec4>(data, *f.Border));
                if (f.Center) out.Slice.CenterFill = (SliceFill)ui::FieldAs<int>(data, *f.Center);
                if (f.Edge) out.Slice.EdgeFill = (SliceFill)ui::FieldAs<int>(data, *f.Edge);
                if (f.DrawCenter) out.Slice.DrawCenter = ui::FieldAs<bool>(data, *f.DrawCenter);
                if (f.Sprite) out.Sprite = ui::FieldAs<glm::vec4>(data, *f.Sprite);
                if (f.Path) out.Path = ui::FieldAs<std::string>(data, *f.Path);
                const float ps = f.PixelScale ? ui::FieldAs<float>(data, *f.PixelScale) : 0.0f;
                const bool snap = f.Snap && ui::FieldAs<bool>(data, *f.Snap);
                out.PixelScale = ui::SlicedPixelScale(ps, snap, 1.0f);
                if (const ui::Element* el = reg.try_get<ui::Element>(obj.Entity()))
                    out.ElementSize = (el->Resolved.x > 0.0f && el->Resolved.y > 0.0f) ? el->Resolved
                                                                                    : el->Size;
                out.Title = obj.Name() + "  \xe2\x80\xba  " + T(part->Title);
                if (f.LookHeader) out.Title += std::string("  \xe2\x80\xba  ") + T(f.LookHeader->Label);
                return true;
            }
        }
        // Элемента или вида больше нет (удалили, сменили тип) — остаётся
        // картинка, которую правили: закрывать окно посреди работы незачем.
        m_filePath = out.Path.empty() ? m_texPath : out.Path;
        m_target.ElementId = 0;
    }
    out.Path = m_filePath;
    out.Slice = m_fileSlice;
    out.PixelScale = 1.0f;
    out.Title = T("A picture, without an element");
    return true;
}

void NineSlicePanel::Write(EditorHost& host, const NineSlice& slice) {
    if (m_target.ElementId == 0) {
        m_fileSlice = slice;
        return;
    }
    Scene& scene = host.CurrentScene();
    entt::registry& reg = scene.Registry();
    GameObject obj = scene.Get(m_target.ElementId);
    const ui::PartType* part = ui::FindPart(m_target.PartId);
    if (!obj.Valid() || !part || !part->Fields || !part->Has(reg, obj.Entity())) return;
    void* data = part->GetMutable(reg, obj.Entity());
    const std::vector<ui::PartField> fields = ui::EditableFields(*part->Fields);
    const SliceFields f = Locate(fields, m_target.BorderKey);
    if (!data || !f.Border) return;
    ui::FieldAs<glm::vec4>(data, *f.Border) = slice.Border();
    if (f.Center) ui::FieldAs<int>(data, *f.Center) = (int)slice.CenterFill;
    if (f.Edge) ui::FieldAs<int>(data, *f.Edge) = (int)slice.EdgeFill;
    if (f.DrawCenter) ui::FieldAs<bool>(data, *f.DrawCenter) = slice.DrawCenter;
    // Правят девятину — значит, режим девятины: иначе числа меняются, а
    // элемент остаётся растянутым, и это выглядит как «не работает».
    if (f.Fit) ui::FieldAs<int>(data, *f.Fit) = (int)ui::Image::Mode::NineSlice;
}

void NineSlicePanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(900.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 300.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin(T("9-slice editor###NineSlice"), &open)) {
        ImGui::End();
        return;
    }

    Live live;
    Read(host, live);
    EnsureTexture(live.Path);

    DrawHeader(host, live);
    ImGui::Separator();

    // Таблица из двух столбцов: картинка и всё остальное. Ширину делит сама
    // таблица, а каждый столбец — дочернее окно своего размера с прокруткой:
    // содержимое не может вылезти за окно ни при какой ширине.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.y > 40.0f &&
        ImGui::BeginTable("##ns_layout", 2,
                          ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("canvas", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        ImGui::TableSetupColumn("side", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const float h = ImGui::GetContentRegionAvail().y;
        if (ImGui::BeginChild("##ns_canvas", ImVec2(0.0f, h), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            DrawCanvas(host, live, ImGui::GetContentRegionAvail());
        }
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        if (ImGui::BeginChild("##ns_side", ImVec2(0.0f, h))) DrawSide(host, live);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}

void NineSlicePanel::DrawHeader(EditorHost& host, Live& live) {
    ImGui::TextUnformatted(T("Editing:"));
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "%s", live.Title.c_str());

    if (!live.Bound) {
        // Без элемента картинка выбирается слотом, как любой ассет.
        const assetslot::Result r = assetslot::Draw(host, "##ns_image", assetslot::Kind::Texture,
                                                    m_filePath, nullptr, T("Drop a picture here"));
        if (r.Changed) {
            EditorHost::NineSliceTarget t;
            t.ImagePath = r.Path;
            OpenFor(t);
            Read(host, live);
            EnsureTexture(live.Path);
        }
    }
    if (!m_tex) {
        ImGui::TextDisabled("%s", T("No picture: pick one in the element's look, or drop it here."));
        return;
    }

    if (ImGui::Button(T("Guess"))) {
        NineSlice guess;
        if (ui::GuessBorderFromFile(live.Path, guess)) {
            guess.CenterFill = live.Slice.CenterFill;
            guess.EdgeFill = live.Slice.EdgeFill;
            guess.DrawCenter = live.Slice.DrawCenter;
            host.PushUndoSnapshot();
            Write(host, guess);
            host.PushUndoSnapshot();
            m_status = T("Guessed from the picture — check and correct");
        } else {
            m_status = T("Could not guess: the picture has no plain border");
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Looks for plain strips along the edges — the corners."));
    ImGui::SameLine();
    if (ImGui::Button(T("Save .sage9"))) {
        std::string err;
        if (live.Slice.SaveFile(NineSlice::SidecarPath(live.Path), err))
            m_status = T("Saved next to the picture");
        else
            m_status = T("Could not save: ") + err;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("The cut is kept next to the picture, so any other element\n"
                                  "can load it with one click."));
    ImGui::SameLine();
    if (ImGui::Button(T("Load .sage9"))) {
        NineSlice loaded;
        std::string err;
        if (NineSlice::LoadFile(NineSlice::SidecarPath(live.Path), loaded, err)) {
            host.PushUndoSnapshot();
            Write(host, loaded);
            host.PushUndoSnapshot();
            m_status = T("Loaded next to the picture");
        } else {
            m_status = T("No description next to the picture");
        }
    }
    if (!m_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_status.c_str());
    }
}

void NineSlicePanel::DrawCanvas(EditorHost& host, Live& live, ImVec2 size) {
    if (!m_tex || size.x < 16.0f || size.y < 16.0f) {
        ImGui::TextDisabled("%s", T("No picture."));
        return;
    }
    const float tw = (float)m_tex->Width(), th = (float)m_tex->Height();
    if (tw <= 0.0f || th <= 0.0f) return;
    // Кусок листа, который режем: нарезка считается ОТ НЕГО, а не от файла.
    const bool whole = live.Sprite.z <= 0.0f || live.Sprite.w <= 0.0f;
    const float sx = whole ? 0.0f : live.Sprite.x, sy = whole ? 0.0f : live.Sprite.y;
    const float iw = whole ? tw : live.Sprite.z, ih = whole ? th : live.Sprite.w;

    ImGui::InvisibleButton("##ns_area", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    host.TrackLastImGuiItem();
    const ImGuiID areaId = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool activated = ImGui::IsItemActivated();
    const ImVec2 origin = ImGui::GetItemRectMin();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    if (m_zoom <= 0.0f) {
        m_zoom = std::max(0.25f, std::min((size.x - 24.0f) / iw, (size.y - 24.0f) / ih));
        if (m_zoom >= 1.0f) m_zoom = std::floor(m_zoom);
        m_pan = ImVec2((size.x - iw * m_zoom) * 0.5f, (size.y - ih * m_zoom) * 0.5f);
    }
    // Колесо — увеличение вокруг курсора; средняя кнопка — сдвиг.
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        const float before = m_zoom;
        m_zoom = std::clamp(m_zoom * (ImGui::GetIO().MouseWheel > 0.0f ? 1.25f : 0.8f), 0.25f, 48.0f);
        const ImVec2 at(mouse.x - origin.x - m_pan.x, mouse.y - origin.y - m_pan.y);
        m_pan.x -= at.x * (m_zoom / before - 1.0f);
        m_pan.y -= at.y * (m_zoom / before - 1.0f);
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        m_pan.x += ImGui::GetIO().MouseDelta.x;
        m_pan.y += ImGui::GetIO().MouseDelta.y;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 areaB(origin.x + size.x, origin.y + size.y);
    dl->PushClipRect(origin, areaB, true);
    dl->AddRectFilled(origin, areaB, IM_COL32(34, 34, 38, 255));
    const ImVec2 a(origin.x + m_pan.x, origin.y + m_pan.y);
    const ImVec2 b(a.x + iw * m_zoom, a.y + ih * m_zoom);
    if (m_showCheckers) DrawCheckers(dl, a, b, std::max(4.0f, m_zoom * 4.0f));
    // V переворачивается: у текстуры движка начало координат внизу.
    dl->AddImage((ImTextureID)(std::intptr_t)m_tex->NativeHandle(), a, b,
                 ImVec2(sx / tw, 1.0f - sy / th), ImVec2((sx + iw) / tw, 1.0f - (sy + ih) / th));

    NineSlice& s = live.Slice;
    const float l = std::clamp(s.Left, 0.0f, iw), r = std::clamp(s.Right, 0.0f, iw - l);
    const float t = std::clamp(s.Top, 0.0f, ih), bt = std::clamp(s.Bottom, 0.0f, ih - t);
    const float gx[4] = {a.x, a.x + l * m_zoom, b.x - r * m_zoom, b.x};
    const float gy[4] = {a.y, a.y + t * m_zoom, b.y - bt * m_zoom, b.y};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            if (gx[col + 1] <= gx[col] || gy[row + 1] <= gy[row]) continue;
            const bool corner = row != 1 && col != 1;
            const bool center = row == 1 && col == 1;
            if (center && !s.DrawCenter) continue;
            dl->AddRectFilled({gx[col], gy[row]}, {gx[col + 1], gy[row + 1]},
                              corner ? kCornerTint : (center ? kCenterTint : kEdgeTint));
        }

    // Линии: ближайшая к курсору подсвечивается и берётся мышью.
    const float lines[4] = {gx[1], gy[1], gx[2], gy[2]};
    const bool vertical[4] = {true, false, true, false};
    int hot = m_dragEdge;
    if (hot < 0 && hovered) {
        float best = kGrabPx;
        for (int i = 0; i < 4; ++i) {
            const float d = std::fabs((vertical[i] ? mouse.x : mouse.y) - lines[i]);
            if (d <= best) { best = d; hot = i; }
        }
    }
    if (hot >= 0) ImGui::SetMouseCursor(vertical[hot] ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (activated && hot >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_dragEdge = hot;
    if (m_dragEdge >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_dragEdge = -1;
    if (m_dragEdge >= 0 && active) {
        // Целые пиксели исходника: нарезка по полпикселя даёт на экране шов.
        const int i = m_dragEdge;
        const float px = vertical[i] ? (mouse.x - a.x) / m_zoom : (mouse.y - a.y) / m_zoom;
        const float fromEdge = (i == 2) ? iw - px : (i == 3) ? ih - px : px;
        const float limit = (i == 0 || i == 2) ? iw : ih;
        float* edge[4] = {&s.Left, &s.Top, &s.Right, &s.Bottom};
        const float v = std::clamp(std::round(fromEdge), 0.0f, limit);
        if (v != *edge[i]) {
            *edge[i] = v;
            Write(host, s);
            ImGui::MarkItemEdited(areaId);   // одна запись истории на всё движение
        }
    }
    for (int i = 0; i < 4; ++i) {
        const ImU32 col = i == hot ? kGuideHot : kGuide;
        const float thick = i == hot ? 2.5f : 1.5f;
        if (vertical[i]) dl->AddLine({lines[i], origin.y}, {lines[i], areaB.y}, col, thick);
        else dl->AddLine({origin.x, lines[i]}, {areaB.x, lines[i]}, col, thick);
    }
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 70));
    char info[128];
    std::snprintf(info, sizeof(info), "%.0fx%.0f  %.0f%%", iw, ih, m_zoom * 100.0f);
    dl->AddText({origin.x + 6.0f, areaB.y - 18.0f}, IM_COL32(210, 210, 215, 210), info);
    dl->PopClipRect();
}

void NineSlicePanel::DrawSide(EditorHost& host, Live& live) {
    NineSlice s = live.Slice;
    const float iw = m_tex ? (float)(live.Sprite.z > 0.0f ? live.Sprite.z : m_tex->Width()) : 4096.0f;
    const float ih = m_tex ? (float)(live.Sprite.w > 0.0f ? live.Sprite.w : m_tex->Height()) : 4096.0f;

    ImGui::SeparatorText(T("Fixed edges, source pixels"));
    // Целые числа: у нарезки не бывает долей пикселя.
    int v[4] = {(int)s.Left, (int)s.Top, (int)s.Right, (int)s.Bottom};
    const char* names[4] = {T("Left"), T("Top"), T("Right"), T("Bottom")};
    const int limits[4] = {(int)iw, (int)ih, (int)iw, (int)ih};
    bool changed = false;
    ImGui::PushItemWidth(-ImGui::CalcTextSize(T("Bottom")).x - 12.0f);
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        if (ImGui::DragInt(names[i], &v[i], 0.2f, 0, limits[i])) changed = true;
        host.TrackLastImGuiItem();
        ImGui::PopID();
    }
    if (changed) {
        s.Left = (float)v[0]; s.Top = (float)v[1]; s.Right = (float)v[2]; s.Bottom = (float)v[3];
        Write(host, s);
    }

    ImGui::SeparatorText(T("Stretching pieces"));
    const char* fills[] = {T("Stretch"), T("Repeat")};
    int center = (int)s.CenterFill, edge = (int)s.EdgeFill;
    if (ImGui::Combo(T("Middle"), &center, fills, 2)) {
        s.CenterFill = (SliceFill)center;
        Write(host, s);
        host.PushUndoSnapshot();
    }
    if (ImGui::Combo(T("Edges"), &edge, fills, 2)) {
        s.EdgeFill = (SliceFill)edge;
        Write(host, s);
        host.PushUndoSnapshot();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Repeat keeps an ornament crisp; stretch smears it."));
    if (ImGui::Checkbox(T("Draw the middle"), &s.DrawCenter)) {
        Write(host, s);
        host.PushUndoSnapshot();
    }
    ImGui::PopItemWidth();
    ImGui::Checkbox(T("Checkerboard under transparency"), &m_showCheckers);

    // Легенда — те же три цвета, что на картинке.
    ImGui::SeparatorText(T("What stretches"));
    auto legend = [](ImU32 col, const char* text) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        ImGui::Dummy(ImVec2(h, h));
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + h, p.y + h), col | IM_COL32(0, 0, 0, 255));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", text);
    };
    legend(kCornerTint, T("Corners — never stretch"));
    legend(kEdgeTint, T("Edges — stretch along one side"));
    legend(kCenterTint, T("Middle — stretches both ways"));

    live.Slice = s;
    DrawPreview(live);
}

void NineSlicePanel::DrawPreview(const Live& live) {
    ImGui::SeparatorText(T("Preview"));
    if (!m_tex) {
        ImGui::TextDisabled("%s", T("No picture."));
        return;
    }
    if (m_previewSize.x <= 0.0f || m_previewSize.y <= 0.0f)
        m_previewSize = (live.ElementSize.x > 0.0f && live.ElementSize.y > 0.0f) ? live.ElementSize
                                                                                 : glm::vec2(240.0f, 96.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloat2("##ns_size", &m_previewSize.x, 1.0f, 8.0f, 4096.0f, "%.0f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Preview size. It starts at the size of the element."));
    if (live.Bound && ImGui::Button(T("Element size"))) m_previewSize = live.ElementSize;

    // Предпросмотр ВПИСЫВАЕТСЯ в доступную ширину: крупный элемент показан
    // уменьшенным целиком, а не вылезает за окно.
    const float availW = std::max(40.0f, ImGui::GetContentRegionAvail().x);
    const float k = std::min(1.0f, std::min(availW / m_previewSize.x, 320.0f / m_previewSize.y));
    const ImVec2 box(m_previewSize.x * k, m_previewSize.y * k);
    ImGui::InvisibleButton("##ns_preview", box);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (m_showCheckers) DrawCheckers(dl, a, b, 8.0f);

    // Тем же решателем, что и в игре (sage::ui::Solve): предпросмотр по своим
    // правилам — второй источник правды.
    const float tw = (float)m_tex->Width(), th = (float)m_tex->Height();
    const bool whole = live.Sprite.z <= 0.0f || live.Sprite.w <= 0.0f;
    SliceRequest req;
    req.SrcX = whole ? 0.0f : live.Sprite.x;
    req.SrcY = whole ? 0.0f : live.Sprite.y;
    req.SrcW = whole ? tw : live.Sprite.z;
    req.SrcH = whole ? th : live.Sprite.w;
    req.DstX = a.x; req.DstY = a.y; req.DstW = box.x; req.DstH = box.y;
    req.Scale = live.PixelScale * k;
    const ImTextureID id = (ImTextureID)(std::intptr_t)m_tex->NativeHandle();
    dl->PushClipRect(a, b, true);
    for (const SliceQuad& q : ui::Solve(live.Slice, req)) {
        const ImVec2 uv0(q.SrcX / tw, 1.0f - q.SrcY / th);
        const ImVec2 uv1((q.SrcX + q.SrcW) / tw, 1.0f - (q.SrcY + q.SrcH) / th);
        dl->AddImage(id, {q.DstX, q.DstY}, {q.DstX + q.DstW, q.DstY + q.DstH}, uv0, uv1);
    }
    dl->PopClipRect();
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 70));
    if (k < 0.999f) ImGui::TextDisabled(T("Shown at %.0f%%"), k * 100.0f);
}
