#pragma once
#include <glad/glad.h>

// ---------------------------------------------------------------------
// Настройки прохода пост-процессинга (см. assets/shaders/post.frag). Все
// значения — обычные uniform'ы, заливаемые каждый кадр в main.cpp, поэтому
// игра (или debug-переопределения) может менять картинку без правки движка.
// ---------------------------------------------------------------------
struct PostProcessSettings {
    float Exposure = 1.05f;         // экспозиция HDR перед тон-маппингом
    float Gamma = 2.2f;             // гамма-коррекция (sRGB ~2.2)
    float Saturation = 1.16f;       // 1.0 — без изменений, >1 — сочнее, 0 — ч/б
    float Contrast = 1.06f;         // 1.0 — без изменений
    float VignetteStrength = 0.35f; // 0 — выкл, затемнение к углам экрана
    bool Enabled = true;
};

// ---------------------------------------------------------------------
// PostProcess — владеет полноэкранным треугольником и рисует его. HDR-текстура
// цвета сцены (из Framebuffer) сэмплируется post.frag: тон-маппинг HDR->LDR,
// гамма-коррекция, виньетка/насыщенность/контраст — результат пишется в
// текущий привязанный буфер (в обычном режиме — экранный).
//
// main.cpp сам создаёт post-шейдер и выставляет его uniform'ы (как и для
// всех остальных шейдеров) — этот класс отвечает только за геометрию прохода.
//
// Часть ЯДРА рендера — не зависит от вокселей/The Boat.
// ---------------------------------------------------------------------
class PostProcess {
public:
    PostProcess() {
        // Пустой VAO: вершины полноэкранного треугольника генерируются в
        // post.vert из gl_VertexID, буфер не нужен. Но в core-профиле для
        // любого draw-вызова VAO должен быть привязан — поэтому создаём его.
        glGenVertexArrays(1, &m_vao);
    }
    ~PostProcess() { if (m_vao) glDeleteVertexArrays(1, &m_vao); }

    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    // Рисует полноэкранный проход. Вызывающий уже сделал shader.Use(),
    // выставил его uniform'ы и привязал текстуру сцены к юниту 0.
    void Draw() const {
        // Полноэкранный треугольник накрывает весь кадр — глубина не нужна
        // (и не должна мешать), временно выключаем depth-тест.
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(m_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
    }

private:
    unsigned int m_vao = 0;
};
