#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "sage/assets/format/Blob.h"
#include "sage/assets/import/Importer.h"
#include "sage/core/Log.h"

// ---------------------------------------------------------------------------
// FBX (.fbx) — двоичный.
//
// ЗАЧЕМ ЭТО ЗДЕСЬ. До сих пор движок открывал .obj, .gltf/.glb, .blend и
// .bbmodel. Список выглядит достаточным ровно до первой встречи с чужой
// моделью: FBX — то, во что по умолчанию экспортируют Blender, Maya, 3ds Max,
// Mixamo, Sketchfab и любой ассет-стор. Человек, скачавший модель, получал
// «формат не поддерживается» и вывод «свою модель загрузить нельзя».
//
// ЧТО ЭТО ЗА ФОРМАТ. Двоичный FBX (2013 года и новее) — дерево записей. У
// записи есть смещение конца, список типизированных свойств и вложенные записи;
// конец списка отмечен нулевой записью. Массивы чисел могут лежать сжатыми
// zlib — распаковываются тем же miniz, которым движок читает свои пакеты.
// Никакой внешней библиотеки, как и у остальных импортёров движка.
//
// ЧТО БЕРЁТСЯ. Геометрия: позиции, нормали, UV первого слоя, разбиение
// многоугольников на треугольники. Единицы приводятся к метрам по
// UnitScaleFactor из GlobalSettings (FBX почти всегда в сантиметрах), ось
// «вверх» — по UpAxis (Z-up разворачивается в Y-up движка). Трансформы узлов
// берутся из Model::Lcl Translation/Rotation/Scaling.
//
// ЧЕГО НЕ БЕРЁТСЯ, И ЭТО СКАЗАНО ЧЕСТНО: скиннинг, анимация и материалы. Для
// них у FBX своя история версий, и делать вид, что «FBX поддержан целиком», —
// значит обещать больше, чем сделано. Скелетную модель по-прежнему правильнее
// принести в .glb: там анимация — часть формата, а не приложение к нему.
//
// ТЕКСТОВЫЙ FBX (старый, до 2013) не разбирается, и вместо молчания об этом
// говорится прямым текстом с указанием, что делать.
// ---------------------------------------------------------------------------

namespace sage::assets {
namespace {

namespace fs = std::filesystem;

// --- Чтение дерева записей ---------------------------------------------------

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

    const Node* Find(const char* name) const {
        for (const Node& c : Children) {
            if (c.Name == name) return &c;
        }
        return nullptr;
    }
};

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

std::string Text(const Node* n, size_t index = 0) {
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

    // Значение для (индекс вершины в полигоне, индекс контрольной точки).
    glm::vec3 At(size_t polyVertex, int64_t controlPoint) const {
        int64_t idx = 0;
        if (Mapping == "ByVertice" || Mapping == "ByVertex" || Mapping == "ByControlPoint") {
            idx = controlPoint;
        } else if (Mapping == "AllSame") {
            idx = 0;
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

// Геометрия одного узла Geometry -> MeshData.
sage::render::MeshData BuildMesh(const Node& geometry, float unitScale, bool zUp,
                                 std::vector<std::string>& warnings) {
    sage::render::MeshData mesh;
    const std::vector<double>* verts = Doubles(geometry.Find("Vertices"));
    const std::vector<int64_t>* polys = Ints(geometry.Find("PolygonVertexIndex"));
    if (!verts || !polys || verts->size() < 3 || polys->empty()) return mesh;

    const LayerData normals =
        ReadLayer(geometry.Find("LayerElementNormal"), "Normals", "NormalsIndex", 3);
    const LayerData uvs = ReadLayer(geometry.Find("LayerElementUV"), "UV", "UVIndex", 2);
    if (normals.Empty()) warnings.push_back("в FBX нет нормалей — посчитаны по граням");

    auto controlPoint = [&](int64_t index) {
        glm::vec3 p(0.0f);
        const size_t base = (size_t)index * 3;
        if (index >= 0 && base + 2 < verts->size()) {
            p = glm::vec3((float)(*verts)[base], (float)(*verts)[base + 1], (float)(*verts)[base + 2]);
        }
        // Единицы -> метры и ось «вверх» -> Y. Разворот делается ЗДЕСЬ, на
        // сырых координатах: сделай его позже, и нормали с трансформами узлов
        // остались бы в чужой системе — модель выглядела бы правильно и
        // освещалась неправильно.
        p *= unitScale;
        if (zUp) p = glm::vec3(p.x, p.z, -p.y);
        return p;
    };

    // Многоугольники: индекс отрицателен на ПОСЛЕДНЕЙ вершине полигона
    // (хранится как ~index). Разбиваем веером от первой вершины.
    std::vector<size_t> polygon; // позиции в плоском массиве polys
    for (size_t i = 0; i < polys->size(); ++i) {
        polygon.push_back(i);
        const int64_t raw = (*polys)[i];
        if (raw >= 0) continue; // полигон ещё не кончился

        for (size_t k = 2; k < polygon.size(); ++k) {
            const size_t corner[3] = {polygon[0], polygon[k - 1], polygon[k]};
            const unsigned int firstIndex = (unsigned int)mesh.Vertices.size();
            for (int c = 0; c < 3; ++c) {
                const size_t pv = corner[c];
                int64_t cp = (*polys)[pv];
                if (cp < 0) cp = ~cp;

                Vertex v;
                v.Position = controlPoint(cp);
                if (!normals.Empty()) {
                    glm::vec3 n = normals.At(pv, cp);
                    if (zUp) n = glm::vec3(n.x, n.z, -n.y);
                    const float len = glm::length(n);
                    v.Normal = len > 1e-6f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
                }
                if (!uvs.Empty()) {
                    const glm::vec3 uv = uvs.At(pv, cp);
                    v.TexCoords = glm::vec2(uv.x, uv.y);
                }
                mesh.Vertices.push_back(v);
            }
            if (normals.Empty()) {
                const glm::vec3 a = mesh.Vertices[firstIndex + 1].Position - mesh.Vertices[firstIndex].Position;
                const glm::vec3 b = mesh.Vertices[firstIndex + 2].Position - mesh.Vertices[firstIndex].Position;
                glm::vec3 n = glm::cross(a, b);
                const float len = glm::length(n);
                n = len > 1e-6f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
                for (int c = 0; c < 3; ++c) mesh.Vertices[firstIndex + c].Normal = n;
            }
            for (int c = 0; c < 3; ++c) mesh.Indices.push_back(firstIndex + (unsigned int)c);
        }
        polygon.clear();
    }
    return mesh;
}

} // namespace

bool ImportFbx(const std::string& path, ImportedScene& out, std::string& err) {
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
    Node root;
    while (r.Has(wide ? 25 : 13)) {
        Node node;
        if (!ReadNode(r, node, wide)) break;
        root.Children.push_back(std::move(node));
    }
    if (root.Children.empty()) {
        err = "файл FBX не разобрался (версия " + std::to_string(version) + ")";
        return false;
    }

    // --- Единицы и ось «вверх» ---------------------------------------------
    //
    // FBX почти всегда в САНТИМЕТРАХ, а движок — в метрах. Импортировать «как
    // есть» значит получить персонажа ростом в сто восемьдесят единиц, который
    // не влезает ни в кадр, ни в физику, — и списать это на «модель кривая».
    float unitScale = 0.01f;
    bool zUp = false;
    if (const Node* settings = root.Find("GlobalSettings")) {
        const double unit = Property70(settings, "UnitScaleFactor", 1.0);
        if (unit > 0.0) unitScale = (float)(unit / 100.0);
        zUp = (int)Property70(settings, "UpAxis", 1.0) == 2; // 0=X, 1=Y, 2=Z
    }

    const Node* objects = root.Find("Objects");
    if (!objects) {
        err = "в файле FBX нет блока Objects";
        return false;
    }

    // Имена узлов Model — по связям Connections было бы точнее, но для одного
    // меша на файл (самый частый случай) достаточно порядка: имя i-й геометрии
    // берём у i-й модели, если она есть.
    std::vector<std::string> modelNames;
    for (const Node& n : objects->Children) {
        if (n.Name != "Model") continue;
        std::string name = n.Props.size() > 1 ? n.Props[1].Text : std::string();
        const size_t sep = name.find('\0');
        if (sep != std::string::npos) name = name.substr(0, sep);
        modelNames.push_back(name);
    }

    size_t geometryIndex = 0;
    for (const Node& n : objects->Children) {
        if (n.Name != "Geometry") continue;
        sage::render::MeshData mesh = BuildMesh(n, unitScale, zUp, out.Warnings);
        if (mesh.Empty()) {
            ++geometryIndex;
            continue;
        }
        ImportedNode node;
        node.Name = geometryIndex < modelNames.size() && !modelNames[geometryIndex].empty()
                        ? modelNames[geometryIndex]
                        : fs::path(path).stem().string();
        node.Mesh = std::move(mesh);
        out.Nodes.push_back(std::move(node));
        ++geometryIndex;
    }

    if (out.Nodes.empty()) {
        err = "в файле FBX не нашлось геометрии (возможно, там только кости, "
              "камеры или кривые — экспортируйте меши)";
        return false;
    }

    // Про непринятое говорим вслух: молчаливая потеря данных хуже отказа.
    if (root.Find("Objects")) {
        bool hasDeformer = false;
        for (const Node& n : objects->Children) {
            if (n.Name == "Deformer") { hasDeformer = true; break; }
        }
        if (hasDeformer) {
            out.Warnings.push_back(
                "в FBX есть скиннинг — он НЕ импортируется; для скелетной анимации "
                "экспортируйте модель в .glb");
        }
    }
    return true;
}

} // namespace sage::assets
