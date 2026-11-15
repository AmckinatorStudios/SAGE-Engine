#include "SpriteSheetPanel.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "imgui.h"
#include <stb_image.h>
#include <stb_image_write.h>

#include <filesystem>

#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../Localization.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Pack.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"
#include "sage/core/Paths.h"

using sage::ui::SpriteRegion;
using sage::ui::SpriteSheet;

void SpriteSheetPanel::ForgetProject() {
    m_path.clear();
    m_tex.reset();
    m_pixels.clear();
    m_sheet = SpriteSheet();
    m_selected = -1;
}

void SpriteSheetPanel::Open(const std::string& path, int region, int sequence) {
    Load(path);
    if (region >= 0 && region < (int)m_sheet.Regions.size()) {
        m_selected = region;
        std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_sheet.Regions[(size_t)region].Name.c_str());
    }
    if (sequence >= 0 && sequence < (int)m_sheet.Sequences.size()) m_seqSelected = sequence;
}

void SpriteSheetPanel::Load(const std::string& path) {
    m_path = path;
    m_sheet = SpriteSheet();
    m_selected = -1;
    m_zoom = 0.0f;
    m_dirty = false;
    m_pixels.clear();
    m_w = m_h = 0;
    m_tex = path.empty() ? nullptr : ResourceManager::Instance().GetTexture(path, TextureFilter::Nearest, false);
    if (path.empty()) return;
    // Пиксели — для нарезки по островам и пропуска пустых клеток.
    int ch = 0;
    stbi_set_flip_vertically_on_load_thread(false);
    m_disk = sage::AssetDatabase::Instance().LocatePath(path);
    if (unsigned char* data = sage::assets::vfs::LoadImage(m_disk, &m_w, &m_h, &ch, 4)) {
        m_pixels.assign(data, data + (size_t)m_w * m_h * 4);
        stbi_image_free(data);
    }
    std::string err;
    if (SpriteSheet::LoadFile(SpriteSheet::SidecarPath(m_disk), m_sheet, err))
        m_status = T("Loaded the sheet next to the picture");
    else
        m_status.clear();
}

void SpriteSheetPanel::Draw(EditorHost& host, bool* open) {
    ImGui::SetNextWindowSize(ImVec2(880, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("Sprite sheet###SpriteSheet"), open)) {
        ImGui::End();
        return;
    }
    const assetslot::Result r = assetslot::Draw(host, "##ss_image", assetslot::Kind::Texture, m_path, nullptr,
                                                T("Drop a picture here"));
    if (r.Changed) Load(r.Path);
    DrawAtlas(host);
    if (!m_tex || m_pixels.empty()) {
        ImGui::TextDisabled("%s", T("Pick a picture: it will be cut into named pieces."));
        ImGui::End();
        return;
    }

    // --- Нарезка ---------------------------------------------------------
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt2(T("Cell"), &m_grid.CellW);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt2(T("Gap"), &m_grid.PadX);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::InputInt2(T("Margin"), &m_grid.OffX);
    ImGui::SetNextItemWidth(160);
    ImGui::InputText(T("Name prefix"), m_prefixBuf, sizeof(m_prefixBuf));
    ImGui::SameLine();
    if (ImGui::Button(T("Cut by grid"))) {
        m_grid.CellW = std::max(1, m_grid.CellW);
        m_grid.CellH = std::max(1, m_grid.CellH);
        m_grid.Prefix = m_prefixBuf[0] ? m_prefixBuf : "sprite";
        const int n = m_sheet.SliceGrid(m_w, m_h, m_grid, m_pixels.data(), false);
        m_status = T("Pieces added: ") + std::to_string(n);
        m_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Find pieces"))) {
        const int n = m_sheet.SliceAuto(m_pixels.data(), m_w, m_h, false, 0.05f, 2,
                                        m_prefixBuf[0] ? m_prefixBuf : "sprite");
        m_status = T("Pieces added: ") + std::to_string(n);
        m_dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Every island of opaque pixels becomes a piece."));
    ImGui::SameLine();
    if (ImGui::Button(T("Clear"))) {
        m_sheet.Regions.clear();
        m_selected = -1;
        m_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(m_dirty ? T("Save *") : T("Save"))) {
        std::string err;
        if (m_sheet.SaveFile(SpriteSheet::SidecarPath(m_disk), err)) {
            m_status = T("Saved next to the picture");
            m_dirty = false;
        } else {
            m_status = T("Could not save: ") + err;
        }
    }
    if (!m_status.empty()) ImGui::TextDisabled("%s", m_status.c_str());

    // --- Картинка и список -------------------------------------------------
    const float listW = 300.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##ss_canvas", ImVec2(std::max(100.0f, avail.x - listW - 8.0f), avail.y), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    DrawCanvas(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##ss_side", ImVec2(0, avail.y), true);
    DrawRegion();
    ImGui::Separator();
    if (ImGui::CollapsingHeader(T("Pieces"), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i = 0; i < (int)m_sheet.Regions.size(); ++i) {
            const SpriteRegion& reg = m_sheet.Regions[(size_t)i];
            ImGui::PushID(i);
            if (ImGui::Selectable(reg.Name.c_str(), m_selected == i)) {
                m_selected = i;
                std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", reg.Name.c_str());
            }
            ImGui::PopID();
        }
    }
    DrawSequences();
    ImGui::EndChild();
    ImGui::End();
}

void SpriteSheetPanel::DrawCanvas(float width, float height) {
    if (m_zoom <= 0.0f) m_zoom = std::max(1.0f, std::min(width / (float)m_w, height / (float)m_h));
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f)
        m_zoom = std::clamp(m_zoom * (ImGui::GetIO().MouseWheel > 0 ? 1.25f : 0.8f), 0.25f, 32.0f);
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 size((float)m_w * m_zoom, (float)m_h * m_zoom);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), IM_COL32(40, 40, 46, 255));
    // V переворачивается: у текстуры движка начало координат внизу.
    dl->AddImage((ImTextureID)(std::intptr_t)m_tex->NativeHandle(), a, ImVec2(a.x + size.x, a.y + size.y),
                 ImVec2(0, 1), ImVec2(1, 0));
    for (int i = 0; i < (int)m_sheet.Regions.size(); ++i) {
        const glm::ivec4 rc = m_sheet.Regions[(size_t)i].Rect;
        const ImVec2 p0(a.x + rc.x * m_zoom, a.y + rc.y * m_zoom);
        const ImVec2 p1(p0.x + rc.z * m_zoom, p0.y + rc.w * m_zoom);
        dl->AddRect(p0, p1, i == m_selected ? IM_COL32(255, 210, 80, 255) : IM_COL32(90, 200, 255, 200), 0.0f, 0,
                    i == m_selected ? 2.0f : 1.0f);
    }
    // Точка опоры выбранного куска — крестиком: её видно, а не только число.
    if (m_selected >= 0 && m_selected < (int)m_sheet.Regions.size()) {
        const SpriteRegion& sel = m_sheet.Regions[(size_t)m_selected];
        const ImVec2 pv(a.x + (sel.Rect.x + sel.Rect.z * sel.Pivot.x) * m_zoom,
                        a.y + (sel.Rect.y + sel.Rect.w * sel.Pivot.y) * m_zoom);
        dl->AddLine(ImVec2(pv.x - 6, pv.y), ImVec2(pv.x + 6, pv.y), IM_COL32(255, 80, 80, 255), 2.0f);
        dl->AddLine(ImVec2(pv.x, pv.y - 6), ImVec2(pv.x, pv.y + 6), IM_COL32(255, 80, 80, 255), 2.0f);
    }
    if (m_dragging) {
        const glm::ivec2 lo = glm::min(m_dragFrom, m_dragTo), hi = glm::max(m_dragFrom, m_dragTo);
        dl->AddRect(ImVec2(a.x + lo.x * m_zoom, a.y + lo.y * m_zoom), ImVec2(a.x + hi.x * m_zoom, a.y + hi.y * m_zoom),
                    IM_COL32(120, 255, 140, 255), 0.0f, 0, 2.0f);
    }
    ImGui::InvisibleButton("##ss_pick", ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)));
    // НОВЫЙ КУСОК — рамкой мышью: тянуть от угла до угла. Короткий щелчок —
    // выбор куска, как и раньше.
    {
        const ImVec2 m = ImGui::GetMousePos();
        const glm::ivec2 px = glm::clamp(glm::ivec2((int)((m.x - a.x) / m_zoom), (int)((m.y - a.y) / m_zoom)),
                                         glm::ivec2(0), glm::ivec2(m_w, m_h));
        if (ImGui::IsItemActivated()) m_dragFrom = m_dragTo = px;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
            m_dragging = true;
            m_dragTo = px;
        }
        if (ImGui::IsItemDeactivated() && m_dragging) {
            m_dragging = false;
            const glm::ivec2 lo = glm::min(m_dragFrom, m_dragTo), hi = glm::max(m_dragFrom, m_dragTo);
            if (hi.x - lo.x >= 1 && hi.y - lo.y >= 1) {
                SpriteRegion nr;
                int index = (int)m_sheet.Regions.size();
                do { nr.Name = std::string(m_prefixBuf[0] ? m_prefixBuf : "sprite") + "_" + std::to_string(index++); }
                while (m_sheet.Find(nr.Name));
                nr.Rect = {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y};
                m_sheet.Set(nr);
                m_selected = (int)m_sheet.Regions.size() - 1;
                std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", nr.Name.c_str());
                m_dirty = true;
            }
            return;
        }
    }
    if (ImGui::IsItemClicked()) {
        const ImVec2 m = ImGui::GetMousePos();
        const int px = (int)((m.x - a.x) / m_zoom), py = (int)((m.y - a.y) / m_zoom);
        m_selected = -1;
        for (int i = (int)m_sheet.Regions.size() - 1; i >= 0; --i) {
            const glm::ivec4 rc = m_sheet.Regions[(size_t)i].Rect;
            if (px >= rc.x && py >= rc.y && px < rc.x + rc.z && py < rc.y + rc.w) {
                m_selected = i;
                std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", m_sheet.Regions[(size_t)i].Name.c_str());
                break;
            }
        }
    }
}

void SpriteSheetPanel::DrawRegion() {
    if (m_selected < 0 || m_selected >= (int)m_sheet.Regions.size()) {
        ImGui::TextDisabled("%s", T("Click a piece to edit it."));
        return;
    }
    SpriteRegion& reg = m_sheet.Regions[(size_t)m_selected];
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##ss_name", m_nameBuf, sizeof(m_nameBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (m_sheet.Rename(reg.Name, m_nameBuf)) m_dirty = true;
        else m_status = T("This name is taken");
    }
    if (ImGui::InputInt4(T("Rect"), &reg.Rect.x)) m_dirty = true;
    float slice[4] = {reg.Slice.x, reg.Slice.y, reg.Slice.z, reg.Slice.w};
    if (ImGui::InputFloat4(T("9-slice"), slice, "%.0f")) {
        reg.Slice = glm::vec4(slice[0], slice[1], slice[2], slice[3]);
        m_dirty = true;
    }
    if (ImGui::SliderFloat2(T("Pivot"), &reg.Pivot.x, 0.0f, 1.0f, "%.2f")) m_dirty = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("The point the piece turns around and is placed by: 0,0 — top left."));
    if (ImGui::Button(T("Remove piece"))) {
        m_sheet.Remove(reg.Name);
        m_selected = -1;
        m_dirty = true;
    }
}

// ПОСЛЕДОВАТЕЛЬНОСТИ: куски по порядку и скорость — анимация из листа. Кадры
// набираются по началу имени (run_0, run_1, …); превью листает их здесь же.
void SpriteSheetPanel::DrawSequences() {
    if (!ImGui::CollapsingHeader(T("Sequences"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::SetNextItemWidth(120);
    ImGui::InputText(T("Name##seq"), m_seqName, sizeof(m_seqName));
    ImGui::SetNextItemWidth(120);
    ImGui::InputText(T("Pieces starting with"), m_seqPrefix, sizeof(m_seqPrefix));
    if (ImGui::Button(T("Make sequence")) && m_seqName[0]) {
        const int n = m_sheet.SequenceFromPrefix(m_seqName, m_seqPrefix);
        m_status = T("Frames in the sequence: ") + std::to_string(n);
        for (int i = 0; i < (int)m_sheet.Sequences.size(); ++i)
            if (m_sheet.Sequences[(size_t)i].Name == m_seqName) m_seqSelected = i;
        m_dirty = true;
    }
    for (int i = 0; i < (int)m_sheet.Sequences.size(); ++i) {
        ImGui::PushID(1000 + i);
        const sage::ui::SpriteSequence& q = m_sheet.Sequences[(size_t)i];
        const std::string label = q.Name + "  (" + std::to_string(q.Frames.size()) + ")";
        if (ImGui::Selectable(label.c_str(), m_seqSelected == i)) m_seqSelected = i;
        ImGui::PopID();
    }
    if (m_seqSelected < 0 || m_seqSelected >= (int)m_sheet.Sequences.size()) return;
    sage::ui::SpriteSequence& q = m_sheet.Sequences[(size_t)m_seqSelected];
    if (ImGui::DragFloat(T("Frames per second"), &q.Fps, 0.1f, 0.5f, 120.0f, "%.1f")) m_dirty = true;
    if (ImGui::Checkbox(T("Loop"), &q.Loop)) m_dirty = true;
    if (ImGui::Button(T("Remove sequence"))) {
        m_sheet.Sequences.erase(m_sheet.Sequences.begin() + m_seqSelected);
        m_seqSelected = -1;
        m_dirty = true;
        return;
    }
    // Превью: текущий кадр в рамке 96 пикселей.
    m_previewTime += ImGui::GetIO().DeltaTime;
    const SpriteRegion* frame = m_sheet.Find(q.FrameAt(m_previewTime));
    if (!frame || frame->Rect.z <= 0 || frame->Rect.w <= 0) return;
    const float k = 96.0f / (float)std::max(frame->Rect.z, frame->Rect.w);
    const ImVec2 sz((float)frame->Rect.z * k, (float)frame->Rect.w * k);
    const float u0 = (float)frame->Rect.x / (float)m_w, u1 = (float)(frame->Rect.x + frame->Rect.z) / (float)m_w;
    const float v0 = 1.0f - (float)frame->Rect.y / (float)m_h;
    const float v1 = 1.0f - (float)(frame->Rect.y + frame->Rect.w) / (float)m_h;
    ImGui::Image((ImTextureID)(std::intptr_t)m_tex->NativeHandle(), sz, ImVec2(u0, v0), ImVec2(u1, v1));
}

// АТЛАС: все картинки папки — в одну, с листом кусков по именам файлов.
// Пишется рядом с папкой: «<папка>_atlas.png» и его «.sagesheet»; атлас
// сразу открывается здесь же.
void SpriteSheetPanel::DrawAtlas(EditorHost& host) {
    if (!ImGui::CollapsingHeader(T("Atlas from a folder"))) return;
    const assetslot::Result r = assetslot::Draw(host, "##ss_atlas", assetslot::Kind::Folder, m_atlasFolder, nullptr,
                                                T("A folder of pictures to pack into one atlas"));
    if (r.Changed) m_atlasFolder = r.Path;
    if (m_atlasFolder.empty() || !ImGui::Button(T("Pack into an atlas"))) return;
    namespace fs = std::filesystem;
    const fs::path dir = sage::PathFromUtf8(sage::AssetDatabase::Instance().LocatePath(m_atlasFolder));
    std::vector<std::vector<unsigned char>> pixels;
    std::vector<sage::ui::AtlasInput> inputs;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string ext = entry.path().extension().string();
        if (entry.is_regular_file() && (ext == ".png" || ext == ".PNG")) files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    pixels.reserve(files.size());
    for (const fs::path& f : files) {
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load_thread(false);
        unsigned char* data = stbi_load(sage::PathToUtf8(f).c_str(), &w, &h, &ch, 4);
        if (!data) continue;
        pixels.emplace_back(data, data + (size_t)w * h * 4);
        stbi_image_free(data);
        inputs.push_back({sage::PathToUtf8(f.stem()), pixels.back().data(), w, h});
    }
    std::vector<unsigned char> atlas;
    int aw = 0, ah = 0;
    SpriteSheet sheet;
    std::string err;
    if (!SpriteSheet::PackAtlas(inputs, 2, 4096, atlas, aw, ah, sheet, err)) {
        m_status = T("Could not pack: ") + err;
        return;
    }
    const fs::path out = dir.parent_path() / sage::PathFromUtf8(sage::PathToUtf8(dir.filename()) + "_atlas.png");
    if (!stbi_write_png(sage::PathToUtf8(out).c_str(), aw, ah, 4, atlas.data(), aw * 4) ||
        !sheet.SaveFile(SpriteSheet::SidecarPath(sage::PathToUtf8(out)), err)) {
        m_status = T("Could not save: ") + err;
        return;
    }
    const std::string ref = m_atlasFolder.substr(0, m_atlasFolder.find_last_of('/') + 1) +
                            sage::PathToUtf8(out.filename());
    Load(ref);
    m_status = T("Atlas packed: ") + std::to_string(inputs.size()) + " / " + std::to_string(aw) + "x" + std::to_string(ah);
}
