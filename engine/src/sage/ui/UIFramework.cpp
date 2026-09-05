#include "sage/ui/UIFramework.h"

#include <chrono>

namespace sage::ui {

void UIInitialize() {
    RegisterBuiltinUIComponents();
    RegisterBuiltinUIEffects();
    RegisterBuiltinUIWidgets();
    RegisterBuiltinUIEmitters();
}

UIRuntime::UIRuntime() {
    UIInitialize();
    m_theme = UITheme::Default();
}

void UIRuntime::Update(float dt) {
    m_ctx.DeltaTime = dt;
    m_ctx.Time += dt;

    // Порядок кадра фиксирован и объясним: поведение виджетов может изменить
    // размеры (текст в поле ввода), стиль может изменить оформление, и только
    // после этого имеет смысл считать раскладку.
    UIUpdateWidgets(m_doc, dt);
    if (m_doc.IsDirty(UIDirty_Style)) m_theme.Apply(m_doc);
    m_layout.Solve(m_doc, m_ctx);
    m_profile.Layout = m_layout.Stats();
}

UIInputReport UIRuntime::HandleInput(const UIInputFrame& input) {
    const auto t0 = std::chrono::steady_clock::now();
    UIInputReport r = m_input.Update(m_doc, m_layout, m_ctx, input, m_bus);
    const auto t1 = std::chrono::steady_clock::now();
    m_profile.InputMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    // Состояния взаимодействия изменились — оформление состояний обязано
    // догнать их в этом же кадре, иначе подсветка кнопки опаздывает на кадр.
    m_theme.Apply(m_doc);
    return r;
}

void UIRuntime::Build() {
    UIBuildDrawList(m_doc, m_layout, m_ctx, m_list);
    m_profile.Render = m_list.Stats();
    m_profile.TotalMs = m_profile.Layout.LayoutMs + m_profile.Render.PrepareMs + m_profile.InputMs;
}

void UIRuntime::Render(IUIDrawBackend& backend) {
    Build();
    backend.Begin(m_ctx.ScreenPixels);
    backend.Submit(m_list);
    backend.End();
}

} // namespace sage::ui
