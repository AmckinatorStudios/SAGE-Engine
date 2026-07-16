#pragma once
#include "Shader.h"
#include "Texture.h"
#include "Camera.h"
#include <glad/glad.h>
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

    ~BillboardSystem() {
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
    }

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

    size_t Count() const { return m_sprites.size(); }

private:
    void BeginBatch(Shader& shader, const Camera& camera, const glm::mat4& view, const glm::mat4& proj) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE); // билборды не пишут в depth — иначе перекрывают друг друга жёстко по границе квада

        shader.Use();
        shader.SetMat4("uView", view);
        shader.SetMat4("uProjection", proj);
        shader.SetVec3("uCameraRight", camera.Right);
        shader.SetVec3("uCameraUp", camera.Up);
        glBindVertexArray(m_vao);
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

        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    void EndBatch() {
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
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

        glGenVertexArrays(1, &m_vao);
        glGenBuffers(1, &m_vbo);

        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

        glBindVertexArray(0);
    }

    unsigned int m_vao = 0, m_vbo = 0;
    std::unordered_map<int, BillboardSprite> m_sprites;
    int m_nextId = 1;
};
