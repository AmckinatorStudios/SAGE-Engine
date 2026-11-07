#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// СЛОИ ОТРИСОВКИ — какие объекты рисует проход камеры.
//
// Слой — просто имя у объекта (MeshRendererComponent::Layer, "" — основной).
// Что за слой и зачем он, движок не знает и знать не должен: «рука», «небо»,
// «оружие», «3D-интерфейс» — имена игры. Камера раскладывает кадр на ПРОХОДЫ
// (CameraComponent::Passes), каждый рисует свои слои, со своей глубиной и
// своей проекцией, — а игра собирает из этого нужное ей поведение.
// ---------------------------------------------------------------------------
namespace sage::ecs {

// Основной слой: в данных — пустая строка, в списках проходов и в
// редакторе — «default» (пустую строку не напечатать в поле).
inline bool IsDefaultLayer(const std::string& layer) { return layer.empty() || layer == "default"; }
inline bool SameLayer(const std::string& a, const std::string& b) {
    return a == b || (IsDefaultLayer(a) && IsDefaultLayer(b));
}

struct LayerFilter {
    std::vector<std::string> Only;      // не пусто — только эти слои
    std::vector<std::string> Exclude;   // эти — никогда

    bool Accepts(const std::string& layer) const {
        for (const std::string& e : Exclude)
            if (SameLayer(e, layer)) return false;
        if (Only.empty()) return true;
        for (const std::string& o : Only)
            if (SameLayer(o, layer)) return true;
        return false;
    }
};

} // namespace sage::ecs
