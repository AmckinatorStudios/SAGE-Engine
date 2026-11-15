#pragma once
#include <string>
#include <vector>

class Scene;

// ---------------------------------------------------------------------------
// СЛОЙ СЦЕНЫ — классификация объектов для работы в редакторе и в скриптах:
// «Окружение», «Враги», «Свет», «Звук». Один слой на объект, по нему
// фильтруют и находят объекты.
//
// Это НЕ слой рендера (MeshRendererComponent::Layer — какой камерой рисуется
// меш): тот решает, что видно, а этот — к какой группе объект относится.
// Список слоёв хранится в самой сцене (Scene::Layers, ключ "layers" файла
// сцены): слой можно завести заранее, до первого объекта в нём, и он не
// пропадает, когда последний объект из него убрали.
// ---------------------------------------------------------------------------
struct SceneLayerComponent {
    std::string Layer;
};

namespace sage::scene {

// Регистрирует компонент в реестре (sage/scene/ComponentRegistry.h): запись в
// сцену, чтение, копирование и префабы — тем же путём, что у всех.
void RegisterSceneLayerComponent();

// Слои сцены: заведённые списком и те, что назначены объектам (сцена из
// старого файла или префаб со своим слоем), без повторов, в порядке списка.
std::vector<std::string> SceneLayerNames(Scene& scene);
// Завести слой. false — пустое имя или такой уже есть.
bool AddSceneLayer(Scene& scene, const std::string& name);
// Переименовать слой — и у всех объектов в нём. false — нового имени нет или
// оно занято.
bool RenameSceneLayer(Scene& scene, const std::string& from, const std::string& to);
// Удалить слой: из списка и у объектов (они остаются без слоя).
void RemoveSceneLayer(Scene& scene, const std::string& name);

// Слой объекта ("" — без слоя) и назначение ("" — снять).
std::string LayerOf(Scene& scene, int objectId);
void SetLayerOf(Scene& scene, int objectId, const std::string& layer);

// Теги объектов сцены без повторов, по алфавиту — подсказки поля тега.
std::vector<std::string> SceneTags(Scene& scene);

} // namespace sage::scene
