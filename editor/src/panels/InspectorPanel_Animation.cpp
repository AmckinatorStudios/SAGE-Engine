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
#include "InspectorFields.h"
#include "InspectorControls.h"
#include <cctype>
#include <cstdarg>
#include "sage/scene/CharacterFit.h"
#include "sage/scene/ScaleLock.h"
#include "EditorTheme.h"
#include "ui/UI.h"
#include "InspectorPanel.h"
#include "../SignalLinksEditor.h"
#include "../UIElementProperties.h"
#include "../TweenUi.h"

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

namespace ifield = sage::editor::ifield;

namespace fs = std::filesystem;
namespace ictl = sage::editor::ictl;

using namespace sage::editor::inspectordetail;

// Секции анимации объекта: скелет, анимация свойств, твины, зонд отражений, IK.
void InspectorPanel::DrawAnimationSections(EditorHost& host, GameObject obj, entt::registry& reg,
                                   EntityRemovals& rm) {
    // --- Анимация: клип поверх скелета из Mesh -----------------------------
    //
    // Модели здесь больше нет: она задаётся в Mesh, как у любого другого
    // объекта. Секция отвечает только на вопрос «как это движется», и первое,
    // что она обязана сказать, — ЕСТЬ ЛИ ЧТО анимировать: компонент,
    // добавленный к кубу, обязан объяснить, почему ничего не происходит.
    if (reg.all_of<AnimationComponent>(obj.Entity()) && EditorTheme::SectionHeader("anim", T("Animation" "###Animation"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Animation,
                                       T("Skeletal animation: a clip or a graph plays on the skeleton of the Mesh model"))) {
        if (AnimationComponent* am = reg.try_get<AnimationComponent>(obj.Entity())) {
            const MeshRendererComponent* mesh = reg.try_get<MeshRendererComponent>(obj.Entity());
            const bool hasModelRef = mesh && mesh->Ref.type == MeshRef::Type::Model &&
                                     !mesh->Ref.path.empty();
            if (!hasModelRef) {
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                                   T("No model in Mesh — there is nothing to animate"));
                ImGui::TextDisabled("%s", T("Put a model with a skeleton into Mesh (.glb/.gltf/.fbx)."));
            }
            if (hasModelRef && am->Ready && !am->Model) {
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                                   T("This model has no skeleton — nothing to animate"));
            }

            // ИСТОЧНИК ПОЗЫ — КЛИП ИЛИ ГРАФ, и выбран всегда один. Раньше оба
            // слота висели рядом, и, назначив оба, человек не знал, какой
            // играет (играл граф). Переключение снимает другой источник —
            // одним шагом отмены.
            const bool graphMode = !am->GraphPath.empty() || m_animUseGraph[obj.Id()];
            ictl::GroupItem modes[2];
            modes[0].Text = T("Clip");
            modes[0].Tooltip = T("One clip plays on the skeleton");
            modes[0].Active = !graphMode;
            modes[1].Text = T("Graph");
            modes[1].Tooltip = T("An animation graph blends several clips by its parameters");
            modes[1].Active = graphMode;
            if (const int hit = ictl::ButtonGroup("##anim_source", modes, 2, ictl::ButtonLook::Standard); hit >= 0) {
                if (hit == 0 && graphMode) {
                    if (!am->GraphPath.empty()) host.PushUndoSnapshot();
                    am->GraphPath.clear();
                    m_animUseGraph[obj.Id()] = false;
                } else if (hit == 1 && !graphMode) {
                    if (!am->ClipPath.empty()) host.PushUndoSnapshot();
                    am->ClipPath.clear();
                    m_animUseGraph[obj.Id()] = true;
                }
            }

            const int objId = obj.Id();
            if (graphMode) {
                assetslot::Actions graphActions;
                graphActions.SuggestedName = obj.Name() + "Graph";
                graphActions.Edit = [&host, objId, am] { host.EditAnimation(objId, am->GraphPath); };
                const assetslot::Result graphSlot =
                    assetslot::Draw(host, "animgraph", assetslot::Kind::AnimGraph, am->GraphPath, &m_preview,
                                    T("No graph: create one or drop a .sageanimgraph"), &graphActions);
                if (graphSlot.Changed) {
                    host.PushUndoSnapshot();
                    am->GraphPath = graphSlot.Path;
                    if (!graphSlot.Path.empty()) am->ClipPath.clear();
                }
                if (graphSlot.BrowseRequested) {
                    FileBrowser::Config c;
                    c.Title = T("Choose an animation graph");
                    c.Filters = assetslot::Extensions(assetslot::Kind::AnimGraph);
                    c.FilterLabel = T("Animation graph");
                    c.StartDir = c.Root = assetslot::ProjectRoot(host);
                    m_browser.Open(c);
                    m_browseTarget = &am->GraphPath;
                    m_browseIsShader = m_browseIsMesh = m_browseIsMaterial = false;
                }
                if (!am->GraphPath.empty()) {
                    if (EditorIcons::Button("wire", T("Edit Graph"), T("Opens the graph in the Animation workspace")))
                        host.EditAnimation(objId, am->GraphPath);
                    if (am->Model && !am->Graph.Graph())
                        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                                           T("The graph could not be read (see the console)"));
                    for (const std::string& problem : am->Graph.Problems())
                        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s", problem.c_str());
                } else {
                    ImGui::TextDisabled("%s", T("Create a graph with + in the slot, or drop one from Assets."));
                }
            } else {
                // КЛИП ФАЙЛОМ; пусто — клип из самой модели по номеру (сцены до
                // появления файлов клипов, модели с невынутыми клипами).
                assetslot::Actions clipActions;
                clipActions.Create = [&host, objId] { host.CreateAnimationClip(objId, true); };
                clipActions.Edit = [&host, objId, am] { host.EditAnimation(objId, am->ClipPath); };
                const assetslot::Result clipSlot =
                    assetslot::Draw(host, "animclip", assetslot::Kind::Animation, am->ClipPath, &m_preview,
                                    T("Playing a clip from the model itself"), &clipActions);
                if (clipSlot.Changed) {
                    host.PushUndoSnapshot();
                    am->ClipPath = clipSlot.Path;
                }
                if (clipSlot.BrowseRequested) {
                    FileBrowser::Config c;
                    c.Title = T("Choose an animation clip");
                    c.Filters = assetslot::Extensions(assetslot::Kind::Animation);
                    c.FilterLabel = T("Animation");
                    c.StartDir = c.Root = assetslot::ProjectRoot(host);
                    m_browser.Open(c);
                    m_browseTarget = &am->ClipPath;
                    m_browseIsShader = m_browseIsMesh = m_browseIsMaterial = false;
                }
                if (!am->ClipPath.empty() && am->MissingBones > 0) {
                    ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn),
                                       T("%d bones of the clip are missing from this skeleton"),
                                       am->MissingBones);
                }
                // Править — назначенный клип; создать — свой клип для этого
                // скелета, сразу назначенный и открытый.
                if (!am->ClipPath.empty()) {
                    if (EditorIcons::Button("anim", T("Edit Clip"), T("Opens the clip in the Animation workspace")))
                        host.EditAnimation(objId, am->ClipPath);
                } else {
                    if (EditorIcons::Button("plus", T("Create Clip"),
                                            T("A new editable clip for this skeleton: assigned and opened")))
                        host.CreateAnimationClip(objId, true);
                    // Клипы, встроенные в модель: выбор по имени и вынос в файлы.
                    const int clipCount = am->Anim.ClipCount();
                    if (clipCount > 0) {
                        if (am->Clip >= clipCount) am->Clip = 0;
                        std::string preview = am->Anim.ClipName(am->Clip);
                        if (ictl::BeginDropdown(T("Model clip"), preview.c_str())) {
                            for (int i = 0; i < clipCount; ++i) {
                                if (ImGui::Selectable(am->Anim.ClipName(i).c_str(), am->Clip == i) && i != am->Clip) {
                                    host.PushUndoSnapshot();
                                    am->Clip = i;
                                }
                            }
                            ictl::EndDropdown();
                        }
                    } else if (am->Model) {
                        ImGui::TextDisabled("%s", T("No animation clips (bind pose)"));
                    }
                    if (hasModelRef && clipCount > 0 &&
                        EditorIcons::Button("import", T("Extract clips to files"),
                                            T("Writes every clip of the model next to it\n"
                                              "as .sageanim (read only). Existing files are kept."))) {
                        const ClipImportResult r = ImportModelClips(host.CurrentProject(), mesh->Ref.path);
                        if (r.Found == 0) {
                            host.SetStatusMessage(T("The model has no animation clips"));
                        } else {
                            host.SetStatusMessage(T("Clips: ") + std::to_string(r.Found) +
                                                  T(", files written: ") + std::to_string(r.Created));
                            if (am->ClipPath.empty() && !r.Refs.empty()) {
                                host.PushUndoSnapshot();
                                am->ClipPath = r.Refs.front();
                            }
                        }
                    }
                }
            }
            // ЗАПУСК — свойство объекта: начинает ли он играть сам. Остальное —
            // редкие правки экземпляра — в Advanced: скорость и повтор задаёт
            // ресурс, а в игре ими управляет скрипт (animator:SetSpeed…).
            ifield::Check(host, T("Play On Start"), &am->Playing);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Starts playing when the game starts. Scripts can play and stop it any time."));
            if (ictl::SubSection(("anim_adv_" + std::to_string(objId)).c_str(), T("Advanced"), false)) {
                ifield::Float(host, T("Speed"), &am->Speed, 0.02f, 0.0f, 8.0f);
                if (ifield::Check(host, T("Loop"), &am->Loop)) am->Anim.Play(am->Clip, am->Loop);
                ifield::Float(host, T("Blend Time"), &am->BlendTime, 0.01f, 0.0f, 2.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", T("Cross-fade when the clip of this object changes"));
                ifield::Check(host, T("Root Motion"), &am->RootMotion);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", T("The root bone moves the object instead of the model"));
                if (!am->Layers.empty())
                    ImGui::TextDisabled(T("Layers: %d (edit in the Animation workspace)"), (int)am->Layers.size());
                if (ImGui::Button(T("Reload"))) { am->Ready = false; am->Model = nullptr; }
                ictl::EndSubSection();
            }
        }
    }

    // --- Анимация свойств: клип .sageclip на этом объекте -------------------
    //
    // ОТДЕЛЬНО ОТ «Animation», И ЭТО НЕ ДУБЛИРОВАНИЕ. Тот компонент играет
    // движение КОСТЕЙ модели, этот — движение СВОЙСТВ объекта и его детей
    // (положение, цвет, прозрачность, размер). Создать и править клип можно
    // прямо отсюда: не надо сначала идти в Animation и искать кнопку.
    if (reg.all_of<PropertyAnimatorComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader("clock", T("Property Animation" "###Property Animation"),
                                   ImGuiTreeNodeFlags_DefaultOpen, &rm.PropertyAnim,
                                   T("Plays a .sageclip: properties of this object and its children"))) {
        if (PropertyAnimatorComponent* pa = reg.try_get<PropertyAnimatorComponent>(obj.Entity())) {
            const int objId = obj.Id();
            assetslot::Actions clipActions;
            clipActions.Create = [&host, objId] { host.CreateAnimationClip(objId, false); };
            clipActions.Edit = [&host, objId, pa] { host.EditAnimation(objId, pa->ClipPath); };
            const assetslot::Result clipSlot =
                assetslot::Draw(host, "propclip", assetslot::Kind::Animation, pa->ClipPath,
                                &m_preview, T("No clip — nothing is playing"), &clipActions);
            if (clipSlot.Changed) {
                host.PushUndoSnapshot();
                pa->ClipPath = clipSlot.Path;
                pa->Clip.reset();
                pa->Ready = false;
            }
            if (clipSlot.BrowseRequested) {
                FileBrowser::Config c;
                c.Title = T("Choose an animation clip");
                c.Filters = assetslot::Extensions(assetslot::Kind::Animation);
                c.FilterLabel = T("Animation");
                c.StartDir = c.Root = assetslot::ProjectRoot(host);
                m_browser.Open(c);
                m_browseTarget = &pa->ClipPath;
                m_browseIsShader = m_browseIsMesh = m_browseIsMaterial = false;
            }
            if (pa->ClipPath.empty()) {
                if (EditorIcons::Button("plus", T("Create Clip"),
                                        T("Writes a .sageclip into Assets, assigns it here and opens it")))
                    host.CreateAnimationClip(objId, false);
            } else if (EditorIcons::Button("anim", T("Edit"), T("Opens this clip in the Animation workspace"))) {
                host.EditAnimation(objId, pa->ClipPath);
            }
            ifield::Check(host, T("Play On Start"), &pa->Playing);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", T("Starts playing when the game starts. Scripts can play and stop it any time."));
            if (ictl::SubSection(("propanim_adv_" + std::to_string(objId)).c_str(), T("Advanced"), false)) {
                // Повтор — свойство клипа; здесь его можно перебить для этого
                // объекта (одно движение — и петля фона, и разовый отклик).
                ifield::Check(host, T("Override Loop"), &pa->LoopOverride);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", T("Off: the clip decides whether it loops. On: this object decides."));
                if (pa->LoopOverride) ifield::Check(host, T("Loop"), &pa->Loop);
                ifield::Float(host, T("Speed"), &pa->Speed, 0.02f, 0.0f, 8.0f);
                ictl::EndSubSection();
            }
        }
    }

    // --- Твины: только сводка -------------------------------------------------
    //
    // ОДНО МЕСТО НА НАСТРОЙКУ. Значения, время и кривые правятся в окне Tween;
    // здесь — что за твин, сколько в нём и две кнопки. Прежде инспектор
    // повторял длительность, кривую и повтор из окна, и две копии одной
    // настройки расходились в голове быстрее, чем в данных.
    if (reg.all_of<sage::anim::TweenComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader("clock", T("Tween" "###Tween"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Tween,
                                   T("Quick changes of properties over time — no clip needed"))) {
        auto& tc = reg.get<sage::anim::TweenComponent>(obj.Entity());
        const float btn = ImGui::GetFrameHeight();
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        for (size_t i = 0; i < tc.Tweens.size(); ++i) {
            const sage::anim::TweenClip& c = tc.Tweens[i];
            const bool empty = c.Tracks.empty();
            ImGui::PushID((int)i);
            const float lineRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(c.Name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tweenui::Summary(c).c_str());
            const float buttons = empty ? btn : btn * 2.0f + gap;
            ImGui::SameLine(std::max(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + gap, lineRight - buttons));
            if (empty) {
                if (EditorIcons::IconOnlyButton("plus", T("Add property")))
                    host.OpenTween(obj.Id(), (int)i, EditorHost::TweenOpen::AddProperty);
            } else {
                ImGui::BeginDisabled(host.InPlayMode());
                if (EditorIcons::IconOnlyButton("play", T("Play")))
                    host.OpenTween(obj.Id(), (int)i, EditorHost::TweenOpen::Play);
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (EditorIcons::IconOnlyButton("pencil", T("Edit in the Tween window")))
                    host.OpenTween(obj.Id(), (int)i, EditorHost::TweenOpen::Edit);
            }
            ImGui::PopID();
        }
        if (EditorIcons::Button("plus", T("Add tween"))) {
            sage::anim::TweenClip c;
            c.Name = "Tween " + std::to_string(tc.Tweens.size() + 1);
            tc.Tweens.push_back(c);
            host.PushUndoSnapshot();
            host.OpenTween(obj.Id(), (int)tc.Tweens.size() - 1, EditorHost::TweenOpen::AddProperty);
        }
    }

    // --- Зонд отражений -----------------------------------------------------
    if (reg.all_of<ReflectionProbeComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader("probe", T("Reflection Probe" "###Reflection Probe"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Probe,
                                       Hint2(T("Captures the scene around this point into a cubemap"),
                                       T("Realtime = 6 scene passes per frame - use sparingly")).c_str())) {
        if (ReflectionProbeComponent* p = reg.try_get<ReflectionProbeComponent>(obj.Entity())) {

            // Любая правка охвата или разрешения означает «снять заново»: карта
            // снята под прежние числа, и оставить её значило бы показывать
            // отражение, которого в сцене уже нет.
            const int prevRes = p->Resolution;
            const char* resNames[] = {"32", "64", "128", "256"};
            const int resValues[] = {32, 64, 128, 256};
            int resIdx = 2;
            for (int i = 0; i < 4; ++i) if (resValues[i] == p->Resolution) resIdx = i;
            if (ifield::Combo(host, T("Resolution"), &resIdx, resNames, 4)) p->Resolution = resValues[resIdx];
            if (p->Resolution != prevRes) p->Dirty = true;

            if (ictl::DragVec(&host, T("Box Half Extents"), &p->BoxHalfExtents.x, 3, 0.1f, 0.1f, 500.0f))
                p->Dirty = true;
            host.TrackLastImGuiItem();

            ifield::Slider(host, T("Intensity"), &p->Intensity, 0.0f, 3.0f);
            if (EditorTheme::BeginMore("probeMore")) {
                if (ifield::Check(host, T("Box Parallax"), &p->BoxParallax)) p->Dirty = true;
                if (ifield::Float(host, T("Far Clip"), &p->FarClip, 0.5f, 1.0f, 2000.0f)) p->Dirty = true;
                ifield::Check(host, T("Realtime (re-capture every frame)"), &p->Realtime);
                EditorTheme::EndMore();
            }


            if (ImGui::Button(T("Bake Probe"))) p->Dirty = true;
        }
    }

    // --- Обратная кинематика: цели поверх позы клипа ------------------------
    // Кость задаётся ИМЕНЕМ, а не индексом: модель может смениться (или ещё не
    // загрузиться), а имя переживает и то, и другое. Список имён показываем
    // только когда скелет уже есть.
    if (reg.all_of<IKComponent>(obj.Entity()) && EditorTheme::SectionHeader("ik", T("IK" "###IK"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Ik)) {
        if (IKComponent* ik = reg.try_get<IKComponent>(obj.Entity())) {
            ifield::Check(host, T("IK Enabled"), &ik->Enabled);
            const AnimationComponent* am = reg.try_get<AnimationComponent>(obj.Entity());
            if (!am) ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn),
                                        "%s", T("No Animation component - goals do nothing"));

            int remove = -1;
            for (int gi = 0; gi < (int)ik->Goals.size(); ++gi) {
                IKGoal& g = ik->Goals[(size_t)gi];
                ImGui::PushID(gi);
                const std::string title = "Goal " + std::to_string(gi) +
                                          (g.Bone.empty() ? "" : " (" + g.Bone + ")");
                if (ictl::SubSection(("ik_goal_" + std::to_string(gi)).c_str(), title.c_str(), true)) {
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
                        if (sk.Count() > 0 && ictl::BeginDropdown("Pick Bone", g.Bone.c_str())) {
                            for (int b = 0; b < sk.Count(); ++b) {
                                const std::string& n = sk.Joints[(size_t)b].Name;
                                if (ImGui::Selectable(n.c_str(), n == g.Bone) && n != g.Bone) {
                                    g.Bone = n;
                                    g.Resolved = false;
                                }
                            }
                            ictl::EndDropdown();
                        }
                    }
                    ifield::Check(host, T("Enabled"), &g.Enabled);
                    ImGui::SameLine();
                    if (ifield::Check(host, T("Aim (look-at)"), &g.Aim)) g.Resolved = false;
                    if (g.Aim) {
                        ictl::DragVec(&host, T("Aim Axis"), &g.AimAxis.x, 3, 0.01f);
                        host.TrackLastImGuiItem();
                        ifield::Slider(host, T("Max Angle"), &g.AimMaxAngle, 0.0f, 180.0f);
                    } else {
                        if (ifield::Int(host, T("Chain Length"), &g.ChainLength, 2, 8)) g.Resolved = false;
                        EditorTheme::Hint(T("2 = analytic two-bone, more = FABRIK"));
                        // Полюс, выравнивание стопы и её фиксация — тонкая
                        // настройка ног; цель и вес — выше и ниже, на виду.
                        if (EditorTheme::BeginMore("ikMore")) {
                        ifield::Check(host, T("Use Pole"), &g.UsePole);
                        if (g.UsePole) {
                            ictl::DragVec(&host, T("Pole"), &g.Pole.x, 3, 0.02f);
                            host.TrackLastImGuiItem();
                        }
                        ictl::DragVec(&host, T("Align Normal"), &g.AlignNormal.x, 3, 0.02f);
                        host.TrackLastImGuiItem();
                        ifield::Check(host, T("Foot Lock"), &g.Lock);
                        if (g.Lock) {
                            ifield::Float(host, T("Plant Height"), &g.PlantHeight, 0.005f, 0.0f, 1.0f);
                            ifield::Float(host, T("Release Time"), &g.ReleaseTime, 0.005f, 0.0f, 1.0f);
                        }
                        EditorTheme::EndMore();
                        }
                    }
                    ictl::DragVec(&host, T("Target (world)"), &g.Target.x, 3, 0.02f);
                    host.TrackLastImGuiItem();
                    ifield::Slider(host, T("Weight"), &g.Weight, 0.0f, 1.0f);
                    if (g.Resolved && g.EndJoint < 0)
                        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s",
                                           T("bone not found in skeleton"));
                    if (ImGui::Button(T("Remove Goal"))) remove = gi;
                    ictl::EndSubSection();
                }
                ImGui::PopID();
            }
            if (remove >= 0) {
                host.PushUndoSnapshot();
                ik->Goals.erase(ik->Goals.begin() + remove);
            }
            if (ImGui::Button(T("Add Goal"))) ik->Goals.emplace_back();
        }
    }
}
