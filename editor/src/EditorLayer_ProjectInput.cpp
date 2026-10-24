// ---------------------------------------------------------------------------
// EditorLayer — проект, сцена и сборка игры.
//
// Здесь всё, что отвечает на вопрос «с чем мы работаем»: создать сцену,
// открыть и сохранить её, завести или открыть проект, собрать из него игру,
// показать спасённое после падения. Одна связная область: у неё общий предмет
// (файлы проекта на диске) и общий способ ошибаться — путь, которого нет, или
// файл, который не читается.
//
// Часть класса EditorLayer: объявления методов остались в EditorLayer.h, здесь
// только тела. Разбит он ровно потому, что дорос до двух с половиной тысяч
// строк, в которых рядом лежали сборка игры, отмена правки и раскладка окон —
// три области, у которых нет ничего общего, кроме имени класса.
// ---------------------------------------------------------------------------
#include "EditorLayer.h"
#include "CodeEditorApp.h"
#include "ProjectLauncher/ProjectDatabase.h"
#include "SceneCover.h"
#include "Thumbnails.h"
#include "sage/assets/Pack.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <cfloat>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder API (создание раскладки по умолчанию)
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"

#include "EditorTheme.h"
#include "EditorIcons.h"
#include "ModelMaterialImport.h"
#include "sage/render/DebugView.h"
#include "sage/app/Application.h"
#include "sage/core/Paths.h"
#include "sage/render/ModelMaterial.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Systems.h"
#include "sage/core/Version.h"
#include "sage/core/CrashHandler.h"
#include "sage/render/MeshRaycast.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/render/LightingUpload.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/gi/GI.h"
#include "sage/scene/Components.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIDemos.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/SceneSerializer.h"
#include "Localization.h"

namespace fs = std::filesystem;

// Раскладка управления проекта: файл, применение, запись и перечитывание.
// ============================================================================
//  Раскладка управления проекта (<проект>/input.sageinput)
//
//  Отдельный файл, а не часть sage.cfg: качество картинки настраивает тот, кто
//  играет, а раскладку — тот, кто делает игру. Смешать их значило бы позволить
//  игроку случайно снести управление, поправив яркость.
// ============================================================================

fs::path EditorLayer::ProjectInputFile() const {
    return m_project.Loaded() ? (m_project.Dir() / "input.sageinput") : fs::path();
}


// Переносит раскладку проекта в РАБОТАЮЩИЙ ввод Play-режима.
//
// Через строку, а не копированием объекта: InputSystem некопируем намеренно
// (у него подписки, курсор и шины — то, что принадлежит хозяину, а не
// раскладке). Сериализация же переносит ровно то, что и должно переноситься, —
// контексты, действия, привязки и настройки, — и это тот же путь, которым
// раскладка попадает в собранную игру. Один путь на оба случая: разойдись они,
// расхождение вылезло бы после сборки.
void EditorLayer::ApplyProjectInputMapping() {
    if (m_projectInput.ContextNames().empty()) return;
    bool hasAnyAction = false;
    for (sage::input::Context* ctx : m_projectInput.ContextsByPriority())
        if (!ctx->ActionNames().empty()) hasAnyAction = true;
    if (!hasAnyAction) return;  // пустая раскладка не должна затирать умолчания скриптов

    m_play.Input().LoadMappingFromString(m_projectInput.SaveMappingToString());
}


bool EditorLayer::SaveProjectInput() {
    const fs::path file = ProjectInputFile();
    if (file.empty()) return false;
    if (!m_projectInput.SaveMapping(file)) return false;
    m_projectInputDirty = false;
    LOG_INFO("Editor") << "Раскладка управления сохранена: " << file.string();
    return true;
}


bool EditorLayer::ReloadProjectInput() {
    const fs::path file = ProjectInputFile();
    // Начинаем с чистого листа: Load ЗАМЕЩАЕТ привязки известных действий, но
    // не убирает те, которых в файле нет, — без сброса «перечитать» оставляло
    // бы на экране действия, удалённые из файла руками.
    m_projectInput.ClearActions();
    m_projectInputDirty = false;
    if (file.empty()) return false;
    // Нет файла — не ошибка: у проекта просто ещё нет своей раскладки.
    std::error_code ec;
    if (!fs::exists(file, ec)) return true;
    return m_projectInput.LoadMapping(file);
}
