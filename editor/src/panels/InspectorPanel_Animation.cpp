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

namespace fs = std::filesystem;
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
                                       T("Skeleton comes from the Mesh model"))) {
        if (AnimationComponent* am = reg.try_get<AnimationComponent>(obj.Entity())) {
            const MeshRendererComponent* mesh = reg.try_get<MeshRendererComponent>(obj.Entity());
            const bool hasModelRef = mesh && mesh->Ref.type == MeshRef::Type::Model &&
                                     !mesh->Ref.path.empty();
            if (!hasModelRef) {
                // РАНЬШЕ ЗДЕСЬ ЗАВОДИЛОСЬ ДЕМО-ЩУПАЛЬЦЕ. Компонент, добавленный
                // к объекту без модели, показывал шевелящийся отросток — то есть
                // на вопрос «почему не работает моя анимация» отвечал чужой
                // работающей анимацией. Теперь ответ словами и по делу.
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                                   T("No model in Mesh — there is nothing to animate"));
                ImGui::TextDisabled("%s", T("Put a model with a skeleton into Mesh (.glb/.gltf/.fbx)."));
            }
            // Модель есть, а скелета в ней нет — это тот самый случай, который
            // ТЗ требует объяснять словами, а не молчанием.
            if (hasModelRef && am->Ready && !am->Model) {
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                                   T("This model has no skeleton — nothing to animate"));
            }
            if (ImGui::Button(T("Reload"))) { am->Ready = false; am->Model = nullptr; }

            // --- КЛИП ФАЙЛОМ ---------------------------------------------
            //
            // Слот ассета, а не номер в списке: номер зависит от порядка клипов
            // в модели и молча меняется при переэкспорте (см. AnimationComponent).
            // Пустой слот — играем клип из самой модели, как раньше.
            ImGui::TextUnformatted(T("Clip file"));
            const assetslot::Result clipSlot =
                assetslot::Draw(host, "animclip", assetslot::Kind::Animation, am->ClipPath,
                                &m_preview, T("Playing a clip from the model itself"));
            if (clipSlot.Changed) {
                host.PushUndoSnapshot();
                am->ClipPath = clipSlot.Path;
            }
            if (clipSlot.BrowseRequested) {
                FileBrowser::Config c;
                c.Title = T("Choose an animation clip");
                c.Filters = assetslot::Extensions(assetslot::Kind::Animation);
                c.FilterLabel = T("Animation");
                // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
                c.StartDir = c.Root = assetslot::ProjectRoot(host);
                m_browser.Open(c);
                m_browseTarget = &am->ClipPath;
                m_browseIsShader = false;
                m_browseIsMesh = false;
                m_browseIsMaterial = false;
            }
            if (!am->ClipPath.empty() && am->MissingBones > 0) {
                // Клип от другого персонажа подходит частично — и молчать об
                // этом нельзя: «двигается половина скелета» иначе не объяснить.
                ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn),
                                   T("%d bones of the clip are missing from this skeleton"),
                                   am->MissingBones);
            }

            // Вынуть клипы модели в файлы — то же действие, что импорт
            // материалов, и в том же месте, где о них вспоминают.
            if (hasModelRef && EditorIcons::Button("import", T("Extract clips to files"),
                                                   T("Writes every clip of the model next to it\n"
                                                     "as .sageanim. Existing files are kept."))) {
                const ClipImportResult r = ImportModelClips(host.CurrentProject(), mesh->Ref.path);
                if (r.Found == 0) {
                    host.SetStatusMessage(T("The model has no animation clips"));
                } else {
                    host.SetStatusMessage(T("Clips: ") + std::to_string(r.Found) +
                                          T(", files written: ") + std::to_string(r.Created));
                    // Первый клип сразу назначаем: иначе человек нажал кнопку,
                    // файлы появились, а объект не двинулся — и непонятно, что
                    // ещё от него хотят.
                    if (am->ClipPath.empty() && !r.Refs.empty()) {
                        host.PushUndoSnapshot();
                        am->ClipPath = r.Refs.front();
                    }
                }
            }

            // Список клипов — из проигрывателя (модель уже загружена системой).
            int clipCount = am->Anim.ClipCount();
            if (clipCount > 0 && am->ClipPath.empty()) {
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
            } else if (clipCount == 0) {
                ImGui::TextDisabled("%s", T("No animation clips (bind pose)"));
            }
            ImGui::DragFloat(T("Speed"), &am->Speed, 0.02f, 0.0f, 8.0f); host.TrackLastImGuiItem();
            if (ImGui::Checkbox(T("Loop"), &am->Loop)) am->Anim.Play(am->Clip, am->Loop);
            ImGui::SameLine();
            ImGui::Checkbox(T("Playing"), &am->Playing);
            ImGui::Checkbox(T("Root Motion"), &am->RootMotion);
        }
    }

    // --- Анимация свойств: клип .sageclip на этом объекте -------------------
    //
    // ОТДЕЛЬНО ОТ «Animation», И ЭТО НЕ ДУБЛИРОВАНИЕ. Тот компонент играет
    // движение КОСТЕЙ модели, этот — движение СВОЙСТВ объекта и его детей
    // (положение, цвет, прозрачность, размер). Скелета для него не нужно:
    // мигающая кнопка интерфейса и всплывающая подсказка — это он.
    //
    // ИМЕННО ЭТОТ КОМПОНЕНТ ОТВЕЧАЕТ НА ВОПРОС «ЧТО ИГРАЕТ ЭТОТ ОБЪЕКТ».
    // Пока его не было видно в инспекторе, клип существовал только внутри
    // инструмента анимации: объект «анимировался», но в сцене не было ничего,
    // что это подтверждало бы, и после закрытия редактора движение пропадало.
    if (reg.all_of<PropertyAnimatorComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader("clock", T("Property Animation" "###Property Animation"),
                                   ImGuiTreeNodeFlags_DefaultOpen, &rm.PropertyAnim,
                                   T("Plays a .sageclip: properties of this object and its children"))) {
        if (PropertyAnimatorComponent* pa = reg.try_get<PropertyAnimatorComponent>(obj.Entity())) {
            const assetslot::Result clipSlot =
                assetslot::Draw(host, "propclip", assetslot::Kind::Animation, pa->ClipPath,
                                &m_preview, T("No clip — nothing is playing"));
            if (clipSlot.Changed) {
                host.PushUndoSnapshot();
                pa->ClipPath = clipSlot.Path;
                // Путь сменился — старый клип в памяти больше не тот.
                pa->Clip.reset();
                pa->Ready = false;
            }
            if (clipSlot.BrowseRequested) {
                FileBrowser::Config c;
                c.Title = T("Choose an animation clip");
                c.Filters = assetslot::Extensions(assetslot::Kind::Animation);
                c.FilterLabel = T("Animation");
                // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
                c.StartDir = c.Root = assetslot::ProjectRoot(host);
                m_browser.Open(c);
                m_browseTarget = &pa->ClipPath;
                m_browseIsShader = false;
                m_browseIsMesh = false;
                m_browseIsMaterial = false;
            }
            if (pa->ClipPath.empty()) {
                ImGui::TextDisabled("%s", T("Create a clip in the Animation tool and it lands here."));
            } else if (EditorIcons::Button("anim", T("Edit the clip"),
                                           T("Opens this clip in the Animation tool"))) {
                host.OpenAnimationClip(pa->ClipPath);
            }
            ImGui::Checkbox(T("Playing"), &pa->Playing);
            ImGui::SameLine();
            ImGui::Checkbox(T("Loop"), &pa->Loop);
            ImGui::DragFloat(T("Speed"), &pa->Speed, 0.02f, 0.0f, 8.0f);
            host.TrackLastImGuiItem();
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
            if (ImGui::Combo(T("Resolution"), &resIdx, resNames, 4)) p->Resolution = resValues[resIdx];
            if (p->Resolution != prevRes) p->Dirty = true;

            if (ImGui::DragFloat3(T("Box Half Extents"), &p->BoxHalfExtents.x, 0.1f, 0.1f, 500.0f))
                p->Dirty = true;
            host.TrackLastImGuiItem();

            ImGui::SliderFloat(T("Intensity"), &p->Intensity, 0.0f, 3.0f);
            host.TrackLastImGuiItem();
            if (EditorTheme::BeginMore("probeMore")) {
                if (ImGui::Checkbox(T("Box Parallax"), &p->BoxParallax)) p->Dirty = true;
                if (ImGui::DragFloat(T("Far Clip"), &p->FarClip, 0.5f, 1.0f, 2000.0f)) p->Dirty = true;
                host.TrackLastImGuiItem();
                ImGui::Checkbox(T("Realtime (re-capture every frame)"), &p->Realtime);
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
            ImGui::Checkbox(T("IK Enabled"), &ik->Enabled);
            const AnimationComponent* am = reg.try_get<AnimationComponent>(obj.Entity());
            if (!am) ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn),
                                        "%s", T("No Animation component - goals do nothing"));

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
                        // Полюс, выравнивание стопы и её фиксация — тонкая
                        // настройка ног; цель и вес — выше и ниже, на виду.
                        if (EditorTheme::BeginMore("ikMore")) {
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
                        EditorTheme::EndMore();
                        }
                    }
                    ImGui::DragFloat3(T("Target (world)"), &g.Target.x, 0.02f);
                    host.TrackLastImGuiItem();
                    ImGui::SliderFloat(T("Weight"), &g.Weight, 0.0f, 1.0f);
                    host.TrackLastImGuiItem();
                    if (g.Resolved && g.EndJoint < 0)
                        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s",
                                           T("bone not found in skeleton"));
                    if (ImGui::Button(T("Remove Goal"))) remove = gi;
                    ImGui::TreePop();
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
