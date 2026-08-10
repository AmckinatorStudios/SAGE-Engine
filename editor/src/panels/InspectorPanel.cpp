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
#include "sage/ui/UI.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "../Localization.h"

namespace fs = std::filesystem;

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
