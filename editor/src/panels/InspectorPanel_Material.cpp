// ---------------------------------------------------------------------------
// Инспектор — материалы и меши.
//
// Слот меша, слот материала, редактор материала, подматериалы и переопределения
// экземпляра. Всё об одном: ЧЕМ и КАК нарисован объект. Здесь же и самая
// хитрая часть инспектора — правило, по которому правка попадает то в общий
// материал, то в переопределение конкретного объекта.
//
// Часть класса InspectorPanel: объявления остались в InspectorPanel.h, здесь
// только тела. Разбит потому, что дорос до двух тысяч строк, в которых рядом
// лежали редактор материала, якоря интерфейса и список компонентов.
// ---------------------------------------------------------------------------
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

#include "ui/UI.h"

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
#include "sage/ui/UI.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
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
        c.StartDir = host.CurrentProject().AssetsDir();
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
    Sage::UI::BeginProperties("mesh_src");
    Sage::UI::PropertyLabel(T("Source"));
    const bool kindChanged = Sage::UI::PropertyCombo("src", &kind, kinds, IM_ARRAYSIZE(kinds));
    Sage::UI::EndProperties();
    if (kindChanged) {
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
            c.StartDir = host.CurrentProject().AssetsDir();
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
        if (!mr.Ref.path.empty() && std::filesystem::path(mr.Ref.path).is_absolute()) {
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
    dir = host.CurrentProject().AssetsDir();

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
        // Через AssignMaterial: назначение материала возвращает поправки
        // экземпляра в нейтраль (см. RenderComponents.h). Иначе назначенный
        // материал показывался бы умноженным на старый тон объекта.
        AssignMaterial(mr, r.Path,
                       r.Path.empty() ? nullptr
                                      : ResourceManager::Instance().GetMaterial(r.Path));
    }
    if (r.BrowseRequested) {
        FileBrowser::Config c;
        c.Title = T("Choose a material");
        c.Filters = assetslot::Extensions(assetslot::Kind::Material);
        c.FilterLabel = T("Materials (*.sagemat)");
        c.StartDir = host.CurrentProject().AssetsDir();
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
// Модель из редактора почти никогда не одноматериальна: у
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
            c.StartDir = host.CurrentProject().AssetsDir();
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
