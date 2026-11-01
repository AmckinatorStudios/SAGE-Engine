#pragma once
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/MeshData.h"

// ---------------------------------------------------------------------------
// ПОСТРОИТЕЛЬ МЕША — геометрия, собранная кодом: рельеф по шуму, дорога по
// сплайну, лабиринт, планета, кристалл.
//
// Это CPU-часть: вершины, треугольники, подмеши по материалам, пересчёт
// нормалей и касательных. На видеокарту меш уходит отдельным шагом
// (ToMeshData -> Mesh), поэтому всё здесь проверяется тестами без окна.
//
// Индексы вершин — с НУЛЯ (C++). Скрипты видят их с единицы, как всё в Lua;
// перевод делает привязка (ScriptApi_Procedural.cpp), а не этот класс.
// ---------------------------------------------------------------------------
namespace sage::render {

class MeshBuilder {
public:
    // Новая вершина; нормаль (0,0,0) — «посчитать потом» (RecalculateNormals).
    unsigned int AddVertex(const glm::vec3& pos, const glm::vec3& normal = glm::vec3(0.0f),
                           const glm::vec2& uv = glm::vec2(0.0f));
    // Готовая вершина целиком — со вторым UV-каналом (TexCoords2). Им свой
    // шейдер получает данные вершины, которых нет в позиции/нормали/UV:
    // запечённый свет, затенение углов, вес ветра, номер слоя атласа.
    unsigned int AddVertex(const Vertex& v);
    // Пачка вершин и треугольников одним вызовом: индексы — от первой новой
    // вершины (с нуля). Меш, который пересобирают на ходу (рельеф, участок
    // мира, след), идёт сюда массивами, а не тысячами вызовов по вершине —
    // из скрипта это разница между миллисекундой и кадром. false — индекс
    // вне пачки (ничего не добавлено).
    bool AddArrays(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices);
    // Треугольник против часовой (лицом к смотрящему). false — индекс вне меша.
    bool AddTriangle(unsigned int a, unsigned int b, unsigned int c);
    // Четырёхугольник a-b-c-d по кругу — двумя треугольниками.
    bool AddQuad(unsigned int a, unsigned int b, unsigned int c, unsigned int d);

    // Дальше треугольники красятся материалом слота slot (с нуля). Подмеш на
    // слот: так у одного меша бывают трава, камень и вода.
    void UseMaterial(int slot);

    size_t VertexCount() const { return m_vertices.size(); }
    size_t TriangleCount() const;
    Vertex& At(unsigned int i) { return m_vertices[i]; }
    const Vertex& At(unsigned int i) const { return m_vertices[i]; }

    // Сглаженные (общие вершины усредняют нормаль) или плоские (у каждого
    // треугольника свои вершины) нормали.
    void RecalculateNormals(bool smooth = true);
    void RecalculateTangents();
    // Сдвинуть каждую вершину функцией: fn(позиция, нормаль, uv) -> позиция.
    void Deform(const std::function<glm::vec3(const glm::vec3&, const glm::vec3&, const glm::vec2&)>& fn);
    void Translate(const glm::vec3& d);
    void Scale(const glm::vec3& s);
    // Приклеить другой меш (его вершины — со сдвигом индексов).
    void Append(const MeshBuilder& other);
    void Clear();
    void Bounds(glm::vec3& lo, glm::vec3& hi) const;

    // Готовые данные: подмеши в порядке слотов, нормали досчитаны, если их не
    // задали, касательные — по развёртке.
    MeshData ToMeshData() const;

    // --- Готовые формы — начало, которое потом гнут Deform'ом ---------------
    // Сетка в плоскости XZ с центром в нуле: resX x resZ клеток, UV 0..1.
    // height(x, z) -> y — рельеф прямо при построении.
    static MeshBuilder Grid(float sizeX, float sizeZ, int resX, int resZ,
                            const std::function<float(float, float)>& height = {});
    static MeshBuilder Box(const glm::vec3& size);
    static MeshBuilder Sphere(float radius, int segments, int rings);
    static MeshBuilder Cylinder(float radius, float height, int segments, bool caps = true);

    // Объёмный спрайт: кусок картинки, «выдавленный» в толщину по альфе.
    // Лицо и оборот — по прямоугольнику куска (прозрачное отбрасывает материал
    // с вырезом по альфе), бока — только по краю непрозрачных пикселей, каждый
    // своим цветом. Плоская картинка предмета становится вещью, у которой есть
    // торец, — меч, ключ, листок, монета.
    //
    // rgba — W×H пикселей; rowsBottomUp — строки снизу вверх (так их хранит
    // движок). Кусок (rx, ry, rw, rh) — в пикселях от ЛЕВОГО ВЕРХНЕГО угла
    // картинки (как спрайт интерфейса). Меш — в единицах «длинная сторона
    // куска = 1», с центром в нуле, лицом к +Z; depthPixels — толщина в
    // пикселях картинки. Пустой кусок — пустой меш.
    static MeshBuilder ExtrudeImage(const unsigned char* rgba, int W, int H, bool rowsBottomUp, int rx,
                                    int ry, int rw, int rh, float depthPixels = 1.0f,
                                    float alphaCutoff = 0.5f);

private:
    std::vector<Vertex> m_vertices;
    // Треугольники по слотам материала: m_slots[i] — индексы слота i.
    std::vector<std::vector<unsigned int>> m_slots{std::vector<unsigned int>{}};
    int m_current = 0;
};

} // namespace sage::render
