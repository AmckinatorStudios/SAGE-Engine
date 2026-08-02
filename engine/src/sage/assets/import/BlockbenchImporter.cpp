#include <cmath>
#include <filesystem>
#include <fstream>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "sage/assets/import/Importer.h"

// ---------------------------------------------------------------------------
// Blockbench (.bbmodel) — редактор блочных моделей.
//
// ПОЧЕМУ ИМЕННО ОН. Для игры про постройку из блоков это самый прямой источник
// контента: в Blockbench рисуют ровно то, из чего такие игры и состоят —
// коробки с пиксельной текстурой. Гонять их через Blender ради .obj значит
// терять привязку к сетке и попутно ломать UV.
//
// ЕДИНИЦЫ. Blockbench меряет в «пикселях» — шестнадцатых долях блока, и куб
// 16x16x16 там означает один блок. Движок работает в единицах, где примитивный
// куб равен единице, поэтому импорт делит на 16: модель приходит в масштабе, в
// котором блок есть блок, и её не нужно подгонять руками.
//
// ЧТО ПОДДЕРЖИВАЕТСЯ: элементы-коробки (cube) с поворотом вокруг своей точки
// отсчёта, обе схемы UV (пофейсовая и box_uv), группы outliner как узлы сцены.
// Элементы-меши свободной формы (в Blockbench это отдельный тип) пропускаются
// с явным предупреждением: молча отдать модель без половины деталей хуже, чем
// сказать, каких именно.
// ---------------------------------------------------------------------------

namespace sage::assets {

namespace {

using json = nlohmann::json;

constexpr float kPixelsPerUnit = 16.0f;

glm::vec3 ReadVec3(const json& j, const glm::vec3& fallback = glm::vec3(0.0f)) {
    if (!j.is_array() || j.size() < 3) return fallback;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

// Шесть граней коробки. Порядок фиксирован и совпадает с именами Blockbench —
// по ним берутся UV.
struct FaceDef {
    const char* Name;
    glm::vec3 Normal;
    int Corner[4];   // индексы углов коробки, против часовой при взгляде снаружи
};

// Углы: бит 0 — X, бит 1 — Y, бит 2 — Z (0 = from, 1 = to).
const FaceDef kFaces[6] = {
    {"north", {0, 0, -1}, {0, 2, 3, 1}},
    {"south", {0, 0, 1},  {5, 7, 6, 4}},
    {"west",  {-1, 0, 0}, {4, 6, 2, 0}},
    {"east",  {1, 0, 0},  {1, 3, 7, 5}},
    {"up",    {0, 1, 0},  {2, 6, 7, 3}},
    {"down",  {0, -1, 0}, {4, 0, 1, 5}},
};

// Стандартная развёртка box_uv: крест из шести прямоугольников, размеры которых
// заданы габаритами самой коробки. Это документированная раскладка Blockbench, а
// не догадка — по ней же её рисует и сам редактор.
void BoxUV(const char* face, float u, float v, float dx, float dy, float dz, float out[4]) {
    struct Rect { float X, Y, W, H; };
    Rect r{0, 0, 0, 0};
    if (std::string(face) == "up")         r = {u + dz,            v,      dx, dz};
    else if (std::string(face) == "down")  r = {u + dz + dx,       v,      dx, dz};
    else if (std::string(face) == "east")  r = {u,                 v + dz, dz, dy};
    else if (std::string(face) == "north") r = {u + dz,            v + dz, dx, dy};
    else if (std::string(face) == "west")  r = {u + dz + dx,       v + dz, dz, dy};
    else /* south */                       r = {u + 2 * dz + dx,   v + dz, dx, dy};
    out[0] = r.X;
    out[1] = r.Y;
    out[2] = r.X + r.W;
    out[3] = r.Y + r.H;
}

} // namespace

bool ImportBlockbench(const std::string& path, ImportedScene& out, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "не открыть файл: " + path;
        return false;
    }
    json doc;
    try {
        f >> doc;
    } catch (const std::exception& e) {
        err = std::string("файл .bbmodel не разбирается как JSON: ") + e.what();
        return false;
    }

    // Разрешение текстуры задаёт систему координат UV. Без него нормировать UV
    // не во что, поэтому берём разумное значение по умолчанию и говорим об этом.
    float texW = 16.0f, texH = 16.0f;
    if (doc.contains("resolution") && doc["resolution"].is_object()) {
        texW = doc["resolution"].value("width", 16);
        texH = doc["resolution"].value("height", 16);
    } else {
        out.Warnings.push_back("нет блока resolution — UV нормированы по 16x16");
    }
    if (texW <= 0.0f) texW = 16.0f;
    if (texH <= 0.0f) texH = 16.0f;

    const bool globalBoxUV =
        doc.contains("meta") && doc["meta"].is_object() && doc["meta"].value("box_uv", false);

    if (!doc.contains("elements") || !doc["elements"].is_array()) {
        err = "в файле нет массива elements — это не модель Blockbench";
        return false;
    }

    int skippedMeshes = 0;
    for (const json& el : doc["elements"]) {
        if (!el.is_object()) continue;
        const std::string type = el.value("type", "cube");
        if (type != "cube") {
            ++skippedMeshes;
            continue;
        }

        const glm::vec3 from = ReadVec3(el.value("from", json::array()));
        const glm::vec3 to = ReadVec3(el.value("to", json::array()));
        // Раздутие (inflate) расширяет коробку во все стороны — им в Blockbench
        // делают слои одежды поверх тела, и без него они z-конфликтуют с телом.
        const float inflate = el.value("inflate", 0.0f);
        const glm::vec3 lo = glm::min(from, to) - glm::vec3(inflate);
        const glm::vec3 hi = glm::max(from, to) + glm::vec3(inflate);
        const glm::vec3 size = hi - lo;

        ImportedNode node;
        node.Name = el.value("name", "cube");

        // Поворот вокруг СВОЕЙ точки отсчёта, а не вокруг начала координат:
        // origin в Blockbench — это шарнир, и повернуть вокруг нуля значит
        // уехать деталью в сторону.
        const glm::vec3 rotDeg = ReadVec3(el.value("rotation", json::array()));
        const glm::vec3 origin = ReadVec3(el.value("origin", json::array()));
        glm::mat4 xform(1.0f);
        if (rotDeg != glm::vec3(0.0f)) {
            const glm::vec3 pivot = origin / kPixelsPerUnit;
            xform = glm::translate(glm::mat4(1.0f), pivot);
            xform = glm::rotate(xform, glm::radians(rotDeg.z), glm::vec3(0, 0, 1));
            xform = glm::rotate(xform, glm::radians(rotDeg.y), glm::vec3(0, 1, 0));
            xform = glm::rotate(xform, glm::radians(rotDeg.x), glm::vec3(1, 0, 0));
            xform = glm::translate(xform, -pivot);
        }
        node.Transform = xform;

        const json faces = el.value("faces", json::object());
        const bool boxUV = globalBoxUV || !faces.is_object() || faces.empty();
        glm::vec2 uvOffset(0.0f);
        if (el.contains("uv_offset") && el["uv_offset"].is_array() && el["uv_offset"].size() >= 2)
            uvOffset = glm::vec2(el["uv_offset"][0].get<float>(), el["uv_offset"][1].get<float>());

        for (const FaceDef& fd : kFaces) {
            // Грань, у которой в Blockbench снят материал, не рисуется вовсе —
            // на этом держится оптимизация внутренних граней в блочных моделях.
            if (!boxUV && faces.is_object()) {
                if (!faces.contains(fd.Name)) continue;
                const json& fj = faces[fd.Name];
                if (fj.is_object() && fj.contains("texture") && fj["texture"].is_null()) continue;
            }

            float uv[4] = {0, 0, texW, texH};
            int rotation = 0;
            if (boxUV) {
                BoxUV(fd.Name, uvOffset.x, uvOffset.y, size.x, size.y, size.z, uv);
            } else {
                const json& fj = faces[fd.Name];
                if (fj.is_object() && fj.contains("uv") && fj["uv"].is_array() &&
                    fj["uv"].size() >= 4) {
                    for (int i = 0; i < 4; ++i) uv[i] = fj["uv"][i].get<float>();
                }
                if (fj.is_object()) rotation = fj.value("rotation", 0);
            }

            // Пиксели текстуры -> [0,1]. V переворачивается: у Blockbench начало
            // отсчёта сверху, у движка (как у OpenGL) — снизу.
            const float u0 = uv[0] / texW, u1 = uv[2] / texW;
            const float v0 = 1.0f - uv[1] / texH, v1 = 1.0f - uv[3] / texH;
            glm::vec2 corners[4] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
            // Поворот UV грани кратен 90° — крутим набор углов, а не координаты.
            const int steps = ((rotation / 90) % 4 + 4) % 4;
            for (int s = 0; s < steps; ++s) {
                const glm::vec2 first = corners[0];
                corners[0] = corners[1];
                corners[1] = corners[2];
                corners[2] = corners[3];
                corners[3] = first;
            }

            const unsigned int base = (unsigned int)node.Mesh.Vertices.size();
            for (int c = 0; c < 4; ++c) {
                const int mask = fd.Corner[c];
                Vertex v;
                v.Position = glm::vec3((mask & 1) ? hi.x : lo.x, (mask & 2) ? hi.y : lo.y,
                                       (mask & 4) ? hi.z : lo.z) /
                             kPixelsPerUnit;
                v.Normal = fd.Normal;
                v.TexCoords = corners[c];
                node.Mesh.Vertices.push_back(v);
            }
            const unsigned int quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
            for (unsigned int i : quad) node.Mesh.Indices.push_back(i);
        }

        if (!node.Mesh.Empty()) out.Nodes.push_back(std::move(node));
    }

    if (skippedMeshes > 0) {
        out.Warnings.push_back("пропущено элементов свободной формы (не коробки): " +
                               std::to_string(skippedMeshes) +
                               " — движок импортирует из .bbmodel только коробки");
    }
    if (out.Nodes.empty()) {
        err = "в модели не оказалось ни одной коробки" +
              std::string(skippedMeshes ? " (все элементы — свободной формы)" : "");
        return false;
    }

    // Текстуры модели — как материалы: путь у Blockbench относительный, и
    // разрешать его должен тот, кто знает, куда положен ассет.
    if (doc.contains("textures") && doc["textures"].is_array()) {
        for (const json& t : doc["textures"]) {
            if (!t.is_object()) continue;
            ImportedMaterial m;
            m.Name = t.value("name", "texture");
            m.AlbedoTexture = t.value("relative_path", t.value("path", std::string()));
            m.Roughness = 1.0f;   // пиксель-арт: блик на нём выглядит инородно
            out.Materials.push_back(std::move(m));
        }
    }
    return true;
}

} // namespace sage::assets
