#pragma once
#include "sage/ui/render/UIRenderList.h"

class UIRenderer;

// ---------------------------------------------------------------------------
// БЭКЕНД РИСОВАНИЯ — единственное место, где интерфейс встречается с GPU.
//
// Интерфейс отдаёт список команд; бэкенд решает, как их нарисовать. Такой
// границы раньше не было вовсе: интерфейс и рисование жили в одном классе, и
// поэтому «нарисовать интерфейс без окна» (тест, снимок, инструмент) было
// невозможно, а «нарисовать другим API» — тем более.
// ---------------------------------------------------------------------------
namespace sage::ui {

class IUIDrawBackend {
public:
    virtual ~IUIDrawBackend() = default;

    virtual void Begin(glm::vec2 screenPixels) = 0;
    virtual void Submit(const UIRenderList& list) = 0;
    virtual void End() = 0;

    // Есть ли у бэкенда промежуточные цели рисования. Нет — эффекты, которым
    // они нужны, честно деградируют (§134: безопасный откат и предупреждение,
    // а не пустой экран).
    virtual bool SupportsOffscreen() const { return false; }
};

// Бэкенд поверх штатного UIRenderer движка. Нужен, чтобы новая система
// заработала на существующем конвейере рисования сразу, не дожидаясь своего
// шейдера: список команд от этого не меняется, меняется только исполнитель.
class UIClassicBackend : public IUIDrawBackend {
public:
    explicit UIClassicBackend(UIRenderer& renderer) : m_ui(renderer) {}

    void Begin(glm::vec2 screenPixels) override;
    void Submit(const UIRenderList& list) override;
    void End() override;

private:
    void DrawRect(const UIRenderCommand& c);
    void DrawBorder(const UIRenderCommand& c);
    void DrawImage(const UIRenderCommand& c);
    void DrawNineSlice(const UIRenderCommand& c);
    void DrawGlyphs(const UIRenderCommand& c, const UIRenderList& list);
    void DrawPolygon(const UIRenderCommand& c);
    void DrawRing(const UIRenderCommand& c);

    UIRenderer& m_ui;
    bool m_clipOpen = false;
};

} // namespace sage::ui
