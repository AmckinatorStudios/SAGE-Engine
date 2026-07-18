#pragma once
#include <glm/glm.hpp>

class Scene;
struct LightingEnvironment;

// ---------------------------------------------------------------------------
// Система скелетной анимации — связывает ECS со скелетным пайплайном (аналог
// того, как RenderSystem связывает ECS с отрисовкой мешей). Две операции над
// сущностями с AnimatedModelComponent:
//   • UpdateAnimators — ленивая инициализация (загрузка модели/демо + rig +
//     старт клипа) и продвижение времени/палитры костей каждый кадр;
//   • DrawAnimatedModels — отрисовка скиннинг-шейдером в текущий буфер, с
//     трансформом сущности и общим освещением сцены.
// Вызывается и редактором (в Play-режиме и для превью), и рантаймом SagePlayer.
// ---------------------------------------------------------------------------
namespace sage::anim {

// Инициализирует (при первом вызове) и продвигает анимации всех сущностей с
// AnimatedModelComponent. Безопасно вызывать каждый кадр.
void UpdateAnimators(Scene& scene, float dt);

// Рисует все анимированные модели сцены в текущий фреймбуфер (после основного
// прохода сцены — тест глубины включён). Освещение ПОЛНОЕ и совпадает со
// статическими мешами: передаются позиция камеры, окружение и карта теней
// солнца (матрица света + нативный хендл depth-текстуры + флаг), как в
// статическом lit-проходе.
void DrawAnimatedModels(Scene& scene, const glm::mat4& view, const glm::mat4& proj,
                        const glm::vec3& viewPos, const LightingEnvironment& env,
                        const glm::mat4& lightMatrix, unsigned int shadowMap, bool shadowsEnabled);

} // namespace sage::anim
