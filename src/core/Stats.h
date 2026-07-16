#pragma once

// Простые счётчики текущего кадра — используются debug-оверлеем (F3).
// Обнуляются в начале каждого кадра в main.cpp, увеличиваются из мест,
// где реально идёт отрисовка (Mesh::Draw, Skybox::Draw...).
// Глобальная переменная — сознательно просто, это чисто отладочный счётчик,
// не часть игровой логики.
struct RenderStats {
    int DrawCalls = 0;

    void Reset() { DrawCalls = 0; }
};

extern RenderStats g_renderStats;
