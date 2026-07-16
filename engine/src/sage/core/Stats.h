#pragma once

// Простые счётчики текущего кадра — используются debug-оверлеем.
// Обнуляются в начале каждого кадра игрой, увеличиваются из мест,
// где реально идёт отрисовка (Mesh::Draw, Skybox::Draw...).
// Глобальная переменная — сознательно просто, это чисто отладочный счётчик,
// не часть игровой логики.
struct RenderStats {
    int DrawCalls = 0;
    int ChunksRendered = 0;

    void Reset() { DrawCalls = 0; ChunksRendered = 0; }
};

extern RenderStats g_renderStats;
