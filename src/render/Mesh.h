#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
};

// Геометрия в виде обычных данных в ОЗУ — без всякого GL. Именно её готовят
// на фоновом потоке при асинхронной загрузке (парсинг .obj, генерация меша),
// а затем в главном потоке из неё делают Mesh (GL-загрузка VAO/VBO/EBO).
struct MeshData {
    std::vector<Vertex> Vertices;
    std::vector<unsigned int> Indices;
};

// Хранит геометрию на GPU (VAO/VBO/EBO) и умеет себя отрисовать
class Mesh {
public:
    Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices);
    // Удобный конструктор из CPU-данных (см. MeshData) — GL-загрузка. Должен
    // вызываться в главном потоке (с GL-контекстом), как и вариант выше.
    explicit Mesh(const MeshData& data) : Mesh(data.Vertices, data.Indices) {}
    ~Mesh();

    // Владеет GL-объектами (VAO/VBO/EBO) — копирование запрещено (иначе
    // деструктор одной копии удалил бы хендлы, которыми "владеет" и другая:
    // именно так раньше ломался рендер — CreateCube() возвращает Mesh по
    // значению, и неявный копирующий конструктор копировал голые числа
    // хендлов, а деструктор временного объекта тут же удалял их из-под
    // "настоящего", ещё живого Mesh). Перемещение разрешено и безопасно.
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    void Draw() const;

    static Mesh CreateCube();

private:
    unsigned int m_vao = 0, m_vbo = 0, m_ebo = 0;
    size_t m_indexCount = 0;

    void SetupMesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices);
};
