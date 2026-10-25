#pragma once
#include <cstdint>

#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// ШУМЫ — основа процедурной генерации: рельеф, облака, мрамор, раскраска,
// разброс объектов, дрожание камеры.
//
// Все шумы ДЕТЕРМИНИРОВАНЫ по зерну (seed): один и тот же мир строится
// одинаково на любой машине и при каждом запуске — без этого сохранение
// «зерно мира» ничего бы не значило, а сетевая игра видела бы разные горы у
// разных игроков. Поэтому здесь своя таблица перестановок по зерну, а не
// rand(): у rand() разная реализация в разных стандартных библиотеках.
//
// Диапазоны:
//   Perlin, Simplex — примерно [-1, 1], ноль в узлах решётки;
//   Value           — [-1, 1];
//   Worley (F1/F2)  — расстояние до ближайшей/второй точки, в долях ячейки
//                     (обычно 0..1.5); Cellular — номер ячейки в [0, 1).
// Фракталы (Fbm, Ridged, Turbulence) нормируются так, чтобы не зависеть от
// числа октав: добавить октаву — добавить деталь, а не поднять горы.
// ---------------------------------------------------------------------------
namespace sage::noise {

enum class Kind { Perlin, Simplex, Value, Worley, WorleyF2, Cellular };

// Таблица перестановок для одного зерна. Дёшево строится и копируется
// (512 байт) — заводите по одной на генератор.
class Source {
public:
    explicit Source(uint32_t seed = 0) { Reseed(seed); }
    void Reseed(uint32_t seed);
    uint32_t Seed() const { return m_seed; }

    float Perlin(float x, float y) const;
    float Perlin(float x, float y, float z) const;
    float Simplex(float x, float y) const;
    float Simplex(float x, float y, float z) const;
    float Value(float x, float y) const;
    float Value(float x, float y, float z) const;
    // Клеточный шум Ворлея: f1 — до ближайшей точки, f2 — до второй, cell —
    // случайное число ячейки ближайшей точки (для раскраски «по ячейкам»).
    void Worley(float x, float y, float z, float& f1, float& f2, float& cell) const;

    float Sample(Kind kind, float x, float y, float z) const;

    // Хэш целых координат в [0, 1) — разброс «по клеткам» без шума.
    float Hash(int x, int y = 0, int z = 0) const;

private:
    uint32_t m_seed = 0;
    uint8_t m_perm[512] = {};
    int P(int i) const { return m_perm[i & 511]; }
};

// Параметры фрактала: сколько октав, во сколько раз растёт частота
// (lacunarity) и падает вклад (gain) от октавы к октаве.
struct Fractal {
    int Octaves = 4;
    float Lacunarity = 2.0f;
    float Gain = 0.5f;
    float Frequency = 1.0f;
};

// fBm: сумма октав. Ridged: острые хребты (1 - |n|)^2. Turbulence: |n| —
// клубы дыма, огонь. Результаты: Fbm ~[-1, 1], Ridged и Turbulence [0, 1].
float Fbm(const Source& s, Kind kind, glm::vec3 p, const Fractal& f);
float Ridged(const Source& s, Kind kind, glm::vec3 p, const Fractal& f);
float Turbulence(const Source& s, Kind kind, glm::vec3 p, const Fractal& f);

// Искажение координат шумом (domain warp): p + strength * шум(p). Даёт
// разводы мрамора и «текучие» континенты вместо ровных пятен.
glm::vec3 Warp(const Source& s, Kind kind, glm::vec3 p, float strength, const Fractal& f);

} // namespace sage::noise
