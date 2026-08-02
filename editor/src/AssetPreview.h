#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <glm/glm.hpp>

#include "sage/ecs/RenderBatch.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/Reflection.h"
#include "sage/render/SkyRenderer.h"

class Mesh;
struct Material;

// ---------------------------------------------------------------------------
// 3D-превью ассета: материал на сфере, модель — сама собой.
//
// ЗАЧЕМ. Материал в инспекторе показывался КВАДРАТИКОМ ЦВЕТА albedo. По нему
// нельзя понять ничего из того, ради чего материал и настраивают:
// металлический он или диэлектрик, гладкий или матовый, как легла карта
// нормалей, светится ли. Всё это видно только на изогнутой поверхности при
// свете — то есть на превью. Без него настройка материала шла вслепую: правишь
// ползунок, идёшь во вьюпорт искать объект с этим материалом, возвращаешься.
//
// Сфера, а не куб: у куба три плоские грани, и на них шероховатость и нормали
// почти не читаются — блик либо есть, либо нет. На сфере зрачок блика меняется
// плавно, и разница между 0.2 и 0.4 видна сразу.
//
// Превью рисуется В СВОЙ буфер и только когда панель открыта: это полный проход
// со светом, и платить за него, когда на него никто не смотрит, незачем.
// ---------------------------------------------------------------------------
class AssetPreview {
public:
    // Готовит буфер и геометрию (нужен активный графический контекст).
    void Init();

    // Освобождает GPU-ресурсы, ПОКА КОНТЕКСТ ЕЩЁ ЖИВ.
    //
    // Обязателен: буфер превью и куб окружения — объекты драйвера, а деструктор
    // панели срабатывает уже после того, как контекст разрушен. Это не
    // теоретическая аккуратность — именно на этом редактор падал при выходе,
    // причём ПОСЛЕ всей работы, когда всё уже сохранено и списать падение не на
    // что.
    void Shutdown();

    // Рисует превью материала и возвращает хендл текстуры для ImGui::Image.
    // 0 — превью недоступно.
    uint64_t RenderMaterial(const std::shared_ptr<Material>& material, int size);

    // То же для модели: меш вписывается в кадр по своим габаритам.
    uint64_t RenderMesh(const std::shared_ptr<Mesh>& mesh, int size);

    // Угол обзора превью — крутится мышью в панели.
    void Orbit(float dYaw, float dPitch);
    void Zoom(float delta);
    void ResetView();

private:
    uint64_t Render(const std::shared_ptr<Mesh>& mesh, const std::shared_ptr<Material>& material,
                    int size, float fitRadius);

    std::optional<Framebuffer> m_fbo;
    sage::ecs::RenderBatch m_batch;
    // Своё окружение для превью. Без него металл в превью ЧЁРНЫЙ: физически это
    // верно (металлу нечего отражать в пустоте), но бесполезно — а материал
    // настраивают именно по тому, что он отражает.
    std::optional<SkyRenderer> m_sky;
    sage::render::ReflectionSystem m_reflections;
    std::shared_ptr<Mesh> m_sphere;
    bool m_ready = false;

    float m_yaw = 35.0f;
    float m_pitch = 20.0f;
    float m_distance = 3.0f;
};
