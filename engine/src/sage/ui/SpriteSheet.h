#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/ui/NineSlice.h"

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
    // Девятина куска целиком — СВОЯ у каждого куска: заливка краёв и середины,
    // рисовать ли середину. Общая на всю текстуру (.sage9) к куску не
    // относится — иначе рамка и кнопка одного листа тянулись бы одинаково.
    SliceFill EdgeFill = SliceFill::Stretch;
    SliceFill CenterFill = SliceFill::Stretch;
    bool DrawCenter = true;
    // Точка опоры в долях куска (0,0 — левый верх): вокруг неё кусок
    // поворачивается и ею ставится — у меча это рукоять, а не середина.
    glm::vec2 Pivot{0.5f, 0.5f};

    NineSlice NineSliceOf() const {
        NineSlice n;
        n.SetBorder(Slice);
        n.EdgeFill = EdgeFill;
        n.CenterFill = CenterFill;
        n.DrawCenter = DrawCenter;
        return n;
    }
};

// ПОСЛЕДОВАНИЕ КАДРОВ — куски листа по порядку и скорость: бег персонажа,
// вращающаяся монетка, анимированная иконка. Картинка интерфейса проигрывает
// его (Image::Sequence), игра берёт кадр по времени (FrameAt).
struct SpriteSequence {
    std::string Name;
    std::vector<std::string> Frames;   // имена кусков
    float Fps = 12.0f;
    bool Loop = true;
    // Кадр на момент time (секунды). Пусто — нет кадров.
    std::string FrameAt(float time) const;
};

// Картинка для упаковки в атлас (RGBA, строки сверху вниз).
struct AtlasInput {
    std::string Name;
    const unsigned char* Rgba = nullptr;
    int Width = 0, Height = 0;
};

struct SpriteGrid {
    int CellW = 16, CellH = 16;  // размер клетки
    int PadX = 0, PadY = 0;      // промежуток между клетками (Spacing)
    int OffX = 0, OffY = 0;      // сдвиг первой клетки (Offset)
    // Поля от краёв картинки (Margin): лево, верх, право, низ — область нарезки.
    glm::ivec4 Margin{0};
    std::string Prefix = "sprite";
    bool SkipEmpty = true;       // пустые (прозрачные) клетки не заводить
};

// Автоматическая нарезка по островам непрозрачных пикселей.
struct SpriteAuto {
    float AlphaCutoff = 0.05f;   // с какой непрозрачности пиксель — часть куска
    int MinW = 2, MinH = 2;      // меньшие острова пропускаются
    int Padding = 0;             // запас вокруг найденного куска
    std::string Prefix = "sprite";
};

// Чем резали последний раз — чтобы, открыв лист снова, продолжить с теми же
// настройками. Описывает СПОСОБ, а не куски: куски — в Regions.
struct SpriteSlicing {
    enum class Mode { Grid, Automatic, Manual };
    Mode Method = Mode::Grid;
    SpriteGrid Grid;
    SpriteAuto Auto;
};

// Клетка сетки для предпросмотра: где и годится ли (целиком внутри картинки).
struct GridCell {
    glm::ivec4 Rect{0};
    bool Inside = true;          // false — выходит за картинку или поля
    bool Empty = false;          // целиком прозрачная (rgba дан)
};
// Клетки сетки — не создавая кусков (живой предпросмотр). rgba (строки сверху
// вниз) нужен для Empty.
std::vector<GridCell> GridCells(int width, int height, const SpriteGrid& grid, const unsigned char* rgba = nullptr);
// Острова непрозрачных пикселей (строки сверху вниз): прямоугольники в порядке
// сверху вниз, слева направо.
std::vector<glm::ivec4> FindIslands(const unsigned char* rgba, int width, int height, const SpriteAuto& params);

class SpriteSheet {
public:
    std::vector<SpriteRegion> Regions;
    std::vector<SpriteSequence> Sequences;
    SpriteSlicing Slicing;
    // Свободное имя «prefix_N»: имена куска уникальны в листе.
    std::string FreeName(const std::string& prefix) const;
    const SpriteSequence* FindSequence(const std::string& name) const;
    // Последование из кусков, чьи имена начинаются с prefix, по порядку
    // номеров в конце имени (run_2 раньше run_10). Возвращает число кадров.
    int SequenceFromPrefix(const std::string& name, const std::string& prefix, float fps = 12.0f);

    // АТЛАС: несколько картинок — в одну, с листом кусков по их именам.
    // Полки по высоте, сторона — степень двойки не больше maxSide.
    static bool PackAtlas(const std::vector<AtlasInput>& images, int padding, int maxSide,
                          std::vector<unsigned char>& rgba, int& width, int& height, SpriteSheet& sheet,
                          std::string& err);

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
