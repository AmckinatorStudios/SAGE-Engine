#include "NineSlicePanel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../Localization.h"
#include "../Project.h"

#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"

namespace fs = std::filesystem;

using sage::ui::NineSlice;
using sage::ui::SliceFill;
using sage::ui::SliceQuad;
using sage::ui::SliceRequest;

namespace {

// Оттенки для трёх ролей куска. Не «красиво», а по делу: из чего состоит
// нарезка, должно читаться с картинки без легенды. Углы — самое важное (они не
// тянутся вовсе), поэтому у них самый заметный оттенок.
constexpr ImU32 kCornerTint = IM_COL32(255, 196, 64, 46);
constexpr ImU32 kEdgeTint = IM_COL32(96, 176, 255, 34);
constexpr ImU32 kCenterTint = IM_COL32(128, 255, 160, 22);
constexpr ImU32 kGuide = IM_COL32(255, 210, 90, 220);
constexpr ImU32 kGuideHot = IM_COL32(255, 255, 255, 255);

// Толщина полосы захвата направляющей в ЭКРАННЫХ пикселях. Не в пикселях
// картинки: на увеличении 16x пиксель картинки занимает полэкрана, а на
// «вписать» — треть экранного, и в обоих случаях за направляющую надо
// попадать одинаково легко.
constexpr float kGrabPx = 5.0f;

// Шахматка под прозрачностью: у рамок интерфейса прозрачные углы — норма, и
// на сплошном фоне их край не виден вовсе.
void DrawCheckers(ImDrawList* dl, ImVec2 a, ImVec2 b, float cell) {
    dl->AddRectFilled(a, b, IM_COL32(58, 58, 62, 255));
    for (float y = a.y; y < b.y; y += cell) {
        for (float x = a.x + (std::fmod((y - a.y) / cell, 2.0f) < 1.0f ? 0.0f : cell);
             x < b.x; x += cell * 2.0f) {
            dl->AddRectFilled({x, y}, {std::min(x + cell, b.x), std::min(y + cell, b.y)},
                              IM_COL32(74, 74, 79, 255));
        }
    }
}

} // namespace

void NineSlicePanel::OpenFor(const std::string& imagePath) {
    if (imagePath != m_path) LoadImage(imagePath);
}

void NineSlicePanel::ForgetProject() {
    m_path.clear();
    m_tex.reset();
    m_slice = NineSlice{};
    m_status.clear();
}

void NineSlicePanel::LoadImage(const std::string& path) {
    m_path = path;
    m_tex = path.empty() ? nullptr : ResourceManager::Instance().GetTexture(path);
    m_slice = NineSlice{};
    m_zoom = 0.0f; // пересчитается под размер холста при первом кадре
    m_pan = ImVec2(0.0f, 0.0f);
    m_status.clear();
    if (path.empty()) return;

    // Описание, лежащее рядом с картинкой, подхватывается САМО. Иначе первое,
    // что делал бы человек после выбора файла, — вспоминал, что надо нажать
    // «Загрузить», и до тех пор правил бы нули поверх готовой нарезки.
    NineSlice loaded;
    std::string err;
    if (NineSlice::LoadFile(NineSlice::SidecarPath(path), loaded, err)) {
        m_slice = loaded;
        m_status = T("Loaded the description next to the picture");
    }
}

void NineSlicePanel::Draw(EditorHost& host, bool& open) {
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(940.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("9-slice editor###NineSlice"), &open)) {
        ImGui::End();
        return;
    }

    DrawToolbar(host);
    ImGui::Separator();

    // Картинка слева, числа и предпросмотр справа. Картинка получает всё
    // оставшееся место: считать пиксели уголка — то, ради чего окно открыли.
    const float rightWidth = 320.0f;
    const float canvasWidth = std::max(240.0f, ImGui::GetContentRegionAvail().x - rightWidth - 8.0f);

    ImGui::BeginChild("##slice_canvas", ImVec2(canvasWidth, 0.0f), true,
                      ImGuiWindowFlags_NoScrollWithMouse);
    DrawCanvas(host);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##slice_side", ImVec2(0.0f, 0.0f), false);
    DrawFields(host);
    ImGui::Separator();
    DrawPreview();
    ImGui::EndChild();

    ImGui::End();
}

void NineSlicePanel::DrawToolbar(EditorHost& host) {
    // Картинка выбирается СЛОТОМ, как любой ассет в редакторе (см. AssetSlot.h):
    // её можно бросить сюда из панели ассетов.
    const assetslot::Result r =
        assetslot::Draw(host, "##slice_image", assetslot::Kind::Texture, m_path, nullptr,
                        T("Drop a picture here"));
    if (r.Changed) LoadImage(r.Path);

    if (!m_tex) {
        ImGui::TextDisabled("%s", T("Pick a picture: a frame, a panel, a button from a UI set."));
        return;
    }

    ImGui::SameLine();
    if (ImGui::Button(T("Guess"))) {
        // Догадка по самой картинке. Предложение, а не ответ: результат виден
        // на холсте и правится направляющими.
        NineSlice guess;
        if (sage::ui::GuessBorderFromFile(m_path, guess)) {
            guess.CenterFill = m_slice.CenterFill;
            guess.EdgeFill = m_slice.EdgeFill;
            guess.DrawCenter = m_slice.DrawCenter;
            m_slice = guess;
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
        if (m_slice.SaveFile(NineSlice::SidecarPath(m_path), err)) {
            m_status = T("Saved next to the picture");
            host.SetStatusMessage(T("9-slice saved: ") + fs::path(m_path).filename().string());
        } else {
            m_status = T("Could not save: ") + err;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("The description lives next to the picture, so one cut\n"
                                  "serves every element that uses it."));

    ImGui::SameLine();
    if (ImGui::Button(T("Load .sage9"))) {
        NineSlice loaded;
        std::string err;
        if (NineSlice::LoadFile(NineSlice::SidecarPath(m_path), loaded, err)) {
            m_slice = loaded;
            m_status = T("Loaded next to the picture");
        } else {
            m_status = T("No description next to the picture");
        }
    }

    ImGui::SameLine();
    // Применение к выбранному — правка сцены, значит через историю отката.
    GameObject sel = host.SelectedObject();
    const bool canApply = sel.Valid() &&
                          host.CurrentScene().Registry().try_get<sage::ui::Image>(sel.Entity());
    ImGui::BeginDisabled(!canApply);
    if (ImGui::Button(T("Apply to selection"))) {
        host.PushUndoSnapshot();
        sage::ui::Image& img = host.CurrentScene().Registry().get<sage::ui::Image>(sel.Entity());
        img.Path = m_path;
        img.Tex = m_tex;
        img.SetSlice(m_slice);
        m_status = T("Applied to the selected element");
    }
    ImGui::EndDisabled();
    if (!canApply && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Select an element with a Picture part."));

    if (!m_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_status.c_str());
    }
}

void NineSlicePanel::DrawCanvas(EditorHost& host) {
    if (!m_tex) {
        ImGui::TextDisabled("%s", T("No picture."));
        return;
    }
    const float iw = (float)m_tex->Width(), ih = (float)m_tex->Height();
    if (iw <= 0.0f || ih <= 0.0f) return;

    const ImVec2 area = ImGui::GetContentRegionAvail();
    if (area.x < 16.0f || area.y < 16.0f) return;

    // «Вписать» считается один раз при загрузке: дальше масштаб принадлежит
    // человеку, и пересчитывать его на каждом изменении размера окна значило бы
    // сбрасывать увеличение, на котором он как раз считает пиксели.
    if (m_zoom <= 0.0f) m_zoom = std::max(1.0f, std::floor(std::min(area.x / iw, area.y / ih)));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##slice_area", area,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // Колесо — увеличение ВОКРУГ КУРСОРА: иначе интересующий уголок уезжает за
    // край ровно в тот момент, когда к нему присматриваются.
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const float before = m_zoom;
            m_zoom = std::clamp(m_zoom * (wheel > 0.0f ? 1.25f : 0.8f), 0.25f, 32.0f);
            const ImVec2 anchor(mouse.x - origin.x - m_pan.x, mouse.y - origin.y - m_pan.y);
            m_pan.x -= anchor.x * (m_zoom / before - 1.0f);
            m_pan.y -= anchor.y * (m_zoom / before - 1.0f);
        }
    }
    // Средняя кнопка — сдвиг. Не правая: правая в редакторе везде значит
    // «контекстное меню», и отнимать её у одной панели нельзя.
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        m_pan.x += ImGui::GetIO().MouseDelta.x;
        m_pan.y += ImGui::GetIO().MouseDelta.y;
    }

    const ImVec2 imgA(origin.x + m_pan.x, origin.y + m_pan.y);
    const ImVec2 imgB(imgA.x + iw * m_zoom, imgA.y + ih * m_zoom);

    dl->PushClipRect(origin, ImVec2(origin.x + area.x, origin.y + area.y), true);
    if (m_showCheckers) DrawCheckers(dl, imgA, imgB, std::max(4.0f, m_zoom * 4.0f));
    dl->AddImage((ImTextureID)(std::intptr_t)m_tex->NativeHandle(), imgA, imgB,
                 ImVec2(0, 1), ImVec2(1, 0));

    // --- закраска девяти зон -------------------------------------------------
    const float l = std::clamp(m_slice.Left, 0.0f, iw);
    const float r = std::clamp(m_slice.Right, 0.0f, iw - l);
    const float t = std::clamp(m_slice.Top, 0.0f, ih);
    const float b = std::clamp(m_slice.Bottom, 0.0f, ih - t);
    const float gx[4] = {imgA.x, imgA.x + l * m_zoom, imgB.x - r * m_zoom, imgB.x};
    const float gy[4] = {imgA.y, imgA.y + t * m_zoom, imgB.y - b * m_zoom, imgB.y};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if (gx[col + 1] <= gx[col] || gy[row + 1] <= gy[row]) continue;
            const bool corner = (row != 1) && (col != 1);
            const bool center = (row == 1) && (col == 1);
            if (center && !m_slice.DrawCenter) continue;
            dl->AddRectFilled({gx[col], gy[row]}, {gx[col + 1], gy[row + 1]},
                              corner ? kCornerTint : (center ? kCenterTint : kEdgeTint));
        }
    }

    // --- направляющие: подсветка ближайшей и перетаскивание -------------------
    //
    // Направляющие — главное в этом окне, поэтому за них можно взяться и когда
    // они сошлись в одну точку: ближайшая выбирается по расстоянию, а не по
    // порядку проверки.
    float* edges[4] = {&m_slice.Left, &m_slice.Top, &m_slice.Right, &m_slice.Bottom};
    const float lines[4] = {gx[1], gy[1], gx[2], gy[2]};
    const bool vertical[4] = {true, false, true, false};

    int hot = -1;
    if (hovered && m_dragEdge < 0) {
        float best = kGrabPx;
        for (int i = 0; i < 4; ++i) {
            const float d = std::fabs((vertical[i] ? mouse.x : mouse.y) - lines[i]);
            if (d <= best) { best = d; hot = i; }
        }
    }
    if (m_dragEdge >= 0) hot = m_dragEdge;
    if (hot >= 0)
        ImGui::SetMouseCursor(vertical[hot] ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);

    if (hot >= 0 && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        m_dragEdge = hot;
    if (m_dragEdge >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_dragEdge = -1;
        // Правка записана в историю один раз, на отпускании: тащат
        // направляющую десятки кадров, и запись на каждый кадр означала бы
        // десятки шагов Ctrl+Z на одно движение.
        host.PushUndoSnapshot();
    }

    if (m_dragEdge >= 0) {
        // Значение считается ОТ КРАЯ картинки и округляется до целого пикселя
        // ИСХОДНИКА: нарезка по половине пикселя показывает на экране шов.
        const int i = m_dragEdge;
        const float px = vertical[i] ? (mouse.x - imgA.x) / m_zoom : (mouse.y - imgA.y) / m_zoom;
        const float fromEdge = (i == 2) ? iw - px : (i == 3) ? ih - px : px;
        const float limit = (i == 0 || i == 2) ? iw : ih;
        *edges[i] = std::clamp(std::round(fromEdge), 0.0f, limit);
    }

    for (int i = 0; i < 4; ++i) {
        const ImU32 col = (i == hot) ? kGuideHot : kGuide;
        const float thick = (i == hot) ? 2.0f : 1.0f;
        if (vertical[i]) dl->AddLine({lines[i], imgA.y}, {lines[i], imgB.y}, col, thick);
        else dl->AddLine({imgA.x, lines[i]}, {imgB.x, lines[i]}, col, thick);
    }
    dl->AddRect(imgA, imgB, IM_COL32(255, 255, 255, 60));
    dl->PopClipRect();

    // Подпись — снизу слева, поверх холста: размер картинки и увеличение нужны
    // постоянно, а отдельная строка под холстом отъедала бы у него высоту.
    char info[128];
    std::snprintf(info, sizeof(info), "%dx%d  %.0f%%", m_tex->Width(), m_tex->Height(),
                  m_zoom * 100.0f);
    dl->AddText({origin.x + 6.0f, origin.y + area.y - 18.0f}, IM_COL32(210, 210, 215, 200), info);
}

void NineSlicePanel::DrawFields(EditorHost& host) {
    ImGui::TextUnformatted(T("Fixed corners, in pixels of the source"));
    const float iw = m_tex ? (float)m_tex->Width() : 4096.0f;
    const float ih = m_tex ? (float)m_tex->Height() : 4096.0f;

    // Числа и направляющие связаны в обе стороны: правка здесь двигает линии на
    // холсте, перетаскивание линии меняет число. Одно состояние, два способа
    // его править — а не две копии, которые придётся синхронизировать.
    bool edited = false;
    edited |= ImGui::DragFloat(T("Left##9l"), &m_slice.Left, 0.25f, 0.0f, iw, "%.0f");
    host.TrackLastImGuiItem();
    edited |= ImGui::DragFloat(T("Top##9t"), &m_slice.Top, 0.25f, 0.0f, ih, "%.0f");
    host.TrackLastImGuiItem();
    edited |= ImGui::DragFloat(T("Right##9r"), &m_slice.Right, 0.25f, 0.0f, iw, "%.0f");
    host.TrackLastImGuiItem();
    edited |= ImGui::DragFloat(T("Bottom##9b"), &m_slice.Bottom, 0.25f, 0.0f, ih, "%.0f");
    host.TrackLastImGuiItem();

    ImGui::Spacing();
    ImGui::TextUnformatted(T("How the stretching pieces are filled"));
    const char* fills[] = {T("Stretch"), T("Repeat")};
    int center = (int)m_slice.CenterFill;
    if (ImGui::Combo(T("Middle##9cf"), &center, fills, 2)) m_slice.CenterFill = (SliceFill)center;
    int edge = (int)m_slice.EdgeFill;
    if (ImGui::Combo(T("Edges##9ef"), &edge, fills, 2)) m_slice.EdgeFill = (SliceFill)edge;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Repeat is the only right answer for pixel art and patterns:\n"
                                  "an ornament of 16 pixels stretched to 300 turns to mush."));

    ImGui::Checkbox(T("Draw the middle##9dc"), &m_slice.DrawCenter);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("An outline frame has no middle — what is under\n"
                                  "the element shows through."));
    ImGui::Checkbox(T("Checkerboard under transparency##9chk"), &m_showCheckers);
    (void)edited;
}

void NineSlicePanel::DrawPreview() {
    ImGui::TextUnformatted(T("Preview — drag the corner to resize"));
    if (!m_tex) {
        ImGui::TextDisabled("%s", T("No picture."));
        return;
    }

    ImGui::SliderFloat(T("Pixel scale##9ps"), &m_previewScale, 0.25f, 8.0f, "%.2fx");

    const ImVec2 area = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    if (area.x < 40.0f || area.y < 40.0f) return;

    // Размер предпросмотра не может стать меньше самой рамки: там она начинает
    // ужиматься, и видно уже не нарезку, а её аварийное поведение. Нижняя
    // граница — ровно сумма полей, чтобы дотащить ДО неё было можно.
    const glm::vec2 minSize = sage::ui::MinimumSize(m_slice, m_previewScale);
    m_previewSize.x = std::clamp(m_previewSize.x, std::max(8.0f, minSize.x), area.x - 4.0f);
    m_previewSize.y = std::clamp(m_previewSize.y, std::max(8.0f, minSize.y), area.y - 4.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = origin;
    const ImVec2 b(a.x + m_previewSize.x, a.y + m_previewSize.y);
    if (m_showCheckers) DrawCheckers(dl, a, b, 8.0f);

    // Рисуется ТЕМ ЖЕ решателем, что и в игре (sage::ui::Solve). Не «похоже
    // нарисовано в редакторе»: предпросмотр, считающий по своим правилам, — это
    // второй источник правды, и расходиться с игрой он начинает в первый же
    // день.
    SliceRequest req;
    req.SrcW = (float)m_tex->Width();
    req.SrcH = (float)m_tex->Height();
    req.DstX = a.x; req.DstY = a.y;
    req.DstW = m_previewSize.x; req.DstH = m_previewSize.y;
    req.Scale = m_previewScale;

    const float tw = req.SrcW, th = req.SrcH;
    const ImTextureID id = (ImTextureID)(std::intptr_t)m_tex->NativeHandle();
    for (const SliceQuad& q : sage::ui::Solve(m_slice, req)) {
        // V переворачивается: у текстуры движка начало координат внизу, у ImGui
        // — вверху.
        const ImVec2 uv0(q.SrcX / tw, 1.0f - q.SrcY / th);
        const ImVec2 uv1((q.SrcX + q.SrcW) / tw, 1.0f - (q.SrcY + q.SrcH) / th);
        dl->AddImage(id, {q.DstX, q.DstY}, {q.DstX + q.DstW, q.DstY + q.DstH}, uv0, uv1);
    }
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 70));

    // Уголок изменения размера — там же, где у окон системы, и той же формы:
    // объяснять, за что тянуть, не приходится.
    const ImVec2 gripA(b.x - 14.0f, b.y - 14.0f);
    ImGui::SetCursorScreenPos(gripA);
    ImGui::InvisibleButton("##9grip", ImVec2(14.0f, 14.0f));
    const bool gripHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (gripHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
    if (ImGui::IsItemActive()) {
        m_previewSize.x += ImGui::GetIO().MouseDelta.x;
        m_previewSize.y += ImGui::GetIO().MouseDelta.y;
    }
    for (int i = 0; i < 3; ++i) {
        const float o = 4.0f + (float)i * 4.0f;
        dl->AddLine({b.x - o, b.y - 2.0f}, {b.x - 2.0f, b.y - o},
                    gripHot ? IM_COL32(255, 255, 255, 220) : IM_COL32(200, 200, 205, 140), 1.5f);
    }

    char info[96];
    std::snprintf(info, sizeof(info), "%.0f x %.0f", m_previewSize.x, m_previewSize.y);
    dl->AddText({a.x + 4.0f, b.y + 2.0f}, IM_COL32(210, 210, 215, 200), info);
    ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 20.0f));
}
