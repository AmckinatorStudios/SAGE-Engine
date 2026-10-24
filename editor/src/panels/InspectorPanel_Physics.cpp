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
#include <cctype>
#include <cstdarg>
#include "sage/scene/CharacterFit.h"
#include "sage/scene/ScaleLock.h"
#include "EditorTheme.h"
#include "ui/UI.h"
#include "InspectorPanel.h"
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

#include "InspectorPanelDetail.h"

namespace fs = std::filesystem;
using namespace sage::editor::inspectordetail;

namespace sage::editor::inspectordetail {
// Слой — номер (1..32), а хранится битом: так его понимает маска лучей и зон,
// а человек думает «слой 3», а не «бит 4». Общий для тела и персонажа, иначе
// одно и то же поле в двух секциях рисовалось бы по-разному.
void LayerSlider(EditorHost& host, sage::physics::LayerMask& mask) {
    int layer = 1;
    for (int b = 0; b < 32; ++b)
        if (mask & (1u << b)) { layer = b + 1; break; }
    if (ImGui::SliderInt(T("Layer"), &layer, 1, 32)) {
        host.PushUndoSnapshot();
        mask = 1u << (layer - 1);
    }
}

// Маска слоёв списком галочек: «все», «ничего» или сколько отмечено.
void LayerMaskCombo(EditorHost& host, const char* label, sage::physics::LayerMask& mask) {
    char preview[32];
    if (mask == sage::physics::kAllLayers) std::snprintf(preview, sizeof(preview), "%s", T("All layers"));
    else if (mask == 0) std::snprintf(preview, sizeof(preview), "%s", T("Nothing"));
    else {
        int n = 0;
        for (int b = 0; b < 32; ++b) n += (mask >> b) & 1u;
        std::snprintf(preview, sizeof(preview), T("%d layers"), n);
    }
    if (ImGui::BeginCombo(label, preview)) {
        if (ImGui::Selectable(T("All layers"), mask == sage::physics::kAllLayers)) {
            host.PushUndoSnapshot();
            mask = sage::physics::kAllLayers;
        }
        for (int b = 0; b < 32; ++b) {
            char item[32];
            std::snprintf(item, sizeof(item), T("Layer %d"), b + 1);
            bool on = (mask >> b) & 1u;
            ImGui::PushID(b);
            if (ImGui::Checkbox(item, &on)) {
                host.PushUndoSnapshot();
                if (on) mask |= 1u << b;
                else mask &= ~(1u << b);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}
} // namespace sage::editor::inspectordetail

// Секции физики объекта: твёрдое тело, коллайдер, персонаж, соединение.
void InspectorPanel::DrawPhysicsSections(EditorHost& host, GameObject obj, entt::registry& reg,
                                   EntityRemovals& rm) {
    // --- Твёрдое тело (симулируется в Play-режиме выбранным бэкендом физики) ---
    if (reg.all_of<RigidBodyComponent>(obj.Entity()) && EditorTheme::SectionHeader("physics", T("Rigid Body" "###Rigid Body"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Body,
                                       T("Dynamic falls under gravity; Static/Kinematic don't"))) {
        if (RigidBodyComponent* rb = reg.try_get<RigidBodyComponent>(obj.Entity())) {
            // Порядок строго совпадает с sage::physics::BodyType.
            const char* types[] = {T("Static"), T("Dynamic"), T("Kinematic")};
            int kind = (int)rb->Type;
            if (ImGui::Combo(T("Body Type"), &kind, types, IM_ARRAYSIZE(types))) {
                host.PushUndoSnapshot();
                rb->Type = (sage::physics::BodyType)kind;
            }
            const bool dynamic = rb->Type == sage::physics::BodyType::Dynamic;
            // Масса — только у падающего тела: статике и кинематике она ни к
            // чему, и поле, которое ни на что не влияет, только путает.
            if (dynamic) {
                // Логарифмическая шкала: и граммы, и тонны в одном поле без
                // ввода с клавиатуры.
                ImGui::DragFloat(T("Mass"), &rb->Mass, 0.05f, 0.001f, 100000.0f, "%.3f",
                                 ImGuiSliderFlags_Logarithmic);
                host.TrackLastImGuiItem();
            }

            // Как тело движется прямо сейчас — в игре видно, крутится ли оно и
            // с какой скоростью летит, без отладочного вывода из скрипта.
            if (PhysicsScene* physics = host.PlayPhysics(); physics && dynamic &&
                                                            rb->RuntimeBody != sage::physics::kInvalidBody) {
                const glm::vec3 v = physics->GetLinearVelocity(rb->RuntimeBody);
                const glm::vec3 w = physics->GetAngularVelocity(rb->RuntimeBody);
                ImGui::TextDisabled(T("Speed %.2f m/s, spin %.2f rad/s%s"), glm::length(v), glm::length(w),
                                    physics->IsSleeping(rb->RuntimeBody) ? T(", asleep") : "");
            }

            // Трение, упругость, слой и тонкая настройка движения — нужны
            // реже, чем тип и масса.
            if (EditorTheme::BeginMore("rbMore")) {
            ImGui::DragFloat(T("Friction"), &rb->Friction, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Restitution"), &rb->Restitution, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Bounciness: 0 lands dead, 1 bounces back to the same height"));

            if (dynamic) {
                ImGui::DragFloat(T("Linear Damping"), &rb->LinearDamping, 0.005f, 0.0f, 1.0f);
                host.TrackLastImGuiItem();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", T("Air or water drag: share of speed lost per second"));
                ImGui::DragFloat(T("Angular Damping"), &rb->AngularDamping, 0.005f, 0.0f, 1.0f);
                host.TrackLastImGuiItem();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", T("Slows spinning and rolling: raise it so balls stop sooner"));
                ImGui::DragFloat(T("Gravity Scale"), &rb->GravityScale, 0.01f, -10.0f, 10.0f);
                host.TrackLastImGuiItem();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", T("0 floats, 1 is normal weight, 2 falls twice as hard"));
                bool ccd = rb->Continuous;
                if (ImGui::Checkbox(T("Continuous Collision"), &ccd)) {
                    host.PushUndoSnapshot();
                    rb->Continuous = ccd;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", T("Checks the whole path each step: fast objects neither pass through walls nor sink into the floor"));

                // Заморозка осей — одной строкой из трёх галочек на группу:
                // «не опрокидывается» и «игра в плоскости» так делаются за
                // клик, без скрипта, который гасил бы скорость каждый кадр.
                auto lockRow = [&](const char* label, uint8_t firstBit) {
                    ImGui::PushID(label);
                    ImGui::TextUnformatted(label);
                    const char* axes[] = {"X", "Y", "Z"};
                    for (int i = 0; i < 3; ++i) {
                        ImGui::SameLine();
                        const uint8_t bit = (uint8_t)(firstBit << i);
                        bool on = (rb->Locks & bit) != 0;
                        if (ImGui::Checkbox(axes[i], &on)) {
                            host.PushUndoSnapshot();
                            rb->Locks = on ? (uint8_t)(rb->Locks | bit) : (uint8_t)(rb->Locks & ~bit);
                        }
                    }
                    ImGui::PopID();
                };
                lockRow(T("Freeze Position"), sage::physics::kLockPosX);
                lockRow(T("Freeze Rotation"), sage::physics::kLockRotX);
            }

            LayerSlider(host, rb->Layer);
            EditorTheme::EndMore();
            }

            // Триггер: пропускает сквозь себя и сообщает скриптам
            // OnTriggerEnter/Stay/Exit. Раньше включался только из скрипта —
            // в редакторе зону было не сделать вовсе.
            bool sensor = rb->Sensor;
            if (ImGui::Checkbox(T("Is Trigger"), &sensor)) {
                host.PushUndoSnapshot();
                rb->Sensor = sensor;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Objects pass through; scripts get OnTriggerEnter / Stay / Exit"));
            if (rb->Sensor) {
                LayerMaskCombo(host, T("Detects Layers"), rb->TriggerMask);
                // Кто внутри прямо сейчас — в игре видно, почему дверь не
                // открылась: гость не дошёл, или его слой зона не замечает.
                if (PhysicsScene* physics = host.PlayPhysics()) {
                    std::vector<entt::entity> inside;
                    physics->ObjectsInTrigger(obj.Entity(), inside);
                    ImGui::TextDisabled(T("Inside now: %d"), (int)inside.size());
                    for (entt::entity e : inside) {
                        GameObject g(&reg, e);
                        if (g.Valid()) ImGui::BulletText("%s", g.Name().c_str());
                    }
                }
            }
        }
    }

    // --- Коллайдер (форма для физики; размеры домножаются на Transform.Scale) ---
    if (reg.all_of<ColliderComponent>(obj.Entity()) && EditorTheme::SectionHeader("physics", T("Collider" "###Collider"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Collider,
                                       Hint2(T("Sizes are scaled by the entity's Transform scale"),
                                       T("Parts override the single shape above")).c_str())) {
        if (ColliderComponent* col = reg.try_get<ColliderComponent>(obj.Entity())) {
            // Порядок строго совпадает с sage::physics::ShapeType.
            const char* shapes[] = {T("Box"), T("Sphere"), T("Capsule"), T("Convex Hull"), T("Mesh")};
            int shape = (int)col->Shape;
            if (ImGui::Combo(T("Shape"), &shape, shapes, IM_ARRAYSIZE(shapes))) {
                host.PushUndoSnapshot();
                col->Shape = (sage::physics::ShapeType)shape;
            }
            if (col->Shape == sage::physics::ShapeType::ConvexHull ||
                col->Shape == sage::physics::ShapeType::Mesh) {
                // Форма — из модели. Пустой слот означает меш самого объекта:
                // коллайдер «по тому, что нарисовано» без единой настройки.
                const assetslot::Result slot =
                    assetslot::Draw(host, "colmesh", assetslot::Kind::Model, col->MeshPath, &m_preview,
                                    T("Uses this object's own mesh"));
                if (slot.Changed) {
                    host.PushUndoSnapshot();
                    col->MeshPath = slot.Path;
                }
                if (slot.BrowseRequested) {
                    FileBrowser::Config c;
                    c.Title = T("Choose a collision mesh");
                    c.Filters = assetslot::Extensions(assetslot::Kind::Model);
                    c.FilterLabel = T("Models");
                    // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
                    c.StartDir = c.Root = assetslot::ProjectRoot(host);
                    m_browser.Open(c);
                    m_browseTarget = &col->MeshPath;
                    m_browseIsShader = false;
                    m_browseIsMesh = false;
                    m_browseIsMaterial = false;
                }
                // Сетка треугольников у падающего тела невозможна (нет объёма и
                // массы) — говорим, что будет вместо неё, а не молчим.
                const RigidBodyComponent* body = reg.try_get<RigidBodyComponent>(obj.Entity());
                if (col->Shape == sage::physics::ShapeType::Mesh && body &&
                    body->Type == sage::physics::BodyType::Dynamic)
                    ImGui::TextDisabled("%s", T("A falling body uses the convex hull of the mesh"));
                else if (col->Shape == sage::physics::ShapeType::Mesh)
                    ImGui::TextDisabled("%s", T("Exact triangles: for floors, walls and terrain"));
                else
                    ImGui::TextDisabled("%s", T("Convex wrap of the mesh: works for falling bodies"));
            } else if (col->Shape == sage::physics::ShapeType::Box) {
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


            // --- Составная (compound) форма: список дочерних примитивов ---
            //
            // Нужна редко (тележка из коробки и четырёх колёс), поэтому — под
            // «Ещё»; но если части уже есть, группа раскрыта: их надо видеть.
            if (!col->Parts.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            if (EditorTheme::BeginMore("colliderParts")) {
            ImGui::Text(T("Compound parts: %d"), (int)col->Parts.size());

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
                    if (ImGui::Button(T("Remove Part"))) removePart = pi;
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
            EditorTheme::EndMore();
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
        EditorTheme::SectionHeader("physics", T("Character Controller" "###Character Controller"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Character)) {
        if (CharacterControllerComponent* ch =
                reg.try_get<CharacterControllerComponent>(obj.Entity())) {
            ImGui::DragFloat(T("Radius"), &ch->Radius, 0.01f, 0.05f, 5.0f);
            host.TrackLastImGuiItem();
            ImGui::DragFloat(T("Height"), &ch->Height, 0.02f, 0.1f, 10.0f);
            host.TrackLastImGuiItem();
            ImGui::DragFloat3(T("Center"), &ch->Center.x, 0.01f);
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("Body offset from the object origin"));
            }

            // Столкновения — на виду, а не в «Ещё»: с чем персонаж сталкивается,
            // спрашивают первым делом («почему он проходит сквозь стену?»).
            ImGui::SeparatorText(T("Collision"));
            LayerSlider(host, ch->Layer);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("Layer of the character itself: rays, triggers and other masks see it here"));
            }
            LayerMaskCombo(host, T("Collides With"), ch->CollidesWith);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("Layers the character bumps into; the rest it walks through"));
            }
            bool events = ch->CollisionEvents;
            if (ImGui::Checkbox(T("Collision Events"), &events)) {
                host.PushUndoSnapshot();
                ch->CollisionEvents = events;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("Scripts get OnCollisionEnter / Exit when the character touches something"));
            }
            // Кого касается прямо сейчас — в игре видно, почему событие не
            // пришло: касания нет, или слой не тот.
            if (host.PlayPhysics()) {
                ImGui::TextDisabled(T("Touching now: %d"), (int)ch->Touching.size());
            }
            if (EditorTheme::BeginMore("charMore")) {
            ImGui::DragFloat(T("Slope Limit"), &ch->SlopeLimit, 0.5f, 0.0f, 89.0f, "%.0f°");
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Steeper than this and the character slides"));
            ImGui::DragFloat(T("Step Offset"), &ch->StepOffset, 0.01f, 0.0f, 2.0f);
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("How tall a step the character climbs without jumping"));
            }
            ImGui::DragFloat(T("Skin Width"), &ch->SkinWidth, 0.001f, 0.0f, 0.2f, "%.3f");
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("Contact gap that keeps the character from jittering on seams"));
            }
            ImGui::DragFloat(T("Gravity"), &ch->Gravity, 0.1f, -100.0f, 100.0f);
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("Own gravity of this character; 0 means it does not fall"));
            }
            ImGui::DragFloat(T("Mass"), &ch->Mass, 0.5f, 0.1f, 1000.0f);
            host.TrackLastImGuiItem();
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", T("How hard the character pushes dynamic bodies"));
            }
            EditorTheme::EndMore();
            }
        }
    }

    // --- Соединение (constraint/joint) с другим телом или миром ---
    if (reg.all_of<JointComponent>(obj.Entity()) && EditorTheme::SectionHeader("ik", T("Joint" "###Joint"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Joint,
                                       T("Needs a Rigid Body; only the Jolt backend simulates joints"))) {
        if (JointComponent* jc = reg.try_get<JointComponent>(obj.Entity())) {
            const char* types[] = {T("Fixed"), T("Point"), T("Hinge"), T("Slider"),
                                   T("Distance"), T("Cone")};
            int t = (int)jc->Type;
            if (ImGui::Combo(T("Type"), &t, types, IM_ARRAYSIZE(types))) {
                host.PushUndoSnapshot();
                jc->Type = (sage::physics::JointType)t;
            }
            // Второе тело — СЛОТОМ ОБЪЕКТА, а не номером: номер сущности
            // человек не видит нигде и сверить его ему не с чем.
            ImGui::TextUnformatted(T("Connected to"));
            objectslot::Options target;
            target.AllowWorld = true;
            target.WorldId = -1;
            target.SelfId = obj.Id();
            target.Accept = [](const entt::registry& r, entt::entity e) {
                return r.all_of<RigidBodyComponent>(e);
            };
            target.AcceptHint = "Only an object with a Rigid Body can hold a joint";
            const objectslot::Result tr = objectslot::Draw(host, "jointTarget", jc->TargetId, target);
            if (tr.Changed) {
                host.PushUndoSnapshot();
                // Пусто у сустава — это «к миру»: висеть ни на чём он не умеет.
                jc->TargetId = tr.Id > 0 ? tr.Id : -1;
            }
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

        }
    }
}
