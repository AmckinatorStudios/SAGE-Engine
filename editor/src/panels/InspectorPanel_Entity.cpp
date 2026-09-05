// ---------------------------------------------------------------------------
// Инспектор — свойства сущности и её компоненты.
//
// Компоненты трёхмерного объекта: трансформ, свет, камера, физика, частицы,
// звук, скрипты. Самая длинная часть инспектора и самая механическая: на каждый
// компонент — свой блок полей.
//
// Часть класса InspectorPanel: объявления остались в InspectorPanel.h, здесь
// только тела. Разбит потому, что дорос до двух тысяч строк, в которых рядом
// лежали редактор материала, якоря интерфейса и список компонентов.
// ---------------------------------------------------------------------------
#include <cstdarg>
#include "EditorTheme.h"
#include "ui/UI.h"
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
#include "VarsEditor.h"
#include "sage/core/Log.h"
#include "sage/ecs/LightSystem.h"
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

// --- СОЛНЦЕ СЦЕНЫ ----------------------------------------------------------
//
// Солнце — это ОБЪЕКТ, а не строчка в отдельном окне. Раньше его направление и
// цвет правились в панели Lighting, и это было не просто неудобно, а неверно:
// направленный свет-сущность ПЕРЕКРЫВАЕТ солнце из настроек сцены (см.
// ecs::CollectLighting), поэтому у сцены с объектом-солнцем те ползунки молча
// не делали НИЧЕГО. Человек крутил направление и смотрел на неподвижные тени.
//
// Теперь всё, что относится к солнцу, лежит здесь — на объекте:
//   • куда светит (поворот объекта, плюс азимут и высота, которыми это удобно
//     задавать словами «утро» и «полдень», а не тремя углами Эйлера);
//   • диск в небе — его размер и цвет: небо рисует солнце ТАМ ЖЕ, куда светит
//     этот источник (см. CelestialsFromEnvironment), и держать их порознь
//     значило бы уметь развести свет и его источник в разные стороны.
void InspectorPanel::DrawSunSection(EditorHost& host, GameObject obj) {
    Scene& scene = host.CurrentScene();
    const entt::entity sun = sage::ecs::FindSunEntity(scene);
    const bool isSun = sun == obj.Entity();

    if (!isSun) {
        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                           T("Not the scene's sun: a directional light with a lower number in "
                             "the hierarchy is already the sun, and only one takes part in "
                             "the frame."));
        if (sun != entt::null) {
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Show the sun"))) {
                const IdComponent* id = scene.Registry().try_get<IdComponent>(sun);
                if (id) host.SetSelectedId(id->Id);
            }
        }
        return;
    }

    ImGui::SeparatorText(T("Sun of the scene"));
    HintWrapped(T("This light IS the sun: it casts the cascaded shadows and it is where the "
                  "procedural sky draws its disc. Point it by rotating the object, or by "
                  "azimuth and elevation below."));

    // Азимут и высота вместо трёх углов Эйлера. Так солнце и ставят: «с юга,
    // низко» — это утро, «сверху» — полдень. Считаются из направления и
    // записываются обратно поворотом объекта, поэтому гизмо во вьюпорте и эти
    // два поля — одно и то же, а не два способа с разными результатами.
    Transform& tr = obj.GetTransform();
    const glm::vec3 fwd = sage::ecs::ForwardFromEuler(tr.Rotation);
    const glm::vec3 toSun = -fwd;   // светит ОТТУДА, куда смотрит, со знаком минус
    float elevation = glm::degrees(std::asin(glm::clamp(toSun.y, -1.0f, 1.0f)));
    float azimuth = glm::degrees(std::atan2(toSun.x, toSun.z));

    bool changed = false;
    changed |= ImGui::SliderFloat(T("Elevation"), &elevation, -20.0f, 90.0f, "%.0f°");
    host.TrackLastImGuiItem();
    changed |= ImGui::SliderFloat(T("Azimuth"), &azimuth, -180.0f, 180.0f, "%.0f°");
    host.TrackLastImGuiItem();
    if (changed) {
        const float e = glm::radians(elevation), a = glm::radians(azimuth);
        const glm::vec3 dir(std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a));
        tr.Rotation = sage::ecs::EulerFromForward(-dir);   // светит ПРОТИВ направления на солнце
    }

    // Диск в небе. Живёт в настройках неба сцены, но правят его здесь: это
    // внешность ЭТОГО источника, а не отдельное светило.
    LightingEnvironment& env = scene.Lighting;
    ImGui::BeginDisabled(!env.Skybox.Enabled || !env.Skybox.Celestials);
    ImGui::ColorEdit3(T("Disc Colour"), &env.Skybox.SunColor.x); host.TrackLastImGuiItem();
    ImGui::SliderFloat(T("Disc Size"), &env.Skybox.SunSize, 0.005f, 0.2f, "%.3f");
    host.TrackLastImGuiItem();
    ImGui::EndDisabled();
    if (!env.Skybox.Enabled || !env.Skybox.Celestials) {
        ImGui::TextDisabled("%s", T("The sky or its celestials are off — see Environment."));
        // Кнопка прямо здесь, а не только адрес окна: две галки в другом окне —
        // ровно тот путь, из-за которого «где настраивается солнце» и стало
        // вопросом. Правка чужая (свойства сцены), поэтому со снимком undo.
        ImGui::SameLine();
        if (ImGui::SmallButton(T("Turn on"))) {
            host.PushUndoSnapshot();
            env.Skybox.Enabled = true;
            env.Skybox.Celestials = true;
        }
    }
}

void InspectorPanel::DrawEntityProperties(EditorHost& host) {
    GameObject obj = host.SelectedObject();
    entt::registry& reg = host.CurrentScene().Registry();

    // Шапка объекта — та же сетка свойств, что и у всех разделов ниже.
    // Отдельная вёрстка здесь означала бы, что имя и номер начинаются не на
    // той координате, что позиция и поворот, — а это первое, что видно при
    // открытии инспектора.
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", obj.Name().c_str());
    Sage::UI::BeginProperties("entity_head");
    Sage::UI::PropertyLabel(T("Name"));
    if (Sage::UI::PropertyText("name", buf, sizeof(buf))) obj.SetName(buf);
    host.TrackLastImGuiItem();
    Sage::UI::PropertyLabel(T("Id"));
    Sage::UI::PropertyValue("%d", obj.Id());
    Sage::UI::EndProperties();
    Sage::UI::Separator();

    if (EditorTheme::SectionHeader(T("Transform" "###Transform"), ImGuiTreeNodeFlags_DefaultOpen)) {
        // СЕТКА СВОЙСТВ, а не DragFloat3 с подписью.
        //
        // У ImGui подпись стоит СПРАВА от поля, и инспектор читался задом
        // наперёд: «1.500 0.500 2.200 Позиция». Хуже того, подпись съедала
        // ширину у самих полей и в узкой панели обрезалась посреди слова, а
        // строки соседних разделов начинались каждая со своей координаты —
        // сетки не было вовсе.
        //
        // Sage::UI ставит подпись слева в колонке одной ширины, а значения —
        // на одной координате во ВСЕХ строках и всех разделах (см. ui/UI.h).
        Transform& tr = obj.GetTransform();
        Sage::UI::BeginProperties("transform");
        Sage::UI::PropertyLabel(T("Position"));
        Sage::UI::PropertyVec3("pos", &tr.Position.x, 0.05f); host.TrackLastImGuiItem();
        Sage::UI::PropertyLabel(T("Rotation"));
        Sage::UI::PropertyVec3("rot", &tr.Rotation.x, 0.5f, "%.1f"); host.TrackLastImGuiItem();
        Sage::UI::PropertyLabel(T("Scale"));
        Sage::UI::PropertyVec3("scl", &tr.Scale.x, 0.05f); host.TrackLastImGuiItem();
        Sage::UI::EndProperties();
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
    if (EditorTheme::SectionHeader(T("Mesh Renderer" "###Mesh Renderer"), ImGuiTreeNodeFlags_DefaultOpen)) {
        MeshRendererComponent& mr = obj.Renderer();
        DrawMeshSlot(host, mr);
        DrawMaterialSlot(host, mr);
        DrawInstanceOverrides(host, mr, obj.Id());
    }

    // --- Камера (игровая): панель Game рендерит от первой Primary-камеры ---
    if (reg.all_of<CameraComponent>(obj.Entity()) && EditorTheme::SectionHeader(T("Camera" "###Camera"), ImGuiTreeNodeFlags_DefaultOpen)) {
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

    if (reg.all_of<LightComponent>(obj.Entity()) && EditorTheme::SectionHeader(T("Light" "###Light"), ImGuiTreeNodeFlags_DefaultOpen)) {
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
                DrawSunSection(host, obj);
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
        EditorTheme::SectionHeader(T("Decal" "###Decal"), ImGuiTreeNodeFlags_DefaultOpen)) {
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
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
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
    if (reg.all_of<GIStaticComponent>(obj.Entity()) && EditorTheme::SectionHeader(T("GI Static" "###GI Static"), ImGuiTreeNodeFlags_DefaultOpen)) {
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

    if (reg.all_of<NetReplicatedComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader(T("Net Replicated" "###Net Replicated"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("%s", T("The server sends this object to clients; they follow it"));
        ImGui::TextDisabled("%s", T("Position, rotation, scale, color and primitive are replicated"));
        if (ImGui::Button(T("Remove##netreplicated"))) {
            host.PushUndoSnapshot();
            reg.remove<NetReplicatedComponent>(obj.Entity());
        }
    }

    if (reg.all_of<ScriptComponent>(obj.Entity()) && EditorTheme::SectionHeader(T("Script" "###Script"), ImGuiTreeNodeFlags_DefaultOpen)) {
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
                c.StartDir = host.CurrentProject().AssetsDir();
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

    // --- ПУБЛИЧНЫЕ ПЕРЕМЕННЫЕ И ССЫЛКИ ---------------------------------------
    //
    // Показываются у объекта СО СКРИПТОМ (скрипт их объявляет) и у любого, у
    // кого они уже есть: публичные данные нужны и объекту без скрипта — точке
    // появления с именем волны, зоне с названием следующего уровня, кнопке с
    // аргументом события.
    //
    // Секция идёт СРАЗУ ЗА СКРИПТОМ намеренно: «какой скрипт» и «с какими
    // настройками» — один вопрос, и разносить их по разным концам списка
    // значит заставлять прокручивать инспектор туда-обратно.
    {
        const bool hasScript = reg.all_of<ScriptComponent>(obj.Entity());
        const bool hasVars = reg.all_of<VarsComponent>(obj.Entity());
        if ((hasScript || hasVars) &&
            EditorTheme::SectionHeader(T("Variables" "###Variables"), ImGuiTreeNodeFlags_DefaultOpen)) {
            VarsComponent& vc = reg.get_or_emplace<VarsComponent>(obj.Entity());
            // Объявление скрипта подмешивается ПЕРЕД показом, а не однажды при
            // назначении: файл правят снаружи редактора, и переменная,
            // добавленная в скрипт минуту назад, обязана появиться здесь сама.
            if (hasScript) host.MergeScriptVars(obj);
            // Снимок для отмены берут сами поля (см. VarsEditor.cpp): у
            // ползунка он нужен ДО перетаскивания, а не после.
            varsui::DrawTable(host, obj, vc.Values, &m_preview);
        }
    }

    // --- Твёрдое тело (симулируется в Play-режиме выбранным бэкендом физики) ---
    if (reg.all_of<RigidBodyComponent>(obj.Entity()) && EditorTheme::SectionHeader(T("Rigid Body" "###Rigid Body"), ImGuiTreeNodeFlags_DefaultOpen)) {
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
    if (reg.all_of<ColliderComponent>(obj.Entity()) && EditorTheme::SectionHeader(T("Collider" "###Collider"), ImGuiTreeNodeFlags_DefaultOpen)) {
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
        EditorTheme::SectionHeader(T("Character Controller" "###Character Controller"), ImGuiTreeNodeFlags_DefaultOpen)) {
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
    if (reg.all_of<JointComponent>(obj.Entity()) && EditorTheme::SectionHeader(T("Joint" "###Joint"), ImGuiTreeNodeFlags_DefaultOpen)) {
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
    if (reg.all_of<AnimatedModelComponent>(obj.Entity()) && EditorTheme::SectionHeader(T("Animated Model" "###Animated Model"), ImGuiTreeNodeFlags_DefaultOpen)) {
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
                    ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Info), T("cross-fading %.0f%%"),
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
        EditorTheme::SectionHeader(T("Reflection Probe" "###Reflection Probe"), ImGuiTreeNodeFlags_DefaultOpen)) {
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
    if (reg.all_of<IKComponent>(obj.Entity()) && EditorTheme::SectionHeader(T("IK" "###IK"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (IKComponent* ik = reg.try_get<IKComponent>(obj.Entity())) {
            ImGui::Checkbox(T("IK Enabled"), &ik->Enabled);
            const AnimatedModelComponent* am = reg.try_get<AnimatedModelComponent>(obj.Entity());
            if (!am) ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn),
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
                        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s",
                                           T("bone not found in skeleton"));
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
    if (reg.all_of<ParticleEmitterComponent>(obj.Entity()) && EditorTheme::SectionHeader(T("Particle Emitter" "###Particle Emitter"), ImGuiTreeNodeFlags_DefaultOpen)) {
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

    if (reg.all_of<sage::ui::Transform>(obj.Entity()) &&
        EditorTheme::SectionHeader(T("UI Element" "###UI Element"), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawUIElement(host, obj);
    }

    // --- Единое «Add Component»: добавляет любой ОТСУТСТВУЮЩИЙ компонент ---
    DrawAddComponentMenu(host, obj);

    // КНОПКИ «УДАЛИТЬ ОБЪЕКТ» ЗДЕСЬ НЕТ, И ЭТО НАМЕРЕННО.
    //
    // Инспектор — про свойства того, что выбрано, и его листают сверху вниз,
    // правя поля. Красная кнопка во всю ширину в конце этого списка стоит
    // ровно там, куда приходит колесо мыши и палец на тачпаде после длинного
    // компонента, и удаляет объект целиком — цена промаха несопоставима с
    // ценой соседних действий.
    //
    // Удалить объект по-прежнему можно тремя путями, и все они там, где
    // человек ДУМАЕТ об объекте целиком, а не о его полях: клавиша Del,
    // «Объект > Удалить» и правая кнопка на строке в иерархии.
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
        {"Net Replicated", "Logic", "network",
         "The server replicates this object to clients", HasComp<NetReplicatedComponent>,
         AddComp<NetReplicatedComponent>},
        // Интерфейс добавляется ОБЯЗАТЕЛЬНОЙ частью — прямоугольником; из чего
        // элемент состоит дальше, выбирается в самом инспекторе (или заготовкой).
        {"UI Element", "Interface", "rect",
         "Panel, label, image or bar on screen", HasComp<sage::ui::Transform>,
         AddComp<sage::ui::Transform>},
    };
    return kEntries;
}

} // namespace

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
