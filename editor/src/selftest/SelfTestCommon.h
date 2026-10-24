#pragma once
// Общее для файлов самопроверки редактора (SAGE_EDITOR_SELFTEST=1):
// заголовки, которыми пользуются пробы, и маленькие генераторы эталонных
// файлов. Только для editor/src/selftest/ — снаружи его не включают.

// ---------------------------------------------------------------------------
// Headless-проверки редактора: self-test ядра и E2E-сборка игры.
//
// ПОЧЕМУ ОТДЕЛЬНЫМ ФАЙЛОМ. Они занимали 43% EditorLayer.cpp — 966 строк из
// 2204. Это не редактор: ни одна из этих строк не выполняется, когда человек
// работает в программе. Соседство с реальной логикой мешало обеим сторонам —
// править редактор приходилось, пролистывая сотни строк проверок, а сами
// проверки терялись в хвосте файла.
//
// Перенос — ЧИСТОЕ ПЕРЕМЕЩЕНИЕ: ни строки логики не изменено. Обе функции
// остаются методами EditorLayer (им нужен доступ к его состоянию) и
// вызываются оттуда же, откуда вызывались.
// ---------------------------------------------------------------------------
#include "EditorLayer.h"
#include "sage/lang/SageCompiler.h"
#include "sage/anim/Tween.h"
#include "SceneCover.h"
#include "EditorIcons.h"
#include "CodeEditorApp.h"
#include "AssetSlot.h"
#include "ObjectSlot.h"
#include "AssetCovers.h"
#include "Thumbnails.h"
#include "ModelImportDialog.h"
#include "PanelWindows.h"
#include "UIElementProperties.h"
#include "Progress.h"
#include "imgui_internal.h" // NextWindowData: проверка флага вьюпорта у отдельных окон
#include "panels/AssetsPanel.h"
#include "FolderColors.h"
#include "panels/HierarchyPanel.h"

#include "sage/ecs/DecalSystem.h"
#include "sage/ecs/RenderComponents.h"

#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <fstream>
#include <thread>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/app/Application.h"
#include "sage/core/Paths.h"
#include <nlohmann/json.hpp>

#include "ImGuizmo.h"
#include "EditorTheme.h"
#include "TemplateStore.h"
#include "sage/assets/Pack.h"
#include "ScreenColor.h"
#include "sage/core/Version.h"
#include "sage/render/ResourceManager.h"
#include "sage/anim/ClipFile.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/render/Screenshot.h"
#include <stb_image_write.h>   // реализация развёрнута в render/Screenshot.cpp
#include "sage/ui/NineSlice.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/physics/Ragdoll.h"
#include "sage/render/Frustum.h"
#include "sage/audio/AudioSystem.h"
#include "sage/render/ParticleECS.h"
#include "sage/rhi/ResourceLedger.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/gi/GI.h"
#include "sage/scene/Components.h"
#include "sage/anim/AnimProperty.h"
#include "sage/anim/PropertyAnimator.h"
#include "sage/anim/PropertyClip.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UIPresets.h"
#include "panels/TweenPanel.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/events/Events.h"
#include "sage/scene/Signals.h"
#include "sage/ecs/DayNightCycle.h"
#include "sage/vars/VarsComponent.h"
#include "UILayoutOps.h"
#include "EditorPrefs.h"
#include "PathScope.h"
#include "Localization.h"
#include "ObjectCatalog.h"
#include "panels/InspectorPanel.h"
#include "RectSelect.h"
#include "sage/scene/SceneSerializer.h"

#include "sage/scene/Prefab.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/SkinnedModel.h"
#include "ViewGizmo.h"
#include "sage/render/ModelMaterial.h"
#include "sage/render/PostFX.h"
#include "sage/render/PostProcessComponent.h"
#include "sage/assets/import/Convert.h"
#include "AssetPreview.h"


namespace fs = std::filesystem;

namespace sage_selftest {
void WriteFloats(std::ofstream& out, const std::vector<float>& v);
bool WriteRigFixture(const fs::path& dir, const std::string& name, bool skinned, std::string& err);
void WriteSelfTestZip(const std::filesystem::path& path);
} // namespace sage_selftest
using namespace sage_selftest;
