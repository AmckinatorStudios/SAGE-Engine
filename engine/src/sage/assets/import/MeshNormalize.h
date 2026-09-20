#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/Mesh.h"
#include "sage/render/MeshData.h"

// ---------------------------------------------------------------------------
// НОРМАЛИЗАЦИЯ ГЕОМЕТРИИ — ОДНА НА ВСЕ ФОРМАТЫ.
//
// ЗАЧЕМ. Форматы договариваются о разном: glTF почти всегда несёт нормали и
// иногда касательные, FBX — нормали по углам и никаких касательных, OBJ — как
// повезёт, а у Blockbench их нет вовсе. Пока каждый импортёр решал это сам,
// получалось три разных ответа на один вопрос, и два из них — «никак»:
// касательных у статической геометрии не считал НИКТО, то есть карта нормалей
// на модели из файла ложилась по касательной по умолчанию (1,0,0) — то есть
// куда попало. Заметно это только на свету и объясняется «странно блестит».
//
// Здесь общий слой: импортёр отдаёт то, что нашёл в файле, а недостающее
// достраивается ОДНИМ кодом. Отсюда правило: формат отвечает за чтение, а не
// за то, каким движок увидит меш.
//
// ЧТО ЗДЕСЬ ЕСТЬ И ЧЕГО НЕТ. Здесь только то, что выводится из самой
// геометрии: нормали по треугольникам, касательные по развёртке,
// направление обхода при зеркальном трансформе. Единицы измерения, оси и
// повороты — НЕ здесь: они свойство формата и живут в его импортёре.
// ---------------------------------------------------------------------------
namespace sage::assets {

// Касательные по развёртке — накопление по треугольникам с ортогонализацией
// Грама-Шмидта. Шаблон, а не функция: вершина статического меша и вершина скина
// различаются полями, но формула у них одна, и вторая её копия разошлась бы с
// первой (так и было — см. SkinnedModel::RebuildTangents).
//
// Доступ к полям — четырьмя маленькими функциями, чтобы не заводить общий
// базовый класс вершины: у вершины нет поведения, и наследование здесь ничего
// не объясняет.
template <class GetPos, class GetUV, class GetNormal, class SetTangent>
void BuildTangents(std::size_t vertexCount, const std::vector<unsigned int>& indices, GetPos pos,
                   GetUV uv, GetNormal normal, SetTangent setTangent) {
    std::vector<glm::vec3> tan(vertexCount, glm::vec3(0.0f));
    std::vector<glm::vec3> bit(vertexCount, glm::vec3(0.0f));
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const unsigned ia = indices[i], ib = indices[i + 1], ic = indices[i + 2];
        if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount) continue;
        const glm::vec3 e1 = pos(ib) - pos(ia);
        const glm::vec3 e2 = pos(ic) - pos(ia);
        const glm::vec2 d1 = uv(ib) - uv(ia);
        const glm::vec2 d2 = uv(ic) - uv(ia);
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;   // вырожденная развёртка
        const float f = 1.0f / det;
        const glm::vec3 t = f * (d2.y * e1 - d1.y * e2);
        const glm::vec3 b = f * (d1.x * e2 - d2.x * e1);
        tan[ia] += t; tan[ib] += t; tan[ic] += t;
        bit[ia] += b; bit[ib] += b; bit[ic] += b;
    }
    for (std::size_t i = 0; i < vertexCount; ++i) {
        const glm::vec3 n = normal(i);
        glm::vec3 t = tan[i] - n * glm::dot(n, tan[i]);
        if (glm::dot(t, t) < 1e-12f) {
            // Треугольников с годной развёрткой не нашлось — берём любую ось,
            // перпендикулярную нормали: карта нормалей всё равно ляжет криво,
            // но кадр не получит NaN.
            t = std::fabs(n.x) < 0.9f ? glm::cross(n, glm::vec3(1, 0, 0))
                                      : glm::cross(n, glm::vec3(0, 1, 0));
        }
        t = glm::normalize(t);
        // w — знак бинормали: без него зеркальные по развёртке грани получают
        // вывернутый рельеф.
        const float w = glm::dot(glm::cross(n, t), bit[i]) < 0.0f ? -1.0f : 1.0f;
        setTangent(i, glm::vec4(t, w));
    }
}

// Есть ли у меша осмысленные нормали. Нулевая нормаль — это не «нормаль вниз»,
// а «её нет»: формат её не дал, и освещать по ней нельзя.
bool HasNormals(const sage::render::MeshData& mesh);

// Нормали по треугольникам, усреднённые по вершине (площадь треугольника —
// вес). Зовётся, только когда своих нормалей нет: считать поверх авторских
// значило бы стереть заданное вручную сглаживание.
void GenerateNormals(sage::render::MeshData& mesh);

// Есть ли касательные (не «поле по умолчанию», а настоящие).
bool HasTangents(const sage::render::MeshData& mesh);

// Касательные по развёртке. Требует нормалей и развёртки; без развёртки не
// делает ничего — придумывать её нельзя.
void GenerateTangents(sage::render::MeshData& mesh);

// Разворачивает обход треугольников. Нужно при ЗЕРКАЛЬНОМ трансформе
// (определитель < 0): отражение меняет направление обхода, и меш, оставленный
// как есть, оказывается вывернутым наизнанку — грани отбрасываются отсечением
// задних, и половина модели исчезает.
void FlipWinding(sage::render::MeshData& mesh);

// Достраивает то, чего не дал формат: нормали, касательные. Одна точка на все
// импортёры — её зовёт реестр (ImporterRegistry::Import) после любого из них.
void NormalizeImportedMesh(sage::render::MeshData& mesh);

} // namespace sage::assets
