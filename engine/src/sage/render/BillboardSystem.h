#pragma once
#include "Shader.h"
#include "Texture.h"
#include "Camera.h"
#include "sage/rhi/GraphicsDevice.h"
#include <memory>
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>

// ---------------------------------------------------------------------
// BillboardSystem — часть ядра рендера: рисует спрайты, которые всегда
// развёрнуты лицом к камере (billboard) — как в старых играх для деревьев,
// иконок над предметами, маркеров интереса, вспышек и т.п. Не путать с
// ParticleSystem: тот заточен под тысячи короткоживущих частиц с физикой
// (гравитация/старение), этот — под штучные именованные спрайты с текстурой,
// которые держатся сколько нужно (иконка "!" над NPC, маркер квеста,
// подпись предмета в мире), обновляясь только когда что-то реально меняется.
//
// Разворот к камере считается в шейдере (billboard.vert) через
// uCameraRight/uCameraUp — тем же способом, что и в ParticleSystem, поэтому
// оба рисуют билборды идентично и их легко визуально сочетать.
//
// Использование:
//   BillboardSystem billboards;
//   int id = billboards.Add({worldPos, {0.4f,0.4f}, &iconTexture});
//   ...
//   billboards.SetPosition(id, newPos);   // если объект двигается
//   billboards.Remove(id);                // когда спрайт больше не нужен
//   ...каждый кадр...
//   billboards.Draw(shader, camera, view, proj);
//
// Для одноразовой отрисовки без хранения (например, курсор наведения)
// есть статический DrawImmediate() — не требует создавать/удалять запись.
// ---------------------------------------------------------------------

// Точка привязки квада к его мировой позиции — где "низ" спрайта:
// Center — спрайт центрирован на точке (эффекты, маркеры в воздухе)
// Bottom — низ спрайта стоит на точке (иконка растёт вверх от места на земле)
enum class BillboardPivot { Center, Bottom };

struct BillboardSprite {
    glm::vec3 WorldPos{0.0f};
    glm::vec2 Size{0.5f, 0.5f};       // (ширина, высота) в мировых единицах
    const Texture* SpriteTexture = nullptr; // nullptr — рисуется сплошным цветом Tint
    glm::vec4 Tint{1.0f};             // цвет/альфа поверх текстуры (или сам цвет, если текстуры нет)
    float Rotation = 0.0f;            // поворот вокруг оси взгляда, радианы
    BillboardPivot Pivot = BillboardPivot::Center;
    bool Visible = true;
};

class BillboardSystem {
public:
    BillboardSystem() { SetupQuad(); }

    BillboardSystem(const BillboardSystem&) = delete;
    BillboardSystem& operator=(const BillboardSystem&) = delete;

    // Добавляет спрайт, возвращает id для последующего обновления/удаления
    int Add(const BillboardSprite& sprite) {
        int id = m_nextId++;
        m_sprites[id] = sprite;
        return id;
    }

    void Remove(int id) { m_sprites.erase(id); }
    void Clear() { m_sprites.clear(); }

    bool Has(int id) const { return m_sprites.find(id) != m_sprites.end(); }

    BillboardSprite* Get(int id) {
        auto it = m_sprites.find(id);
        return it != m_sprites.end() ? &it->second : nullptr;
    }

    void SetPosition(int id, glm::vec3 pos) {
        if (auto* s = Get(id)) s->WorldPos = pos;
    }
    void SetVisible(int id, bool visible) {
        if (auto* s = Get(id)) s->Visible = visible;
    }
    void SetTint(int id, glm::vec4 tint) {
        if (auto* s = Get(id)) s->Tint = tint;
    }

    // Рисует все хранимые спрайты. Группирует по текстуре внутри одного
    // прохода не делает (для десятков-сотен билбордов это не узкое место;
    // если понадобятся тысячи — тогда есть смысл добавить инстансинг, как
    // в ParticleSystem), зато рисует в порядке добавления, что достаточно
    // предсказуемо для UI-подобных маркеров в 3D-мире.
    void Draw(Shader& shader, const Camera& camera, const glm::mat4& view, const glm::mat4& proj) {
        if (m_sprites.empty()) return;
        BeginBatch(shader, camera, view, proj);
        for (auto& [id, sprite] : m_sprites) {
            if (sprite.Visible) DrawOne(shader, sprite);
        }
        EndBatch();
    }

    // Разовая отрисовка одного спрайта без сохранения — для случаев вроде
    // "нарисовать курсор/подсказку в этом кадре и забыть". Дороже, чем
    // групповой Draw() при многих спрайтах (BeginBatch/EndBatch на каждый),
    // но удобнее для редких одиночных вызовов.
    static void DrawImmediate(BillboardSystem& system, Shader& shader, const Camera& camera,
                               const glm::mat4& view, const glm::mat4& proj, const BillboardSprite& sprite) {
        if (!sprite.Visible) return;
        system.BeginBatch(shader, camera, view, proj);
        system.DrawOne(shader, sprite);
        system.EndBatch();
    }

    // Отрисовка ВСТРОЕННЫМ шейдером, без ассетов и без объекта Camera: оси
    // камеры берутся прямо из матрицы вида.
    //
    // ЗАЧЕМ. До этого нарисовать билборд мог только тот, кто принёс СВОЙ
    // billboard.vert/.frag, — и не принёс никто: ни плеер, ни редактор, ни
    // игры. Система жила в движке, скрипты её звали (sage.fx.AddBillboard), а
    // в кадре не появлялось ничего, и понять, почему, было невозможно: ошибок
    // не было, спрайты честно лежали в списке. Своим шейдером — ровно как у
    // частиц (ParticleSystem::BuiltinShader) — система становится работающей
    // сама по себе, а не заготовкой.
    void DrawFromView(const glm::mat4& view, const glm::mat4& proj) {
        if (m_sprites.empty()) return;
        Camera cam;
        cam.Right = glm::normalize(glm::vec3(view[0][0], view[1][0], view[2][0]));
        cam.Up = glm::normalize(glm::vec3(view[0][1], view[1][1], view[2][1]));
        Draw(BuiltinShader(), cam, view, proj);
    }

    size_t Count() const { return m_sprites.size(); }

private:
    // Встроенный шейдер билбордов (общий, ленивая компиляция; определён в .cpp).
    static Shader& BuiltinShader();

    void BeginBatch(Shader& shader, const Camera& camera, const glm::mat4& view, const glm::mat4& proj) {
        sage::rhi::GraphicsDevice& device = sage::rhi::GraphicsDevice::Get();
        device.SetBlend(true);
        device.SetDepthWrite(false); // билборды не пишут в depth — иначе перекрывают друг друга жёстко по границе квада

        shader.Use();
        shader.SetMat4("uView", view);
        shader.SetMat4("uProjection", proj);
        shader.SetVec3("uCameraRight", camera.Right);
        shader.SetVec3("uCameraUp", camera.Up);
    }

    void DrawOne(Shader& shader, const BillboardSprite& sprite) {
        shader.SetVec3("uWorldPos", sprite.WorldPos);
        shader.SetVec2("uSize", sprite.Size);
        shader.SetVec2("uPivot", sprite.Pivot == BillboardPivot::Bottom ? glm::vec2(0.0f, 0.5f) : glm::vec2(0.0f, 0.0f));
        shader.SetFloat("uRotation", sprite.Rotation);
        shader.SetVec4("uTint", sprite.Tint);

        if (sprite.SpriteTexture) {
            sprite.SpriteTexture->Bind(0);
            shader.SetInt("uTexture", 0);
            shader.SetInt("uUseTexture", 1);
        } else {
            shader.SetInt("uUseTexture", 0);
        }

        m_geometry->DrawArrays(6);
    }

    void EndBatch() {
        sage::rhi::GraphicsDevice::Get().SetDepthWrite(true);
    }

    void SetupQuad() {
        // Единичный квад (-0.5..0.5) + UV (0,0)..(1,1) — pivot/поворот/размер
        // применяются в шейдере, здесь только сырая геометрия
        float verts[] = {
            // pos            uv
            -0.5f, -0.5f,     0.0f, 0.0f,
             0.5f, -0.5f,     1.0f, 0.0f,
             0.5f,  0.5f,     1.0f, 1.0f,
            -0.5f, -0.5f,     0.0f, 0.0f,
             0.5f,  0.5f,     1.0f, 1.0f,
            -0.5f,  0.5f,     0.0f, 1.0f,
        };

        sage::rhi::VertexLayout layout;
        layout.Stride = 4 * sizeof(float);
        layout.Attributes = {
            {0, 2, sage::rhi::AttribType::Float, 0},
            {1, 2, sage::rhi::AttribType::Float, 2 * (int)sizeof(float)},
        };
        m_geometry = sage::rhi::GraphicsDevice::Get().CreateGeometry(layout);
        m_geometry->SetVertexData(verts, sizeof(verts), /*dynamic=*/false);
    }

    std::unique_ptr<sage::rhi::Geometry> m_geometry;
    std::unordered_map<int, BillboardSprite> m_sprites;
    int m_nextId = 1;
};
