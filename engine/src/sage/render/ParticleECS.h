#pragma once

class Scene;
class ParticleSystem;

// ---------------------------------------------------------------------------
// Связка ECS ↔ система частиц (как AnimationSystem для скелетов). Каждая сущность
// с ParticleEmitterComponent рождает частицы в позиции своего Transform; общий
// пул/отрисовка живут в одном ParticleSystem, которым владеет слой (редактор/
// рантайм/игра). Вызывается каждый кадр, отрисовку делает сам слой (sys.Draw).
// ---------------------------------------------------------------------------
namespace sage::fx {

// Спавнит частицы из всех активных эмиттеров сцены и продвигает пул на dt.
void UpdateEmitters(Scene& scene, ParticleSystem& sys, float dt);

} // namespace sage::fx
