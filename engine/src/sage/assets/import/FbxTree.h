#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// ДЕРЕВО ЗАПИСЕЙ ДВОИЧНОГО FBX — общий разбор для всех, кто читает этот формат.
//
// ЗАЧЕМ ОТДЕЛЬНЫМ ФАЙЛОМ. Читателей у FBX стало двое: статическая геометрия
// (FbxImporter.cpp) и скелетная модель со скином и анимацией (FbxSkin.cpp).
// Формат один, и разбирать его двумя копиями кода нельзя: копии расходятся при
// первой же правке, и тогда один и тот же файл читается по-разному в
// зависимости от того, каким путём его принесли. Ровно эта беда уже случалась с
// материалами модели (см. editor/src/ModelMaterialImport.h).
//
// Здесь — только МЕХАНИКА формата: запись, свойство, массивы (в т.ч. сжатые
// zlib), поиск по имени, свойства из Properties70 и разбор многоугольников в
// треугольники. Смысл прочитанного (что считать мешем, где скелет, как назвать
// материал) живёт у потребителей.
// ---------------------------------------------------------------------------
namespace sage::assets::fbx {

// Одно свойство записи. Числовое приводится к double, массивы — к double/int64:
// FBX хранит одни и те же величины то float, то double, и разбирать это должен
// формат, а не каждый читатель.
struct Property {
    char Type = 0;                 // Y C I F D L S R f d l i b
    double Number = 0.0;           // для скалярных
    std::string Text;              // для S/R
    std::vector<double> Numbers;   // для массивов (любой числовой -> double)
    std::vector<int64_t> Ints;     // для целочисленных массивов
};

struct Node {
    std::string Name;
    std::vector<Property> Props;
    std::vector<Node> Children;

    const Node* Find(const char* name) const;
};

// Читает файл целиком и разбирает его в дерево записей. false — файл не
// открылся, это ТЕКСТОВЫЙ FBX или дерево не разобралось; err объясняет, что
// именно (человеку с .fbx в руках ответ «формат не поддерживается» бесполезен).
bool ReadTree(const std::string& path, Node& root, std::string& err);

// --- Доступ к содержимому ----------------------------------------------------
const std::vector<double>* Doubles(const Node* n);
const std::vector<int64_t>* Ints(const Node* n);
std::string Text(const Node* n, size_t index = 0);

// Значение свойства из блока Properties70.
double Property70(const Node* root, const char* name, double fallback);
glm::vec3 Property70Vec(const Node* root, const char* name, glm::vec3 fallback);

// --- Единицы и ось «вверх» ---------------------------------------------------
//
// FBX почти всегда в САНТИМЕТРАХ, а движок — в метрах; ось «вверх» бывает Z
// (3ds Max, старый Blender). Приведение к системе движка обязано быть ОДНИМ на
// все пути чтения, иначе меш и скелет приезжают в разных системах координат и
// персонаж выглядит правильно, но анимируется наизнанку.
struct Units {
    float Scale = 0.01f;   // множитель «единицы файла -> метры»
    bool ZUp = false;      // ось Z смотрит вверх — разворачиваем в Y-up

    // Точка файла -> точка движка.
    glm::vec3 ToEngine(const glm::vec3& p) const {
        const glm::vec3 s = p * Scale;
        return ZUp ? glm::vec3(s.x, s.z, -s.y) : s;
    }
    // Направление (нормаль): масштаб не нужен, разворот нужен.
    glm::vec3 DirToEngine(const glm::vec3& d) const {
        return ZUp ? glm::vec3(d.x, d.z, -d.y) : d;
    }
    // Матрица перехода. Нужна тем, кто переносит в систему движка не точку, а
    // ПРЕОБРАЗОВАНИЕ (кость, обратную матрицу привязки): такое переносится
    // сопряжением C * M * C⁻¹, и обе половины обязаны быть из одного места.
    glm::mat4 Matrix() const;
};

Units ReadUnits(const Node& root);

// --- Геометрия ---------------------------------------------------------------

// Один угол треугольника: где он в пространстве движка и из какой КОНТРОЛЬНОЙ
// ТОЧКИ файла получен. Контрольная точка нужна скину: веса в FBX заданы по
// контрольным точкам, а вершин после разбиения многоугольников больше.
struct MeshCorner {
    int64_t ControlPoint = 0;
    glm::vec3 Position{0.0f};
    glm::vec3 Normal{0.0f, 1.0f, 0.0f};
    glm::vec2 TexCoords{0.0f};
};

// Треугольники узла Geometry: по три угла на треугольник, уже в пространстве
// движка (nodeXform применяется к сырым координатам, затем единицы и ось).
// Нормали, которых нет в файле, считаются по граням.
std::vector<MeshCorner> BuildCorners(const Node& geometry, const Units& units,
                                     const glm::mat4& nodeXform,
                                     std::vector<std::string>& warnings);

} // namespace sage::assets::fbx
