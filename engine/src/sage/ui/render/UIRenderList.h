#pragma once
#include <memory>
#include <vector>

#include "sage/ui/render/UIRenderCommand.h"

// ---------------------------------------------------------------------------
// СПИСОК КОМАНД — результат подготовки кадра интерфейса.
//
// Владеет всем, на что команды ссылаются (глифы, материалы), чтобы бэкенду
// хватило одного объекта и чтобы указатели гарантированно жили до конца кадра.
// Здесь же собираются батчи и считается статистика (§110): «сколько команд,
// сколько батчей, сколько проходов масок и эффектов» — вопрос, на который
// разработчик обязан получать ответ, не запуская профайлер GPU.
// ---------------------------------------------------------------------------
namespace sage::ui {

struct UIRenderStats {
    int Commands = 0;
    int Batches = 0;
    int Quads = 0;
    int Glyphs = 0;
    int MaskPasses = 0;
    int EffectPasses = 0;
    int RenderTargets = 0;
    int CulledNodes = 0;
    double PrepareMs = 0.0;
};

class UIRenderList {
public:
    void Clear();

    UIRenderCommand& Add();
    int GlyphBase() const { return (int)m_glyphs.size(); }
    void AddGlyph(const UIGlyphDraw& g) { m_glyphs.push_back(g); }
    // Материалы живут в списке: команда хранит указатель, и он обязан
    // пережить кадр.
    const UIMaterialRef* AddMaterial(const UIMaterialRef& m);

    // Собрать батчи. Команды НЕ переставляются: порядок уже задан ключом
    // сортировки на этапе подготовки, а перестановка ради батчинга ломала бы
    // прозрачность (интерфейс рисуется поверх, порядок значим).
    void Build();

    const std::vector<UIRenderCommand>& Commands() const { return m_commands; }
    const std::vector<UIGlyphDraw>& Glyphs() const { return m_glyphs; }
    const std::vector<UIRenderBatch>& Batches() const { return m_batches; }
    UIRenderStats& Stats() { return m_stats; }
    const UIRenderStats& Stats() const { return m_stats; }

    bool Empty() const { return m_commands.empty(); }

private:
    std::vector<UIRenderCommand> m_commands;
    std::vector<UIGlyphDraw> m_glyphs;
    std::vector<UIRenderBatch> m_batches;
    std::vector<std::unique_ptr<UIMaterialRef>> m_materials;
    UIRenderStats m_stats;
};

} // namespace sage::ui
