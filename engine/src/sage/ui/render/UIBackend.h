#pragma once
#include <memory>
#include <vector>

#include "sage/render/Framebuffer.h"
#include "sage/render/Texture.h"
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
    bool SupportsOffscreen() const override { return true; }

    // КУДА ВОЗВРАЩАТЬСЯ после промежуточного прохода. nullptr — буфер по
    // умолчанию (игра, плеер). Редактор рисует интерфейс в свой буфер и обязан
    // сказать об этом: иначе после первого же размытия остаток кадра уехал бы
    // на экран мимо панели.
    void SetRootTarget(Framebuffer* target) { m_root = target; }

private:
    void DrawRect(const UIRenderCommand& c);
    void DrawBorder(const UIRenderCommand& c);
    void DrawImage(const UIRenderCommand& c);
    void DrawNineSlice(const UIRenderCommand& c);
    void DrawGlyphs(const UIRenderCommand& c, const UIRenderList& list);
    void DrawPolygon(const UIRenderCommand& c);
    void DrawRing(const UIRenderCommand& c);

    // --- промежуточные цели (§36) ------------------------------------------
    //
    // Пул, а не «создать и выбросить»: цель размером с экран стоит памяти, а
    // размытие включают на панели, которая живёт весь кадр и все следующие.
    struct Target {
        std::unique_ptr<Framebuffer> Fbo;
        std::shared_ptr<Texture> View;
    };
    Target& Acquire(size_t slot, int width, int height);
    void BindRoot();
    void BeginOffscreen();
    void EndOffscreen(const UIRenderCommand& c);
    // Размытие даунсэмплом: цепочка уменьшений и увеличений с билинейной
    // фильтрацией. Своего шейдера не требует вовсе — а повторённое билинейное
    // усреднение и есть приближение гауссова размытия.
    const Texture* BlurChain(size_t firstSlot, const Texture* source, float radius, int passes);

    UIRenderer& m_ui;
    bool m_clipOpen = false;
    Framebuffer* m_root = nullptr;
    std::vector<Target> m_pool;
    glm::vec2 m_screen{0.0f, 0.0f};
    int m_offscreenDepth = 0;
};

} // namespace sage::ui
