#pragma once
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "sage/render/MeshData.h"

// ---------------------------------------------------------------------------
// Импорт из ЧУЖИХ форматов — открытый реестр, а не список в switch.
//
// ЗАЧЕМ РЕЕСТР. Поддержка форматов раньше жила прямо в загрузчике: цепочка
// сравнений расширения в ModelLoader, по ветке на формат. Пока форматов три,
// это незаметно; проблема в том, что добавить четвёртый может ТОЛЬКО тот, кто
// правит движок. Автор игры, которому нужен свой формат уровней, и автор
// плагина редактора остаются снаружи.
//
// Реестр переворачивает зависимость: движок объявляет, ЧТО такое импортёр, а
// кто угодно — движок, игра, плагин — регистрирует свой. Встроенные форматы
// регистрируются ровно тем же вызовом, что и чужие: если бы у них был
// привилегированный путь, интерфейс никто бы не проверял.
//
// ЕДИНОЕ ПРЕДСТАВЛЕНИЕ. Все импортёры отдают ImportedScene: список мешей с
// именами, материалами и трансформами. Это намеренно НЕ «сразу Mesh»: импорт
// не должен требовать графического контекста (иначе конвертер нельзя запустить
// в CI), и не должен терять иерархию (иначе сцена из .blend превращается в один
// слипшийся меш).
//
// ЧТО ИМПОРТЁР ОБЯЗАН ДЕЛАТЬ ПРИ ОШИБКЕ: вернуть false и НАПИСАТЬ ПОЧЕМУ,
// человеческим текстом, с указанием на действие. «Не удалось загрузить» —
// бесполезный ответ; «файл сжат (Blender 3.0+ по умолчанию), пересохраните с
// выключенным Compress» — полезный.
// ---------------------------------------------------------------------------

namespace sage::assets {

// Материал, как его описал исходный формат. Намеренно беден: сюда попадает
// только то, что переживает перенос между любыми форматами.
struct ImportedMaterial {
    std::string Name;
    glm::vec3 Albedo{1.0f};
    glm::vec3 Emissive{0.0f};
    float Metallic = 0.0f;
    float Roughness = 0.8f;
    float Opacity = 1.0f;
    // Карты — путями относительно файла-источника (или просто именем файла:
    // экспортёры пишут чужие абсолютные пути, и искать картинку приходится
    // рядом с моделью). Пусто — карты нет.
    std::string AlbedoTexture;
    std::string NormalTexture;
    std::string MetallicTexture;
    std::string RoughnessTexture;
    std::string AOTexture;
    std::string EmissiveTexture;
};

// Один объект сцены: геометрия + куда её поставить.
struct ImportedNode {
    std::string Name;
    sage::render::MeshData Mesh;
    glm::mat4 Transform{1.0f};
    int MaterialIndex = -1;      // в ImportedScene::Materials; -1 — нет
};

struct ImportedScene {
    std::vector<ImportedNode> Nodes;
    std::vector<ImportedMaterial> Materials;
    // Свободные заметки импортёра для лога и панели импорта: «взято 3 объекта из
    // 5, два пропущены — кривые Безье». Молчаливая потеря данных хуже отказа.
    std::vector<std::string> Warnings;

    bool Empty() const { return Nodes.empty(); }
    size_t TotalVertices() const;
    size_t TotalTriangles() const;
    // Склеить всё в один меш — для тех мест, где нужен ровно один Mesh
    // (MeshRendererComponent). Трансформы узлов при этом ЗАПЕКАЮТСЯ в вершины.
    //
    // Границы по материалам при этом НЕ теряются: узлы группируются по своему
    // MaterialIndex, и каждая группа становится подмешем (sage::render::Submesh)
    // с тем же индексом материала. Именно это и делает многоматериальную модель
    // рисуемой: один Mesh на сущность, но по подмешу на материал.
    //
    // Группировка, а не «узел = подмеш», намеренно: в .glb сорок примитивов на
    // четырнадцать материалов — обычное дело, и сорок вызовов отрисовки вместо
    // четырнадцати были бы платой ни за что.
    sage::render::MeshData Flatten() const;
};

// Импортёр: путь -> сцена. err заполняется при false.
using ImportFn = std::function<bool(const std::string& path, ImportedScene& out, std::string& err)>;

struct ImporterInfo {
    std::string Extension;   // с точкой, в нижнем регистре: ".bbmodel"
    std::string Label;       // для диалога открытия: "Blockbench"
    ImportFn Import;
};

class ImporterRegistry {
public:
    static ImporterRegistry& Instance();

    // Регистрирует импортёр. Повторная регистрация того же расширения ЗАМЕНЯЕТ
    // прежний: так игра или плагин может перебить встроенный разбор своим, не
    // трогая движок.
    void Register(const std::string& extension, const std::string& label, ImportFn fn);

    bool CanImport(const std::string& extension) const;
    // Импорт по пути (расширение берётся из него). false + err.
    bool Import(const std::string& path, ImportedScene& out, std::string& err) const;

    const std::vector<ImporterInfo>& All() const { return m_importers; }
    // Строка фильтра для файлового диалога: ".obj,.gltf,.glb,.bbmodel,..."
    std::vector<std::string> Extensions() const;

private:
    ImporterRegistry();
    std::vector<ImporterInfo> m_importers;
};

// Встроенные импортёры. Зовётся один раз из ImporterRegistry — вынесено, чтобы
// каждый формат жил в своём .cpp и не тянул чужие зависимости.
void RegisterBuiltinImporters(ImporterRegistry& registry);

// Реализации отдельных форматов (регистрируются функцией выше).
bool ImportObj(const std::string& path, ImportedScene& out, std::string& err);
bool ImportGltf(const std::string& path, ImportedScene& out, std::string& err);
bool ImportBlockbench(const std::string& path, ImportedScene& out, std::string& err);
bool ImportBlend(const std::string& path, ImportedScene& out, std::string& err);
bool ImportFbx(const std::string& path, ImportedScene& out, std::string& err);
bool ImportSageMesh(const std::string& path, ImportedScene& out, std::string& err);

} // namespace sage::assets
