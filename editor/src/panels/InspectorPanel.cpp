#include "InspectorPanel.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/scene/Components.h"

// Редактор материала: правит поля РАЗДЕЛЯЕМОГО экземпляра из кэша
// ResourceManager — все сущности с этим материалом обновляются в вьюпорте
// сразу; Save фиксирует значения на диск, Revert перечитывает файл.
void InspectorPanel::DrawMaterialEditor(EditorHost& host) {
    std::string pathStr = host.SelectedAssetPath().string();
    std::shared_ptr<Material> material = ResourceManager::Instance().GetMaterial(pathStr);

    ImGui::TextDisabled("Material: %s", host.SelectedAssetPath().filename().string().c_str());
    ImGui::ColorEdit3("Albedo", &material->Albedo.x);
    ImGui::ColorEdit3("Emissive", &material->Emissive.x);
    ImGui::DragFloat("Shininess", &material->Shininess, 0.5f, 1.0f, 256.0f);
    char texBuf[512];
    std::snprintf(texBuf, sizeof(texBuf), "%s", material->TexturePath.c_str());
    if (ImGui::InputText("Texture", texBuf, sizeof(texBuf))) material->TexturePath = texBuf;
    ImGui::TextDisabled("Edits apply live to every entity using this material.");

    if (ImGui::Button("Save Material")) {
        try {
            material->SaveToFile(pathStr);
            LOG_INFO("Editor") << "Material saved: " << pathStr;
        } catch (const std::exception& e) {
            LOG_ERROR("Editor") << "Material save failed: " << e.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) ResourceManager::Instance().ReloadMaterial(pathStr);
}

void InspectorPanel::DrawEntityProperties(EditorHost& host) {
    GameObject obj = host.SelectedObject();

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
    if (ImGui::InputText("Name", buf, sizeof(buf))) obj.SetName(buf);
    host.TrackLastImGuiItem();
    ImGui::TextDisabled("Id: %d", obj.Id());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        Transform& tr = obj.GetTransform();
        ImGui::DragFloat3("Position", &tr.Position.x, 0.05f); host.TrackLastImGuiItem();
        ImGui::DragFloat3("Rotation", &tr.Rotation.x, 0.5f); host.TrackLastImGuiItem();
        ImGui::DragFloat3("Scale", &tr.Scale.x, 0.05f, 0.01f, 100.0f); host.TrackLastImGuiItem();
    }

    if (ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
        MeshRendererComponent& mr = obj.Renderer();
        ImGui::ColorEdit3("Color", &mr.Color.x); host.TrackLastImGuiItem();

        // Порядок строго совпадает с MeshRef::Type (индекс комбо = значение enum).
        const char* kinds[] = {"None", "Cube", "Sphere", "Plane", "Cylinder", "Cone", "Model"};
        int kind = (int)mr.Ref.type;
        if (ImGui::Combo("Mesh", &kind, kinds, IM_ARRAYSIZE(kinds))) {
            host.PushUndoSnapshot(); // дискретное изменение — прямая запись undo
            mr.Ref.type = (MeshRef::Type)kind;
            if (mr.Ref.type == MeshRef::Type::Model) {
                // Model — путь задаётся ниже и грузится по кнопке Load.
            } else {
                mr.Ref.path.clear();
                mr.MeshPtr = ResourceManager::Instance().GetPrimitive(mr.Ref.type); // None -> nullptr
            }
        }
        if (mr.Ref.type == MeshRef::Type::Model) {
            char pathBuf[512];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", mr.Ref.path.c_str());
            if (ImGui::InputText("Path", pathBuf, sizeof(pathBuf))) mr.Ref.path = pathBuf;
            host.TrackLastImGuiItem();
            ImGui::SameLine();
            if (ImGui::Button("Load")) {
                try {
                    mr.MeshPtr = ResourceManager::Instance().GetModel(mr.Ref.path);
                } catch (const std::exception& e) {
                    LOG_ERROR("Editor") << "Model load failed: " << e.what();
                }
            }
        }
    }

    // --- Материал (.sagemat): заменяет Color, общий для всех сущностей с ним ---
    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
        MeshRendererComponent& mr = obj.Renderer();
        if (mr.MaterialPath.empty()) {
            ImGui::TextDisabled("No material (entity uses Color above)");
        } else {
            if (mr.MaterialPtr) {
                ImGui::ColorButton("##mat_preview",
                                   ImVec4(mr.MaterialPtr->Albedo.r, mr.MaterialPtr->Albedo.g,
                                          mr.MaterialPtr->Albedo.b, 1.0f));
                ImGui::SameLine();
            }
            ImGui::TextWrapped("%s", mr.MaterialPath.c_str());
        }

        // Назначение: выбери .sagemat в Assets (клик по тайлу) — тут появится
        // кнопка Assign. Отдельного файлового диалога нет намеренно: панель
        // Assets и есть браузер файлов проекта.
        if (host.SelectedAssetPath().extension() == ".sagemat") {
            std::string label = "Assign \"" + host.SelectedAssetPath().filename().string() + "\"";
            if (ImGui::Button(label.c_str())) {
                host.PushUndoSnapshot();
                mr.MaterialPath = host.SelectedAssetPath().string();
                mr.MaterialPtr = ResourceManager::Instance().GetMaterial(mr.MaterialPath);
            }
        } else {
            ImGui::TextDisabled("Select a .sagemat in Assets to assign it");
        }
        if (!mr.MaterialPath.empty()) {
            if (ImGui::Button("Clear Material")) {
                host.PushUndoSnapshot();
                mr.MaterialPath.clear();
                mr.MaterialPtr = nullptr;
            }
        }
    }

    // --- Камера (игровая): панель Game рендерит от первой Primary-камеры ---
    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        entt::registry& reg = host.CurrentScene().Registry();
        if (CameraComponent* cam = reg.try_get<CameraComponent>(obj.Entity())) {
            ImGui::DragFloat("FOV", &cam->Fov, 0.5f, 10.0f, 140.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Near", &cam->NearClip, 0.01f, 0.001f, 10.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Far", &cam->FarClip, 1.0f, 1.0f, 5000.0f); host.TrackLastImGuiItem();
            if (ImGui::Checkbox("Primary", &cam->Primary)) host.PushUndoSnapshot();
            ImGui::TextDisabled("Game panel renders from the first Primary camera");
            if (ImGui::Button("Remove Camera")) {
                host.PushUndoSnapshot();
                reg.remove<CameraComponent>(obj.Entity());
            }
        } else {
            if (ImGui::Button("Add Camera")) {
                host.PushUndoSnapshot();
                reg.emplace<CameraComponent>(obj.Entity());
            }
        }
    }

    // --- Свет (позиция — Transform сущности; тип: точечный / прожектор) ---
    if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
        entt::registry& reg = host.CurrentScene().Registry();
        if (LightComponent* light = reg.try_get<LightComponent>(obj.Entity())) {
            const char* types[] = {"Point", "Spot"};
            int kind = (int)light->Kind;
            if (ImGui::Combo("Type", &kind, types, 2)) {
                host.PushUndoSnapshot(); // дискретное изменение — прямая запись undo
                light->Kind = (LightComponent::Type)kind;
            }
            ImGui::ColorEdit3("Light Color", &light->Color.x); host.TrackLastImGuiItem();
            ImGui::DragFloat("Intensity", &light->Intensity, 0.02f, 0.0f, 10.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Range", &light->Range, 0.1f, 0.5f, 100.0f); host.TrackLastImGuiItem();
            if (light->Kind == LightComponent::Type::Spot) {
                ImGui::DragFloat("Inner Cone", &light->InnerConeDeg, 0.5f, 1.0f, 89.0f); host.TrackLastImGuiItem();
                ImGui::DragFloat("Outer Cone", &light->OuterConeDeg, 0.5f, 1.0f, 89.0f); host.TrackLastImGuiItem();
                // Внешний угол не должен быть уже внутреннего (иначе конус
                // «выворачивается»): подтягиваем внешний до внутреннего.
                if (light->OuterConeDeg < light->InnerConeDeg) light->OuterConeDeg = light->InnerConeDeg;
                ImGui::TextDisabled("Cone points along the entity's forward (-Z rotation)");
            } else {
                ImGui::TextDisabled("Point light at this entity's position");
            }
            if (ImGui::Button("Remove Light")) {
                host.PushUndoSnapshot();
                reg.remove<LightComponent>(obj.Entity());
            }
        } else {
            if (ImGui::Button("Add Light")) {
                host.PushUndoSnapshot();
                reg.emplace<LightComponent>(obj.Entity());
            }
        }
    }

    // --- Скрипт (поведение в Play-режиме) ---
    if (ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen)) {
        entt::registry& reg = host.CurrentScene().Registry();
        if (ScriptComponent* sc = reg.try_get<ScriptComponent>(obj.Entity())) {
            char scriptBuf[512];
            std::snprintf(scriptBuf, sizeof(scriptBuf), "%s", sc->Path.c_str());
            if (ImGui::InputText("Lua file", scriptBuf, sizeof(scriptBuf))) sc->Path = scriptBuf;
            host.TrackLastImGuiItem();
            ImGui::TextDisabled("Runs in Play mode: OnStart(entity), OnUpdate(entity, dt)");
            if (ImGui::Button("Remove Script")) {
                host.PushUndoSnapshot();
                reg.remove<ScriptComponent>(obj.Entity());
            }
        } else {
            if (ImGui::Button("Add Script")) {
                host.PushUndoSnapshot();
                reg.emplace<ScriptComponent>(obj.Entity(), ScriptComponent{"assets/scripts/spin.lua"});
            }
        }
    }

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button("Delete Entity", ImVec2(-1, 0))) host.DeleteSelected();
    ImGui::PopStyleColor();
}

void InspectorPanel::Draw(EditorHost& host) {
    ImGui::Begin("Inspector");

    // Выбранный в Assets материал редактируется здесь же — независимо от
    // того, выбрана ли сущность.
    if (host.SelectedAssetPath().extension() == ".sagemat") {
        DrawMaterialEditor(host);
        ImGui::Separator();
    }

    if (host.SelectedObject().Valid()) {
        DrawEntityProperties(host);
    } else {
        ImGui::TextDisabled("Nothing selected");
    }

    ImGui::End();
}
