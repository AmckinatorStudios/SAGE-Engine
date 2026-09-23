#include "sage/assets/import/FbxTree.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include "sage/assets/format/Blob.h"
#include "sage/assets/import/PolygonTriangulate.h"

// Разбор дерева записей двоичного FBX — см. FbxTree.h о том, почему он общий.
namespace sage::assets::fbx {

const Node* Node::Find(const char* name) const {
    for (const Node& c : Children) {
        if (c.Name == name) return &c;
    }
    return nullptr;
}

namespace {

class Reader {
public:
    Reader(const std::vector<uint8_t>& bytes) : m_bytes(bytes) {}

    bool Has(size_t n) const { return m_pos + n <= m_bytes.size(); }
    size_t Pos() const { return m_pos; }
    void Seek(size_t p) { m_pos = p; }

    uint8_t U8() { return Has(1) ? m_bytes[m_pos++] : 0; }
    uint32_t U32() {
        uint32_t v = 0;
        if (Has(4)) std::memcpy(&v, &m_bytes[m_pos], 4);
        m_pos += 4;
        return v;
    }
    uint64_t U64() {
        uint64_t v = 0;
        if (Has(8)) std::memcpy(&v, &m_bytes[m_pos], 8);
        m_pos += 8;
        return v;
    }
    int32_t I32() { return (int32_t)U32(); }
    int64_t I64() { return (int64_t)U64(); }
    float F32() {
        float v = 0.0f;
        if (Has(4)) std::memcpy(&v, &m_bytes[m_pos], 4);
        m_pos += 4;
        return v;
    }
    double F64() {
        double v = 0.0;
        if (Has(8)) std::memcpy(&v, &m_bytes[m_pos], 8);
        m_pos += 8;
        return v;
    }
    std::string Str(size_t n) {
        std::string s;
        if (Has(n)) s.assign((const char*)&m_bytes[m_pos], n);
        m_pos += n;
        return s;
    }
    const std::vector<uint8_t>& Bytes() const { return m_bytes; }

private:
    const std::vector<uint8_t>& m_bytes;
    size_t m_pos = 0;
};

// Массив в FBX: length, encoding (0 — как есть, 1 — deflate), compressedLength.
// Возвращает распакованные байты.
bool ReadArrayBytes(Reader& r, size_t elementSize, uint32_t& count, std::vector<uint8_t>& out) {
    count = r.U32();
    const uint32_t encoding = r.U32();
    const uint32_t compressed = r.U32();
    const size_t rawSize = (size_t)count * elementSize;
    if (encoding == 0) {
        out.resize(rawSize);
        for (size_t i = 0; i < rawSize; ++i) out[i] = r.U8();
        return true;
    }
    std::vector<uint8_t> packed(compressed);
    for (uint32_t i = 0; i < compressed; ++i) packed[i] = r.U8();
    // Тот же miniz, которым читаются пакеты игры: своего inflate заводить незачем.
    return InflateBytes(packed, rawSize, out);
}

template <typename T>
void BytesToNumbers(const std::vector<uint8_t>& bytes, uint32_t count, std::vector<double>& out) {
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        T v{};
        if ((i + 1) * sizeof(T) <= bytes.size()) std::memcpy(&v, &bytes[i * sizeof(T)], sizeof(T));
        out[i] = (double)v;
    }
}

template <typename T>
void BytesToInts(const std::vector<uint8_t>& bytes, uint32_t count, std::vector<int64_t>& out) {
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        T v{};
        if ((i + 1) * sizeof(T) <= bytes.size()) std::memcpy(&v, &bytes[i * sizeof(T)], sizeof(T));
        out[i] = (int64_t)v;
    }
}

bool ReadProperty(Reader& r, Property& p) {
    p.Type = (char)r.U8();
    switch (p.Type) {
        case 'Y': { int16_t v = 0; v = (int16_t)(r.U8() | (r.U8() << 8)); p.Number = v; return true; }
        case 'C': p.Number = r.U8() ? 1.0 : 0.0; return true;
        case 'I': p.Number = r.I32(); return true;
        case 'F': p.Number = r.F32(); return true;
        case 'D': p.Number = r.F64(); return true;
        case 'L': p.Number = (double)r.I64(); return true;
        case 'S':
        case 'R': {
            const uint32_t len = r.U32();
            p.Text = r.Str(len);
            return true;
        }
        case 'f': case 'd': case 'l': case 'i': case 'b': {
            const size_t sizes[] = {4, 8, 8, 4, 1};
            const char types[] = {'f', 'd', 'l', 'i', 'b'};
            size_t elem = 4;
            for (int i = 0; i < 5; ++i) {
                if (types[i] == p.Type) elem = sizes[i];
            }
            uint32_t count = 0;
            std::vector<uint8_t> raw;
            if (!ReadArrayBytes(r, elem, count, raw)) return false;
            if (p.Type == 'f') BytesToNumbers<float>(raw, count, p.Numbers);
            else if (p.Type == 'd') BytesToNumbers<double>(raw, count, p.Numbers);
            else if (p.Type == 'l') BytesToInts<int64_t>(raw, count, p.Ints);
            else if (p.Type == 'i') BytesToInts<int32_t>(raw, count, p.Ints);
            else BytesToInts<int8_t>(raw, count, p.Ints);
            return true;
        }
        default:
            return false; // неизвестный тип — дальше разбирать нечего
    }
}

// Одна запись. Возвращает false на нулевой записи (конец списка) или ошибке.
bool ReadNode(Reader& r, Node& node, bool wide) {
    const uint64_t endOffset = wide ? r.U64() : r.U32();
    const uint64_t numProps = wide ? r.U64() : r.U32();
    const uint64_t propsLen = wide ? r.U64() : r.U32();
    const uint8_t nameLen = r.U8();
    if (endOffset == 0) return false; // нулевая запись — конец списка
    node.Name = r.Str(nameLen);

    const size_t propsEnd = r.Pos() + (size_t)propsLen;
    node.Props.reserve((size_t)numProps);
    for (uint64_t i = 0; i < numProps; ++i) {
        Property p;
        if (!ReadProperty(r, p)) break;
        node.Props.push_back(std::move(p));
    }
    r.Seek(propsEnd);

    // Вложенные записи есть, если до конца записи ещё что-то осталось.
    while (r.Pos() + 13 <= (size_t)endOffset) {
        Node child;
        if (!ReadNode(r, child, wide)) break;
        node.Children.push_back(std::move(child));
    }
    r.Seek((size_t)endOffset);
    return true;
}

// --- Разбор геометрии --------------------------------------------------------


} // namespace

const std::vector<double>* Doubles(const Node* n) {
    if (!n) return nullptr;
    for (const Property& p : n->Props) {
        if (!p.Numbers.empty()) return &p.Numbers;
    }
    return nullptr;
}

const std::vector<int64_t>* Ints(const Node* n) {
    if (!n) return nullptr;
    for (const Property& p : n->Props) {
        if (!p.Ints.empty()) return &p.Ints;
    }
    return nullptr;
}

std::string Text(const Node* n, size_t index) {
    if (!n || n->Props.size() <= index) return {};
    return n->Props[index].Text;
}

// Значение свойства из блока Properties70: ищем запись P с нужным именем и
// берём последнее числовое значение.
double Property70(const Node* root, const char* name, double fallback) {
    if (!root) return fallback;
    const Node* props = root->Find("Properties70");
    if (!props) return fallback;
    for (const Node& p : props->Children) {
        if (p.Name != "P" || p.Props.empty() || p.Props[0].Text != name) continue;
        for (size_t i = p.Props.size(); i-- > 0;) {
            if (p.Props[i].Type != 'S' && p.Props[i].Type != 'R') return p.Props[i].Number;
        }
    }
    return fallback;
}

glm::vec3 Property70Vec(const Node* root, const char* name, glm::vec3 fallback) {
    if (!root) return fallback;
    const Node* props = root->Find("Properties70");
    if (!props) return fallback;
    for (const Node& p : props->Children) {
        if (p.Name != "P" || p.Props.empty() || p.Props[0].Text != name) continue;
        std::vector<double> nums;
        for (const Property& v : p.Props) {
            if (v.Type != 'S' && v.Type != 'R') nums.push_back(v.Number);
        }
        if (nums.size() >= 3) {
            return glm::vec3((float)nums[nums.size() - 3], (float)nums[nums.size() - 2],
                             (float)nums[nums.size() - 1]);
        }
    }
    return fallback;
}

struct LayerData {
    std::vector<double> Values;   // плоский массив компонент
    std::vector<int64_t> Indices; // непустой при IndexToDirect
    std::string Mapping;          // ByPolygonVertex / ByVertice / ByPolygon / AllSame
    std::string Reference;        // Direct / IndexToDirect
    int Components = 3;

    bool Empty() const { return Values.empty(); }

    // Значение для (индекс вершины в полигоне, индекс контрольной точки,
    // номер многоугольника).
    glm::vec3 At(size_t polyVertex, int64_t controlPoint, size_t polygon) const {
        int64_t idx = 0;
        if (Mapping == "ByVertice" || Mapping == "ByVertex" || Mapping == "ByControlPoint") {
            idx = controlPoint;
        } else if (Mapping == "AllSame") {
            idx = 0;
        } else if (Mapping == "ByPolygon") {
            // Одно значение на ГРАНЬ (плоские нормали, материал). Раньше этот
            // случай читался как «по углу», и угол k брал значение грани k —
            // нормали и цвета перемешивались по всей модели.
            idx = (int64_t)polygon;
        } else {
            idx = (int64_t)polyVertex; // ByPolygonVertex — самый частый случай
        }
        if (Reference == "IndexToDirect" || Reference == "Index") {
            if (idx < 0 || (size_t)idx >= Indices.size()) return glm::vec3(0.0f);
            idx = Indices[(size_t)idx];
        }
        const size_t base = (size_t)idx * (size_t)Components;
        if (idx < 0 || base + (size_t)Components > Values.size()) return glm::vec3(0.0f);
        glm::vec3 v(0.0f);
        for (int c = 0; c < Components && c < 3; ++c) v[c] = (float)Values[base + (size_t)c];
        return v;
    }
};

LayerData ReadLayer(const Node* layer, const char* valuesName, const char* indexName, int components) {
    LayerData data;
    if (!layer) return data;
    data.Components = components;
    if (const std::vector<double>* v = Doubles(layer->Find(valuesName))) data.Values = *v;
    if (indexName) {
        if (const std::vector<int64_t>* idx = Ints(layer->Find(indexName))) data.Indices = *idx;
    }
    data.Mapping = Text(layer->Find("MappingInformationType"));
    data.Reference = Text(layer->Find("ReferenceInformationType"));
    return data;
}


glm::mat4 Units::Matrix() const {
    glm::mat4 m(1.0f);
    if (ZUp) {
        // Тот же разворот, что и у точек: (x, y, z) -> (x, z, -y), то есть
        // поворот на -90° вокруг X. Записан матрицей, потому что преобразования
        // (кости, обратные матрицы привязки) переносятся сопряжением.
        m = glm::mat4(glm::vec4(1, 0, 0, 0), glm::vec4(0, 0, -1, 0), glm::vec4(0, 1, 0, 0),
                      glm::vec4(0, 0, 0, 1));
    }
    return glm::scale(glm::mat4(1.0f), glm::vec3(Scale)) * m;
}

Units ReadUnits(const Node& root) {
    Units units;
    if (const Node* settings = root.Find("GlobalSettings")) {
        const double unit = Property70(settings, "UnitScaleFactor", 1.0);
        if (unit > 0.0) units.Scale = (float)(unit / 100.0);
        units.ZUp = (int)Property70(settings, "UpAxis", 1.0) == 2; // 0=X, 1=Y, 2=Z
    }
    return units;
}

bool ReadTree(const std::string& path, Node& root, std::string& err) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        err = "не открыть файл: " + path;
        return false;
    }
    const std::streamsize size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> bytes((size_t)std::max<std::streamsize>(size, 0));
    if (size > 0) f.read((char*)bytes.data(), size);

    static const char kMagic[] = "Kaydara FBX Binary";
    if (bytes.size() < 27 || std::memcmp(bytes.data(), kMagic, sizeof(kMagic) - 1) != 0) {
        // Текстовый FBX начинается с комментария «; FBX 6.1.0 project file».
        // Ответ «формат не поддерживается» здесь бесполезен: человек видит .fbx
        // и в списке поддерживаемых .fbx тоже видит.
        err = "это ТЕКСТОВЫЙ FBX (старый формат). Пересохраните его двоичным: в "
              "экспорте Blender/Maya снимите галочку ASCII, либо переэкспортируйте в .glb";
        return false;
    }

    uint32_t version = 0;
    std::memcpy(&version, &bytes[23], 4);
    const bool wide = version >= 7500; // с 7.5 смещения 64-битные

    Reader r(bytes);
    r.Seek(27);
    while (r.Has(wide ? 25 : 13)) {
        Node node;
        if (!ReadNode(r, node, wide)) break;
        root.Children.push_back(std::move(node));
    }
    if (root.Children.empty()) {
        err = "файл FBX не разобрался (версия " + std::to_string(version) + ")";
        return false;
    }
    return true;
}

std::vector<MeshCorner> BuildCorners(const Node& geometry, const Units& units,
                                     const glm::mat4& nodeXform,
                                     std::vector<std::string>& warnings) {
    std::vector<MeshCorner> corners;
    const std::vector<double>* verts = Doubles(geometry.Find("Vertices"));
    const std::vector<int64_t>* polys = Ints(geometry.Find("PolygonVertexIndex"));
    if (!verts || !polys || verts->size() < 3 || polys->empty()) return corners;

    const LayerData normals =
        ReadLayer(geometry.Find("LayerElementNormal"), "Normals", "NormalsIndex", 3);
    // Нормали переводит матрица, ОБРАТНАЯ транспонированной: при неравномерном
    // масштабе узла обычная матрица перекашивает их, и объект освещается так,
    // будто его поверхность смотрит не туда.
    const glm::mat3 normalXform = glm::transpose(glm::inverse(glm::mat3(nodeXform)));
    const LayerData uvs = ReadLayer(geometry.Find("LayerElementUV"), "UV", "UVIndex", 2);
    if (normals.Empty()) warnings.push_back("в FBX нет нормалей — посчитаны по граням");

    // Материал ГРАНИ. Массив целых, а не чисел с плавающей точкой, поэтому
    // читается мимо ReadLayer. AllSame — вся геометрия одним материалом,
    // ByPolygon — по номеру на многоугольник.
    const Node* materialLayer = geometry.Find("LayerElementMaterial");
    const std::vector<int64_t>* polyMaterials =
        materialLayer ? Ints(materialLayer->Find("Materials")) : nullptr;
    const bool materialPerPolygon =
        polyMaterials && Text(materialLayer->Find("MappingInformationType")) == "ByPolygon";
    auto materialOf = [&](size_t polygon) -> int {
        if (!polyMaterials || polyMaterials->empty()) return 0;
        const size_t k = materialPerPolygon ? polygon : 0;
        if (k >= polyMaterials->size()) return 0;
        const int64_t m = (*polyMaterials)[k];
        return m >= 0 ? (int)m : 0;
    };

    // ЗЕРКАЛЬНЫЙ УЗЕЛ (масштаб -1 по оси — обычный приём для симметричных
    // деталей) меняет обход треугольников: лицевые становятся задними, и
    // отсечение съедает деталь целиком. Разворачиваем обход — он и есть то, что
    // отражение сломало. Смена осей в Units — поворот, обход она не меняет.
    const bool mirrored = glm::determinant(glm::mat3(nodeXform)) < 0.0f;

    auto controlPoint = [&](int64_t index) {
        glm::vec3 p(0.0f);
        const size_t base = (size_t)index * 3;
        if (index >= 0 && base + 2 < verts->size()) {
            p = glm::vec3((float)(*verts)[base], (float)(*verts)[base + 1], (float)(*verts)[base + 2]);
        }
        // Трансформ УЗЛА (Lcl Translation/Rotation/Scaling у Model, плюс
        // геометрическое смещение). Без него все части модели сваливаются в
        // начало координат и теряют свой масштаб: рюкзак из 53 деталей
        // приезжал кучей в три сантиметра, а не собранной сумкой.
        //
        // Единицы и ось «вверх» — ПОСЛЕ него и одним местом на весь формат
        // (Units::ToEngine): разворот, сделанный в разных местах по-разному,
        // разводит меш и скелет по разным системам координат.
        return units.ToEngine(glm::vec3(nodeXform * glm::vec4(p, 1.0f)));
    };

    // Многоугольники: индекс отрицателен на ПОСЛЕДНЕЙ вершине полигона
    // (хранится как ~index).
    std::vector<size_t> polygon; // позиции в плоском массиве polys
    std::vector<MeshCorner> ring;
    std::vector<glm::vec3> ringPos;
    size_t polygonIndex = 0;
    for (size_t i = 0; i < polys->size(); ++i) {
        polygon.push_back(i);
        const int64_t raw = (*polys)[i];
        if (raw >= 0) continue; // полигон ещё не кончился

        ring.clear();
        ringPos.clear();
        const int material = materialOf(polygonIndex);
        for (size_t pv : polygon) {
            int64_t cp = (*polys)[pv];
            if (cp < 0) cp = ~cp;
            MeshCorner out;
            out.ControlPoint = cp;
            out.Position = controlPoint(cp);
            out.Material = material;
            if (!normals.Empty()) {
                const glm::vec3 n =
                    units.DirToEngine(normalXform * normals.At(pv, cp, polygonIndex));
                const float len = glm::length(n);
                out.Normal = len > 1e-6f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
            }
            if (!uvs.Empty()) {
                const glm::vec3 uv = uvs.At(pv, cp, polygonIndex);
                out.TexCoords = glm::vec2(uv.x, uv.y);
            }
            ring.push_back(out);
            ringPos.push_back(out.Position);
        }

        const std::vector<uint32_t> tris = sage::assets::TriangulatePolygon(ringPos);
        for (size_t t = 0; t + 2 < tris.size(); t += 3) {
            const size_t first = corners.size();
            corners.push_back(ring[tris[t]]);
            if (mirrored) {
                corners.push_back(ring[tris[t + 2]]);
                corners.push_back(ring[tris[t + 1]]);
            } else {
                corners.push_back(ring[tris[t + 1]]);
                corners.push_back(ring[tris[t + 2]]);
            }
            if (normals.Empty()) {
                const glm::vec3 a = corners[first + 1].Position - corners[first].Position;
                const glm::vec3 b = corners[first + 2].Position - corners[first].Position;
                glm::vec3 n = glm::cross(a, b);
                const float len = glm::length(n);
                n = len > 1e-6f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
                for (int c = 0; c < 3; ++c) corners[first + (size_t)c].Normal = n;
            }
        }
        polygon.clear();
        ++polygonIndex;
    }
    return corners;
}

// --- Трансформ узла -----------------------------------------------------------

glm::mat4 EulerMatrix(const glm::vec3& deg, int order) {
    const glm::mat4 X = glm::eulerAngleX(glm::radians(deg.x));
    const glm::mat4 Y = glm::eulerAngleY(glm::radians(deg.y));
    const glm::mat4 Z = glm::eulerAngleZ(glm::radians(deg.z));
    // Порядок ПРИМЕНЕНИЯ: XYZ значит «сначала X», то есть матрица Z * Y * X.
    switch (order) {
        case 1: return Y * Z * X;   // XZY
        case 2: return X * Z * Y;   // YZX
        case 3: return Z * X * Y;   // YXZ
        case 4: return Y * X * Z;   // ZXY
        case 5: return X * Y * Z;   // ZYX
        default: return Z * Y * X;  // XYZ
    }
}

glm::mat4 NodeTransform::Matrix() const {
    const glm::mat4 T = glm::translate(glm::mat4(1.0f), Translation);
    const glm::mat4 Roff = glm::translate(glm::mat4(1.0f), RotationOffset);
    const glm::mat4 Rp = glm::translate(glm::mat4(1.0f), RotationPivot);
    const glm::mat4 RpInv = glm::translate(glm::mat4(1.0f), -RotationPivot);
    const glm::mat4 Soff = glm::translate(glm::mat4(1.0f), ScalingOffset);
    const glm::mat4 Sp = glm::translate(glm::mat4(1.0f), ScalingPivot);
    const glm::mat4 SpInv = glm::translate(glm::mat4(1.0f), -ScalingPivot);
    const glm::mat4 S = glm::scale(glm::mat4(1.0f), Scale);
    return T * Roff * Rp * EulerMatrix(PreRotation) * EulerMatrix(Rotation, RotationOrder) *
           glm::inverse(EulerMatrix(PostRotation)) * RpInv * Soff * Sp * S * SpInv;
}

NodeTransform ReadTransform(const Node& n) {
    NodeTransform t;
    t.Translation = Property70Vec(&n, "Lcl Translation", glm::vec3(0.0f));
    t.Rotation = Property70Vec(&n, "Lcl Rotation", glm::vec3(0.0f));
    t.Scale = Property70Vec(&n, "Lcl Scaling", glm::vec3(1.0f));
    t.PreRotation = Property70Vec(&n, "PreRotation", glm::vec3(0.0f));
    t.PostRotation = Property70Vec(&n, "PostRotation", glm::vec3(0.0f));
    t.RotationOffset = Property70Vec(&n, "RotationOffset", glm::vec3(0.0f));
    t.RotationPivot = Property70Vec(&n, "RotationPivot", glm::vec3(0.0f));
    t.ScalingOffset = Property70Vec(&n, "ScalingOffset", glm::vec3(0.0f));
    t.ScalingPivot = Property70Vec(&n, "ScalingPivot", glm::vec3(0.0f));
    const int order = (int)Property70(&n, "RotationOrder", 0.0);
    t.RotationOrder = order >= 0 && order <= 5 ? order : 0;
    return t;
}

glm::mat4 GeometricMatrix(const Node& n) {
    const glm::vec3 t = Property70Vec(&n, "GeometricTranslation", glm::vec3(0.0f));
    const glm::vec3 r = Property70Vec(&n, "GeometricRotation", glm::vec3(0.0f));
    const glm::vec3 s = Property70Vec(&n, "GeometricScaling", glm::vec3(1.0f));
    return glm::translate(glm::mat4(1.0f), t) * EulerMatrix(r) * glm::scale(glm::mat4(1.0f), s);
}

std::vector<int64_t> ChildrenInOrder(const Node& root, int64_t parent) {
    std::vector<int64_t> out;
    const Node* conns = root.Find("Connections");
    if (!conns) return out;
    for (const Node& c : conns->Children) {
        if (c.Name != "C" || c.Props.size() < 3 || c.Props[0].Text != "OO") continue;
        if ((int64_t)c.Props[2].Number == parent) out.push_back((int64_t)c.Props[1].Number);
    }
    return out;
}

} // namespace sage::assets::fbx
