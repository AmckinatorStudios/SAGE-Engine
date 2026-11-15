#pragma once
#include <string>

// ---------------------------------------------------------------------------
// НАСТРОЙКИ ГЕОМЕТРИИ ПРОЦЕДУРНОГО ПРИМИТИВА — детализация и сглаживание, а не
// размер.
//
// Размер объекта — Transform → Scale, и второго способа задать ширину, радиус
// или длину здесь нет: два источника размера (масштаб и «радиус» в
// геометрии) расходятся при первой же правке, и уже не понять, какой из них
// решает. Единственное исключение — высота капсулы: Scale по Y растянул бы и
// полусферы на торцах (капсула стала бы овальной), а Height удлиняет только
// цилиндрическую часть, сохраняя круглые торцы.
//
// НОЛЬ — «КАК ПО УМОЛЧАНИЮ». Все поля по умолчанию нулевые и при построении
// заменяются значениями формы (Resolved). Поэтому:
//   * старые сцены, где настроек нет, открываются той же геометрией;
//   * код, который ставит только тип меша (каталог объектов, скрипты), не
//     обязан знать про детализацию;
//   * в файл сцены пишется только то, что отличается от умолчаний.
//
// Слой render: без ECS, без GL — генераторы MeshData и тесты пользуются им
// headless.
// ---------------------------------------------------------------------------
namespace sage::render {

enum class PrimitiveShape { Cube, Sphere, Plane, Cylinder, Cone, Capsule };

struct PrimitiveParams {
    // Куб — по осям X/Y/Z; плоскость — X и Z.
    int SubX = 0, SubY = 0, SubZ = 0;
    // Сфера, цилиндр, конус, капсула — по окружности.
    int Segments = 0;
    // Сфера — кольца от полюса до полюса; капсула — кольца КАЖДОЙ полусферы.
    int Rings = 0;
    // Цилиндр и конус — пояса по высоте; капсула — пояса цилиндрической части.
    int HeightSegments = 0;
    // Капсула — общая высота исходной геометрии (0 — стандартная, 2).
    float Height = 0.0f;
    // Плоские грани вместо сглаженных (сфера, капсула; у цилиндра и конуса —
    // только боковая поверхность: торцы плоские всегда). Меняет нормали, а не
    // число треугольников.
    bool Flat = false;

    bool operator==(const PrimitiveParams& o) const {
        return SubX == o.SubX && SubY == o.SubY && SubZ == o.SubZ && Segments == o.Segments && Rings == o.Rings &&
               HeightSegments == o.HeightSegments && Height == o.Height && Flat == o.Flat;
    }
    bool operator!=(const PrimitiveParams& o) const { return !(*this == o); }

    // Все поля — «по умолчанию».
    bool IsDefault() const { return *this == PrimitiveParams{}; }

    // Значения, с которыми форма строится: нули заменены умолчаниями формы,
    // всё зажато в пределы (Limits ниже). Сравнение «тот же вариант» и ключ
    // кэша — по ним, а не по сырым полям.
    PrimitiveParams Resolved(PrimitiveShape shape) const;
    // Ключ кэша геометрии: одинаковые варианты делят один меш.
    std::string Key(PrimitiveShape shape) const;
    // Даёт ли этот набор ту же геометрию, что форма по умолчанию.
    bool SameAsDefault(PrimitiveShape shape) const;
};

// Умолчания формы — то, чем примитив был до настроек: сфера 32×24, цилиндр и
// конус 32 сектора, капсула 24×12 высотой 2 (размер коллайдера по умолчанию).
PrimitiveParams DefaultPrimitiveParams(PrimitiveShape shape);

// Пределы: ниже — форма вырождается (два сектора — не цилиндр), выше — сетка
// тяжелее, чем нужно примитиву, и правка ползунком перестаёт быть мгновенной.
struct PrimitiveLimits {
    static constexpr int kSubdivMax = 64;     // куб и плоскость, по оси
    static constexpr int kSegmentsMin = 3;
    static constexpr int kSegmentsMax = 128;
    static constexpr int kRingsMin = 2;       // сфера (полусфера капсулы — от 1)
    static constexpr int kRingsMax = 96;
    static constexpr int kHeightSegMax = 64;
    // Капсула: не ниже диаметра — полусферы сходятся в сферу и не пересекаются.
    static constexpr float kCapsuleHeightMin = 1.0f;
    static constexpr float kCapsuleHeightMax = 20.0f;
    static constexpr float kCapsuleRadius = 0.5f;
};

} // namespace sage::render
