#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// ЛИСТ СПРАЙТОВ — картинка, нарезанная на именованные куски.
//
// Набор интерфейса или предметов приходит одним PNG: рамки, кнопки, ячейки,
// значки. Кусок по прямоугольнику движок брал давно (Sprite у картинки), но
// числа прямоугольника каждый раз писались руками в каждом месте, где кусок
// нужен. Лист описывает нарезку ОДИН раз — рядом с картинкой
// («gui.png.sagesheet», JSON): имя -> прямоугольник и, если кусок —
// растягиваемая рамка, его девятина. Дальше кусок берут по имени.
//
// Нарезать можно сеткой (клетки одного размера, с отступами и полями) и
// автоматически — по непрозрачным островам картинки (то, что разделено
// прозрачными промежутками, — отдельный кусок).
//
// Прямоугольники — в пикселях от ЛЕВОГО ВЕРХНЕГО угла картинки, как Sprite
// элемента интерфейса.
// ---------------------------------------------------------------------------
namespace sage::ui {

struct SpriteRegion {
    std::string Name;
    glm::ivec4 Rect{0};          // x, y, w, h
    glm::vec4 Slice{0.0f};       // девятина: лево, верх, право, низ (нули — нет)
};

struct SpriteGrid {
    int CellW = 16, CellH = 16;  // размер клетки
    int PadX = 0, PadY = 0;      // промежуток между клетками
    int OffX = 0, OffY = 0;      // поле от левого верхнего угла
    std::string Prefix = "sprite";
    bool SkipEmpty = true;       // пустые (прозрачные) клетки не заводить
};

class SpriteSheet {
public:
    std::vector<SpriteRegion> Regions;

    const SpriteRegion* Find(const std::string& name) const;
    SpriteRegion* Find(const std::string& name);
    // Добавить или заменить кусок с этим именем.
    void Set(const SpriteRegion& region);
    bool Remove(const std::string& name);
    bool Rename(const std::string& from, const std::string& to);

    // Нарезка сеткой. rgba (может быть nullptr) нужен только для SkipEmpty;
    // строки — сверху вниз, если rowsBottomUp = false. Возвращает, сколько
    // кусков заведено. Имена — Prefix_0, Prefix_1, … по строкам.
    int SliceGrid(int width, int height, const SpriteGrid& grid, const unsigned char* rgba = nullptr,
                  bool rowsBottomUp = false);
    // Нарезка по островам: связные (по сторонам) области с альфой не ниже
    // порога; острова меньше minSize по обеим сторонам пропускаются.
    // Порядок — сверху вниз, слева направо.
    int SliceAuto(const unsigned char* rgba, int width, int height, bool rowsBottomUp = false,
                  float alphaCutoff = 0.05f, int minSize = 2, const std::string& prefix = "sprite");

    // --- Файл ("картинка.sagesheet") --------------------------------------
    std::string ToJsonString() const;
    static bool FromJsonString(const std::string& text, SpriteSheet& out, std::string& err);
    bool SaveFile(const std::string& path, std::string& err) const;
    static bool LoadFile(const std::string& path, SpriteSheet& out, std::string& err);
    static std::string SidecarPath(const std::string& imagePath);
};

} // namespace sage::ui
