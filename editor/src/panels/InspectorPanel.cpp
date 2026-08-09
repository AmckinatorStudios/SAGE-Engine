#include <cstdarg>
#include "InspectorPanel.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "sage/core/Log.h"
#include <algorithm>

#include "AssetSlot.h"
#include "EditorIcons.h"
#include "ModelMaterialImport.h"
#include "Project.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/import/Importer.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/render/ParticlePresets.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/ui/UIIcons.h"
#include "../Localization.h"

namespace fs = std::filesystem;

// Редактор материала: правит поля РАЗДЕЛЯЕМОГО экземпляра из кэша
// ResourceManager — все сущности с этим материалом обновляются в вьюпорте
// сразу; Save фиксирует значения на диск, Revert перечитывает файл.
// Слот текстуры: превью + путь + «Обзор…» + «Из Assets» + «Очистить».
// Слот текстуры материала. Вся механика — общий виджет (см. AssetSlot.h): и
// превью, и приём перетаскивания с проверкой типа, и «показать в Assets», и
// очистка. Здесь остаётся только то, что своё у карт материала: подпись, к
// какому каналу карта относится, и перезагрузка текстур материала после правки.
//
// Раньше этот слот был написан отдельно от слотов меша и материала и вёл себя
// иначе: путь правился полем ввода по Enter, бросок файла не того типа молча
// ничего не делал, а «где лежит эта текстура» узнавалось поиском по проекту.
void InspectorPanel::DrawTextureSlot(EditorHost& host, const char* label, std::string& path,
                                     const std::shared_ptr<Texture>& tex, const char* tooltip) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);

    const assetslot::Result r =
        assetslot::Draw(host, label, assetslot::Kind::Texture, path, &m_preview);
    if (r.Changed) {
        host.PushUndoSnapshot();
        path = r.Path;
        // Материал держит СВОИ указатели на текстуры: без пересборки картинка в
        // слоте новая, а объект в сцене остаётся со старой.
        if (std::shared_ptr<Material> mat =
                ResourceManager::Instance().GetMaterial(host.SelectedAssetPath().string())) {
            ResourceManager::Instance().ResolveMaterialTextures(*mat);
        }
    }
    if (r.BrowseRequested) {
        FileBrowser::Config c;
        c.Title = std::string(T("Texture: ")) + label;
        c.Filters = assetslot::Extensions(assetslot::Kind::Texture);
        c.FilterLabel = T("Images");
        if (host.CurrentProject().Loaded()) c.StartDir = host.CurrentProject().AssetsDir();
        m_browser.Open(c);
        m_browseTarget = &path;
        m_browseIsShader = false;
        m_browseIsMesh = false;
        m_browseIsMaterial = false;
    }
    // Путь есть, а текстуры нет — файл не прочитался. Слот показывает сам факт,
    // а причина уходит в консоль загрузчиком.
    if (!path.empty() && !tex) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), T("Failed to load: %s"), path.c_str());
    }
    ImGui::PopID();
    ImGui::Spacing();
}

void InspectorPanel::DrawMaterialEditor(EditorHost& host) {
    std::string pathStr = host.SelectedAssetPath().string();
    std::shared_ptr<Material> material = ResourceManager::Instance().GetMaterial(pathStr);

    // Файл мог не прочитаться (удалён, битый JSON) — тогда весь редактор ниже
    // разыменовывал бы nullptr. Выбор испорченного .sagemat в Assets ронял
    // редактор: превью-то от null защищалось, а первое же поле — нет.
    if (!material) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s", T("Material cannot be read"));
        ImGui::TextDisabled("%s", T("The file is gone or damaged — details in the console."));
        return;
    }

    // --- 3D-превью ------------------------------------------------------------
    //
    // Раньше материал показывался КВАДРАТИКОМ ЦВЕТА albedo. По нему нельзя
    // понять ничего из того, ради чего материал и настраивают: металл или
    // диэлектрик, гладкий или матовый, как легла карта нормалей. Всё это видно
    // только на изогнутой поверхности при свете.
    if (material) {
        const float side = std::min(ImGui::GetContentRegionAvail().x, 220.0f);
        const uint64_t tex = m_preview.RenderMaterial(material, (int)side);
        if (tex) {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)(std::intptr_t)tex, ImVec2(side, side), ImVec2(0, 1),
                         ImVec2(1, 0));
            // Вращение мышью: блик и шероховатость читаются только в движении —
            // на неподвижной картинке гладкое и почти гладкое неотличимы.
            if (ImGui::IsItemHovered()) {
                ImGuiIO& io = ImGui::GetIO();
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    m_preview.Orbit(io.MouseDelta.x * 0.5f, -io.MouseDelta.y * 0.5f);
                }
                if (io.MouseWheel != 0.0f) m_preview.Zoom(io.MouseWheel);
                ImGui::SetTooltip("%s", T("LMB orbits, wheel zooms"));
            }
            (void)p0;
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", T("Preview"));
        if (ImGui::SmallButton(T("Reset view"))) m_preview.ResetView();
        ImGui::EndGroup();
        ImGui::Spacing();
    }

    ImGui::ColorEdit3(T("Albedo"), &material->Albedo.x);
    ImGui::ColorEdit3(T("Emissive"), &material->Emissive.x);
    // Сила свечения отдельным ползунком, и его предел заметно больше единицы:
    // bloom срабатывает от яркости ВЫШЕ 1 (EngineConfig::BloomThreshold), а цвет
    // в редакторе зажат в 0..1. Без множителя «свечение» оставалось бы просто
    // светлым цветом без ореола.
    ImGui::DragFloat(T("Emissive Strength"), &material->EmissiveStrength, 0.05f, 0.0f, 20.0f, "%.2f");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("Emissive multiplier. Values above 1 produce a bloom halo."));
    }

    // PBR (metallic-roughness) — основной путь освещения (Cook-Torrance).
    ImGui::SeparatorText(T("PBR"));
    ImGui::SliderFloat(T("Metallic"), &material->Metallic, 0.0f, 1.0f);
    ImGui::SliderFloat(T("Roughness"), &material->Roughness, 0.0f, 1.0f);

    // Карты: путь правится вручную; по Enter/потере фокуса перезагружаем текстуры
    // материала (albedo/normal), чтобы вьюпорт сразу показал результат.
        // Слоты текстур: превью, путь, «Обзор…», «Из Assets», «Очистить».
    //
    // Раньше здесь стояли пять голых полей ввода, применявшихся по Enter: путь
    // к текстуре надо было ЗНАТЬ и напечатать без опечатки, а единственной
    // обратной связью была строчка в консоли. Слот показывает саму картинку —
    // назначено ли что-то и то ли это, что хотели, видно сразу.
    DrawTextureSlot(host, "Albedo", material->TexturePath, material->AlbedoTex,
                    T("Base colour. Multiplied by the Albedo above."));
    DrawTextureSlot(host, "Normal", material->NormalMapPath, material->NormalTex,
                    T("Normal map, tangent space (OpenGL: green is up)."));
    DrawTextureSlot(host, "Metallic", material->MetallicMapPath, material->MetallicTex,
                    T("Channel R. Multiplied by the Metallic factor above."));
    DrawTextureSlot(host, "Roughness", material->RoughnessMapPath, material->RoughnessTex,
                    T("Channel R. Multiplied by the Roughness factor above."));
    DrawTextureSlot(host, "Emissive", material->EmissiveMap, material->EmissiveTex,
                    T("Where it glows and where it does not. Multiplied by the emissive colour and strength."));
    DrawTextureSlot(host, "AO", material->AOMapPath, material->AOTex,
                    T("Ambient occlusion, channel R. Empty means AO = 1."));

    ImGui::TextDisabled("%s", T("Normal: tangent-space (OpenGL). Metallic/Rough/AO use R channel."));
    ImGui::TextDisabled("%s", T("Map value multiplies the factor above; Enter applies the path."));

    ImGui::SeparatorText(T("Transparency"));
    ImGui::SliderFloat(T("Opacity"), &material->Opacity, 0.0f, 1.0f);

    // Свой шейдер материала: пара .vert/.frag (см. docs/custom_shaders.md).
    // Правка файлов подхватывается на лету — ReloadChangedShaders в EditorLayer.
    ImGui::SeparatorText(T("Custom Shader"));
    char vsBuf[512];
    std::snprintf(vsBuf, sizeof(vsBuf), "%s", material->VertexShaderPath.c_str());
    if (ImGui::InputText(T("Vertex"), vsBuf, sizeof(vsBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        material->VertexShaderPath = vsBuf;
        material->ShaderPtr.reset();
    }
    char fsBuf[512];
    std::snprintf(fsBuf, sizeof(fsBuf), "%s", material->FragmentShaderPath.c_str());
    if (ImGui::InputText(T("Fragment"), fsBuf, sizeof(fsBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        material->FragmentShaderPath = fsBuf;
        material->ShaderPtr.reset();
    }
    if (material->HasCustomShader() && !material->Params.empty()) {
        ImGui::TextDisabled(T("Params: %d (edit in the .sagemat file)"), (int)material->Params.size());
    }

    // Свойства рендера рисуются ПО ТАБЛИЦЕ (MaterialRenderFields). Новая
    // возможность рендера появляется в инспекторе сама — от неё требуется поле
    // и строка таблицы, а не ещё одна правка здесь. Раньше каждое такое
    // свойство надо было дописать в пяти местах, и панель редактора отставала
    // от формата файла чаще всего: её забывали.
    ImGui::SeparatorText(T("Render"));
    for (const MaterialRenderField& f : MaterialRenderFields()) {
        if (f.Type == MaterialRenderField::Kind::Bool && f.AsBool) {
            ImGui::Checkbox(f.Label, &(material->Render.*f.AsBool));
        } else if (f.AsFloat) {
            ImGui::SliderFloat(f.Label, &(material->Render.*f.AsFloat), f.Min, f.Max);
        }
        if (f.Tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.Tooltip);
    }

    ImGui::SeparatorText(T("Legacy"));
    ImGui::DragFloat(T("Shininess"), &material->Shininess, 0.5f, 1.0f, 256.0f);
    ImGui::TextDisabled("%s", T("Edits apply live to every entity using this material."));

    if (ImGui::Button(T("Save Material"))) {
        try {
            material->SaveToFile(pathStr);
            LOG_INFO("Editor") << "Material saved: " << pathStr;
        } catch (const std::exception& e) {
            LOG_ERROR("Editor") << "Material save failed: " << e.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Revert"))) ResourceManager::Instance().ReloadMaterial(pathStr);
}

// Настройки импорта выбранной модели (.obj/.gltf/.glb): масштаб/центрирование/
// нормализация в сайдкар .sageimport. Reimport перечитывает меш и обновляет все
// сущности сцены, использующие эту модель.
void InspectorPanel::DrawModelImportEditor(EditorHost& host) {
    std::string path = host.SelectedAssetPath().string();
    ModelLoader::ImportSettings s = ModelLoader::LoadImportSettings(path);
    ImGui::DragFloat(T("Import Scale"), &s.Scale, 0.01f, 0.001f, 1000.0f);
    ImGui::Checkbox(T("Recenter (AABB -> origin)"), &s.Recenter);
    ImGui::Checkbox(T("Normalize size (max side = 1)"), &s.NormalizeSize);

    if (ImGui::Button(T("Reimport"))) {
        if (!ModelLoader::SaveImportSettings(path, s)) {
            host.SetStatusMessage("Import settings save failed");
        } else {
            // Перечитываем меш и переназначаем всем сущностям с этой моделью.
            std::shared_ptr<Mesh> mesh = ResourceManager::Instance().ReloadModel(path);
            Scene& scene = host.CurrentScene();
            auto view = scene.Registry().view<MeshRendererComponent>();
            for (auto e : view) {
                MeshRendererComponent& mr = view.get<MeshRendererComponent>(e);
                if (mr.Ref.type == MeshRef::Type::Model && mr.Ref.path == path) mr.MeshPtr = mesh;
            }
            host.SetStatusMessage("Reimported: " + host.SelectedAssetPath().filename().string());
        }
    }
    ImGui::TextDisabled("%s", T("Baked into the mesh on load — affects editor and built game"));
}

// Материал модели — вместе с моделью.
//
// ЗАЧЕМ. Файл .gltf/.glb/.obj несёт не только треугольники: там лежит материал
// с albedo, металличностью, шероховатостью и картами — всё то, чем движок и так
// умеет рисовать. До сих пор всё это выбрасывалось: в сцену приезжала белая
// болванка, а привести её в тот вид, в котором её экспортировали, значило
// вручную завести .sagemat и вручную прописать шесть путей к картинкам, ещё и
// зная, что glTF пакует металличность и шероховатость в разные каналы одной
// текстуры. Это и есть «модели не так развиты, как PBR-текстуры»: возможности
// рендера были на месте, а дороги от файла модели до них не было.
//
// ЧТО ИМЕННО ДЕЛАЕТСЯ. Рядом с моделью появляется <имя>.sagemat, если его там
// ещё нет, и назначается сущности. Уже существующий файл НЕ перезаписывается:
// материал после импорта правят, и повторное «Загрузить» не повод стирать эту
// правку — оно просто назначает готовое. Материал, назначенный человеком
// вручную, тоже не трогается.
void InspectorPanel::AutoAssignModelMaterial(EditorHost& host, MeshRendererComponent& mr) {
    const ModelMaterialImportResult r = ImportModelMaterials(host.CurrentProject(), mr);
    if (r.Assigned == 0) {
        if (!r.FirstWarning.empty()) host.SetStatusMessage(T("Model material: ") + r.FirstWarning);
        return;
    }
    if (r.Created > 0) {
        host.SetStatusMessage(T("Model materials imported: ") + std::to_string(r.Created) +
                              (r.AnyMaps ? T(" (with maps)") : ""));
        LOG_INFO("Editor") << "Материалов модели импортировано: " << r.Created;
    }
}

// --- Mesh Renderer, часть 1: ЧТО рисуем --------------------------------------
void InspectorPanel::DrawMeshSlot(EditorHost& host, MeshRendererComponent& mr) {
    ImGui::SeparatorText(T("Mesh"));

    // Порядок строго совпадает с MeshRef::Type (индекс комбо = значение enum).
    const char* kinds[] = {T("None"), T("Cube"), T("Sphere"), T("Plane"), T("Cylinder"), T("Cone"), T("Model")};
    int kind = (int)mr.Ref.type;
    if (ImGui::Combo(T("Source"), &kind, kinds, IM_ARRAYSIZE(kinds))) {
        host.PushUndoSnapshot(); // дискретное изменение — прямая запись undo
        const MeshRef::Type chosen = (MeshRef::Type)kind;
        if (chosen != MeshRef::Type::Model) {
            // Примитив вместо модели: путь и слоты её частей уходят вместе с ней.
            SetEntityMesh(mr, chosen, {}, ResourceManager::Instance().GetPrimitive(chosen));
        } else {
            mr.Ref.type = chosen;   // Model — путь задаётся ниже и грузится кнопкой
        }
        // Model — путь задаётся ниже и грузится кнопкой.
    }

    if (mr.Ref.type == MeshRef::Type::Model) {
        // Слот модели — тот же виджет, что у материала и текстур (см.
        // AssetSlot.h). Раньше здесь было поле ввода пути: путь к своей модели
        // надо было ЗНАТЬ и напечатать без опечатки, обложки не было, а бросок
        // файла не той породы молча не делал ничего.
        const assetslot::Result r =
            assetslot::Draw(host, "mesh", assetslot::Kind::Model, mr.Ref.path, &m_preview,
                            T("No model assigned"));
        if (r.Changed) {
            host.PushUndoSnapshot();
            // Сам меш подгружается ниже (m_pendingMeshLoad) — здесь важно, что
            // вместе со сменой модели сбрасываются слоты её частей.
            SetEntityMesh(mr, MeshRef::Type::Model, r.Path, nullptr);
            if (!r.Cleared) m_pendingMeshLoad = true;
        }
        if (r.BrowseRequested) {
            FileBrowser::Config c;
            c.Title = T("Choose a model");
            // Список берётся у РЕЕСТРА импортёров, а не пишется здесь руками.
            // Пока он был жёстким, добавленный в движок формат (или свой, из
            // плагина) в диалог не попадал — файл лежал рядом и не показывался,
            // из чего честно следовал вывод «свою модель загрузить нельзя».
            c.Filters = assetslot::Extensions(assetslot::Kind::Model);
            std::string label = T("Models");
            label += " (";
            for (size_t i = 0; i < c.Filters.size(); ++i) {
                if (i) label += ", ";
                label += "*" + c.Filters[i];
            }
            label += ")";
            c.FilterLabel = label;
            if (host.CurrentProject().Loaded()) c.StartDir = host.CurrentProject().AssetsDir();
            m_browser.Open(c);
            m_browseTarget = &mr.Ref.path;
            m_browseIsShader = false;
            m_browseIsMesh = true;
        }

        if (m_pendingMeshLoad) {
            m_pendingMeshLoad = false;
            mr.MeshPtr = ResourceManager::Instance().GetModel(mr.Ref.path);
            // GetModel сам логирует причину и отдаёт nullptr — сообщаем об
            // этом ЗДЕСЬ, в панели: строчку в консоли легко не заметить, а
            // «модель не появилась» без объяснения выглядит как поломка
            // редактора.
            if (!mr.MeshPtr) {
                host.SetStatusMessage(T("The model failed to load: ") + mr.Ref.path +
                                      T(" — details in Console"));
            } else {
                // Материал модели — вместе с моделью, а не «потом руками».
                // Подробности см. в AutoAssignModelMaterial.
                AutoAssignModelMaterial(host, mr);
            }
        }
        if (!mr.Ref.path.empty() && !mr.MeshPtr) {
            ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", T("Mesh not loaded"));
            ImGui::SameLine();
            if (EditorIcons::Button("refresh", T("Load"))) m_pendingMeshLoad = true;
            if (!ModelLoader::IsSupportedModel(mr.Ref.path)) {
                // Тот же реестр, что и в диалоге: подсказка обязана называть
                // ровно те форматы, которые движок в самом деле откроет.
                std::string list;
                for (const std::string& ext : assetslot::Extensions(assetslot::Kind::Model)) {
                    if (!list.empty()) list += ", ";
                    list += ext;
                }
                ImGui::TextDisabled("%s %s", T("Supported:"), list.c_str());
            }
        }
        // Абсолютный путь работает в редакторе и НЕ работает нигде больше —
        // говорим об этом сразу, а не после сборки игры.
        if (!mr.Ref.path.empty() && host.CurrentProject().Loaded() &&
            std::filesystem::path(mr.Ref.path).is_absolute()) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "%s", T("File outside the project"));
            ImGui::TextDisabled("%s", T("The built game will not find it. Bring it into the project:"));
            ImGui::TextDisabled("%s", T("Assets -> Import..."));
        }
    }

    if (mr.MeshPtr) {
        const glm::vec3 size = mr.MeshPtr->BoundsMax() - mr.MeshPtr->BoundsMin();
        HintWrapped(T("Triangles: %d, bounds %.2f x %.2f x %.2f"),
                            (int)(mr.MeshPtr->IndexCount() / 3), size.x, size.y, size.z);
    }
}

// Создать материал для объекта, у которого его ещё нет.
//
// КУДА ПИШЕМ. В ассеты проекта, если проект открыт, — там ему и место, и
// собранная игра его найдёт. Без проекта пишем рядом со сценой; если и сцена
// не сохранена, спрашиваем путь диалогом. Молча отказать («сначала откройте
// проект») здесь нельзя: демо-сцена редактора живёт без проекта, и именно в
// ней человек первым делом пробует покрасить куб.
void InspectorPanel::CreateMaterialForObject(EditorHost& host, MeshRendererComponent& mr) {
    namespace fs = std::filesystem;

    // Имя — по объекту: «Red Cube» -> Red Cube.sagemat. Так в панели ассетов
    // видно, чей это материал, без открытия файла.
    std::string name = "Material";
    if (GameObject sel = host.SelectedObject(); sel.Valid()) name = sel.Name();
    for (char& c : name) {
        if (std::string("/\\:*?\"<>|").find(c) != std::string::npos) c = '_';
    }

    fs::path dir;
    std::error_code ec;
    if (host.CurrentProject().Loaded()) dir = host.CurrentProject().AssetsDir();

    if (dir.empty()) {
        // Проекта нет — спрашиваем, куда положить. Молча отказать («сначала
        // откройте проект») нельзя: демо-сцена редактора живёт без проекта, и
        // именно в ней человек первым делом пробует покрасить куб.
        FileBrowser::Config c;
        c.Mode = FileBrowser::PickMode::SaveFile;
        c.Title = T("Where to save the material");
        c.Filters = {".sagemat"};
        c.FilterLabel = T("Materials (*.sagemat)");
        c.DefaultName = name + ".sagemat";
        m_browser.Open(c);
        m_browseTarget = &mr.MaterialPath;
        m_browseIsShader = false;
        m_browseIsMesh = false;
        m_browseIsMaterial = true;
        m_browseCreateMaterial = true; // после выбора пути файл ещё надо записать
        return;
    }

    fs::create_directories(dir, ec);
    fs::path path = dir / (name + ".sagemat");
    for (int i = 2; fs::exists(path, ec) && i < 1000; ++i) {
        path = dir / (name + " " + std::to_string(i) + ".sagemat");
    }
    WriteMaterialFromOverrides(host, mr, path.string());
}

// Записывает материал по указанному пути, забирая себе поправки объекта.
void InspectorPanel::WriteMaterialFromOverrides(EditorHost& host, MeshRendererComponent& mr,
                                                const std::string& path) {
    Material material;
    material.Albedo = mr.Color;
    material.Emissive = mr.Emissive;
    material.EmissiveStrength = mr.EmissiveStrength;
    material.Opacity = mr.Opacity;
    material.SaveToFile(path);

    host.PushUndoSnapshot();
    mr.MaterialPath = host.CurrentProject().AssetRef(path);
    mr.MaterialPtr = ResourceManager::Instance().GetMaterial(mr.MaterialPath);
    // Поправки — в нейтраль: их вид теперь несёт материал, и оставить их
    // значило бы покрасить объект дважды.
    mr.Color = glm::vec3(1.0f);
    mr.Emissive = glm::vec3(0.0f);
    mr.EmissiveStrength = 1.0f;
    mr.Opacity = 1.0f;
    host.SetStatusMessage(std::string(T("Material created: ")) +
                          std::filesystem::path(path).filename().string());
}

// --- Mesh Renderer, часть 2: ЧЕМ красим --------------------------------------
void InspectorPanel::DrawMaterialSlot(EditorHost& host, MeshRendererComponent& mr) {
    ImGui::SeparatorText(T("Material"));

    // Общий слот ассета: обложка (материал показывается шариком — по квадратику
    // цвета не отличить металл от диэлектрика и гладкое от матового, то есть
    // ровно то, ради чего материал и назначают), приём перетаскивания с
    // проверкой типа, «показать в Assets», очистка.
    const assetslot::Result r =
        assetslot::Draw(host, "material", assetslot::Kind::Material, mr.MaterialPath, &m_preview,
                        T("The look of an object is defined by its material."));
    if (r.Changed) {
        host.PushUndoSnapshot();
        mr.MaterialPath = r.Path;
        mr.MaterialPtr = r.Path.empty() ? nullptr
                                        : ResourceManager::Instance().GetMaterial(mr.MaterialPath);
    }
    if (r.BrowseRequested) {
        FileBrowser::Config c;
        c.Title = T("Choose a material");
        c.Filters = assetslot::Extensions(assetslot::Kind::Material);
        c.FilterLabel = T("Materials (*.sagemat)");
        if (host.CurrentProject().Loaded()) c.StartDir = host.CurrentProject().AssetsDir();
        m_browser.Open(c);
        m_browseTarget = &mr.MaterialPath;
        m_browseIsShader = false;
        m_browseIsMesh = false;
        m_browseIsMaterial = true;
    }
    if (!mr.MaterialPath.empty() && !mr.MaterialPtr) {
        ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", T("File cannot be read"));
    }

    // СОЗДАТЬ материал — отдельной строкой, а не ещё одной кнопкой в слоте.
    // Это другое действие: слот НАЗНАЧАЕТ существующий материал, эта кнопка
    // ДЕЛАЕТ новый.
    //
    // Без неё требование «вид задаёт материал» означало бы: иди в панель
    // ассетов, создай файл, вернись, назначь — четыре шага там, где раньше был
    // один щелчок по цвету. Новый материал забирает СЕБЕ текущие поправки
    // объекта (цвет, свечение, прозрачность), а сами поправки возвращаются в
    // нейтраль: иначе объект либо побелел бы, либо покрасился дважды.
    if (mr.MaterialPath.empty()) {
        if (EditorIcons::Button("material", T("Create material"))) CreateMaterialForObject(host, mr);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Creates a .sagemat next to the project assets and moves "
                                      "the current colour of this object into it."));
        }
    }

    DrawSubmeshMaterials(host, mr);
}

// --- Материалы ЧАСТЕЙ модели --------------------------------------------------
//
// ЗАЧЕМ ЭТО ЗДЕСЬ. Модель из редактора почти никогда не одноматериальна: у
// персонажа отдельно кожа, отдельно ткань, отдельно глаза. Пока сущность
// держала один материал, всё это красилось одним — и выглядело как «текстуры не
// работают», хотя текстуры были на месте: не было места, где сказать, какая
// часть чем красится. Оно здесь.
//
// Слот на КАЖДУЮ часть меша, в порядке разметки. Пустой слот — не дыра: часть
// красится материалом объекта (см. MaterialForSubmesh), поэтому очистка слота
// возвращает часть к общему виду, а не делает её невидимой.
void InspectorPanel::DrawSubmeshMaterials(EditorHost& host, MeshRendererComponent& mr) {
    if (!mr.MeshPtr || !mr.MeshPtr->HasExplicitSubmeshes()) return;
    const std::vector<sage::render::Submesh>& subs = mr.MeshPtr->Submeshes();

    ImGui::SeparatorText(T("Model parts"));
    ImGui::TextDisabled("%s", T("Each part of the model has its own material. Empty means "
                                "the object material above."));

    // Слоты держим ровно по числу частей: меш могли переимпортировать, и число
    // частей меняется вместе с ним. Лишние обрезаем, недостающие добавляем —
    // молча, потому что это не решение человека, а следствие правки модели.
    if (mr.Slots.size() != subs.size()) mr.Slots.resize(subs.size());

    for (size_t i = 0; i < subs.size(); ++i) {
        ImGui::PushID((int)i);
        // Подпись — имя части из файла модели: по нему видно, что красим, а по
        // «Slot 3» — нет.
        const std::string label = subs[i].Name.empty()
                                      ? std::string(T("Part ")) + std::to_string(i + 1)
                                      : subs[i].Name;
        ImGui::TextUnformatted(label.c_str());
        const assetslot::Result r =
            assetslot::Draw(host, "submesh_material", assetslot::Kind::Material, mr.Slots[i].Path,
                            &m_preview, T("Painted with the object material."));
        if (r.Changed) {
            host.PushUndoSnapshot();
            mr.Slots[i].Path = r.Path;
            mr.Slots[i].Ptr = r.Path.empty()
                                  ? nullptr
                                  : ResourceManager::Instance().GetMaterial(r.Path);
        }
        if (r.BrowseRequested) {
            FileBrowser::Config c;
            c.Title = T("Choose a material");
            c.Filters = assetslot::Extensions(assetslot::Kind::Material);
            c.FilterLabel = T("Materials (*.sagemat)");
            if (host.CurrentProject().Loaded()) c.StartDir = host.CurrentProject().AssetsDir();
            m_browser.Open(c);
            m_browseTarget = &mr.Slots[i].Path;
            m_browseIsShader = false;
            m_browseIsMesh = false;
            m_browseIsMaterial = true;
        }
        if (!mr.Slots[i].Path.empty() && !mr.Slots[i].Ptr) {
            ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", T("File cannot be read"));
        }
        ImGui::PopID();
    }
}

void InspectorPanel::DrawInstanceOverrides(EditorHost& host, MeshRendererComponent& mr,
                                           int entityId) {
    const bool neutral = mr.Color == glm::vec3(1.0f) && mr.Emissive == glm::vec3(0.0f) &&
                         mr.Opacity >= 0.999f;

    // Ширина полей считается по САМОЙ ДЛИННОЙ подписи группы — и по подписям
    // ТЕКУЩЕГО ЯЗЫКА. У ImGui подпись стоит справа от поля, и на узкой панели
    // инспектора она обрезалась: пока интерфейс был русским, длиннее всех была
    // «Непрозрачность», а в английском — «Emissive Strength».
    auto pushWidth = []() {
        float labelWidth = 0.0f;
        for (const char* label : {T("Tint"), T("Emissive"), T("Emissive Strength"), T("Opacity")}) {
            labelWidth = std::max(labelWidth, ImGui::CalcTextSize(label).x);
        }
        ImGui::PushItemWidth(-(labelWidth + ImGui::GetStyle().ItemInnerSpacing.x * 2.0f));
    };

    auto fields = [&]() {
        ImGui::ColorEdit3(T("Tint"), &mr.Color.x);
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Multiplies the material albedo. White means as in the material."));

        ImGui::ColorEdit3(T("Emissive"), &mr.Emissive.x);
        host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Emissive Strength"), &mr.EmissiveStrength, 0.05f, 0.0f, 20.0f, "%.2f");
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Added to the material emissive. Above 1 gives a bloom halo."));

        // Непрозрачность < 1 уводит объект в полупрозрачный проход (сортировка
        // от дальних, блендинг, без записи глубины) — см. ecs/RenderBatch.
        ImGui::SliderFloat(T("Opacity"), &mr.Opacity, 0.0f, 1.0f);
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Multiplies the material opacity."));
    };

    auto reset = [&]() {
        host.PushUndoSnapshot();
        mr.Color = glm::vec3(1.0f);
        mr.Emissive = glm::vec3(0.0f);
        mr.EmissiveStrength = 1.0f;
        mr.Opacity = 1.0f;
    };

    // Материала нет — переопределять НЕЧЕГО, и показывать здесь цвет со
    // свечением нельзя: ровно это и делало вид объекта задаваемым в двух
    // местах. Что делать вместо, сказано в слоте материала выше — там же
    // кнопка «Создать материал».
    if (!mr.MaterialPtr) return;

    // --- Материал есть: поправки — по требованию ---------------------------
    ImGui::SeparatorText(T("Instance overrides"));

    // Выключатель НЕ хранится в сцене: он выведен из самих значений. Ненейтральные
    // поправки — значит, они включены; человек, открывший их вручную, держится
    // отдельным полем панели (состояние интерфейса, а не данные объекта).
    bool open = !neutral || m_overridesOpenFor == entityId;
    if (ImGui::Checkbox(T("Override the material"), &open)) {
        if (open) {
            m_overridesOpenFor = entityId;
        } else {
            m_overridesOpenFor = -1;
            // Выключить — значит вернуть вид материалу. Оставлять поправки
            // «выключенными, но действующими» значило бы завести третье
            // состояние, которого никто не ждёт.
            if (!neutral) reset();
        }
    }

    if (!open) {
        // Вместо четырёх полей — то, что реально красит объект.
        const glm::vec3 albedo = mr.MaterialPtr->Albedo;
        ImGui::ColorButton("##effective_albedo", ImVec4(albedo.r, albedo.g, albedo.b, 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
        ImGui::SameLine();
        HintWrapped("%s", T("The look comes from the material. Turn the switch on only to make "
                            "THIS object differ from others sharing the same material."));
        return;
    }

    pushWidth();
    fields();
    ImGui::PopItemWidth();
    HintWrapped("%s", T("Applied on top of the material."));
}

// Префаб в инспекторе: та же вращаемая обложка, что у материала. Крупнее, чем в
// панели Assets, потому что здесь на неё и смотрят — выбирают, тот ли это ящик.
void InspectorPanel::DrawPrefabPreview(EditorHost& host) {
    const std::string path = host.SelectedAssetPath().string();
    const float side = std::min(ImGui::GetContentRegionAvail().x, 220.0f);
    const uint64_t tex = m_preview.RenderPrefab(path, (int)side);
    if (!tex) {
        ImGui::TextDisabled("%s", T("No cover: the prefab has no visible geometry"));
        ImGui::TextDisabled("%s", T("(or the file cannot be read — details in Console)."));
    } else {
        ImGui::Image((ImTextureID)(std::intptr_t)tex, ImVec2(side, side), ImVec2(0, 1),
                     ImVec2(1, 0));
        if (ImGui::IsItemHovered()) {
            ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                m_preview.Orbit(io.MouseDelta.x * 0.5f, -io.MouseDelta.y * 0.5f);
            if (io.MouseWheel != 0.0f) m_preview.Zoom(io.MouseWheel);
            ImGui::SetTooltip("%s", T("LMB orbits, wheel zooms"));
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", T("Preview"));
        if (ImGui::SmallButton(T("Reset view"))) m_preview.ResetView();
        ImGui::EndGroup();
    }

    ImGui::Spacing();
    if (ImGui::Button(T("Place in scene"), ImVec2(-1, 0))) host.InstantiatePrefab(path);
}

void InspectorPanel::DrawEntityProperties(EditorHost& host) {
    GameObject obj = host.SelectedObject();
    entt::registry& reg = host.CurrentScene().Registry();

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
    if (ImGui::InputText(T("Name"), buf, sizeof(buf))) obj.SetName(buf);
    host.TrackLastImGuiItem();
    ImGui::TextDisabled(T("Id: %d"), obj.Id());
    ImGui::Separator();

    if (ImGui::CollapsingHeader(T("Transform" "###Transform"), ImGuiTreeNodeFlags_DefaultOpen)) {
        Transform& tr = obj.GetTransform();
        ImGui::DragFloat3(T("Position"), &tr.Position.x, 0.05f); host.TrackLastImGuiItem();
        ImGui::DragFloat3(T("Rotation"), &tr.Rotation.x, 0.5f); host.TrackLastImGuiItem();
        ImGui::DragFloat3(T("Scale"), &tr.Scale.x, 0.05f, 0.01f, 100.0f); host.TrackLastImGuiItem();
    }

    // --- Mesh Renderer: ОДНА секция на весь компонент -------------------------
    //
    // Раньше их было две — «Mesh Renderer» и «Material», — и вторая выглядела
    // отдельным компонентом, хотя правила ТЕ ЖЕ два поля того же
    // MeshRendererComponent. Хуже того, они противоречили друг другу: сверху
    // стоял Color, снизу подпись «материал заменяет Color», и после назначения
    // материала верхний ползунок цвета переставал что-либо делать — молча.
    //
    // Теперь порядок повторяет саму структуру компонента (см.
    // ecs/RenderComponents.h): ЧТО рисуем -> ЧЕМ красим -> чем ЭТОТ экземпляр
    // отличается от других таких же.
    if (ImGui::CollapsingHeader(T("Mesh Renderer" "###Mesh Renderer"), ImGuiTreeNodeFlags_DefaultOpen)) {
        MeshRendererComponent& mr = obj.Renderer();
        DrawMeshSlot(host, mr);
        DrawMaterialSlot(host, mr);
        DrawInstanceOverrides(host, mr, obj.Id());
    }

    // --- Камера (игровая): панель Game рендерит от первой Primary-камеры ---
    if (reg.all_of<CameraComponent>(obj.Entity()) && ImGui::CollapsingHeader(T("Camera" "###Camera"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (CameraComponent* cam = reg.try_get<CameraComponent>(obj.Entity())) {
            ImGui::DragFloat(T("FOV"), &cam->Fov, 0.5f, 10.0f, 140.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Near"), &cam->NearClip, 0.01f, 0.001f, 10.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Far"), &cam->FarClip, 1.0f, 1.0f, 5000.0f); host.TrackLastImGuiItem();
            if (ImGui::Checkbox(T("Primary"), &cam->Primary)) host.PushUndoSnapshot();
            ImGui::TextDisabled("%s", T("Game panel renders from the first Primary camera"));
            if (ImGui::Button(T("Remove Camera"))) {
                host.PushUndoSnapshot();
                reg.remove<CameraComponent>(obj.Entity());
            }
        }
    }

    // --- Свет (позиция — Transform сущности; тип: точечный / прожектор / солнце) ---
    if (reg.all_of<LightComponent>(obj.Entity()) && ImGui::CollapsingHeader(T("Light" "###Light"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (LightComponent* light = reg.try_get<LightComponent>(obj.Entity())) {
            const char* types[] = {T("Point"), T("Spot"), T("Directional (sun)")};
            int kind = (int)light->Kind;
            if (ImGui::Combo(T("Type"), &kind, types, 3)) {
                host.PushUndoSnapshot(); // дискретное изменение — прямая запись undo
                light->Kind = (LightComponent::Type)kind;
            }
            ImGui::ColorEdit3(T("Colour"), &light->Color.x); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Intensity"), &light->Intensity, 0.02f, 0.0f, 10.0f); host.TrackLastImGuiItem();
            // Дальность и углы конуса у направленного света не значат ничего:
            // он светит из бесконечности. Показывать поля, которые ни на что не
            // влияют, — обманывать; поэтому их тут просто нет.
            const bool directional = light->Kind == LightComponent::Type::Directional;
            if (!directional) {
                ImGui::DragFloat(T("Range"), &light->Range, 0.1f, 0.5f, 100.0f);
                host.TrackLastImGuiItem();
            }
            if (light->Kind == LightComponent::Type::Spot) {
                ImGui::DragFloat(T("Inner Cone"), &light->InnerConeDeg, 0.5f, 1.0f, 89.0f); host.TrackLastImGuiItem();
                ImGui::DragFloat(T("Outer Cone"), &light->OuterConeDeg, 0.5f, 1.0f, 89.0f); host.TrackLastImGuiItem();
                // Внешний угол не должен быть уже внутреннего (иначе конус
                // «выворачивается»): подтягиваем внешний до внутреннего.
                if (light->OuterConeDeg < light->InnerConeDeg) light->OuterConeDeg = light->InnerConeDeg;
            }
            if (ImGui::Checkbox(T("Casts shadows"), &light->CastShadows)) host.PushUndoSnapshot();
            if (directional) {
                HintWrapped(T("Sun: shines along the object's forward (-Z of its rotation) from "
                              "infinity and has no range. Shadows are cascaded maps. "
                              "The sun is the directional light with the lowest number in "
                              "the hierarchy; a second one takes no part in the frame."));
            } else if (light->Kind == LightComponent::Type::Spot) {
                HintWrapped(T("The cone shines along the object's forward (-Z of its rotation). "
                              "Its shadow is a separate map in the atlas: there are few, and they "
                              "go to the brightest sources."));
            } else {
                HintWrapped(T("A point light at the object's position. Its shadow costs six passes "
                              "of geometry (one per cube face), so it is best left only to "
                              "the lamps that have something to occlude."));
            }
            if (ImGui::Button(T("Remove Light"))) {
                host.PushUndoSnapshot();
                reg.remove<LightComponent>(obj.Entity());
            }
        }
    }

    // --- Наклейка (проекция картинки на геометрию сцены) ---
    if (reg.all_of<DecalComponent>(obj.Entity()) &&
        ImGui::CollapsingHeader(T("Decal" "###Decal"), ImGuiTreeNodeFlags_DefaultOpen)) {
        DecalComponent& dc = reg.get<DecalComponent>(obj.Entity());
        bool changed = false;
        changed |= ImGui::DragFloat(T("Angle Limit"), &dc.AngleLimitDeg, 1.0f, 1.0f, 89.0f, "%.0f°");
        host.TrackLastImGuiItem();
        changed |= ImGui::DragFloat(T("Surface Offset"), &dc.Offset, 0.001f, 0.0f, 0.5f, "%.3f");
        host.TrackLastImGuiItem();

        // Треугольники — главный ответ на «почему наклейки не видно». Ноль
        // значит, что под коробкой не оказалось подходящей геометрии, а не что
        // сломался рендер, и лечится это перемещением, а не настройками.
        //
        // Подсказки — с переносом по ширине панели. Без него текст просто
        // обрезается на границе: панель у людей узкая, и «размер задаёт Scal»
        // читается как поломка редактора, а не как совет.
        if (dc.Triangles > 0) {
            ImGui::TextDisabled(T("Projected triangles: %d"), dc.Triangles);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", T("Landed on nothing: there is no geometry under the box, or it faces away from "
              "the decal."));
            ImGui::PopStyleColor();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
        ImGui::TextWrapped("%s", T("Projection goes along -Z; Scale sets the size"));
        ImGui::PopStyleColor();
        if (ImGui::Button(T("Rebuild##decal"))) changed = true;
        ImGui::SameLine();
        if (ImGui::Button(T("Remove##decal"))) {
            host.PushUndoSnapshot();
            reg.remove<DecalComponent>(obj.Entity());
            changed = false;
        }
        // Правка параметров обязана быть видна сразу: наклейка пересобирается
        // по флагу, и без него ползунок угла не менял бы вообще ничего.
        if (changed && reg.all_of<DecalComponent>(obj.Entity()))
            reg.get<DecalComponent>(obj.Entity()).Dirty = true;
    }

    // --- Скрипт (поведение в Play-режиме) ---
    if (reg.all_of<GIStaticComponent>(obj.Entity()) && ImGui::CollapsingHeader(T("GI Static" "###GI Static"), ImGuiTreeNodeFlags_DefaultOpen)) {
        GIStaticComponent& gs = reg.get<GIStaticComponent>(obj.Entity());
        ImGui::Checkbox(T("Lightmapped"), &gs.Lightmapped); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Texel Scale"), &gs.TexelScale, 0.05f, 0.1f, 8.0f); host.TrackLastImGuiItem();
        ImGui::TextDisabled("%s", T("Static occluder for baked GI; lightmapped = has own lightmap"));
        ImGui::TextDisabled("%s", T("Re-bake lighting after changes (Lighting panel)"));
        if (ImGui::Button(T("Remove##gistatic"))) {
            host.PushUndoSnapshot();
            reg.remove<GIStaticComponent>(obj.Entity());
        }
    }

    if (reg.all_of<ScriptComponent>(obj.Entity()) && ImGui::CollapsingHeader(T("Script" "###Script"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ScriptComponent* sc = reg.try_get<ScriptComponent>(obj.Entity())) {
            // Тот же слот, что у меша, материала и текстур (см. AssetSlot.h):
            // обложка, приём перетаскивания с проверкой типа, «показать в
            // Assets». Раньше здесь было поле ввода пути, и .lua приходилось
            // печатать наизусть, хотя он тут же в дереве проекта.
            const assetslot::Result r =
                assetslot::Draw(host, "script", assetslot::Kind::Script, sc->Path, &m_preview,
                                T("No script attached"));
            if (r.Changed) {
                host.PushUndoSnapshot();
                sc->Path = r.Path;
            }
            if (r.BrowseRequested) {
                FileBrowser::Config c;
                c.Title = T("Choose a script");
                c.Filters = assetslot::Extensions(assetslot::Kind::Script);
                c.FilterLabel = T("Scripts (*.lua)");
                if (host.CurrentProject().Loaded()) c.StartDir = host.CurrentProject().AssetsDir();
                m_browser.Open(c);
                // Цель — СУЩНОСТЬ, а не указатель на поле компонента: диалог
                // отвечает через кадр, а за этот кадр сцену могут перезагрузить
                // (undo, открытие другой сцены), и указатель повис бы.
                m_browseTarget = nullptr;
                m_browseScriptEntity = obj.Id();
                m_browseIsShader = false;
                m_browseIsMesh = false;
                m_browseIsMaterial = false;
            }
            if (!sc->Path.empty() && EditorIcons::Button("code", T("Open in editor"))) {
                host.OpenCodeFile(sc->Path);
            }
            ImGui::TextDisabled("%s", T("Runs in Play mode: OnStart(entity), OnUpdate(entity, dt)"));
            if (ImGui::Button(T("Remove Script"))) {
                host.PushUndoSnapshot();
                reg.remove<ScriptComponent>(obj.Entity());
            }
        }
    }

    // --- Твёрдое тело (симулируется в Play-режиме выбранным бэкендом физики) ---
    if (reg.all_of<RigidBodyComponent>(obj.Entity()) && ImGui::CollapsingHeader(T("Rigid Body" "###Rigid Body"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(obj.Entity())) {
            // Порядок строго совпадает с sage::physics::BodyType.
            const char* types[] = {T("Static"), T("Dynamic"), T("Kinematic")};
            int kind = (int)rb->Type;
            if (ImGui::Combo(T("Body Type"), &kind, types, IM_ARRAYSIZE(types))) {
                host.PushUndoSnapshot();
                rb->Type = (sage::physics::BodyType)kind;
            }
            ImGui::DragFloat(T("Mass"), &rb->Mass, 0.05f, 0.0f, 1000.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Friction"), &rb->Friction, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Restitution"), &rb->Restitution, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
            ImGui::TextDisabled("%s", T("Dynamic falls under gravity; Static/Kinematic don't"));
            if (ImGui::Button(T("Remove Rigid Body"))) {
                host.PushUndoSnapshot();
                reg.remove<RigidBodyComponent>(obj.Entity());
            }
        }
    }

    // --- Коллайдер (форма для физики; размеры домножаются на Transform.Scale) ---
    if (reg.all_of<ColliderComponent>(obj.Entity()) && ImGui::CollapsingHeader(T("Collider" "###Collider"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ColliderComponent* col = reg.try_get<ColliderComponent>(obj.Entity())) {
            // Порядок строго совпадает с sage::physics::ShapeType.
            const char* shapes[] = {T("Box"), T("Sphere"), T("Capsule")};
            int shape = (int)col->Shape;
            if (ImGui::Combo(T("Shape"), &shape, shapes, IM_ARRAYSIZE(shapes))) {
                host.PushUndoSnapshot();
                col->Shape = (sage::physics::ShapeType)shape;
            }
            if (col->Shape == sage::physics::ShapeType::Box) {
                ImGui::DragFloat3(T("Half Extents"), &col->HalfExtents.x, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
            } else if (col->Shape == sage::physics::ShapeType::Sphere) {
                ImGui::DragFloat(T("Radius"), &col->Radius, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
            } else { // Capsule
                ImGui::DragFloat(T("Radius"), &col->Radius, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
                ImGui::DragFloat(T("Half Height"), &col->HalfHeight, 0.02f, 0.001f, 100.0f);
                host.TrackLastImGuiItem();
            }
            ImGui::TextDisabled("%s", T("Sizes are scaled by the entity's Transform scale"));

            // --- Составная (compound) форма: список дочерних примитивов ---
            ImGui::Separator();
            ImGui::Text(T("Compound parts: %d"), (int)col->Parts.size());
            if (!col->Parts.empty())
                ImGui::TextDisabled("%s", T("Parts override the single shape above"));
            const char* shapeNames[] = {T("Box"), T("Sphere"), T("Capsule")};
            int removePart = -1;
            for (int pi = 0; pi < (int)col->Parts.size(); ++pi) {
                ColliderComponent::Part& p = col->Parts[pi];
                ImGui::PushID(pi);
                if (ImGui::TreeNodeEx(T("part" "###part"), ImGuiTreeNodeFlags_DefaultOpen, "Part %d", pi)) {
                    int ps = (int)p.Shape;
                    if (ImGui::Combo(T("Shape"), &ps, shapeNames, IM_ARRAYSIZE(shapeNames))) {
                        host.PushUndoSnapshot();
                        p.Shape = (sage::physics::ShapeType)ps;
                    }
                    if (p.Shape == sage::physics::ShapeType::Box) {
                        ImGui::DragFloat3(T("Half Extents"), &p.HalfExtents.x, 0.02f, 0.001f, 100.0f);
                        host.TrackLastImGuiItem();
                    } else {
                        ImGui::DragFloat(T("Radius"), &p.Radius, 0.02f, 0.001f, 100.0f);
                        host.TrackLastImGuiItem();
                        if (p.Shape == sage::physics::ShapeType::Capsule) {
                            ImGui::DragFloat(T("Half Height"), &p.HalfHeight, 0.02f, 0.001f, 100.0f);
                            host.TrackLastImGuiItem();
                        }
                    }
                    ImGui::DragFloat3(T("Offset"), &p.Offset.x, 0.02f);
                    host.TrackLastImGuiItem();
                    ImGui::DragFloat3(T("Rotation"), &p.EulerDeg.x, 0.5f);
                    host.TrackLastImGuiItem();
                    if (ImGui::SmallButton(T("Remove Part"))) removePart = pi;
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (removePart >= 0) {
                host.PushUndoSnapshot();
                col->Parts.erase(col->Parts.begin() + removePart);
            }
            if (ImGui::Button(T("Add Part"))) {
                host.PushUndoSnapshot();
                col->Parts.push_back(ColliderComponent::Part{});
            }

            if (ImGui::Button(T("Remove Collider"))) {
                host.PushUndoSnapshot();
                reg.remove<ColliderComponent>(obj.Entity());
            }
        }
    }

    // --- Контроллер персонажа ------------------------------------------------
    //
    // Секции здесь не было ВООБЩЕ: компонент сериализовался, исполнялся физикой
    // и был доступен из Lua, но настроить его мышью было нельзя — только
    // скриптом или правкой .sage руками. Компонент, который нельзя увидеть в
    // редакторе, для человека, работающего в редакторе, не существует.
    if (reg.all_of<CharacterControllerComponent>(obj.Entity()) &&
        ImGui::CollapsingHeader(T("Character Controller" "###Character Controller"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (CharacterControllerComponent* ch =
                reg.try_get<CharacterControllerComponent>(obj.Entity())) {
            ImGui::DragFloat(T("Radius"), &ch->Radius, 0.01f, 0.05f, 5.0f);
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Height"), &ch->Height, 0.02f, 0.1f, 10.0f);
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Step Height"), &ch->StepHeight, 0.01f, 0.0f, 2.0f);
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("How tall a step the character climbs without jumping"));
            }
            ImGui::DragFloat(T("Max Slope"), &ch->MaxSlopeDeg, 0.5f, 0.0f, 89.0f, "%.0f°");
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Steeper than this and the character slides"));
            ImGui::DragFloat(T("Mass"), &ch->Mass, 0.5f, 0.1f, 1000.0f);
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("How hard the character pushes dynamic bodies"));
            }

            // Состояние из физики — только для чтения: правка «стоит на земле»
            // из интерфейса не имеет смысла, его вычисляет симуляция.
            ImGui::Separator();
            ImGui::TextDisabled(T("Grounded: %s"), ch->Grounded ? T("yes") : T("no"));

            if (ImGui::Button(T("Remove Character Controller"))) {
                host.PushUndoSnapshot();
                reg.remove<CharacterControllerComponent>(obj.Entity());
            }
        }
    }

    // --- Соединение (constraint/joint) с другим телом или миром ---
    if (reg.all_of<JointComponent>(obj.Entity()) && ImGui::CollapsingHeader(T("Joint" "###Joint"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (JointComponent* jc = reg.try_get<JointComponent>(obj.Entity())) {
            const char* types[] = {T("Fixed"), T("Point"), T("Hinge"), T("Slider"),
                                   T("Distance"), T("Cone")};
            int t = (int)jc->Type;
            if (ImGui::Combo(T("Type"), &t, types, IM_ARRAYSIZE(types))) {
                host.PushUndoSnapshot();
                jc->Type = (sage::physics::JointType)t;
            }
            ImGui::DragInt(T("Target Id (-1 = world)"), &jc->TargetId, 0.1f, -1, 100000);
            host.TrackLastImGuiItem();
            ImGui::DragFloat3(T("Anchor (offset)"), &jc->Anchor.x, 0.02f);
            host.TrackLastImGuiItem();
            using JT = sage::physics::JointType;
            if (jc->Type == JT::Hinge || jc->Type == JT::Slider || jc->Type == JT::Cone) {
                ImGui::DragFloat3(T("Axis"), &jc->Axis.x, 0.02f);
                host.TrackLastImGuiItem();
            }
            if (jc->Type == JT::Hinge || jc->Type == JT::Slider) {
                ImGui::Checkbox(T("Use Limits"), &jc->UseLimits);
                if (jc->UseLimits) {
                    ImGui::DragFloat(T("Min"), &jc->MinLimit, 0.5f);
                    host.TrackLastImGuiItem();
                    ImGui::DragFloat(T("Max"), &jc->MaxLimit, 0.5f);
                    host.TrackLastImGuiItem();
                }
            } else if (jc->Type == JT::Distance) {
                ImGui::DragFloat(T("Min Distance"), &jc->MinDistance, 0.02f, 0.0f, 100.0f);
                host.TrackLastImGuiItem();
                ImGui::DragFloat(T("Max Distance"), &jc->MaxDistance, 0.02f, 0.0f, 100.0f);
                host.TrackLastImGuiItem();
            } else if (jc->Type == JT::Cone) {
                ImGui::DragFloat(T("Cone Half Angle"), &jc->ConeHalfAngle, 0.5f, 0.0f, 180.0f);
                host.TrackLastImGuiItem();
            }
            ImGui::TextDisabled("%s", T("Needs a Rigid Body; only the Jolt backend simulates joints"));
            if (ImGui::Button(T("Remove Joint"))) {
                host.PushUndoSnapshot();
                reg.remove<JointComponent>(obj.Entity());
            }
        }
    }

    // --- Скелетно-анимированная модель (.glb/.gltf или процедурное демо) ---
    if (reg.all_of<AnimatedModelComponent>(obj.Entity()) && ImGui::CollapsingHeader(T("Animated Model" "###Animated Model"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(obj.Entity())) {
            char pathBuf[512];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", am->Path.c_str());
            if (ImGui::InputText(T("Model (.glb)"), pathBuf, sizeof(pathBuf))) am->Path = pathBuf;
            host.TrackLastImGuiItem();
            ImGui::TextDisabled("%s", T("Empty path = procedural demo (\"tentacle\")"));
            if (am->Path.empty()) {
                if (ImGui::SliderInt(T("Demo Segments"), &am->DemoSegments, 2, 16)) {
                    am->Ready = false; am->Model = nullptr; // пересобрать демо
                }
            }
            if (ImGui::Button(T("Reload"))) { am->Ready = false; am->Model = nullptr; }

            // Список клипов — из проигрывателя (модель уже загружена системой).
            int clipCount = am->Anim.ClipCount();
            if (clipCount > 0) {
                if (am->Clip >= clipCount) am->Clip = 0;
                std::string preview = am->Anim.ClipName(am->Clip);
                if (ImGui::BeginCombo("Clip", preview.c_str())) {
                    for (int i = 0; i < clipCount; ++i) {
                        bool sel = (am->Clip == i);
                        if (ImGui::Selectable(am->Anim.ClipName(i).c_str(), sel) && i != am->Clip) {
                            am->Clip = i;
                            // Плавный кросс-фейд к выбранному клипу (или резко, если 0).
                            if (am->BlendTime > 0.0f) am->Anim.CrossFade(i, am->BlendTime, am->Loop);
                            else am->Anim.Play(i, am->Loop);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::DragFloat(T("Blend Time"), &am->BlendTime, 0.01f, 0.0f, 2.0f);
                host.TrackLastImGuiItem();
                if (am->Anim.Fading())
                    ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), T("cross-fading %.0f%%"),
                                       am->Anim.FadeWeight() * 100.0f);
            } else {
                ImGui::TextDisabled("%s", T("No animation clips (bind pose)"));
            }
            ImGui::DragFloat(T("Speed"), &am->Speed, 0.02f, 0.0f, 8.0f); host.TrackLastImGuiItem();
            if (ImGui::Checkbox(T("Loop"), &am->Loop)) am->Anim.Play(am->Clip, am->Loop);
            ImGui::SameLine();
            ImGui::Checkbox(T("Playing"), &am->Playing);
            if (clipCount > 0) {
                ImGui::TextDisabled(T("t = %.2f s"), am->Anim.Time());
            }
            ImGui::Checkbox(T("Root Motion"), &am->RootMotion);
            if (ImGui::Button(T("Remove Animated Model"))) {
                host.PushUndoSnapshot();
                reg.remove<AnimatedModelComponent>(obj.Entity());
            }
        }
    }

    // --- Зонд отражений -----------------------------------------------------
    if (reg.all_of<ReflectionProbeComponent>(obj.Entity()) &&
        ImGui::CollapsingHeader(T("Reflection Probe" "###Reflection Probe"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ReflectionProbeComponent* p = reg.try_get<ReflectionProbeComponent>(obj.Entity())) {
            ImGui::TextDisabled("%s", T("Captures the scene around this point into a cubemap"));
            // Любая правка охвата или разрешения означает «снять заново»: карта
            // снята под прежние числа, и оставить её значило бы показывать
            // отражение, которого в сцене уже нет.
            const int prevRes = p->Resolution;
            const char* resNames[] = {"32", "64", "128", "256"};
            const int resValues[] = {32, 64, 128, 256};
            int resIdx = 2;
            for (int i = 0; i < 4; ++i) if (resValues[i] == p->Resolution) resIdx = i;
            if (ImGui::Combo(T("Resolution"), &resIdx, resNames, 4)) p->Resolution = resValues[resIdx];
            if (p->Resolution != prevRes) p->Dirty = true;

            if (ImGui::DragFloat3(T("Box Half Extents"), &p->BoxHalfExtents.x, 0.1f, 0.1f, 500.0f))
                p->Dirty = true;
            host.TrackLastImGuiItem();
            ImGui::TextDisabled("%s", T("Camera inside this box uses this probe"));
            if (ImGui::Checkbox(T("Box Parallax"), &p->BoxParallax)) p->Dirty = true;
            ImGui::SliderFloat(T("Intensity"), &p->Intensity, 0.0f, 3.0f);
            host.TrackLastImGuiItem();
            if (ImGui::DragFloat(T("Far Clip"), &p->FarClip, 0.5f, 1.0f, 2000.0f)) p->Dirty = true;
            host.TrackLastImGuiItem();
            ImGui::Checkbox(T("Realtime (re-capture every frame)"), &p->Realtime);
            ImGui::TextDisabled("%s", T("Realtime = 6 scene passes per frame - use sparingly"));

            if (ImGui::Button(T("Bake Probe"))) p->Dirty = true;
            ImGui::SameLine();
            ImGui::TextDisabled(p->Dirty ? "queued" : (p->Runtime ? "captured" : "empty"));
            if (ImGui::Button(T("Remove Reflection Probe"))) {
                host.PushUndoSnapshot();
                reg.remove<ReflectionProbeComponent>(obj.Entity());
            }
        }
    }

    // --- Обратная кинематика: цели поверх позы клипа ------------------------
    // Кость задаётся ИМЕНЕМ, а не индексом: модель может смениться (или ещё не
    // загрузиться), а имя переживает и то, и другое. Список имён показываем
    // только когда скелет уже есть.
    if (reg.all_of<IKComponent>(obj.Entity()) && ImGui::CollapsingHeader(T("IK" "###IK"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (IKComponent* ik = reg.try_get<IKComponent>(obj.Entity())) {
            ImGui::Checkbox(T("IK Enabled"), &ik->Enabled);
            const AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(obj.Entity());
            if (!am) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                                        "%s", T("No Animated Model - goals do nothing"));

            int remove = -1;
            for (int gi = 0; gi < (int)ik->Goals.size(); ++gi) {
                IKGoal& g = ik->Goals[(size_t)gi];
                ImGui::PushID(gi);
                const std::string title = "Goal " + std::to_string(gi) +
                                          (g.Bone.empty() ? "" : " (" + g.Bone + ")");
                if (ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    char boneBuf[128];
                    std::snprintf(boneBuf, sizeof(boneBuf), "%s", g.Bone.c_str());
                    if (ImGui::InputText(T("Bone"), boneBuf, sizeof(boneBuf))) {
                        g.Bone = boneBuf;
                        g.Resolved = false;   // имя сменилось — искать заново
                    }
                    host.TrackLastImGuiItem();
                    // Выбор из реального скелета — чтобы не угадывать написание.
                    if (am && am->Model) {
                        const sage::anim::Skeleton& sk = am->Model->GetSkeleton();
                        if (sk.Count() > 0 && ImGui::BeginCombo("Pick Bone", g.Bone.c_str())) {
                            for (int b = 0; b < sk.Count(); ++b) {
                                const std::string& n = sk.Joints[(size_t)b].Name;
                                if (ImGui::Selectable(n.c_str(), n == g.Bone) && n != g.Bone) {
                                    g.Bone = n;
                                    g.Resolved = false;
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                    ImGui::Checkbox(T("Enabled"), &g.Enabled);
                    ImGui::SameLine();
                    if (ImGui::Checkbox(T("Aim (look-at)"), &g.Aim)) g.Resolved = false;
                    if (g.Aim) {
                        ImGui::DragFloat3(T("Aim Axis"), &g.AimAxis.x, 0.01f);
                        host.TrackLastImGuiItem();
                        ImGui::SliderFloat(T("Max Angle"), &g.AimMaxAngle, 0.0f, 180.0f);
                        host.TrackLastImGuiItem();
                    } else {
                        if (ImGui::SliderInt(T("Chain Length"), &g.ChainLength, 2, 8)) g.Resolved = false;
                        host.TrackLastImGuiItem();
                        ImGui::TextDisabled("%s", T("2 = analytic two-bone, more = FABRIK"));
                        ImGui::Checkbox(T("Use Pole"), &g.UsePole);
                        if (g.UsePole) {
                            ImGui::DragFloat3(T("Pole"), &g.Pole.x, 0.02f);
                            host.TrackLastImGuiItem();
                        }
                        ImGui::DragFloat3(T("Align Normal"), &g.AlignNormal.x, 0.02f);
                        host.TrackLastImGuiItem();
                        ImGui::Checkbox(T("Foot Lock"), &g.Lock);
                        if (g.Lock) {
                            ImGui::DragFloat(T("Plant Height"), &g.PlantHeight, 0.005f, 0.0f, 1.0f);
                            host.TrackLastImGuiItem();
                            ImGui::DragFloat(T("Release Time"), &g.ReleaseTime, 0.005f, 0.0f, 1.0f);
                            host.TrackLastImGuiItem();
                        }
                    }
                    ImGui::DragFloat3(T("Target (world)"), &g.Target.x, 0.02f);
                    host.TrackLastImGuiItem();
                    ImGui::SliderFloat(T("Weight"), &g.Weight, 0.0f, 1.0f);
                    host.TrackLastImGuiItem();
                    if (g.Resolved && g.EndJoint < 0)
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", T("bone not found in skeleton"));
                    if (ImGui::SmallButton(T("Remove Goal"))) remove = gi;
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (remove >= 0) {
                host.PushUndoSnapshot();
                ik->Goals.erase(ik->Goals.begin() + remove);
            }
            if (ImGui::Button(T("Add Goal"))) ik->Goals.emplace_back();
            ImGui::SameLine();
            if (ImGui::Button(T("Remove IK"))) {
                host.PushUndoSnapshot();
                reg.remove<IKComponent>(obj.Entity());
            }
        }
    }

    // --- Эмиттер частиц (огонь/дым/искры/…): пресеты + тонкая настройка ---
    if (reg.all_of<ParticleEmitterComponent>(obj.Entity()) && ImGui::CollapsingHeader(T("Particle Emitter" "###Particle Emitter"), ImGuiTreeNodeFlags_DefaultOpen)) {
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
            ImGui::Checkbox(T("Active"), &em->Active);
            ImGui::SameLine();
            ImGui::Checkbox(T("Continuous"), &em->Continuous);
            if (em->Continuous) {
                ImGui::DragFloat(T("Rate (p/s)"), &cfg.EmissionRate, 0.5f, 0.0f, 500.0f); host.TrackLastImGuiItem();
            } else {
                ImGui::DragInt(T("Burst Count"), &em->BurstCount, 1, 1, 500); host.TrackLastImGuiItem();
                ImGui::DragFloat(T("Burst Interval"), &em->BurstInterval, 0.05f, 0.05f, 30.0f); host.TrackLastImGuiItem();
            }
            ImGui::DragFloatRange2("Speed", &cfg.SpeedMin, &cfg.SpeedMax, 0.05f, 0.0f, 50.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Gravity"), &cfg.Gravity, 0.05f, -30.0f, 30.0f); host.TrackLastImGuiItem();
            ImGui::DragFloatRange2("Lifetime", &cfg.LifetimeMin, &cfg.LifetimeMax, 0.02f, 0.02f, 20.0f); host.TrackLastImGuiItem();
            ImGui::DragFloatRange2("Start Size", &cfg.StartSizeMin, &cfg.StartSizeMax, 0.005f, 0.0f, 5.0f); host.TrackLastImGuiItem();
            ImGui::DragFloatRange2("End Size", &cfg.EndSizeMin, &cfg.EndSizeMax, 0.005f, 0.0f, 5.0f); host.TrackLastImGuiItem();
            ImGui::ColorEdit4(T("Start Color"), &cfg.StartColor.x); host.TrackLastImGuiItem();
            ImGui::ColorEdit4(T("End Color"), &cfg.EndColor.x); host.TrackLastImGuiItem();
            ImGui::DragFloat3(T("Dir Min"), &cfg.DirectionMin.x, 0.02f); host.TrackLastImGuiItem();
            ImGui::DragFloat3(T("Dir Max"), &cfg.DirectionMax.x, 0.02f); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Spin"), &cfg.AngularVelocityMax, 0.05f, 0.0f, 20.0f); host.TrackLastImGuiItem();
            if (ImGui::Button(T("Remove Emitter"))) {
                host.PushUndoSnapshot();
                reg.remove<ParticleEmitterComponent>(obj.Entity());
            }
        }
    }

    if (reg.all_of<UIElementComponent>(obj.Entity()) &&
        ImGui::CollapsingHeader(T("UI Element" "###UI Element"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (UIElementComponent* u = reg.try_get<UIElementComponent>(obj.Entity())) {
            DrawUIElement(host, obj, *u);
        }
    }

    // --- Единое «Add Component»: добавляет любой ОТСУТСТВУЮЩИЙ компонент ---
    DrawAddComponentMenu(host, obj);

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button(T("Delete Entity"), ImVec2(-1, 0))) host.DeleteSelected();
    ImGui::PopStyleColor();
}

// Кнопка «Add Component» + попап со списком компонентов, которых у сущности ещё
// нет. Так добавление унифицировано (а удаление — кнопкой Remove в самой секции).
// Серое пояснение, КОТОРОЕ ПЕРЕНОСИТСЯ. Панель инспектора узкая (её ширину
// задаёт раскладка доккинга), а TextDisabled не переносит — поэтому пояснения
// обрезались по краю панели прямо посередине слова: «габарит 1.00 x 1.00 x 1.0(»,
// «задаём вид объекта целик». Пропадал ровно тот текст, ради которого их писали.
void InspectorPanel::HintWrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

// --- Реестр компонентов: ТАБЛИЦА, а не лестница if-ов ---------------------
//
// Список компонентов в меню «Добавить компонент» был написан подряд, строкой на
// каждый, и отставал от движка чаще всего: Mesh Renderer, Decal и Character
// Controller ДОЛГО в нём отсутствовали, хотя секции в инспекторе у них были.
// Получалось противоречие — наклейку можно создать через «Объект > Создать
// наклейку», но нельзя добавить к существующему объекту.
//
// Теперь это данные: строка таблицы — имя, раздел, значок, ОДНА строка о том,
// что компонент делает, и две операции. Новый компонент = новая строка. Отсюда
// же берётся поиск (компонентов два десятка, и листать их глазами дороже, чем
// набрать три буквы) и разбиение по разделам: «Свет» и «Твёрдое тело» в одном
// плоском списке стоят рядом только потому, что их так написали.
namespace {

struct ComponentEntry {
    const char* Name;
    const char* Category;
    const char* Icon;
    const char* Hint;
    bool (*Has)(entt::registry&, entt::entity);
    void (*Add)(entt::registry&, entt::entity);
};

template <typename T>
bool HasComp(entt::registry& reg, entt::entity e) {
    return reg.all_of<T>(e);
}
template <typename T>
void AddComp(entt::registry& reg, entt::entity e) {
    reg.emplace<T>(e);
}

const std::vector<ComponentEntry>& ComponentRegistry() {
    static const std::vector<ComponentEntry> kEntries = {
        {"Mesh Renderer", "Render", "cube",
         "Draws a mesh with a material", HasComp<MeshRendererComponent>, AddComp<MeshRendererComponent>},
        {"Decal", "Render", "texture",
         "Projects a texture onto surfaces underneath", HasComp<DecalComponent>, AddComp<DecalComponent>},
        {"GI Static", "Render", "sun",
         "Marks the object as static for the light bake", HasComp<GIStaticComponent>, AddComp<GIStaticComponent>},
        {"Particle Emitter", "Render", "particles",
         "Fire, smoke, sparks", HasComp<ParticleEmitterComponent>,
         [](entt::registry& reg, entt::entity e) {
             ParticleEmitterComponent em;
             em.Config = ParticlePresets::Registry()[0].Make(); // Fire
             reg.emplace<ParticleEmitterComponent>(e, em);
         }},
        {"Reflection Probe", "Render", "probe",
         "Captures the surroundings for reflections", HasComp<ReflectionProbeComponent>,
         AddComp<ReflectionProbeComponent>},

        {"Camera", "View", "camera",
         "The game looks through this object", HasComp<CameraComponent>, AddComp<CameraComponent>},
        {"Light", "View", "light",
         "Point, spot or directional light", HasComp<LightComponent>, AddComp<LightComponent>},

        {"Rigid Body", "Physics", "physics",
         "The object falls and collides", HasComp<RigidBodyComponent>, AddComp<RigidBodyComponent>},
        {"Collider", "Physics", "physics",
         "The shape physics uses for collisions", HasComp<ColliderComponent>, AddComp<ColliderComponent>},
        {"Joint", "Physics", "ik",
         "Ties this body to another one", HasComp<JointComponent>, AddComp<JointComponent>},
        {"Character Controller", "Physics", "physics",
         "Walking, jumping, stairs", HasComp<CharacterControllerComponent>,
         AddComp<CharacterControllerComponent>},

        {"Animated Model", "Animation", "anim",
         "Skeletal animation clips", HasComp<AnimatedModelComponent>, AddComp<AnimatedModelComponent>},
        {"IK", "Animation", "ik",
         "Bones reach for a target", HasComp<IKComponent>, AddComp<IKComponent>},

        // Скрипт добавляется ПУСТЫМ. Раньше сюда подставлялся путь
        // «assets/scripts/spin.lua» — демонстрационный скрипт движка, которого
        // в проекте человека нет: компонент добавлялся уже сломанным, и при
        // первом же Play в консоль летело «Скрипт не найден». Пустой слот
        // честно говорит «скрипт не прикреплён» и ждёт, когда в него бросят
        // файл.
        {"Script", "Logic", "script",
         "Lua: OnStart and OnUpdate on this object", HasComp<ScriptComponent>,
         AddComp<ScriptComponent>},
        {"UI Element", "Interface", "rect",
         "Panel, label, image or bar on screen", HasComp<UIElementComponent>,
         AddComp<UIElementComponent>},
    };
    return kEntries;
}

} // namespace

// Якорь — сеткой 3x3, а не списком из девяти строк.
//
// Якорь ЕСТЬ положение на экране: «сверху слева», «по центру», «снизу справа».
// Выпадающий список заставлял читать девять названий и держать в голове, какое
// из них соответствует нужному углу; сетка показывает это буквально — кнопка
// стоит там же, где встанет элемент.
bool InspectorPanel::DrawAnchorPicker(UIAnchor& anchor) {
    bool changed = false;
    const float cell = ImGui::GetFrameHeight();
    ImGui::BeginGroup();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            const int index = row * 3 + col;
            if (col) ImGui::SameLine(0.0f, 2.0f);
            ImGui::PushID(index);
            const bool active = (int)anchor == index;
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.216f, 0.322f, 0.520f, 1.0f));
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::Button("##anchor", ImVec2(cell, cell))) {
                anchor = (UIAnchor)index;
                changed = true;
            }
            if (active) ImGui::PopStyleColor();
            // Точка внутри кнопки — в том углу, который эта кнопка и означает.
            const float px = p0.x + cell * (0.25f + 0.25f * (float)col);
            const float py = p0.y + cell * (0.25f + 0.25f * (float)row);
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(px, py), 2.5f,
                ImGui::GetColorU32(active ? ImGuiCol_Text : ImGuiCol_TextDisabled));
            ImGui::PopID();
        }
    }
    ImGui::EndGroup();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T("Anchor"));
    return changed;
}

// Инспектор элемента интерфейса.
//
// РАНЬШЕ ЗДЕСЬ БЫЛА ПРОСТЫНЯ ИЗ ТРИДЦАТИ ПЯТИ ПОЛЕЙ, одна на все виды
// элементов. У надписи спрашивали скругление углов, толщину рамки, градиент,
// тень и внутренний отступ; у полосы прогресса — подсказку поля ввода; у любого
// элемента — предел длины текста и пароль. Работающими из них были три-четыре,
// а какие именно — приходилось выяснять опытом. Отсюда и ощущение «компоненты
// странные»: компонент один, а элементов интерфейса восемь, и он показывал
// объединение всех их свойств сразу.
//
// Теперь показывается только то, что этот вид элемента в самом деле читает
// (см. sage/ui/UIRenderer.h), сгруппированное по смыслу: положение, вид, текст,
// поведение. Сам компонент не тронут: он общий для всех видов и сериализуется
// как раньше — иначе пришлось бы ломать формат сцен, скриптовый API и префабы
// ради вида инспектора.
void InspectorPanel::DrawUIElement(EditorHost& host, GameObject obj, UIElementComponent& u) {
    entt::registry& reg = host.CurrentScene().Registry();
    using Kind = UIElementComponent::Kind;

    // Что читает этот вид элемента.
    const bool hasText = u.Type != Kind::Image;         // надпись есть у всех, кроме картинки
    const bool hasFill = u.Type != Kind::Label;         // подложка (у надписи фона нет)
    const bool hasSprite = u.Type == Kind::Image;
    const bool hasBar = u.Type == Kind::Bar;
    const bool hasInput = u.Type == Kind::Input;
    const bool hasRange = u.Type == Kind::Slider;
    const bool interactive = u.Type == Kind::Panel || u.Type == Kind::Input ||
                             u.Type == Kind::Checkbox || u.Type == Kind::Slider ||
                             u.Type == Kind::Image;

    const char* kinds[] = {T("Panel"), T("Label"), T("Image"), T("Bar"), T("Icon"),
                           T("Input"), T("Checkbox"), T("Slider")};
    int kind = (int)u.Type;
    if (ImGui::Combo(T("Kind"), &kind, kinds, IM_ARRAYSIZE(kinds))) {
        host.PushUndoSnapshot();
        u.Type = (Kind)kind;
    }

    // --- Положение --------------------------------------------------------
    ImGui::SeparatorText(T("Layout"));
    if (DrawAnchorPicker(u.Anchor)) host.PushUndoSnapshot();
    ImGui::DragFloat2(T("Offset"), &u.Offset.x, 1.0f); host.TrackLastImGuiItem();
    ImGui::BeginDisabled(u.AutoWidth);
    ImGui::DragFloat2(T("Size"), &u.Size.x, 1.0f, 0.0f, 4096.0f); host.TrackLastImGuiItem();
    ImGui::EndDisabled();
    ImGui::DragInt(T("Layer"), &u.Layer, 1); host.TrackLastImGuiItem();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Higher draws on top of its siblings"));
    ImGui::Checkbox(T("Visible"), &u.Visible);
    ImGui::SameLine();
    ImGui::Checkbox(T("Clip Children"), &u.ClipChildren);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", T("The subtree is masked by this rectangle (lists, minimaps)"));
    }
    if (hasText) {
        ImGui::Checkbox(T("Auto Width"), &u.AutoWidth);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Width follows the content"));
    }

    // --- Вид ---------------------------------------------------------------
    if (hasFill || hasSprite) {
        ImGui::SeparatorText(T("Appearance"));
        ImGui::ColorEdit4(hasSprite ? T("Tint") : T("Fill"), &u.Color.x); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Rounding"), &u.Rounding, 0.5f, 0.0f, 200.0f); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Border"), &u.BorderThickness, 0.25f, 0.0f, 50.0f); host.TrackLastImGuiItem();
        if (u.BorderThickness > 0.0f) {
            ImGui::ColorEdit4(T("Border Color"), &u.BorderColor.x); host.TrackLastImGuiItem();
        }
        ImGui::ColorEdit4(T("Gradient (a=0 off)"), &u.GradientColor.x); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Shadow"), &u.ShadowSize, 0.5f, 0.0f, 64.0f); host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Padding X"), &u.PadX, 0.5f, 0.0f, 64.0f); host.TrackLastImGuiItem();
    }

    // Значок — у любого вида: заданный на панели или полосе, он рисуется у
    // левого края, а текст сдвигается за ним (см. UIComponents.h).
    ImGui::SeparatorText(T("Icon"));
    const std::vector<std::string>& icons = sage::ui::IconNames();
    std::string current = u.Icon.empty() ? T("(none)") : u.Icon;
    if (ImGui::BeginCombo(T("Icon"), current.c_str())) {
        if (ImGui::Selectable(T("(none)"), u.Icon.empty())) {
            host.PushUndoSnapshot();
            u.Icon.clear();
        }
        for (const std::string& name : icons) {
            if (ImGui::Selectable(name.c_str(), name == u.Icon)) {
                host.PushUndoSnapshot();
                u.Icon = name;
            }
        }
        ImGui::EndCombo();
    }
    if (!u.Icon.empty()) {
        ImGui::ColorEdit4(T("Icon Color"), &u.IconColor.x); host.TrackLastImGuiItem();
    }

    // --- Текст --------------------------------------------------------------
    if (hasText) {
        ImGui::SeparatorText(T("Text"));
        char textBuf[256];
        std::snprintf(textBuf, sizeof(textBuf), "%s", u.Text.c_str());
        if (ImGui::InputText(T("Text"), textBuf, sizeof(textBuf))) u.Text = textBuf;
        host.TrackLastImGuiItem();
        ImGui::DragFloat(T("Text Scale"), &u.TextScale, 0.05f, 0.5f, 12.0f); host.TrackLastImGuiItem();
        ImGui::ColorEdit4(T("Text Color"), &u.TextColor.x); host.TrackLastImGuiItem();
        ImGui::Checkbox(T("Center Text"), &u.TextCentered);
        ImGui::SameLine();
        ImGui::Checkbox(T("Wrap"), &u.WrapText);
    }

    // --- Картинка -----------------------------------------------------------
    if (hasSprite) {
        ImGui::SeparatorText(T("Image"));
        auto loadTexture = [&] {
            u.Tex = u.TexturePath.empty()
                        ? nullptr
                        : u.PixelArt ? ResourceManager::Instance().GetTexture(
                                           u.TexturePath, TextureFilter::Nearest, false)
                                     : ResourceManager::Instance().GetTexture(u.TexturePath);
        };
        // Тот же слот, что у мешей и материалов: перетащить картинку из Assets,
        // увидеть её тут же, узнать, где она лежит.
        const assetslot::Result r = assetslot::Draw(host, "ui_tex", assetslot::Kind::Texture,
                                                    u.TexturePath, &m_preview);
        if (r.Changed) {
            host.PushUndoSnapshot();
            u.TexturePath = r.Path;
            loadTexture();
        }
        if (r.BrowseRequested) {
            FileBrowser::Config c;
            c.Title = T("Choose an image");
            c.Filters = assetslot::Extensions(assetslot::Kind::Texture);
            c.FilterLabel = T("Images");
            if (host.CurrentProject().Loaded()) c.StartDir = host.CurrentProject().AssetsDir();
            m_browser.Open(c);
            m_browseTarget = &u.TexturePath;
            m_browseIsShader = false;
            m_browseIsMesh = false;
            m_browseIsMaterial = false;
        }
        if (!u.TexturePath.empty() && !u.Tex) {
            ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%s", T("Texture not loaded (press Load)"));
            ImGui::SameLine();
            if (EditorIcons::Button("refresh", T("Load"))) {
                host.PushUndoSnapshot();
                loadTexture();
            }
        }
        // Пиксель-арт меняет ФИЛЬТРАЦИЮ загруженной текстуры, поэтому
        // перезагружаем сразу: иначе галка стоит, а картинка мыльная до
        // следующего Load, и это выглядит как «галка не работает».
        if (ImGui::Checkbox(T("Pixel Art (nearest, no mips)"), &u.PixelArt)) {
            host.PushUndoSnapshot();
            loadTexture();
        }
        if (u.Tex) ImGui::TextDisabled(T("Sheet: %d x %d px"), u.Tex->Width(), u.Tex->Height());

        // Спрайт с листа — под своим заголовком: это отдельная тема, и на
        // цельной картинке все эти поля лишние.
        if (ImGui::TreeNode(T("Sprite from a sheet"))) {
            ImGui::DragFloat4(T("Sprite x,y,w,h"), &u.Sprite.x, 1.0f, 0.0f, 8192.0f);
            host.TrackLastImGuiItem();
            ImGui::TextDisabled("%s", T("w or h = 0 — the whole file"));
            ImGui::DragFloat4(T("9-slice l,t,r,b"), &u.SliceBorder.x, 0.5f, 0.0f, 512.0f);
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Pixel Scale"), &u.PixelScale, 0.25f, 0.0f, 16.0f);
            host.TrackLastImGuiItem();
            ImGui::TextDisabled("%s", T("0 — stretch to fit; >0 — source pixel size"));
            ImGui::DragFloat4(T("Hover sprite"), &u.SpriteHover.x, 1.0f, 0.0f, 8192.0f);
            host.TrackLastImGuiItem();
            ImGui::DragFloat4(T("Pressed sprite"), &u.SpritePressed.x, 1.0f, 0.0f, 8192.0f);
            host.TrackLastImGuiItem();
            ImGui::TreePop();
        }
    }

    // --- Полоса -------------------------------------------------------------
    if (hasBar) {
        ImGui::SeparatorText(T("Bar"));
        ImGui::SliderFloat(T("Value"), &u.Value, 0.0f, 1.0f); host.TrackLastImGuiItem();
        ImGui::ColorEdit4(T("Fill Color"), &u.BarFillColor.x); host.TrackLastImGuiItem();
    }

    // --- Поведение ----------------------------------------------------------
    if (interactive) {
        ImGui::SeparatorText(T("Interaction"));
        ImGui::Checkbox(T("Interactive"), &u.Interactive);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", T("Reacts to the mouse: hover, press, focus"));
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!u.Interactive);
        ImGui::Checkbox(T("Enabled"), &u.Enabled);
        ImGui::EndDisabled();
        if (hasInput) {
            char phBuf[256];
            std::snprintf(phBuf, sizeof(phBuf), "%s", u.Placeholder.c_str());
            if (ImGui::InputText(T("Placeholder"), phBuf, sizeof(phBuf))) u.Placeholder = phBuf;
            host.TrackLastImGuiItem();
            ImGui::DragInt(T("Max Length"), &u.MaxLength, 1, 0, 4096);
            host.TrackLastImGuiItem();
            ImGui::Checkbox(T("Password"), &u.Password);
        }
        if (hasRange) {
            ImGui::DragFloat(T("Min Value"), &u.MinValue, 0.1f); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Max Value"), &u.MaxValue, 0.1f); host.TrackLastImGuiItem();
        }
        if (hasRange || u.Type == Kind::Checkbox) {
            ImGui::SliderFloat(T("Value (0..1)"), &u.Value, 0.0f, 1.0f);
            host.TrackLastImGuiItem();
        }
    }

    ImGui::Separator();
    // Вёрстка правится прямо в кадре — рамками и ручками поверх картинки (см.
    // UICanvas). Кнопка здесь потому, что элемент, которого не видно, ищут
    // именно отсюда.
    ImGui::Checkbox(T("Edit layout in the viewport"), &host.UIEditMode());
    if (ImGui::Button(T("Remove UI Element"))) {
        host.PushUndoSnapshot();
        reg.remove<UIElementComponent>(obj.Entity());
    }
}

void InspectorPanel::DrawAddComponentMenu(EditorHost& host, GameObject obj) {
    entt::registry& reg = host.CurrentScene().Registry();
    entt::entity e = obj.Entity();

    ImGui::Separator();
    if (ImGui::Button(T("Add Component"), ImVec2(-1, 0))) {
        ImGui::OpenPopup("##add_component");
        m_addComponentFilter[0] = '\0';
        m_addComponentFocus = true;
    }
    if (!ImGui::BeginPopup("##add_component")) return;

    ImGui::SetNextItemWidth(280.0f);
    if (m_addComponentFocus) {
        ImGui::SetKeyboardFocusHere(); // курсор сразу в поиске — руки уже на клавиатуре
        m_addComponentFocus = false;
    }
    ImGui::InputTextWithHint("##filter", T("Search..."), m_addComponentFilter,
                             sizeof(m_addComponentFilter));
    ImGui::Separator();

    std::string filter = m_addComponentFilter;
    std::transform(filter.begin(), filter.end(), filter.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    auto matches = [&](const ComponentEntry& c) {
        if (filter.empty()) return true;
        std::string hay = std::string(c.Name) + " " + c.Hint + " " + c.Category;
        std::transform(hay.begin(), hay.end(), hay.begin(),
                       [](unsigned char ch) { return (char)std::tolower(ch); });
        return hay.find(filter) != std::string::npos;
    };

    const char* lastCategory = nullptr;
    int shown = 0;
    for (const ComponentEntry& c : ComponentRegistry()) {
        if (c.Has(reg, e) || !matches(c)) continue;
        if (!lastCategory || std::strcmp(lastCategory, c.Category) != 0) {
            if (lastCategory) ImGui::Spacing();
            ImGui::TextDisabled("%s", T(c.Category));
            lastCategory = c.Category;
        }
        ImGui::PushID(c.Name);
        const ImVec2 rowPos = ImGui::GetCursorScreenPos();
        const float iconSize = ImGui::GetTextLineHeight();
        // Строка — один Selectable на две строки текста: имя и что оно делает.
        // Без второй строки список компонентов читается только теми, кто и так
        // знает, что такое «GI Static».
        const bool picked = ImGui::Selectable("##row", false, ImGuiSelectableFlags_None,
                                              ImVec2(300.0f, iconSize * 2.0f + 4.0f));
        EditorIcons::Overlay(rowPos.x + 2.0f, rowPos.y + iconSize * 0.5f - 1.0f, iconSize, c.Icon,
                             glm::vec3(0.62f, 0.72f, 0.85f));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(rowPos.x + iconSize + 10.0f, rowPos.y + 1.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), T(c.Name));
        dl->AddText(ImVec2(rowPos.x + iconSize + 10.0f, rowPos.y + iconSize + 2.0f),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled), T(c.Hint));
        if (picked) {
            host.PushUndoSnapshot();
            c.Add(reg, e);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
        ++shown;
    }
    if (shown == 0) {
        ImGui::TextDisabled("%s", filter.empty() ? T("All components already added")
                                                 : T("Nothing matches the search."));
    }
    ImGui::EndPopup();
}

void InspectorPanel::Draw(EditorHost& host, bool* open) {
    // Результат обзора приходит через кадр после нажатия — кладём его туда, ради
    // чего диалог открывали. Draw() зовётся РОВНО ОДИН РАЗ за кадр: у модалки
    // ImGui одно состояние на всю панель, и второй вызов открывал бы её поверх
    // себя же.
    if (m_browser.Draw()) {
        // AssetRef, а не Result().string(): диалог отдаёт АБСОЛЮТНЫЙ путь, и в
        // таком виде он до сих пор уезжал в сцену — работая в этом редакторе на
        // этой машине и нигде больше (см. Project::AssetRef).
        const std::string picked = host.CurrentProject().AssetRef(m_browser.Result());

        if (m_browseScriptEntity >= 0) {
            // Скрипт адресован СУЩНОСТИ, а не полю: за кадр ожидания сцену могли
            // перезагрузить, и указатель на поле компонента уже никуда не годился бы.
            GameObject target = host.CurrentScene().Get(m_browseScriptEntity);
            m_browseScriptEntity = -1;
            if (target.Valid()) {
                host.PushUndoSnapshot();
                host.CurrentScene().Registry().emplace_or_replace<ScriptComponent>(
                    target.Entity(), ScriptComponent{picked});
            }
        } else if (m_browseTarget) {
            *m_browseTarget = picked;
            m_browseTarget = nullptr;
            if (m_browseIsMesh) {
                m_browseIsMesh = false;
                m_pendingMeshLoad = true;   // грузим в том же кадре, ниже по панели
            }
            if (m_browseIsMaterial && m_browseCreateMaterial) {
                // Путь спросили ради СОЗДАНИЯ: файла ещё нет, его надо записать.
                m_browseIsMaterial = false;
                m_browseCreateMaterial = false;
                if (GameObject sel = host.SelectedObject(); sel.Valid()) {
                    WriteMaterialFromOverrides(host, sel.Renderer(), m_browser.Result().string());
                }
            } else if (m_browseIsMaterial) {
                m_browseIsMaterial = false;
                // Материал грузим сразу: без указателя объект остался бы с путём
                // и без вида — «выбрал материал, ничего не произошло».
                if (GameObject sel = host.SelectedObject(); sel.Valid()) {
                    MeshRendererComponent& mr = sel.Renderer();
                    if (!mr.MaterialPath.empty())
                        mr.MaterialPtr = ResourceManager::Instance().GetMaterial(mr.MaterialPath);
                    // Выбирали могли и для слота части модели. Какой именно это
                    // был слот, здесь уже не известно — путь ушёл прямо в поле, —
                    // а перечитать их все стоит одного обращения к кэшу на слот.
                    for (MaterialSlot& slot : mr.Slots) {
                        slot.Ptr = slot.Path.empty()
                                       ? nullptr
                                       : ResourceManager::Instance().GetMaterial(slot.Path);
                    }
                }
            }
            if (m_browseIsShader) {
                if (std::shared_ptr<Material> m =
                        ResourceManager::Instance().GetMaterial(host.SelectedAssetPath().string())) {
                    m->ShaderPtr.reset();
                }
            }
        }
    }

    ImGui::Begin(T("Inspector" "###Inspector"), open);

    // ------------------------------------------------------------------------
    // Две ПРИНЦИПИАЛЬНО разные вещи — в две вкладки, а не в одну простыню.
    //
    // Раньше панель просто складывала одно под другое: сверху редактор
    // материала, выбранного в Assets, снизу свойства выбранной сущности, между
    // ними голая черта. Это два независимых предмета правки — файл на диске и
    // объект в сцене, — и у них даже разная область действия: материал общий
    // для всех, кто им покрашен, а Transform принадлежит одной сущности.
    // Соседство без границы читалось как один список свойств одного объекта, и
    // человек не понимал, что именно он сейчас меняет.
    //
    // Вкладка САМА переключается на то, что человек выбрал последним: щёлкнул
    // по объекту в сцене — открыт объект, щёлкнул по файлу в Assets — открыт
    // ассет. Иначе за разделение пришлось бы платить лишним кликом на каждое
    // переключение, и оно бы только мешало.
    // ------------------------------------------------------------------------
    const AssetKind assetKind = ClassifyAsset(host.SelectedAssetPath());
    const bool hasEntity = host.SelectedObject().Valid();
    const bool hasAsset = assetKind != AssetKind::None;

    // Что выбрали последним. Сравниваем с прошлым кадром: событий выбора панель
    // не получает, а сравнение состояния даёт ровно тот же ответ.
    const int entityId = hasEntity ? host.SelectedObject().Id() : -1;
    const std::string assetPath = host.SelectedAssetPath().string();
    if (entityId != m_lastEntityId && hasEntity) { m_focus = Focus::Object; m_forceFocus = true; }
    if (assetPath != m_lastAssetPath && hasAsset) { m_focus = Focus::Asset; m_forceFocus = true; }
    m_lastEntityId = entityId;
    m_lastAssetPath = assetPath;

    if (!hasEntity && !hasAsset) {
        ImGui::TextDisabled("%s", T("Nothing selected"));
        ImGui::Spacing();
        // TextWrapped, а не две строки текста: панель узкая и её ширину меняют,
        // а обрезанная посередине подсказка бесполезнее отсутствующей.
        ImGui::TextWrapped("%s", T("Select an object in the viewport or Hierarchy, or a file in Assets."));
        ImGui::End();
        return;
    }

    // Одна сущность выбрана — вкладки не нужны: они бы только съедали строку.
    if (hasEntity && !hasAsset) {
        DrawObjectSection(host);
    } else if (!hasEntity && hasAsset) {
        DrawAssetSection(host, assetKind);
    } else if (ImGui::BeginTabBar("##inspector_tabs", ImGuiTabBarFlags_None)) {
        // SetSelected ставится РОВНО НА ОДИН КАДР — тот, в котором сменился
        // выбор. Передавать его каждый кадр, пока m_focus равен вкладке, нельзя:
        // человек щёлкает по «Ассету», ImGui его открывает, а на следующем кадре
        // флаг у «Объекта» всё ещё выставлен и утаскивает выбор обратно. Вкладка
        // выглядела намертво залипшей — ровно так эта панель и сломалась.
        const bool force = m_forceFocus;
        m_forceFocus = false;
        const ImGuiTabItemFlags objFlags = (force && m_focus == Focus::Object)
                                               ? ImGuiTabItemFlags_SetSelected
                                               : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(T("Object" "###Object"), nullptr, objFlags)) {
            m_focus = Focus::Object;
            DrawObjectSection(host);
            ImGui::EndTabItem();
        }
        const ImGuiTabItemFlags assetFlags = (force && m_focus == Focus::Asset)
                                                 ? ImGuiTabItemFlags_SetSelected
                                                 : ImGuiTabItemFlags_None;
        if (ImGui::BeginTabItem(T("Asset" "###Asset"), nullptr, assetFlags)) {
            m_focus = Focus::Asset;
            DrawAssetSection(host, assetKind);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

// Заголовок раздела: что именно правится. Без него вкладка «Ассет» с полями
// Albedo/Metallic ничем не отличается от материала, назначенного объекту, —
// а это разные вещи: здесь правится ФАЙЛ, общий для всех, кто им покрашен.
void InspectorPanel::DrawSectionHeader(const char* icon, const char* kind, const std::string& name,
                                       const std::string& subtitle) {
    ImGui::Spacing();
    const float s = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    EditorIcons::Overlay(p.x, p.y + s * 0.15f, s, icon, glm::vec3(0.62f, 0.72f, 0.85f));
    ImGui::Dummy(ImVec2(s * 1.35f, s));
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextUnformatted(name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", kind);
    if (!subtitle.empty()) {
        ImGui::TextDisabled("%s", subtitle.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", subtitle.c_str());
    }
    ImGui::Separator();
    ImGui::Spacing();
}

InspectorPanel::AssetKind InspectorPanel::ClassifyAsset(const std::filesystem::path& path) {
    if (path.empty()) return AssetKind::None;
    const std::string ext = path.extension().string();
    if (ext == ".sagemat") return AssetKind::Material;
    if (ext == ".sageprefab") return AssetKind::Prefab;
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb") return AssetKind::Model;
    return AssetKind::Other;
}

void InspectorPanel::DrawObjectSection(EditorHost& host) {
    GameObject obj = host.SelectedObject();
    DrawSectionHeader("cube", T("scene object"), obj.Name(),
                      T("The properties of this entity belong to it alone."));

    // Мультивыделение: правим первичную, но подсказываем размер набора
    // (гизмо двигает все; Delete/Duplicate — по всем выбранным).
    if (host.Selection().size() > 1) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), T("%zu selected — editing the primary one"),
                           host.Selection().size());
        ImGui::Separator();
    }
    DrawEntityProperties(host);
}

void InspectorPanel::DrawAssetSection(EditorHost& host, AssetKind kind) {
    const std::filesystem::path& path = host.SelectedAssetPath();
    const std::string name = path.filename().string();

    switch (kind) {
        case AssetKind::Material:
            DrawSectionHeader("material", T("material"), name,
                              T("A file on disk — the change affects EVERY object using this material."));
            DrawMaterialEditor(host);
            break;
        case AssetKind::Prefab:
            DrawSectionHeader("cube", T("prefab"), name,
                              T("A subtree template: double-clicking in Assets places a copy into the scene."));
            DrawPrefabPreview(host);
            break;
        case AssetKind::Model:
            DrawSectionHeader("model", T("model"), name,
                              T("Import settings are baked into the mesh on load."));
            DrawModelImportEditor(host);
            break;
        default: {
            // Для остальных типов редактора нет — но пустая вкладка выглядит как
            // поломка, поэтому показываем то, что известно о файле.
            DrawSectionHeader("file", T("file"), name, path.string());
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (!ec) ImGui::TextDisabled(T("Size: %llu bytes"), (unsigned long long)size);
            ImGui::Spacing();
            ImGui::TextDisabled("%s", T("There is no editor for this file type."));
            ImGui::TextDisabled("%s", T("Materials (.sagemat) and models (.obj/.gltf/.glb)"));
            ImGui::TextDisabled("%s", T("and are edited right here."));
            break;
        }
    }
}
