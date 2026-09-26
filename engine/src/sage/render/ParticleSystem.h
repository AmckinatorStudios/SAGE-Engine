#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <glm/glm.hpp>

#include "sage/render/ParticleEffect.h"
#include "sage/render/ParticleSim.h"

class Camera;

// ---------------------------------------------------------------------
// ParticleSystem — все эмиттеры кадра и их отрисовка.
//
// КАЖДЫЙ ЭМИТТЕР — СВОЙ. Раньше частицы всех эффектов жили в одном пуле и
// рисовались одним вызовом с одним шейдером: у огня и дыма не могло быть ни
// разной текстуры, ни разного смешивания, ни разного способа отрисовки. Теперь
// у эмиттера свой пул (sage::fx::ParticleEmitter), своя текстура с
// раскадровкой и свой режим — квадрат к камере, вытянутый по скорости,
// лежачий, стоячий, объёмная фигура или лента-след.
//
// Три источника частиц:
//   • эмиттеры ОБЪЕКТОВ сцены — SyncEmitter раз в кадр (это делает
//     sage::fx::UpdateEmitters). Объект пропал — эмиттер перестаёт рождать, а
//     живые частицы доживают: дым от уничтоженного факела не исчезает рывком;
//   • разовые залпы — Burst(эффект, точка, число);
//   • именованные струи скриптов — CreateStream/SetStreamActive/….
//
// Симуляция не требует видеокарты (буферы создаются при первой отрисовке),
// поэтому логику эмиттеров проверяют модульные тесты.
// ---------------------------------------------------------------------
class ParticleSystem {
public:
    ParticleSystem();
    ~ParticleSystem();
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // Потолок живых частиц на весь кадр — сверх него новые не рождаются.
    static constexpr size_t kMaxParticles = 65536;

    // --- Эмиттеры объектов ---------------------------------------------------
    // key — постоянный ключ объекта (см. UpdateEmitters). Эффект копируется:
    // компонент может переехать в памяти ECS, а эмиттер живёт дольше кадра.
    void SyncEmitter(uint64_t key, const sage::fx::ParticleEffect& fx, const glm::mat4& world,
                     bool playing);
    void EmitNow(uint64_t key, int count);   // немедленно родить у эмиттера объекта
    void Restart(uint64_t key);              // цикл заново
    void ClearEmitter(uint64_t key);         // убрать живые частицы
    const sage::fx::ParticleEmitter* FindEmitter(uint64_t key) const;

    // --- Разовые залпы и струи ----------------------------------------------
    void Burst(const sage::fx::ParticleEffect& fx, glm::vec3 position, int count);
    void CreateStream(const std::string& id, const sage::fx::ParticleEffect& fx, glm::vec3 position);
    void SetStreamActive(const std::string& id, bool active);
    void SetStreamPosition(const std::string& id, glm::vec3 position);
    void RemoveStream(const std::string& id);

    // Шаг всех эмиттеров: рождение, силы, столкновения, дочерние эффекты.
    void Update(float dt);
    // ВСЁ ЖИВОЕ — ПРОЧЬ: смена сцены или проекта (залп не должен догорать в
    // чужом проекте).
    void Clear();

    // Столкновения с миром (CollisionMode::World): луч в физику сцены.
    // Без запроса такие частицы сталкиваются только с плоскостью.
    void SetCollisionQuery(sage::fx::CollisionQuery query);
    // Чем открывать файлы дочерних эффектов (.sagefx). По умолчанию — ссылка
    // проекта через AssetDatabase.
    void SetEffectLoader(std::function<bool(const std::string&, sage::fx::ParticleEffect&)> loader);

    // --- Отрисовка ------------------------------------------------------------
    // В текущий буфер, после непрозрачной геометрии: глубину проверяют, но не
    // пишут. camRight/camUp — мировые оси камеры.
    void Draw(glm::vec3 camRight, glm::vec3 camUp, const glm::mat4& view, const glm::mat4& proj);
    void Draw(const Camera& camera, const glm::mat4& view, const glm::mat4& proj);
    void DrawFromView(const glm::mat4& view, const glm::mat4& proj);

    size_t AliveCount() const;
    size_t StreamCount() const;
    size_t EmitterCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
