#include "panels/ViewportToolbarRules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "EditorPrefs.h"
#include "Localization.h"
#include "gizmo/ColliderEdit.h"
#include "sage/physics/PhysicsComponents.h"

namespace sage::editor::vptools {

std::vector<int> FlowRows(const std::vector<float>& widths, float gap, float maxWidth) {
    std::vector<int> rows(widths.size(), 0);
    int row = 0;
    float x = 0.0f;
    for (size_t i = 0; i < widths.size(); ++i) {
        const float w = widths[i];
        // Первая в строке — без отступа; не влезает — на новую строку (кроме
        // самой первой группы: основные инструменты всегда в первой строке).
        if (i > 0 && x > 0.0f && x + gap + w > maxWidth) {
            ++row;
            x = 0.0f;
        }
        rows[i] = row;
        x += (x > 0.0f ? gap : 0.0f) + w;
    }
    return rows;
}

void OverlayPrefs::Load() {
    Enabled = prefs::GetBool("viewport.overlay.enabled", true);
    View = prefs::GetBool("viewport.overlay.view", true);
    Selection = prefs::GetBool("viewport.overlay.selection", true);
    Camera = prefs::GetBool("viewport.overlay.camera", false);
    Stats = prefs::GetBool("viewport.overlay.stats", false);
}

void OverlayPrefs::Save() const {
    prefs::SetBool("viewport.overlay.enabled", Enabled);
    prefs::SetBool("viewport.overlay.view", View);
    prefs::SetBool("viewport.overlay.selection", Selection);
    prefs::SetBool("viewport.overlay.camera", Camera);
    prefs::SetBool("viewport.overlay.stats", Stats);
}

namespace {

// Разряды — пробелом: «89 442» читается с одного взгляда (как в строке состояния).
std::string Grouped(long long v) {
    char raw[32];
    std::snprintf(raw, sizeof(raw), "%lld", v);
    std::string out;
    const int len = (int)std::char_traits<char>::length(raw);
    for (int i = 0; i < len; ++i) {
        if (i > 0 && raw[i - 1] != '-' && (len - i) % 3 == 0) out += ' ';
        out += raw[i];
    }
    return out;
}

} // namespace

std::vector<std::string> OverlayLines(const OverlayPrefs& p, const OverlayInput& in) {
    std::vector<std::string> lines;
    if (!p.Enabled) return lines;
    char buf[256];
    if (p.View) {
        std::string view = in.ViewName;
        if (in.SlotCount > 1) {
            std::snprintf(buf, sizeof(buf), T("view %d of %d"), in.Slot + 1, in.SlotCount);
            view += std::string(" · ") + buf;
            if (in.Active) view += std::string(" · ") + T("active");
        }
        lines.push_back(view);
    }
    if (!in.Active) return lines;
    if (p.Selection) {
        if (in.SelectedCount <= 0) {
            lines.push_back(T("Nothing selected"));
        } else if (in.SelectedCount == 1) {
            lines.push_back(in.SelectedName.empty() ? std::string(T("1 object selected")) : in.SelectedName);
        } else {
            std::snprintf(buf, sizeof(buf), T("%d objects selected"), in.SelectedCount);
            std::string s = buf;
            if (!in.SelectedName.empty()) s += " · " + in.SelectedName;
            lines.push_back(s);
        }
    }
    if (p.Camera) {
        if (in.Perspective) {
            std::snprintf(buf, sizeof(buf), T("Camera X %.2f  Y %.2f  Z %.2f · FOV %.0f°"), in.CameraPos.x,
                          in.CameraPos.y, in.CameraPos.z, in.Fov);
        } else {
            std::snprintf(buf, sizeof(buf), T("Scale: %.2f m in view height"), in.OrthoHeight);
        }
        lines.push_back(buf);
    }
    if (p.Stats) {
        std::snprintf(buf, sizeof(buf), "%s %.0f · %s %d · %s %s", T("FPS"), in.Fps, T("Draw Calls"), in.DrawCalls,
                      T("Triangles"), Grouped(in.Triangles).c_str());
        lines.push_back(buf);
        std::snprintf(buf, sizeof(buf), "%s %d · %s %.2f %s", T("Objects"), in.Objects, T("Mem"), in.MemoryGb, T("GB"));
        lines.push_back(buf);
    }
    return lines;
}

Box PlaceBottomLeft(const Box& cell, float w, float h, float pad, const std::vector<Box>& avoid) {
    Box b{cell.X0 + pad, cell.Y1 - pad - h, cell.X0 + pad + w, cell.Y1 - pad};
    if (b.X1 > cell.X1 - pad) return {};   // шире вида — не рисуем, а не режем
    // Поднимаемся над всем, что мешает, пока не станет свободно (не больше
    // числа препятствий: каждое обходится один раз).
    for (size_t pass = 0; pass <= avoid.size(); ++pass) {
        bool moved = false;
        for (const Box& a : avoid) {
            if (!b.Overlaps(a)) continue;
            const float top = a.Y0 - pad - h;
            b.Y0 = top;
            b.Y1 = top + h;
            moved = true;
        }
        if (!moved) break;
    }
    if (b.Y0 < cell.Y0 + pad) return {};
    for (const Box& a : avoid)
        if (b.Overlaps(a)) return {};
    return b;
}

bool ColliderEditAvailable(const entt::registry& reg, entt::entity e) {
    if (e == entt::null || !reg.valid(e)) return false;
    const ColliderComponent* col = reg.try_get<ColliderComponent>(e);
    return col && collideredit::Editable(*col);
}

std::string SnapLabel(const glm::vec3& m, float rotate, float scale) {
    char buf[96];
    if (m.x == m.y && m.y == m.z) std::snprintf(buf, sizeof(buf), "%g · %g° · %g", m.x, rotate, scale);
    else std::snprintf(buf, sizeof(buf), "%g/%g/%g · %g° · %g", m.x, m.y, m.z, rotate, scale);
    return buf;
}

} // namespace sage::editor::vptools
