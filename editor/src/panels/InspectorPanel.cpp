#include "InspectorPanel.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ParticlePresets.h"
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

    // PBR (metallic-roughness) — основной путь освещения (Cook-Torrance).
    ImGui::SeparatorText("PBR");
    ImGui::SliderFloat("Metallic", &material->Metallic, 0.0f, 1.0f);
    ImGui::SliderFloat("Roughness", &material->Roughness, 0.0f, 1.0f);

    // Карты: путь правится вручную; по Enter/потере фокуса перезагружаем текстуры
    // материала (albedo/normal), чтобы вьюпорт сразу показал результат.
    char texBuf[512];
    std::snprintf(texBuf, sizeof(texBuf), "%s", material->TexturePath.c_str());
    if (ImGui::InputText("Albedo Map", texBuf, sizeof(texBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        material->TexturePath = texBuf;
        ResourceManager::Instance().ResolveMaterialTextures(*material);
    }
    char nrmBuf[512];
    std::snprintf(nrmBuf, sizeof(nrmBuf), "%s", material->NormalMapPath.c_str());
    if (ImGui::InputText("Normal Map", nrmBuf, sizeof(nrmBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        material->NormalMapPath = nrmBuf;
        ResourceManager::Instance().ResolveMaterialTextures(*material);
    }
    char metBuf[512];
    std::snprintf(metBuf, sizeof(metBuf), "%s", material->MetallicMapPath.c_str());
    if (ImGui::InputText("Metallic Map", metBuf, sizeof(metBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        material->MetallicMapPath = metBuf;
        ResourceManager::Instance().ResolveMaterialTextures(*material);
    }
    char rghBuf[512];
    std::snprintf(rghBuf, sizeof(rghBuf), "%s", material->RoughnessMapPath.c_str());
    if (ImGui::InputText("Roughness Map", rghBuf, sizeof(rghBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        material->RoughnessMapPath = rghBuf;
        ResourceManager::Instance().ResolveMaterialTextures(*material);
    }
    char aoBuf[512];
    std::snprintf(aoBuf, sizeof(aoBuf), "%s", material->AOMapPath.c_str());
    if (ImGui::InputText("AO Map", aoBuf, sizeof(aoBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        material->AOMapPath = aoBuf;
        ResourceManager::Instance().ResolveMaterialTextures(*material);
    }
    ImGui::TextDisabled("Normal: tangent-space (OpenGL). Metallic/Rough/AO use R channel.");
    ImGui::TextDisabled("Map value multiplies the factor above; Enter applies the path.");

    ImGui::SeparatorText("Legacy");
    ImGui::DragFloat("Shininess", &material->Shininess, 0.5f, 1.0f, 256.0f);
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
    entt::registry& reg = host.CurrentScene().Registry();

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
    if (reg.all_of<CameraComponent>(obj.Entity()) && ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
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
        }
    }

    // --- Свет (позиция — Transform сущности; тип: точечный / прожектор) ---
    if (reg.all_of<LightComponent>(obj.Entity()) && ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen)) {
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
        }
    }

    // --- Скрипт (поведение в Play-режиме) ---
    if (reg.all_of<ScriptComponent>(obj.Entity()) && ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen)) {
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
        }
    }

    // --- Твёрдое тело (симулируется в Play-режиме выбранным бэкендом физики) ---
    if (reg.all_of<RigidBodyComponent>(obj.Entity()) && ImGui::CollapsingHeader("Rigid Body", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(obj.Entity())) {
            // Порядок строго совпадает с sage::physics::BodyType.
            const char* types[] = {"Static", "Dynamic", "Kinematic"};
            int kind = (int)rb->Type;
            if (ImGui::Combo("Body Type", &kind, types, IM_ARRAYSIZE(types))) {
                host.PushUndoSnapshot();
                rb->Type = (sage::physics::BodyType)kind;
            }
            ImGui::DragFloat("Mass", &rb->Mass, 0.05f, 0.0f, 1000.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Friction", &rb->Friction, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Restitution", &rb->Restitution, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
            ImGui::TextDisabled("Dynamic falls under gravity; Static/Kinematic don't");
            if (ImGui::Button("Remove Rigid Body")) {
                host.PushUndoSnapshot();
                reg.remove<RigidBodyComponent>(obj.Entity());
            }
        }
    }

    // --- Коллайдер (форма для физики; размеры домножаются на Transform.Scale) ---
    if (reg.all_of<ColliderComponent>(obj.Entity()) && ImGui::CollapsingHeader("Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ColliderComponent* col = reg.try_get<ColliderComponent>(obj.Entity())) {
            // Порядок строго совпадает с sage::physics::ShapeType.
            const char* shapes[] = {"Box", "Sphere", "Capsule"};
            int shape = (int)col->Shape;
            if (ImGui::Combo("Shape", &shape, shapes, IM_ARRAYSIZE(shapes))) {
                host.PushUndoSnapshot();
                col->Shape = (sage::physics::ShapeType)shape;
            }
            if (col->Shape == sage::physics::ShapeType::Box) {
                ImGui::DragFloat3("Half Extents", &col->HalfExtents.x, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
            } else if (col->Shape == sage::physics::ShapeType::Sphere) {
                ImGui::DragFloat("Radius", &col->Radius, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
            } else { // Capsule
                ImGui::DragFloat("Radius", &col->Radius, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
                ImGui::DragFloat("Half Height", &col->HalfHeight, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
            }
            ImGui::TextDisabled("Sizes are scaled by the entity's Transform scale");
            if (ImGui::Button("Remove Collider")) {
                host.PushUndoSnapshot();
                reg.remove<ColliderComponent>(obj.Entity());
            }
        }
    }

    // --- Скелетно-анимированная модель (.glb/.gltf или процедурное демо) ---
    if (reg.all_of<AnimatedModelComponent>(obj.Entity()) && ImGui::CollapsingHeader("Animated Model", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(obj.Entity())) {
            char pathBuf[512];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", am->Path.c_str());
            if (ImGui::InputText("Model (.glb)", pathBuf, sizeof(pathBuf))) am->Path = pathBuf;
            host.TrackLastImGuiItem();
            ImGui::TextDisabled("Empty path = procedural demo (\"tentacle\")");
            if (am->Path.empty()) {
                if (ImGui::SliderInt("Demo Segments", &am->DemoSegments, 2, 16)) {
                    am->Ready = false; am->Model = nullptr; // пересобрать демо
                }
            }
            if (ImGui::Button("Reload")) { am->Ready = false; am->Model = nullptr; }

            // Список клипов — из проигрывателя (модель уже загружена системой).
            int clipCount = am->Anim.ClipCount();
            if (clipCount > 0) {
                if (am->Clip >= clipCount) am->Clip = 0;
                std::string preview = am->Anim.ClipName(am->Clip);
                if (ImGui::BeginCombo("Clip", preview.c_str())) {
                    for (int i = 0; i < clipCount; ++i) {
                        bool sel = (am->Clip == i);
                        if (ImGui::Selectable(am->Anim.ClipName(i).c_str(), sel)) {
                            am->Clip = i;
                            am->Anim.Play(i, am->Loop);
                        }
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextDisabled("No animation clips (bind pose)");
            }
            ImGui::DragFloat("Speed", &am->Speed, 0.02f, 0.0f, 8.0f); host.TrackLastImGuiItem();
            if (ImGui::Checkbox("Loop", &am->Loop)) am->Anim.Play(am->Clip, am->Loop);
            ImGui::SameLine();
            ImGui::Checkbox("Playing", &am->Playing);
            if (clipCount > 0) {
                ImGui::TextDisabled("t = %.2f s", am->Anim.Time());
            }
            if (ImGui::Button("Remove Animated Model")) {
                host.PushUndoSnapshot();
                reg.remove<AnimatedModelComponent>(obj.Entity());
            }
        }
    }

    // --- Эмиттер частиц (огонь/дым/искры/…): пресеты + тонкая настройка ---
    if (reg.all_of<ParticleEmitterComponent>(obj.Entity()) && ImGui::CollapsingHeader("Particle Emitter", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ParticleEmitterComponent* em = reg.try_get<ParticleEmitterComponent>(obj.Entity())) {
            ParticleEmitterConfig& cfg = em->Config;
            // Пресеты: применяют готовый конфиг, дальше его можно править.
            const auto& presets = ParticlePresets::Registry();
            std::string preview = (em->Preset >= 0 && em->Preset < (int)presets.size())
                                      ? presets[em->Preset].Name : "Custom";
            if (ImGui::BeginCombo("Preset", preview.c_str())) {
                for (int i = 0; i < (int)presets.size(); ++i) {
                    if (ImGui::Selectable(presets[i].Name, em->Preset == i)) {
                        host.PushUndoSnapshot();
                        em->Preset = i;
                        cfg = presets[i].Make();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Checkbox("Active", &em->Active);
            ImGui::SameLine();
            ImGui::Checkbox("Continuous", &em->Continuous);
            if (em->Continuous) {
                ImGui::DragFloat("Rate (p/s)", &cfg.EmissionRate, 0.5f, 0.0f, 500.0f); host.TrackLastImGuiItem();
            } else {
                ImGui::DragInt("Burst Count", &em->BurstCount, 1, 1, 500); host.TrackLastImGuiItem();
                ImGui::DragFloat("Burst Interval", &em->BurstInterval, 0.05f, 0.05f, 30.0f); host.TrackLastImGuiItem();
            }
            ImGui::DragFloatRange2("Speed", &cfg.SpeedMin, &cfg.SpeedMax, 0.05f, 0.0f, 50.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Gravity", &cfg.Gravity, 0.05f, -30.0f, 30.0f); host.TrackLastImGuiItem();
            ImGui::DragFloatRange2("Lifetime", &cfg.LifetimeMin, &cfg.LifetimeMax, 0.02f, 0.02f, 20.0f); host.TrackLastImGuiItem();
            ImGui::DragFloatRange2("Start Size", &cfg.StartSizeMin, &cfg.StartSizeMax, 0.005f, 0.0f, 5.0f); host.TrackLastImGuiItem();
            ImGui::DragFloatRange2("End Size", &cfg.EndSizeMin, &cfg.EndSizeMax, 0.005f, 0.0f, 5.0f); host.TrackLastImGuiItem();
            ImGui::ColorEdit4("Start Color", &cfg.StartColor.x); host.TrackLastImGuiItem();
            ImGui::ColorEdit4("End Color", &cfg.EndColor.x); host.TrackLastImGuiItem();
            ImGui::DragFloat3("Dir Min", &cfg.DirectionMin.x, 0.02f); host.TrackLastImGuiItem();
            ImGui::DragFloat3("Dir Max", &cfg.DirectionMax.x, 0.02f); host.TrackLastImGuiItem();
            ImGui::DragFloat("Spin", &cfg.AngularVelocityMax, 0.05f, 0.0f, 20.0f); host.TrackLastImGuiItem();
            if (ImGui::Button("Remove Emitter")) {
                host.PushUndoSnapshot();
                reg.remove<ParticleEmitterComponent>(obj.Entity());
            }
        }
    }

    // --- Единое «Add Component»: добавляет любой ОТСУТСТВУЮЩИЙ компонент ---
    DrawAddComponentMenu(host, obj);

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button("Delete Entity", ImVec2(-1, 0))) host.DeleteSelected();
    ImGui::PopStyleColor();
}

// Кнопка «Add Component» + попап со списком компонентов, которых у сущности ещё
// нет. Так добавление унифицировано (а удаление — кнопкой Remove в самой секции).
void InspectorPanel::DrawAddComponentMenu(EditorHost& host, GameObject obj) {
    entt::registry& reg = host.CurrentScene().Registry();
    entt::entity e = obj.Entity();

    ImGui::Separator();
    if (ImGui::Button("Add Component", ImVec2(-1, 0))) ImGui::OpenPopup("##add_component");
    if (ImGui::BeginPopup("##add_component")) {
        bool any = false;
        auto item = [&](const char* label, bool present, auto addFn) {
            if (present) return;
            any = true;
            if (ImGui::MenuItem(label)) {
                host.PushUndoSnapshot();
                addFn();
                ImGui::CloseCurrentPopup();
            }
        };
        item("Camera", reg.all_of<CameraComponent>(e), [&] { reg.emplace<CameraComponent>(e); });
        item("Light", reg.all_of<LightComponent>(e), [&] { reg.emplace<LightComponent>(e); });
        item("Script", reg.all_of<ScriptComponent>(e),
             [&] { reg.emplace<ScriptComponent>(e, ScriptComponent{"assets/scripts/spin.lua"}); });
        item("Rigid Body", reg.all_of<RigidBodyComponent>(e), [&] { reg.emplace<RigidBodyComponent>(e); });
        item("Collider", reg.all_of<ColliderComponent>(e), [&] { reg.emplace<ColliderComponent>(e); });
        item("Animated Model", reg.all_of<AnimatedModelComponent>(e),
             [&] { reg.emplace<AnimatedModelComponent>(e); });
        item("Particle Emitter", reg.all_of<ParticleEmitterComponent>(e), [&] {
            ParticleEmitterComponent em;
            em.Config = ParticlePresets::Registry()[0].Make(); // Fire
            reg.emplace<ParticleEmitterComponent>(e, em);
        });
        if (!any) ImGui::TextDisabled("All components already added");
        ImGui::EndPopup();
    }
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
