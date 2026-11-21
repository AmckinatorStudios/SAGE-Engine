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
#include "../ResetDefault.h"
#include <cctype>
#include <cstdarg>
#include "sage/scene/CharacterFit.h"
#include "sage/scene/ScaleLock.h"
#include "EditorTheme.h"
#include "InspectorPassesUI.h"
#include "ui/UI.h"
#include "InspectorPanel.h"
#include "sage/scene/AiAgentComponent.h"
#include "sage/scene/VisualScriptComponent.h"
#include "../SignalLinksEditor.h"
#include "../UIElementProperties.h"


#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "PostChainUi.h"
#include "ParticleUi.h"
#include "sage/render/ObjectEffects.h"
#include "sage/render/ParticleEffectIO.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Paths.h"
#include "VarsEditor.h"
#include "sage/core/Log.h"
#include "sage/scene/Signals.h"
#include "sage/ecs/LightSystem.h"
#include "sage/physics/PhysicsScene.h"
#include <algorithm>

#include "AssetSlot.h"
#include "ObjectSlot.h"
#include "EditorIcons.h"
#include "InspectorControls.h"
#include "InspectorFields.h"
namespace ifield = sage::editor::ifield;
#include "../EditorPrefs.h"
#include "ModelMaterialImport.h"
#include "Project.h"
#include "sage/render/ResourceManager.h"
#include "sage/assets/import/Importer.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/anim/PropertyAnimator.h"
#include "sage/anim/Tween.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIPresets.h"
#include "../Localization.h"
#include "ui/ColorPicker.h"

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
            if (ImGui::Button(T("Show the sun"))) {
                const IdComponent* id = scene.Registry().try_get<IdComponent>(sun);
                if (id) host.Selection().SetPrimary(id->Id);
            }
        }
        return;
    }

    sage::editor::ictl::SeparatorText(T("Sun of the scene"));
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
    changed |= ifield::Slider(host, T("Elevation"), &elevation, -20.0f, 90.0f, "%.0f°");
    changed |= ifield::Slider(host, T("Azimuth"), &azimuth, -180.0f, 180.0f, "%.0f°");
    if (changed) {
        const float e = glm::radians(elevation), a = glm::radians(azimuth);
        const glm::vec3 dir(std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a));
        tr.Rotation = sage::ecs::EulerFromForward(-dir);   // светит ПРОТИВ направления на солнце
    }

    // Диск в небе. Живёт в настройках неба сцены, но правят его здесь: это
    // внешность ЭТОГО источника, а не отдельное светило.
    LightingEnvironment& env = scene.Lighting;
    ImGui::BeginDisabled(!env.Skybox.Enabled || !env.Skybox.Celestials);
    ifield::Color3(host, T("Disc Colour"), &env.Skybox.SunColor);
    ifield::Slider(host, T("Disc Size"), &env.Skybox.SunSize, 0.005f, 0.2f, "%.3f");
    ImGui::EndDisabled();
    if (!env.Skybox.Enabled || !env.Skybox.Celestials) {
        ImGui::TextDisabled("%s", T("The sky or its celestials are off — see Environment."));
        // Кнопка прямо здесь, а не только адрес окна: две галки в другом окне —
        // ровно тот путь, из-за которого «где настраивается солнце» и стало
        // вопросом. Правка чужая (свойства сцены), поэтому со снимком undo.
        ImGui::SameLine();
        if (ImGui::Button(T("Turn on"))) {
            host.PushUndoSnapshot();
            env.Skybox.Enabled = true;
            env.Skybox.Celestials = true;
        }
    }
}

#include "InspectorPanelDetail.h"
using namespace sage::editor::inspectordetail;

// Связанные оси масштаба: ось, которую потянули, тянет за собой две другие.
//
// ЧЕРЕЗ ОТНОШЕНИЕ, А НЕ ЧЕРЕЗ РАЗНИЦУ. Масштаб — множитель: «прибавить 0.5»
// растянуло бы тонкую ось вдвое, а толстую на четверть, и предмет, который
// хотели увеличить, менял бы форму. Отношение сохраняет пропорции ровно.
//
// Отдельной функцией, а не десятком строк внутри отрисовки: это единственное
// место с арифметикой во всём разделе, и проверить её надо без интерфейса
// (шаг самопроверки scale-lock).
glm::vec3 InspectorPanel::LinkScaleAxes(const glm::vec3& before, const glm::vec3& after) {
    for (int i = 0; i < 3; ++i) {
        if (after[i] == before[i]) continue;
        // Ось была нулевой — отношения нет; тогда всем ставится одно значение.
        if (std::abs(before[i]) <= 1e-4f) return glm::vec3(after[i]);
        const float k = after[i] / before[i];
        glm::vec3 out = before * k;
        out[i] = after[i];   // потянутую ось оставляем ровно такой, какой ввели
        return out;
    }
    return after;
}

// --- Пост-обработка камеры ---------------------------------------------------
//
// Секция есть только у сущности, у которой компонент УЖЕ есть: как у света и
// камеры. Добавляется он из списка «Добавить компонент».
//
// САМ РЕДАКТОР — ОБЩИЙ (editor/src/PostChainUi.cpp): разделы в порядке
// обработки кадра, у каждого эффекта свой выключатель.
static void DrawPostProcessSection(EditorHost& host, entt::registry& reg, entt::entity entity,
                                   bool* removed) {
    using namespace sage::render;
    if (!reg.all_of<PostProcessComponent>(entity)) return;
    if (!EditorTheme::SectionHeader("camera", T("Post Processing" "###Post Processing"),
                                    ImGuiTreeNodeFlags_DefaultOpen, removed))
        return;
    PostProcessComponent* component = reg.try_get<PostProcessComponent>(entity);
    if (!component) return;

    // Общий выключатель — отдельно от удаления компонента: «посмотреть, как без
    // обработки» делают ежеминутно, и терять ради этого настройки нельзя.
    ifield::Check(host, T("Enabled"), &component->Enabled);
    EditorTheme::Hint(T("off — every effect of this component is off"));
    // Где лежит — там и действует (см. PostProcessComponent.h): на камере —
    // только её кадр, на любом другом объекте — все камеры без своего.
    if (reg.all_of<CameraComponent>(entity)) {
        ImGui::TextDisabled("%s", T("Applies to this camera only"));
    } else {
        ImGui::TextDisabled("%s", T("Applies to every camera of the scene without its own"));
        ifield::Int(host, T("Priority"), &component->Priority, -100, 100);
        EditorTheme::Hint(T("Of several scene-wide components the higher priority wins"));
    }

    // Откат и пометку «сцена изменена» редактор делает САМ, по каждому виджету:
    // у дискретной правки — снимок, у протяжки — отслеживание.
    ImGui::BeginDisabled(!component->Enabled);
    sage::editor::DrawPostChainEditor(&host, component->Chain, "camera");
    ImGui::EndDisabled();
}

// Эффекты отдельного объекта (ObjectEffectsComponent): тот же редактор
// цепочки, но только звенья готового кадра — они и накладываются на объект.
static void DrawObjectEffectsSection(EditorHost& host, entt::registry& reg, entt::entity entity, bool* removed) {
    using namespace sage::render;
    if (!reg.all_of<ObjectEffectsComponent>(entity)) return;
    if (!EditorTheme::SectionHeader("particles", T("Object Effects" "###Object Effects"), ImGuiTreeNodeFlags_DefaultOpen,
                                    removed))
        return;
    ObjectEffectsComponent* component = reg.try_get<ObjectEffectsComponent>(entity);
    if (!component) return;
    EditorTheme::Hint(T("Applied to this object and its children, where they are visible"));
    sage::editor::DrawPostChainEditor(&host, component->Effects, "objectfx", /*imageOnly=*/true);
}

void InspectorPanel::DrawEntityProperties(EditorHost& host) {
    GameObject obj = host.InspectedObject();
    entt::registry& reg = host.CurrentScene().Registry();

    // --- ПАПКА СПИСКА: имя, цвет и всё -------------------------------------
    //
    // У папки нет ни положения, ни вида, ни компонентов — она заведена ради
    // порядка в списке и на игру не влияет (см. FolderComponent). Показывать ей
    // Transform, «Меш» и «Добавить компонент» значило бы предлагать настроить
    // то, чего у неё нет и не будет, — и каждый, кто это попробует, решит, что
    // настройка не работает.
    if (FolderComponent* folder = reg.try_get<FolderComponent>(obj.Entity())) {
        sage::editor::ictl::Label(T("Colour"));
        if (Sage::UI::ColorField3("##folder_colour", &folder->Color.x, Sage::UI::ColorField_Compact))
            host.PushUndoSnapshot();
        EditorTheme::Hint(T("Mark in the list. A folder is for sorting only: it does not move its "
                            "contents and is not drawn."));
        host.TrackLastImGuiItem();
        int inside = 0;
        if (const HierarchyComponent* h = reg.try_get<HierarchyComponent>(obj.Entity()))
            inside = (int)h->Children.size();
        ImGui::TextDisabled(T("Objects inside: %d"), inside);
        return;
    }

    // Флаги «убрать компонент»: кнопка живёт в ЗАГОЛОВКЕ секции, а снятие
    // происходит в конце разбора — иначе компонент исчезал бы из-под кода,
    // который прямо сейчас рисует его поля.
    EntityRemovals rm;   // что снять — снимается после всех секций

    bool resetTransform = false;
    const bool transformOpen = EditorTheme::SectionHeader("move", T("Transform" "###Transform"),
                                                          ImGuiTreeNodeFlags_DefaultOpen, nullptr, nullptr,
                                                          &resetTransform);
    if (resetTransform) {
        host.PushUndoSnapshot();
        Transform& tr = obj.GetTransform();
        tr.Position = glm::vec3(0.0f);
        tr.Rotation = glm::vec3(0.0f);
        tr.Scale = glm::vec3(1.0f);
    }
    if (transformOpen) {
        // ТРИ СТРОКИ ОДНОГО ВИДА: «Подпись: [X ..] [Y ..] [Z ..] [кнопки]» —
        // общий контрол инспектора (ictl::Vector3 на NumberCore): метки осей
        // цветом, −/+, перетаскивание, колесо, правый щелчок по полю — сброс
        // оси, по подписи — сброс строки. Справа у каждой строки одно и то
        // же место под кнопки, поэтому столбцы X/Y/Z совпадают.
        namespace ictl = sage::editor::ictl;
        Transform& tr = obj.GetTransform();
        if (!m_uniformScaleLoaded) {
            m_uniformScaleLoaded = true;
            m_uniformScale = sage::editor::prefs::GetBool("inspector.uniform_scale", true);
        }
        const float side = ImGui::GetFrameHeight();
        const float trailing = side + 4.0f;   // одно место: сброс строки или замок
        // Сброс строки — значком, пока значение не по умолчанию (место под
        // него есть всегда, чтобы строки не прыгали).
        auto rowReset = [&](const char* id, glm::vec3& v, const glm::vec3& def) {
            ImGui::SameLine(0.0f, 4.0f);
            if (v == def) {
                ImGui::Dummy(ImVec2(side, side));
                return;
            }
            ImGui::PushID(id);
            if (EditorIcons::IconOnlyButton("refresh", T("Reset this row to its default"))) {
                host.PushUndoSnapshot();
                v = def;
            }
            ImGui::PopID();
        };

        ictl::NumOpts po;
        po.Step = 0.1f;
        po.Format = "%.3f";
        po.HasReset = true;
        po.Reset = 0.0f;
        ictl::Vector3(host, T("Position"), &tr.Position, po, trailing);
        rowReset("pos", tr.Position, glm::vec3(0.0f));

        ictl::NumOpts ro;
        ro.Step = 1.0f;
        ro.Format = "%.1f";
        ro.HasReset = true;
        ro.Reset = 0.0f;
        ictl::Vector3(host, T("Rotation"), &tr.Rotation, ro, trailing);
        rowReset("rot", tr.Rotation, glm::vec3(0.0f));

        // --- МАСШТАБ СО СВЯЗЬЮ ОСЕЙ ------------------------------------
        //
        // Замок в той же строке связывает X, Y и Z: тянешь одну ось — растут
        // все три в тех же пропорциях (LinkScaleAxes). Состояние замка —
        // привычка человека, запоминается между запусками. У камеры, света,
        // звука, зондов и папок масштаба нет (sage/scene/ScaleLock.h): поля
        // видны серыми, замка нет.
        const bool canScale = sage::scene::CanScale(*obj.Registry(), obj.Entity());
        const glm::vec3 before = tr.Scale;
        ictl::NumOpts so;
        so.Step = 0.05f;
        so.Format = "%.3f";
        so.HasReset = true;
        so.Reset = 1.0f;
        ImGui::BeginDisabled(!canScale);
        const bool scaleChanged = ictl::Vector3(host, T("Scale"), &tr.Scale, so, trailing);
        ImGui::EndDisabled();
        if (!canScale && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", T("This object has no size, so it cannot be scaled"));
        if (scaleChanged && m_uniformScale && canScale) tr.Scale = LinkScaleAxes(before, tr.Scale);
        // Место строки — замок; у объекта без масштаба — сброс остатка
        // старых сцен (тогда камеру можно было растянуть, и это искажает её вид).
        ImGui::SameLine(0.0f, 4.0f);
        if (canScale) {
            // Свой ID: в шапке того же окна — замок инспектора с тем же значком.
            ImGui::PushID("scale_lock");
            const bool toggled = EditorIcons::IconOnlyButton(
                    m_uniformScale ? "lock" : "unlock",
                    m_uniformScale ? T("Axes are linked: dragging one scales all three")
                                   : T("Axes are independent: each one scales on its own"),
                    m_uniformScale);
            ImGui::PopID();
            if (toggled) {
                m_uniformScale = !m_uniformScale;
                sage::editor::prefs::SetBool("inspector.uniform_scale", m_uniformScale);
            }
        } else {
            rowReset("scl", tr.Scale, glm::vec3(1.0f));
        }
        sage::editor::inspectordetail::DrawPivotSection(host, obj);
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
    // ecs/RenderComponents.h): ЧТО рисуем -> ЧЕМ красим. Третьей группы,
    // «поправок экземпляра» поверх материала, больше нет: вид объекта задаёт
    // материал, и спрашивать, какое из двух полей сейчас решает, не нужно.
    // СЕКЦИЯ ЕСТЬ, ТОЛЬКО ЕСЛИ ЕСТЬ КОМПОНЕНТ. Пустой объект его не несёт, и
    // рисовать ему «Меш» значило бы показывать цвет, тени и слот материала у
    // того, кто ничем не рисуется; добавляется компонент кнопкой «Добавить
    // компонент», как и все остальные.
    if (reg.all_of<MeshRendererComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader("cube", T("Mesh" "###Mesh"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Mesh)) {
        MeshRendererComponent& mr = obj.Renderer();
        DrawMeshSlot(host, obj.Entity(), mr, reg.all_of<AnimationComponent>(obj.Entity()));
        DrawMaterialSlot(host, mr);
        sage::editor::DrawRenderLayerField(host, mr);
    }
    // Детализация примитива — своим разделом сразу под мешем.
    if (reg.all_of<MeshRendererComponent>(obj.Entity())) sage::editor::DrawPrimitiveGeometry(host, obj.Renderer());

    // --- Камера (игровая): панель Game рендерит от первой Primary-камеры ---
    bool resetCamera = false;
    if (reg.all_of<CameraComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader("camera", T("Camera" "###Camera"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Camera,
                                   nullptr, &resetCamera)) {
        if (CameraComponent* cam = reg.try_get<CameraComponent>(obj.Entity())) {
            if (resetCamera) {
                // «Главная» — не настройка вида, а выбор, с какой камеры играть:
                // сброс её не трогает, иначе игра молча начиналась бы с другой.
                host.PushUndoSnapshot();
                const bool primary = cam->Primary;
                *cam = CameraComponent{};
                cam->Primary = primary;
            }
            ifield::Float(host, T("FOV"), &cam->Fov, 0.5f, 10.0f, 140.0f);
            sage::editor::ResetField(&host, *cam, cam->Fov);
            ifield::Float(host, T("Near"), &cam->NearClip, 0.01f, 0.001f, 10.0f);
            sage::editor::ResetField(&host, *cam, cam->NearClip);
            ifield::Float(host, T("Far"), &cam->FarClip, 1.0f, 1.0f, 5000.0f);
            sage::editor::ResetField(&host, *cam, cam->FarClip);
            ifield::Check(host, T("Primary"), &cam->Primary);
            EditorTheme::Hint(T("Game panel renders from the first Primary camera"));
            sage::editor::DrawCameraPasses(host, *cam);
        }
    }

    // --- Свет (позиция — Transform сущности; тип: точечный / прожектор / солнце) ---

    bool resetLight = false;
    if (reg.all_of<LightComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader("light", T("Light" "###Light"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Light,
                                   nullptr, &resetLight)) {
        if (LightComponent* light = reg.try_get<LightComponent>(obj.Entity())) {
            if (resetLight) {
                // Тип остаётся: сброс возвращает настройки ЭТОГО света, а не
                // превращает прожектор в лампочку.
                host.PushUndoSnapshot();
                const LightComponent::Type kind = light->Kind;
                *light = LightComponent{};
                light->Kind = kind;
            }
            const char* types[] = {T("Point"), T("Spot"), T("Directional (sun)"), T("Area (panel)")};
            int kind = (int)light->Kind;
            if (ifield::Combo(host, T("Type"), &kind, types, IM_ARRAYSIZE(types)))
                light->Kind = (LightComponent::Type)kind;   // снимок истории — внутри поля
            // Что это за свет — при наведении на тип, а не абзацем под полями.
            switch (light->Kind) {
                case LightComponent::Type::Area:
                    EditorTheme::Hint(T("A glowing panel facing the object's forward (-Z): window, screen, softbox. "
                                        "Highlights on glossy surfaces take its shape. It casts no shadow."));
                    break;
                case LightComponent::Type::Spot:
                    EditorTheme::Hint(T("The cone shines along the object's forward (-Z of its rotation). "
                                        "Its shadow is a separate map in the atlas: there are few, and they "
                                        "go to the brightest sources."));
                    break;
                case LightComponent::Type::Point:
                    EditorTheme::Hint(T("A point light at the object's position. Its shadow costs six passes "
                                        "of geometry (one per cube face), so it is best left only to "
                                        "the lamps that have something to occlude."));
                    break;
                default: break;
            }
            ifield::Color3(host, T("Colour"), &light->Color);
            sage::editor::ResetField(&host, *light, light->Color);
            ifield::Float(host, T("Intensity"), &light->Intensity, 0.02f, 0.0f, 10.0f);
            sage::editor::ResetField(&host, *light, light->Intensity);
            // Дальность и углы конуса у направленного света не значат ничего:
            // он светит из бесконечности. Показывать поля, которые ни на что не
            // влияют, — обманывать; поэтому их тут просто нет.
            const bool directional = light->Kind == LightComponent::Type::Directional;
            if (!directional) {
                ifield::Float(host, T("Range"), &light->Range, 0.1f, 0.5f, 100.0f);
                sage::editor::ResetField(&host, *light, light->Range);
            }
            if (light->Kind == LightComponent::Type::Spot) {
                ifield::Float(host, T("Inner Cone"), &light->InnerConeDeg, 0.5f, 1.0f, 89.0f);
                sage::editor::ResetField(&host, *light, light->InnerConeDeg);
                ifield::Float(host, T("Outer Cone"), &light->OuterConeDeg, 0.5f, 1.0f, 89.0f);
                sage::editor::ResetField(&host, *light, light->OuterConeDeg);
                // Внешний угол не должен быть уже внутреннего (иначе конус
                // «выворачивается»): подтягиваем внешний до внутреннего.
                if (light->OuterConeDeg < light->InnerConeDeg) light->OuterConeDeg = light->InnerConeDeg;
            }
            // Текстура света: прожектор проецирует картинку конусом, точечный —
            // панорамой вокруг себя (render/LightCookies.h).
            if (light->Kind == LightComponent::Type::Spot || light->Kind == LightComponent::Type::Point) {
                ImGui::TextUnformatted(T("Light texture"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", light->Kind == LightComponent::Type::Spot
                                                ? T("Picture projected by the cone: lantern pattern, grille, stained glass.\n"
                                                    "Brightness scales the light, colour tints it.")
                                                : T("Panorama around the lamp (equirectangular): the centre looks along -Z.\n"
                                                    "Brightness scales the light, colour tints it."));
                const assetslot::Result r = assetslot::Draw(host, "##light_cookie", assetslot::Kind::Texture,
                                                            light->Cookie, &m_preview, T("No texture: even light"));
                if (r.Changed) {
                    host.PushUndoSnapshot();
                    light->Cookie = r.Path;
                }
                if (r.BrowseRequested) {
                    FileBrowser::Config c;
                    c.Title = T("Light texture");
                    c.Filters = assetslot::Extensions(assetslot::Kind::Texture);
                    c.FilterLabel = T("Images");
                    c.StartDir = c.Root = assetslot::ProjectRoot(host);
                    m_browser.Open(c);
                    m_browseTarget = &light->Cookie;
                    m_browseIsShader = m_browseIsMesh = m_browseIsMaterial = false;
                }
            }
            const bool area = light->Kind == LightComponent::Type::Area;
            if (area) {
                ifield::Float(host, T("Width"), &light->AreaWidth, 0.02f, 0.05f, 50.0f);
                sage::editor::ResetField(&host, *light, light->AreaWidth);
                ifield::Float(host, T("Height"), &light->AreaHeight, 0.02f, 0.05f, 50.0f);
                sage::editor::ResetField(&host, *light, light->AreaHeight);
            } else {
                // Тени у площадного нет — и галочки нет: она бы ничего не делала.
                ifield::Check(host, T("Casts shadows"), &light->CastShadows);
            }
            if (directional) DrawSunSection(host, obj);
        }
    }

    // --- Тракт пост-обработки камеры ---
    DrawPostProcessSection(host, reg, obj.Entity(), &rm.PostProcess);
    DrawObjectEffectsSection(host, reg, obj.Entity(), &rm.ObjectEffects);

    // --- Наклейка (проекция картинки на геометрию сцены) ---
    if (reg.all_of<DecalComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader("texture", T("Decal" "###Decal"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Decal,
                                       T("Projection goes along -Z; Scale sets the size"))) {
        DecalComponent& dc = reg.get<DecalComponent>(obj.Entity());
        bool changed = false;
        changed |= ifield::Float(host, T("Angle Limit"), &dc.AngleLimitDeg, 1.0f, 1.0f, 89.0f, "%.0f°");
        changed |= ifield::Float(host, T("Surface Offset"), &dc.Offset, 0.001f, 0.0f, 0.5f, "%.3f");

        // ГОВОРИМ ТОЛЬКО О БЕДЕ. Число спроецированных треугольников — счётчик
        // для того, кто пишет рендер; человеку, который ставит наклейку, от
        // него ничего не нужно, пока наклейка видна. А вот ноль — это ответ на
        // «почему её нет», и он остаётся.
        //
        // Подсказки — с переносом по ширине панели. Без него текст просто
        // обрезается на границе: панель у людей узкая, и «размер задаёт Scal»
        // читается как поломка редактора, а не как совет.
        if (dc.Triangles <= 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::Color(EditorTheme::Role::Warn));
            ImGui::TextWrapped("%s", T("Landed on nothing: there is no geometry under the box, or it faces away from "
              "the decal."));
            ImGui::PopStyleColor();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

        ImGui::PopStyleColor();
        if (ImGui::Button(T("Rebuild##decal"))) changed = true;
        // Правка параметров обязана быть видна сразу: наклейка пересобирается
        // по флагу, и без него ползунок угла не менял бы вообще ничего.
        if (changed && reg.all_of<DecalComponent>(obj.Entity()))
            reg.get<DecalComponent>(obj.Entity()).Dirty = true;
    }

    // --- Скрипт (поведение в Play-режиме) ---
    // GI STATIC — объект участвует в запечённом освещении (Environment →
    // «Запечённое освещение»). Настроек две, и обе читает бейк: есть ли у
    // объекта своя лайтмапа (иначе он только заслоняет и отражает свет) и во
    // сколько раз она плотнее обычной.
    if (reg.all_of<GIStaticComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader("sun", T("GI Static" "###GIStatic"), ImGuiTreeNodeFlags_DefaultOpen,
                                   &rm.GiStatic, T("Static geometry for baked lighting"))) {
        GIStaticComponent& gs = reg.get<GIStaticComponent>(obj.Entity());
        ifield::Check(host, T("Lightmapped"), &gs.Lightmapped);
        EditorTheme::Hint(T("Off — the object only blocks and bounces light, without its own lightmap"));
        ImGui::BeginDisabled(!gs.Lightmapped);
        ifield::Slider(host, T("Texel Scale"), &gs.TexelScale, 0.25f, 4.0f, "%.2fx");
        EditorTheme::Hint(T("Lightmap density relative to the scene's: more is sharper and slower to bake"));
        ImGui::EndDisabled();
    }

    // У этого компонента НЕТ ПОЛЕЙ: он флаг, и вся его настройка — есть он или
    // нет. Раскрывающаяся секция с пустым телом выглядела бы сломанной, поэтому
    // он и не секция: строка состояния плюс кнопка «убрать» рядом.
    if (reg.all_of<NetReplicatedComponent>(obj.Entity())) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(T("Net Replicated"));
        EditorTheme::Hint(Hint2(T("The server sends this object to clients; they follow it"),
                                T("Position, rotation, scale, color and primitive are replicated"))
                              .c_str());
        ImGui::SameLine();
        if (ImGui::Button(T("Remove##netreplicated"))) rm.Net = true;
    }

    // Скрипты объекта, их группы и переменные — InspectorPanel_Script.cpp.
    DrawScriptSection(host, obj, reg, rm);

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
        // Переменные, заведённые ЧЕЛОВЕКОМ, а не скриптом: точка появления с
        // именем волны, зона с названием следующего уровня, кнопка с
        // аргументом события. Публичные переменные скрипта показываются в его
        // собственной секции выше — вместе с файлом, который их объявил.
        const bool hasVars = reg.all_of<VarsComponent>(obj.Entity());
        if (hasVars &&
            EditorTheme::SectionHeader("list", T("Variables" "###Variables"), ImGuiTreeNodeFlags_DefaultOpen)) {
            VarsComponent& vc = reg.get_or_emplace<VarsComponent>(obj.Entity());
            // Снимок для отмены берут сами поля (см. VarsEditor.cpp): у
            // ползунка он нужен ДО перетаскивания, а не после.
            varsui::DrawTable(host, obj, vc.Values, &m_preview);
        }
    }

    DrawPhysicsSections(host, obj, reg, rm);

    DrawAnimationSections(host, obj, reg, rm);

    DrawEffectSections(host, obj, reg, rm);
    DrawRenderTextureSection(host, obj, reg, &rm.RenderTexture);

    DrawAiSection(host, obj, reg, rm);
    DrawVisualScriptSection(host, obj, reg, rm);

    // ИНТЕРФЕЙС И ЭЛЕМЕНТ — РАЗНЫЕ РАЗДЕЛЫ, и это главное разделение этого
    // окна. Интерфейс — граница: что показывать, в каком порядке и под какой
    // экран верстать. Элемент — прямоугольник внутри него: цвет, текст,
    // отступы. Пока разделения не было, у объекта с интерфейсом в инспекторе
    // оказывались поля кнопки, и понять, чем одно отличается от другого, было
    // нечем.
    if (reg.all_of<sage::ui::InterfaceComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader("layout", T("Interface" "###Interface"),
                                   ImGuiTreeNodeFlags_DefaultOpen, &rm.Interface)) {
        sage::editor::DrawInterfaceProperties(host, obj);
    }

    if (reg.all_of<sage::ui::Element>(obj.Entity()) &&
        EditorTheme::SectionHeader("layout", T("UI Element" "###UI Element"), ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawUIElement(host, obj);
    }

    // --- СОБЫТИЯ: «On Click → Menu.start_game()» -----------------------------
    //
    // У ЛЮБОГО объекта, а не только у кнопки: сигналы объявляют компоненты
    // (кнопка — clicked, тело — collision), а своё событие (`obj:emit(...)`)
    // бывает у чего угодно. Раскрыта сразу, только если есть что показать:
    // пустая раскрытая секция у каждого камня — шум.
    {
        const bool any = reg.all_of<sage::signals::SignalLinksComponent>(obj.Entity()) ||
                         !sage::signals::Of(reg, obj.Entity()).empty();
        if (EditorTheme::SectionHeader("anim", T("Events" "###Events"),
                                       any ? ImGuiTreeNodeFlags_DefaultOpen : 0, nullptr,
                                       T("What happens when this object's events fire: call a method "
                                         "of a script"))) {
            sage::editor::DrawSignalLinks(host, obj);
        }
    }

    // --- Снятие компонентов: ОДНО место, в конце ----------------------------
    //
    // Не там, где нажали: кнопка стоит в заголовке, а под ней рисуются поля
    // того же компонента. Сняв его сразу, мы выдернули бы из-под них данные —
    // на том же кадре. Здесь же собраны и «хвосты» снятия: у анимации это
    // возврат статического меша, у звука — глушение источника.
    // МЕШ СНИМАЕТСЯ, КАК ЛЮБОЙ ДРУГОЙ КОМПОНЕНТ. Он был единственным без кнопки
    // «убрать»: добавить его через «Добавить компонент» было можно, а снять —
    // нельзя, и объект, которому меш достался по ошибке (например, брошенной
    // моделью), оставался с ним навсегда. Никакой особости у него нет: это
    // такое же свойство предмета, как свет или звук, и невидимый объект —
    // законное состояние (узел, точка привязки, держатель скрипта).
    if (rm.Mesh) {
        host.PushUndoSnapshot();
        reg.remove<MeshRendererComponent>(obj.Entity());
    }
    if (rm.Camera) {
        host.PushUndoSnapshot();
        reg.remove<CameraComponent>(obj.Entity());
    }

    if (rm.Light) {
        host.PushUndoSnapshot();
        reg.remove<LightComponent>(obj.Entity());
    }

    if (rm.PostProcess) {
        host.PushUndoSnapshot();
        reg.remove<sage::render::PostProcessComponent>(obj.Entity());
    }
    if (rm.ObjectEffects) {
        host.PushUndoSnapshot();
        reg.remove<sage::render::ObjectEffectsComponent>(obj.Entity());
    }

    if (rm.Interface) {
        host.PushUndoSnapshot();
        reg.remove<sage::ui::InterfaceComponent>(obj.Entity());
    }

    if (rm.Decal) {
        host.PushUndoSnapshot();
        reg.remove<DecalComponent>(obj.Entity());
    }

    if (rm.GiStatic) {
        host.PushUndoSnapshot();
        reg.remove<GIStaticComponent>(obj.Entity());
    }

    if (rm.Net) {
        host.PushUndoSnapshot();
        reg.remove<NetReplicatedComponent>(obj.Entity());
    }

    if (rm.Script) {
        host.PushUndoSnapshot();
        reg.remove<ScriptComponent>(obj.Entity());
    }

    if (rm.Body) {
        host.PushUndoSnapshot();
        reg.remove<RigidBodyComponent>(obj.Entity());
    }

    if (rm.Collider) {
        host.PushUndoSnapshot();
        reg.remove<ColliderComponent>(obj.Entity());
    }

    if (rm.Character) {
        host.PushUndoSnapshot();
        reg.remove<CharacterControllerComponent>(obj.Entity());
    }

    if (rm.Joint) {
        host.PushUndoSnapshot();
        reg.remove<JointComponent>(obj.Entity());
    }

    if (rm.Animation) {
        host.PushUndoSnapshot();
        reg.remove<AnimationComponent>(obj.Entity());
        // Статический меш анимированному объекту не грузится (см. загрузчик
        // сцены): рисует его скелетный проход. Сняли анимацию — рисовать
        // станет некому, и объект пропал бы из кадра до перезагрузки сцены.
        // Догружаем здесь же.
        if (MeshRendererComponent* mesh = reg.try_get<MeshRendererComponent>(obj.Entity())) {
            if (mesh->Ref.type == MeshRef::Type::Model && !mesh->Ref.path.empty() &&
                !mesh->MeshPtr) {
                mesh->MeshPtr = ResourceManager::Instance().GetModel(mesh->Ref.path, mesh->Ref.part);
            }
        }
    }

    if (rm.PropertyAnim) {
        host.PushUndoSnapshot();
        reg.remove<PropertyAnimatorComponent>(obj.Entity());
    }

    if (rm.Tween) {
        host.PushUndoSnapshot();
        reg.remove<sage::anim::TweenComponent>(obj.Entity());
    }

    if (rm.RenderTexture) {
        host.PushUndoSnapshot();
        reg.remove<RenderTextureComponent>(obj.Entity());
    }

    if (rm.Probe) {
        host.PushUndoSnapshot();
        reg.remove<ReflectionProbeComponent>(obj.Entity());
    }

    if (rm.Ik) {
        host.PushUndoSnapshot();
        reg.remove<IKComponent>(obj.Entity());
    }

    if (rm.Emitter) {
        host.PushUndoSnapshot();
        reg.remove<ParticleEmitterComponent>(obj.Entity());
    }

    if (rm.Ai) {
        host.PushUndoSnapshot();
        reg.remove<AiAgentComponent>(obj.Entity());
    }

    if (rm.VisualScript) {
        host.PushUndoSnapshot();
        reg.remove<VisualScriptComponent>(obj.Entity());
    }

    if (rm.Audio) {
        host.PushUndoSnapshot();
        // Глушим ДО снятия компонента: дескриптор звука живёт в нём, и после
        // remove остановить источник было бы нечем — водопад продолжал бы
        // шуметь из точки, где его уже нет.
        if (AudioSourceComponent* au = reg.try_get<AudioSourceComponent>(obj.Entity())) au->Stop();
        reg.remove<AudioSourceComponent>(obj.Entity());
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


// Префаб в инспекторе: та же вращаемая обложка, что у материала. Крупнее, чем в
// панели Assets, потому что здесь на неё и смотрят — выбирают, тот ли это ящик.
void InspectorPanel::DrawPrefabPreview(EditorHost& host) {
    const std::string path = host.InspectedAssetPath().string();
    const float side = std::min(ImGui::GetContentRegionAvail().x, 220.0f);
    const uint64_t tex = m_preview.RenderPrefab(path, (int)side);
    if (!tex) {
        ImGui::TextDisabled("%s", T("No cover: the prefab has no visible geometry"));
        ImGui::TextDisabled("%s", T("(or the file cannot be read — details in Console)."));
    } else {
        // Управление — ровно такое же, как у превью материала (см.
        // InspectorPanel_Material.cpp): невидимая кнопка поверх картинки держит
        // жест до отпускания, а колесо принадлежит тому, на что смотрят, а не
        // прокрутке панели.
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Image((ImTextureID)(std::intptr_t)tex, ImVec2(side, side), ImVec2(0, 1),
                     ImVec2(1, 0));
        ImGui::SetCursorScreenPos(at);
        ImGui::InvisibleButton("##prefab_preview_drag", ImVec2(side, side));
        if (ImGui::IsItemActive()) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            m_preview.Orbit(d.x * 0.5f, -d.y * 0.5f);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) m_preview.Zoom(wheel);
            ImGui::SetTooltip("%s", T("Drag to turn, wheel to zoom"));
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", T("Preview"));
        if (EditorIcons::IconOnlyButton("refresh", T("Reset the preview view")))
            m_preview.ResetView();
        ImGui::EndGroup();
    }

    ImGui::Spacing();
    if (ImGui::Button(T("Place in scene"), ImVec2(-1, 0))) host.InstantiatePrefab(path);
}
