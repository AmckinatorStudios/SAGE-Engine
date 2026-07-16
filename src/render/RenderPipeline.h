#pragma once
#include "RenderPass.h"
#include <memory>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------
// RenderPipeline — упорядоченный список RenderPass, прогоняемый по порядку
// каждый кадр. Заменяет хардкод-последовательность GL-вызовов, которая
// раньше жила прямо в main.cpp: теперь main.cpp один раз при старте
// собирает пайплайн (тени -> контент сцены -> пост-процесс -> ...), а
// каждый кадр только зовёт Execute(ctx).
//
// Часть ЯДРА рендера — сам список ничего не знает про конкретную игру;
// игровое содержимое попадает в проходы через CallbackPass (см.
// CallbackPass.h) или собственные реализации RenderPass, которые задаёт
// вызывающий код.
//
// Использование:
//   RenderPipeline pipeline;
//   auto* shadow = pipeline.Add<ShadowPass>();
//   shadow->DrawCasters = [&](Shader& depth, RenderContext&) { ...рисуем... };
//   pipeline.Add<CallbackPass>([&](RenderContext& ctx) { ...рисуем сцену... });
//   ...
//   // каждый кадр:
//   pipeline.Execute(ctx);
// ---------------------------------------------------------------------
class RenderPipeline {
public:
    // Создаёт проход типа T, добавляет в конец списка и возвращает указатель
    // для настройки. Пайплайн владеет проходом (unique_ptr).
    template <typename T, typename... Args>
    T* Add(Args&&... args) {
        auto pass = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = pass.get();
        m_passes.push_back(std::move(pass));
        return raw;
    }

    void Execute(RenderContext& ctx) {
        for (auto& pass : m_passes) pass->Execute(ctx);
    }

    void Clear() { m_passes.clear(); }
    size_t Count() const { return m_passes.size(); }

private:
    std::vector<std::unique_ptr<RenderPass>> m_passes;
};
