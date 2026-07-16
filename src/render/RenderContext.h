#pragma once
#include "Camera.h"
#include "ShadowMap.h"
#include "../scene/Light.h"
#include <glm/glm.hpp>

// ---------------------------------------------------------------------
// RenderContext — общие per-frame данные, которые нужны почти каждому
// рендер-проходу (см. RenderPass/RenderPipeline). Заменяет десятки отдельных
// параметров, которые раньше передавались вручную в каждый Draw()-вызов
// main.cpp. Часть ЯДРА рендера — не знает ни про одну конкретную игру.
//
// Передаётся по НЕконстантной ссылке через весь пайплайн: одни проходы
// пишут в него результат (ShadowPass выставляет Shadows после своего
// Execute), следующие проходы это читают — так каждому не нужно явно
// передавать ShadowMap& из main.cpp.
//
// ShadowCenter/ShadowRadius задаёт вызывающий (игра) каждый кадр ДО
// pipeline.Execute() — движок не знает "где корабль"/"что в центре сцены",
// это игровое решение.
// ---------------------------------------------------------------------
struct RenderContext {
    glm::mat4 View{1.0f};
    glm::mat4 Proj{1.0f};
    Camera* Cam = nullptr;
    LightingEnvironment* Lighting = nullptr;

    int ScreenWidth = 0, ScreenHeight = 0;
    float Time = 0.0f;      // текущее время (glfwGetTime) — для анимации типа воды
    float DeltaTime = 0.0f;

    glm::vec3 ShadowCenter{0.0f};
    float ShadowRadius = 20.0f;
    bool ShadowsEnabled = true;
    bool PostEnabled = true;

    // Заполняется ShadowPass после своего Execute (nullptr, если тени
    // выключены в этом кадре) — последующие проходы читают карту/матрицу
    // теней отсюда.
    const ShadowMap* Shadows = nullptr;
};
