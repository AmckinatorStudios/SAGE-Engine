#pragma once
// ---------------------------------------------------------------------------
// ЕДИНЫЙ ВХОД В СИСТЕМУ ИНТЕРФЕЙСА SAGE.
//
// Один заголовок для того, кто просто хочет сделать интерфейс. Внутренние
// модули включаются по отдельности теми, кому нужна только их часть (тесты
// раскладки не тянут рисование, инструмент вёрстки не тянет ввод).
//
// УСТРОЙСТВО (§4, §143):
//
//     UIDocument            дерево узлов и компонентов — ДАННЫЕ
//         │
//     UILayoutSolver        где всё стоит         — считает, не рисует
//         │
//     UIDrawBuilder         во что это превращается — команды, не GPU
//         │
//     IUIDrawBackend        кто это рисует         — GPU, не знает про узлы
//
//     UIInputRouter         ввод → события интерфейса (не игровые действия)
//     UITheme/UIStyle       оформление отдельно от структуры
//     UISerializer          документ ↔ файл
//
// ПРОСТОЕ ОСТАЁТСЯ ПРОСТЫМ (§129). Надпись на экране — это три строки:
//
//     sage::ui::UIRuntime ui;
//     auto* label = ui.Doc().Create("Hello");
//     label->Ensure<sage::ui::UIText>().Text = "Привет";
//
// Маски, эффекты, промежуточные цели и материалы появляются только тогда,
// когда их попросили (§130, §131).
// ---------------------------------------------------------------------------
#include "sage/ui/animation/UIAnimationValue.h"
#include "sage/ui/core/UIComponent.h"
#include "sage/ui/core/UIContext.h"
#include "sage/ui/core/UIDocument.h"
#include "sage/ui/core/UINode.h"
#include "sage/ui/core/UIRegistry.h"
#include "sage/ui/core/UITypes.h"
#include "sage/ui/debug/UIDebug.h"
#include "sage/ui/effects/UIEffect.h"
#include "sage/ui/input/UIEvent.h"
#include "sage/ui/input/UIHitTest.h"
#include "sage/ui/input/UIInput.h"
#include "sage/ui/input/UIInteraction.h"
#include "sage/ui/layout/UILayout.h"
#include "sage/ui/layout/UILayoutSolver.h"
#include "sage/ui/layout/UITransform.h"
#include "sage/ui/mask/UIMask.h"
#include "sage/ui/render/UIBackend.h"
#include "sage/ui/render/UIDrawBuilder.h"
#include "sage/ui/serialization/UIPrefab.h"
#include "sage/ui/serialization/UISerializer.h"
#include "sage/ui/style/UIStyle.h"
#include "sage/ui/visual/UIBorder.h"
#include "sage/ui/visual/UIFill.h"
#include "sage/ui/visual/UIImage.h"
#include "sage/ui/visual/UIShape.h"
#include "sage/ui/visual/UIText.h"
#include "sage/ui/visual/UITextLayout.h"
#include "sage/ui/widgets/UIWidgets.h"

namespace sage::ui {

// Зарегистрировать всё встроенное. Звать необязательно (реестры делают это
// лениво), но полезно там, где важен детерминированный момент: тесты, загрузка
// документа до первого кадра.
void UIInitialize();

// ---------------------------------------------------------------------------
// РАНТАЙМ — склейка подсистем в один кадр.
//
// Держит документ, решатель, список команд, ввод и тему вместе и прогоняет их
// в правильном порядке. Ровно эту склейку и повторяли бы иначе и игра, и
// редактор, и плеер — каждый чуть по-своему.
// ---------------------------------------------------------------------------
class UIRuntime {
public:
    UIRuntime();

    UIDocument& Doc() { return m_doc; }
    const UIDocument& Doc() const { return m_doc; }
    UIContext& Context() { return m_ctx; }
    const UIContext& Context() const { return m_ctx; }
    UITheme& Theme() { return m_theme; }
    UIEventBus& Events() { return m_bus; }
    UIInputRouter& Input() { return m_input; }
    const UILayoutSolver& Layout() const { return m_layout; }
    const UIRenderList& DrawList() const { return m_list; }
    const UIProfile& Profile() const { return m_profile; }

    // Посчитать раскладку под текущий контекст. Отдельно от рисования, потому
    // что раскладка нужна и тем, кто не рисует (ввод, редактор, тест).
    void Update(float dt);
    // Раздать ввод. Между Update и Render: события могут поменять документ.
    UIInputResult HandleInput(const UIInputFrame& input);
    // Собрать команды и отдать бэкенду.
    void Render(IUIDrawBackend& backend);
    // Собрать команды без рисования — для тестов и снимков.
    void Build();

    void SetScreen(glm::vec2 pixels) { m_ctx.ScreenPixels = pixels; }

private:
    UIDocument m_doc;
    UIContext m_ctx;
    UITheme m_theme;
    UILayoutSolver m_layout;
    UIRenderList m_list;
    UIInputRouter m_input;
    UIEventBus m_bus;
    UIProfile m_profile;
};

} // namespace sage::ui
