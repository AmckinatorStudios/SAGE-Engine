#include "SpriteSheetPanel.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "imgui.h"
#include <stb_image.h>

#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../Localization.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Pack.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"

using sage::ui::SpriteRegion;
using sage::ui::SpriteSheet;

void SpriteSheetPanel::ForgetProject() {
    m_path.clear();
    m_tex.reset();
    m_pixels.clear();
    m_sheet = SpriteSheet();
    m_selected = -1;
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
    for (int i = 0; i < (int)m_sheet.Regions.size(); ++i) {
        const SpriteRegion& reg = m_sheet.Regions[(size_t)i];
        ImGui::PushID(i);
        if (ImGui::Selectable(reg.Name.c_str(), m_selected == i)) {
            m_selected = i;
            std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", reg.Name.c_str());
        }
        ImGui::PopID();
    }
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
    ImGui::InvisibleButton("##ss_pick", ImVec2(std::max(1.0f, size.x), std::max(1.0f, size.y)));
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
    if (ImGui::Button(T("Remove piece"))) {
        m_sheet.Remove(reg.Name);
        m_selected = -1;
        m_dirty = true;
    }
}
