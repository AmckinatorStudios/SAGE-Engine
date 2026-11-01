#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "sage/rhi/Resources.h"

struct LightingEnvironment;
class Shader;

// ---------------------------------------------------------------------------
// ТЕКСТУРЫ СВЕТА (маски, «cookies») — все в одном атласе.
//
// Источник может светить картинкой: прожектор проецирует её конусом (узор
// фонаря, решётка, витраж, тень листвы), точечный — панорамой вокруг себя.
// Яркость пикселя умножает свет, цвет — окрашивает.
//
// ОДИН АТЛАС, а не текстура на источник: юнитов текстур у видеокарты
// конечное число, а ламп с масками в сцене — сколько угодно. Плиток в атласе
// столько, сколько РАЗНЫХ картинок в кадре (одна маска на сотню фонарей —
// одна плитка); размер плитки — по самой крупной картинке, урезается так,
// чтобы атлас влез в предел видеокарты. Константы «не больше N масок» нет.
//
// Атлас пересобирается только когда меняется НАБОР картинок; в устоявшемся
// кадре это сравнение списка путей.
// ---------------------------------------------------------------------------
namespace sage::render {

constexpr int kLightCookieUnit = 8;

// Где плитка slot лежит в атласе из cols×cols плиток: смещение и размер в UV.
// Общая формула для процессора и шейдера (CookieUV в PbrShaderShading.h).
glm::vec4 CookieTileRect(int slot, int cols);

class LightCookies {
public:
    // Собрать атлас под картинки источников env. maxTextureSize — предел
    // видеокарты (0 — 16384). Возвращает число плиток.
    int Prepare(const LightingEnvironment& env, int maxTextureSize = 0);
    // Номер плитки картинки, -1 — нет (путь пуст или картинка не читается).
    int SlotOf(const std::string& path) const;
    int Columns() const { return m_cols; }
    int TileSize() const { return m_tile; }
    // Привязать атлас и залить юниформы (uCookieAtlas, uCookieCols).
    void Upload(Shader& shader) const;

    // Атлас кадра — один на процесс: его читают и кластерный свет, и прежние
    // массивы источников (свои шейдеры, скелетные модели).
    static LightCookies& Current();

private:
    std::vector<std::string> m_paths;              // набор, под который собран атлас
    std::unordered_map<std::string, int> m_slots;  // путь -> плитка
    std::unique_ptr<sage::rhi::Texture2D> m_texture;
    int m_cols = 0;
    int m_tile = 0;
};

} // namespace sage::render
