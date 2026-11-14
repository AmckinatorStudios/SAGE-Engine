#include "sage/ui/UIMasks.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "sage/render/ResourceManager.h"
#include "sage/scene/SceneComponents.h"
#include "sage/ui/UI.h"
#include "sage/ui/UISceneSolved.h"
#include "sage/ui/components/Layout.h"
#include "sage/ui/components/Visual.h"
#include "sage/ui/ShapePath.h"

namespace sage::ui {

UIRenderer::Mask ShapeMaskFor(const entt::registry& reg, entt::entity owner, const UIRect& window, float scale) {
    const Mask& m = reg.get<Mask>(owner);
    UIRenderer::Mask out;
    out.Rect = {window.x, window.y, window.w, window.h};
    out.Feather = m.Feather * scale;
    out.Invert = m.Invert;
    if (m.Form == Mask::Shape::RoundedRect) {
        const Fill* fill = reg.try_get<Fill>(owner);
        out.Radius = (m.Rounding >= 0.0f ? m.Rounding : (fill ? fill->Rounding : 0.0f)) * scale;
    } else if (m.Form == Mask::Shape::Auto) {
        // ПО ОФОРМЛЕНИЮ ХОЗЯИНА. Форма растеризуется в альфа-картинку размером с
        // окно: шейдер маски уже умеет резать по альфе, и любая форма —
        // круг, эллипс, свой контур с отверстиями — режется одинаково.
        if (const Shape* shape = reg.try_get<Shape>(owner)) {
            const int w = std::clamp((int)std::ceil(window.w), 4, 1024);
            const int h = std::clamp((int)std::ceil(window.h), 4, 1024);
            Shape scaled = *shape;
            scaled.CornerRadius *= scale * ((float)w / std::max(window.w, 1.0f));
            const std::string key = std::to_string((int)scaled.Form) + "|" + std::to_string(scaled.CornerRadius) +
                                    "|" + scaled.Path + "|" + std::to_string(w) + "x" + std::to_string(h);
            if (!m.ShapeTex || m.ShapeKey != key) {
                const std::vector<unsigned char> alpha = RasterizeShape(scaled, w, h);
                // Строки — снизу вверх: так лежат и картинки из файлов, и
                // маска читает их одним правилом.
                std::vector<unsigned char> rgba((size_t)w * (size_t)h * 4, 255);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        rgba[((size_t)(h - 1 - y) * (size_t)w + (size_t)x) * 4 + 3] = alpha[(size_t)y * (size_t)w + (size_t)x];
                m.ShapeTex = std::make_shared<::Texture>(rgba.data(), w, h, TextureFilter::Bilinear, false);
                m.ShapeKey = key;
            }
            out.Image = m.ShapeTex.get();
        } else if (const Fill* fill = reg.try_get<Fill>(owner); fill && fill->Tex) {
            out.Image = fill->Tex.get();
            out.UV = UIRenderer::MaskUV(*fill->Tex, {fill->Sprite.x, fill->Sprite.y, fill->Sprite.z, fill->Sprite.w});
        }
    } else if (m.Form == Mask::Shape::Image && !m.Image.empty()) {
        // Картинка-маска — лениво, при отрисовке: путь правят в инспекторе
        // посреди кадра (так же, как картинку вида, см. LookTexture).
        if (!m.Tex || m.TexPath != m.Image) {
            const bool sharp = m.Filtering == TextureFiltering::Nearest;
            m.Tex = sharp ? ResourceManager::Instance().GetTexture(m.Image, TextureFilter::Nearest, false)
                          : ResourceManager::Instance().GetTexture(m.Image);
            m.TexPath = m.Image;
        }
        if (m.Tex) {
            out.Image = m.Tex.get();
            out.UV = UIRenderer::MaskUV(*m.Tex, {m.Sprite.x, m.Sprite.y, m.Sprite.z, m.Sprite.w});
        }
    }
    return out;
}

// СЛОИ И МАСКИ ИНТЕРФЕЙСА — после раскладки, по готовому списку. Список идёт
// в порядке обхода дерева (родитель раньше детей), поэтому у родителя слой и
// маска уже известны. Затем элементы УСТОЙЧИВО сортируются по слою: внутри
// слоя остаётся порядок дерева, а элемент со слоем выше рисуется поверх всего
// интерфейса — и так же раньше всех ловит мышь (её ловит последний в списке).
void ResolveLayersAndMasks(const entt::registry& reg, std::vector<Solved>& out, size_t first, float scale) {
    std::unordered_map<entt::entity, size_t> index;
    for (size_t i = first; i < out.size(); ++i) index[out[i].Entity] = i;
    bool layered = false;
    for (size_t i = first; i < out.size(); ++i) {
        Solved& it = out[i];
        const Element& el = reg.get<Element>(it.Entity);
        const HierarchyComponent* h = reg.try_get<HierarchyComponent>(it.Entity);
        const auto parent = h ? index.find(h->Parent) : index.end();
        const Solved* up = parent != index.end() ? &out[parent->second] : nullptr;
        it.Layer = el.DrawLayer != 0 ? el.DrawLayer : (up ? up->Layer : 0);
        layered = layered || it.Layer != 0;
        if (!up) continue;
        const Mask* m = reg.try_get<Mask>(up->Entity);
        if (m && m->Shaped() && !m->ShowOutside) {
            it.MaskOwner = up->Entity;
            Mask scaled;
            scaled.Padding = m->Padding * scale;
            it.MaskRect = MaskWindow(scaled, up->Rect);
        } else {
            it.MaskOwner = up->MaskOwner;
            it.MaskRect = up->MaskRect;
        }
    }
    if (layered)
        std::stable_sort(out.begin() + (std::ptrdiff_t)first, out.end(),
                         [](const Solved& a, const Solved& b) { return a.Layer < b.Layer; });
}

} // namespace sage::ui

namespace sage::ui {

bool MaskAccepts(const entt::registry& reg, entt::entity owner, const UIRect& window, glm::vec2 p) {
    if (owner == entt::null) return true;
    const Mask* m = reg.try_get<Mask>(owner);
    if (!m) return true;
    bool inside = p.x >= window.x && p.y >= window.y && p.x < window.x + window.w && p.y < window.y + window.h;
    // Форму знаем точно — по ней и ловим: угол квадрата вокруг круглой
    // аватарки не нажимает кнопку, которую там не видно.
    if (m->Form == Mask::Shape::Auto)
        if (const Shape* shape = reg.try_get<Shape>(owner)) inside = ShapeContains(*shape, window, p);
    return m->Invert ? !inside : inside;
}

} // namespace sage::ui
