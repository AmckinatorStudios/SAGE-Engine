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

// Секции эффектов объекта: частицы и звук.
void InspectorPanel::DrawEffectSections(EditorHost& host, GameObject obj, entt::registry& reg,
                                   EntityRemovals& rm) {
    // --- Эмиттер частиц: весь эффект данными, без пресетов ---------------------
    //
    // Готовых «огня» и «дыма» нет: эффект собирается модулями (ParticleUi) или
    // приходит файлом .sagefx — его можно загрузить сюда, а собранное здесь
    // сохранить в файл и переиспользовать в других сценах.
    if (reg.all_of<ParticleEmitterComponent>(obj.Entity()) && EditorTheme::SectionHeader("particles", T("Particle Emitter" "###Particle Emitter"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Emitter)) {
        if (ParticleEmitterComponent* em = reg.try_get<ParticleEmitterComponent>(obj.Entity())) {
            bool playing = em->Playing;
            if (ImGui::Checkbox(T("Playing"), &playing)) {
                host.PushUndoSnapshot();
                em->Playing = playing;
            }
            ImGui::SameLine();
            // Посмотреть эффект с начала, не трогая сцену: залпы, задержка и
            // разогрев видны только на старте цикла.
            if (ImGui::Button(T("Restart"))) em->RestartRequested = true;
            ImGui::SameLine();
            if (ImGui::Button(T("Clear"))) em->ClearRequested = true;

            // Файл эффекта: загрузить в объект / сохранить объект в файл.
            ImGui::TextUnformatted(T("Load from an effect file"));
            std::string none;
            const assetslot::Result load =
                assetslot::Draw(host, "fxfile", assetslot::Kind::Effect, none, &m_preview,
                                T("Drop a .sagefx here to copy it into this emitter"));
            if (load.Changed && !load.Path.empty()) {
                sage::fx::ParticleEffect loaded;
                std::string err;
                const std::string real = sage::AssetDatabase::Instance().LocatePath(load.Path);
                if (sage::fx::LoadEffectFile(real, loaded, &err)) {
                    host.PushUndoSnapshot();
                    em->Effect = loaded;
                    em->RestartRequested = true;
                } else {
                    LOG_ERROR("Editor") << "Эффект не загрузился: " << err;
                }
            }
            if (load.BrowseRequested) {
                FileBrowser::Config c;
                c.Title = T("Choose a particle effect");
                c.Filters = assetslot::Extensions(assetslot::Kind::Effect);
                c.FilterLabel = T("Particle effects (*.sagefx)");
                c.StartDir = c.Root = assetslot::ProjectRoot(host);
                m_browser.Open(c);
                m_browseTarget = nullptr;
                m_browseLoadEffectEntity = obj.Id();
                m_browseIsShader = m_browseIsMesh = m_browseIsMaterial = false;
            }
            if (ImGui::Button(T("Save as effect file..."))) {
                FileBrowser::Config c;
                c.Title = T("Save the particle effect");
                c.Mode = FileBrowser::PickMode::SaveFile;
                c.DefaultName = obj.Name() + ".sagefx";
                c.Filters = assetslot::Extensions(assetslot::Kind::Effect);
                c.FilterLabel = T("Particle effects (*.sagefx)");
                c.StartDir = c.Root = assetslot::ProjectRoot(host);
                m_browser.Open(c);
                m_browseTarget = nullptr;
                m_browseSaveEffectEntity = obj.Id();
                m_browseIsShader = m_browseIsMesh = m_browseIsMaterial = false;
            }
            ImGui::Separator();

            // Слоты картинок и файлов — крючками: сам редактор эффекта не знает
            // ни о проекте, ни о диалогах (его проверяют модульные тесты).
            sage::editor::ParticleUiHooks hooks;
            hooks.BeforeEdit = [&host] { host.PushUndoSnapshot(); };
            hooks.TrackItem = [&host] { host.TrackLastImGuiItem(); };
            auto slot = [this, &host](const char* id, std::string& path, assetslot::Kind kind) {
                const assetslot::Result r = assetslot::Draw(host, id, kind, path, &m_preview);
                if (r.Changed) {
                    host.PushUndoSnapshot();
                    path = r.Path;
                }
                if (r.BrowseRequested) {
                    FileBrowser::Config c;
                    c.Title = kind == assetslot::Kind::Effect ? T("Choose a particle effect") : T("Choose a picture");
                    c.Filters = assetslot::Extensions(kind);
                    c.FilterLabel = kind == assetslot::Kind::Effect ? T("Particle effects (*.sagefx)") : T("Images");
                    c.StartDir = c.Root = assetslot::ProjectRoot(host);
                    m_browser.Open(c);
                    m_browseTarget = &path;
                    m_browseIsShader = m_browseIsMesh = m_browseIsMaterial = false;
                }
                return r.Changed;
            };
            hooks.TextureSlot = [slot](const char* id, std::string& path) {
                return slot(id, path, assetslot::Kind::Texture);
            };
            hooks.EffectSlot = [slot](const char* id, std::string& path) {
                return slot(id, path, assetslot::Kind::Effect);
            };
            hooks.FileExists = [](const std::string& ref) {
                std::error_code ec;
                return std::filesystem::exists(
                    sage::PathFromUtf8(sage::AssetDatabase::Instance().LocatePath(ref)), ec);
            };
            sage::editor::DrawParticleEffect(em->Effect, hooks);
        }
    }

    // --- Звук объекта ------------------------------------------------------
    //
    // Слот тот же, что у меша и скрипта: файл выбирается мышью или бросается
    // из дерева проекта. Печатать путь к .wav наизусть — ровно тот барьер,
    // из-за которого звук в сценах не появлялся.
    if (reg.all_of<AudioSourceComponent>(obj.Entity()) &&
        EditorTheme::SectionHeader("audio", T("Audio" "###Audio"), ImGuiTreeNodeFlags_DefaultOpen, &rm.Audio)) {
        if (AudioSourceComponent* au = reg.try_get<AudioSourceComponent>(obj.Entity())) {
            const assetslot::Result r =
                assetslot::Draw(host, "audioclip", assetslot::Kind::Audio, au->Clip, &m_preview,
                                T("No sound attached"));
            if (r.Changed) {
                host.PushUndoSnapshot();
                au->Clip = r.Path;
            }
            if (r.BrowseRequested) {
                FileBrowser::Config c;
                c.Title = T("Choose a sound");
                c.Filters = assetslot::Extensions(assetslot::Kind::Audio);
                c.FilterLabel = T("Sounds (*.wav, *.mp3, *.ogg)");
                // Диалог заперт в проекте: ассет выбирается ИЗНУТРИ (см. assetslot::ProjectRoot).
                c.StartDir = c.Root = assetslot::ProjectRoot(host);
                m_browser.Open(c);
                m_browseTarget = nullptr;
                m_browseAudioEntity = obj.Id();
                m_browseIsShader = false;
                m_browseIsMesh = false;
                m_browseIsMaterial = false;
            }

            ImGui::DragFloat(T("Volume"), &au->Volume, 0.01f, 0.0f, 1.0f); host.TrackLastImGuiItem();
            ImGui::Checkbox(T("Loop"), &au->Loop); host.TrackLastImGuiItem();
            ImGui::SameLine();
            ImGui::Checkbox(T("Auto Play"), &au->AutoPlay); host.TrackLastImGuiItem();
            ImGui::Checkbox(T("Spatial (3D)"), &au->Spatial); host.TrackLastImGuiItem();
            if (au->Spatial) {
                ImGui::DragFloat(T("Min Distance"), &au->MinDistance, 0.1f, 0.1f, 1000.0f);
                host.TrackLastImGuiItem();
                ImGui::DragFloat(T("Max Distance"), &au->MaxDistance, 0.5f, 0.2f, 5000.0f);
                host.TrackLastImGuiItem();
            }
            // Высота тона, спад и группа микшера — тонкая настройка.
            if (EditorTheme::BeginMore("audioMore")) {
                ImGui::DragFloat(T("Pitch"), &au->Pitch, 0.01f, 0.25f, 4.0f); host.TrackLastImGuiItem();
                if (au->Spatial) {
                    ImGui::DragFloat(T("Rolloff"), &au->Rolloff, 0.05f, 0.0f, 10.0f);
                    host.TrackLastImGuiItem();
                }
                const char* kCategories[] = {T("Effects"), T("Music"), T("Ambience")};
                int cat = (int)au->Category;
                if (ImGui::Combo(T("Mix Group"), &cat, kCategories, 3)) {
                    host.PushUndoSnapshot();
                    au->Category = (AudioCategory)cat;
                }
                EditorTheme::EndMore();
            }

            // Послушать ПРЯМО В РЕДАКТОРЕ, не запуская игру. Иначе подбор
            // громкости и расстояний превращается в цикл «Play — послушал —
            // Stop — поправил», по десятку секунд на каждую правку.
            if (au->Clip.empty()) {
                ImGui::TextDisabled("%s", T("Attach a sound file to preview it"));
            } else if (au->Playing) {
                if (EditorIcons::Button("stop", T("Stop"))) au->Stop();
            } else {
                if (EditorIcons::Button("play", T("Preview"))) au->Play();
            }
        }
    }
}
