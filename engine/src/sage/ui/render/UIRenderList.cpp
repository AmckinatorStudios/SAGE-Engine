#include "sage/ui/render/UIRenderList.h"

namespace sage::ui {

void UIRenderList::Clear() {
    m_commands.clear();
    m_glyphs.clear();
    m_batches.clear();
    m_materials.clear();
    m_masks.clear();
    m_stats = UIRenderStats{};
}

UIRenderCommand& UIRenderList::Add() {
    m_commands.emplace_back();
    return m_commands.back();
}

const UIMaterialRef* UIRenderList::AddMaterial(const UIMaterialRef& m) {
    m_materials.push_back(std::make_unique<UIMaterialRef>(m));
    return m_materials.back().get();
}

const UIMaskShape* UIRenderList::AddMask(const UIMaskShape& m) {
    // Одинаковая маска у ста строк списка должна быть ОДНИМ объектом — иначе
    // батч рвётся на каждой строке из-за разных указателей.
    for (const auto& existing : m_masks) {
        const UIMaskShape& e = *existing;
        if (e.Form == m.Form && e.Rect.x == m.Rect.x && e.Rect.y == m.Rect.y &&
            e.Rect.w == m.Rect.w && e.Rect.h == m.Rect.h && e.Radius == m.Radius &&
            e.Softness == m.Softness && e.Invert == m.Invert && e.Tex == m.Tex &&
            e.Channel == m.Channel && e.GradientAngle == m.GradientAngle &&
            e.GradientStart == m.GradientStart && e.GradientEnd == m.GradientEnd)
            return existing.get();
    }
    m_masks.push_back(std::make_unique<UIMaskShape>(m));
    return m_masks.back().get();
}

void UIRenderList::Build() {
    m_batches.clear();
    m_stats.Commands = (int)m_commands.size();
    m_stats.Glyphs = (int)m_glyphs.size();
    if (m_commands.empty()) return;

    auto sameState = [](const UIRenderCommand& a, const UIRenderCommand& b) {
        // Что рвёт батч — это ровно то, что меняет состояние GPU (§85). Ни
        // «другой узел», ни «другой компонент» состояние не меняют, поэтому
        // панель с текстом и рамкой — это по-прежнему один батч на текст и
        // один на всё остальное.
        if (a.Blend != b.Blend) return false;
        if (a.Material != b.Material) return false;
        if (a.Clip.HasScissor != b.Clip.HasScissor) return false;
        if (a.Clip.MaskState != b.Clip.MaskState) return false;
        if (a.Clip.Shape != b.Clip.Shape) return false;
        if (a.Clip.HasScissor) {
            const UIRect& x = a.Clip.Scissor;
            const UIRect& y = b.Clip.Scissor;
            if (x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h) return false;
        }
        // Текст и сплошные фигуры НЕ разрывают батч. Это не оптимизация «на
        // всякий случай»: и глифы, и заливки идут через один и тот же шейдер
        // интерфейса (глиф берёт покрытие из атласа шрифта, заливка не
        // сэмплирует ничего), то есть состояние GPU у них одинаковое. Разрыв
        // здесь означал бы отдельный вызов рисования на каждую подпись — а
        // подпись есть почти у каждого элемента, и именно из них и набегала бы
        // «сотня draw call на сотню элементов» (§145).
        if (a.Tex != b.Tex) return false;
        return true;
    };

    for (size_t i = 0; i < m_commands.size(); ++i) {
        const UIRenderCommand& c = m_commands[i];
        if (c.Op != UIPassOp::Draw) {
            // Смена цели рисования всегда рвёт батч: команды из разных целей
            // физически не могут ехать одним вызовом.
            UIRenderBatch b;
            b.First = (int)i;
            b.Count = 1;
            b.Blend = c.Blend;
            b.Clip = c.Clip;
            m_batches.push_back(b);
            ++m_stats.EffectPasses;
            if (c.Op == UIPassOp::BeginOffscreen) ++m_stats.RenderTargets;
            continue;
        }
        if (!m_batches.empty()) {
            UIRenderBatch& last = m_batches.back();
            const UIRenderCommand& prev = m_commands[(size_t)(last.First + last.Count - 1)];
            if (prev.Op == UIPassOp::Draw && sameState(prev, c)) {
                ++last.Count;
                continue;
            }
        }
        UIRenderBatch b;
        b.First = (int)i;
        b.Count = 1;
        b.Tex = c.Tex;
        b.Blend = c.Blend;
        b.Clip = c.Clip;
        b.Material = c.Material;
        b.Text = c.Kind == UIPrimitive::Glyphs;
        (void)0;
        m_batches.push_back(b);
    }

    m_stats.Batches = (int)m_batches.size();
    for (const auto& c : m_commands) {
        if (c.Op != UIPassOp::Draw) continue;
        m_stats.Quads += c.Kind == UIPrimitive::NineSlice ? 9
                        : c.Kind == UIPrimitive::Glyphs   ? c.GlyphCount
                                                          : 1;
        if (c.Clip.MaskState != 0) ++m_stats.MaskPasses;
    }
}

} // namespace sage::ui
