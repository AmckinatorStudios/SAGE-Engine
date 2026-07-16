#pragma once
#include "RenderPass.h"
#include <functional>
#include <utility>

// ---------------------------------------------------------------------
// CallbackPass — тонкая generic-обёртка RenderPass над произвольной лямбдой.
// Для проходов, чьё содержимое (воксельные чанки, вода, частицы, UI —
// что угодно игро-специфичное) на 100% задаётся вызывающим кодом: не нужно
// писать свой класс-наследник RenderPass под каждую мелочь пайплайна,
// достаточно передать функцию. Часть ЯДРА рендера — сам класс не содержит
// ничего игро-специфичного, только вызывает то, что ему дали.
// ---------------------------------------------------------------------
class CallbackPass : public RenderPass {
public:
    using Fn = std::function<void(RenderContext&)>;

    explicit CallbackPass(Fn fn) : m_fn(std::move(fn)) {}

    void Execute(RenderContext& ctx) override {
        if (m_fn) m_fn(ctx);
    }

private:
    Fn m_fn;
};
