#pragma once
#include "Mesh.h"
#include "Texture.h"
#include "Shader.h"
#include <memory>
#include <vector>
#include <string>

// Одна "деталь" модели: геометрия + (опционально) текстура + тонирующий цвет.
// Одна модель обычно состоит из нескольких SubMesh (разные материалы/части).
struct SubMesh {
    std::shared_ptr<Mesh> MeshData;
    std::shared_ptr<Texture> DiffuseTexture; // может быть nullptr — тогда рисуем просто TintColor
    glm::vec3 TintColor{1.0f, 1.0f, 1.0f};
};

// Загружает 3D-модели из файлов .obj (+.mtl рядом), .gltf или .glb.
// Часть ЯДРА движка — не зависит от вокселей.
//
// Использование:
//   auto model = Model::Load("assets/models/dragon.glb");
//   ...
//   model->Draw(shader); // шейдер уже должен иметь uModel/uView/uProjection выставленными
class Model {
public:
    // Определяет формат по расширению файла (.obj / .gltf / .glb) и грузит.
    // Бросает std::runtime_error при ошибке.
    static std::unique_ptr<Model> Load(const std::string& path);

    // Рисует все SubMesh модели. Перед вызовом shader.Use() и uModel/uView/
    // uProjection/свет должны быть уже выставлены — Draw только переключает
    // uUseTexture/uObjectColor/текстуру между отдельными деталями модели.
    void Draw(Shader& shader) const;

    const std::vector<SubMesh>& SubMeshes() const { return m_subMeshes; }

private:
    static std::unique_ptr<Model> LoadObjInternal(const std::string& path);
    static std::unique_ptr<Model> LoadGltfInternal(const std::string& path, bool binary);
    static std::unique_ptr<Model> LoadFbxInternal(const std::string& path);

    std::vector<SubMesh> m_subMeshes;
};
