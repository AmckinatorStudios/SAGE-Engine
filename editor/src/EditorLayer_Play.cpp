// ---------------------------------------------------------------------------
// EditorLayer — режим игры внутри редактора.
//
// Play/Stop и всё, что между ними. Отдельно, потому что это единственное
// место редактора, где сцена живёт НЕ как документ: она запускается, её меняют
// скрипты, и по Stop она обязана вернуться ровно в то состояние, в котором её
// оставил человек. Смешивать это с редактированием опасно — именно на границе
// «играем/правим» и появляются потери работы.
//
// Часть класса EditorLayer: объявления методов остались в EditorLayer.h, здесь
// только тела. Разбит он ровно потому, что дорос до двух с половиной тысяч
// строк, в которых рядом лежали сборка игры, отмена правки и раскладка окон —
// три области, у которых нет ничего общего, кроме имени класса.
// ---------------------------------------------------------------------------
#include "EditorLayer.h"

#include "sage/audio/AudioSystem.h"
#include "sage/assets/Pack.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <fstream>

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
#include "sage/core/Application.h"
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
#include "sage/render/ParticlePresets.h"
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

namespace {

// Пересечение луча с произвольным AABB [bmin, bmax] в локальном пространстве
// объекта (slab-тест). Возвращает t входа (>=0) или отрицательное при промахе.
float RayBox(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& bmin, const glm::vec3& bmax) {
    glm::vec3 inv = 1.0f / rd; // IEEE inf при нулевой компоненте — slab-тест это переживает
    glm::vec3 t0 = (bmin - ro) * inv;
    glm::vec3 t1 = (bmax - ro) * inv;
    glm::vec3 tmin = glm::min(t0, t1), tmax = glm::max(t0, t1);
    float tNear = std::max({tmin.x, tmin.y, tmin.z});
    float tFar  = std::min({tmax.x, tmax.y, tmax.z});
    if (tNear > tFar || tFar < 0.0f) return -1.0f;
    return tNear >= 0.0f ? tNear : tFar;
}

// Луч vs единичный куб [-0.5,0.5]^3 — маркеры невидимых сущностей (камера/свет).
float RayUnitCube(const glm::vec3& ro, const glm::vec3& rd) {
    return RayBox(ro, rd, glm::vec3(-0.5f), glm::vec3(0.5f));
}

constexpr float kStatusBarHeight = 26.0f;

} // namespace


// ============================================================================
//  Play-режим
// ============================================================================

// Слой добавляет к сессии ровно то, чего она про редактор знать не должна:
// заметку шаблона, фокус панелей и способ восстановить сцену.
PlayContext EditorLayer::MakePlayContext() {
    PlayContext ctx;
    ctx.ScenePtr = &m_scenePtr;
    ctx.Systems = &m_systems;
    ctx.Particles = &m_renderer.Particles();
    ctx.ProjectDir = m_project.Dir();
    ctx.RestoreScene = [this](const std::string& s) { return RestoreSceneFromString(s); };
    ctx.ApplyProjectInputMapping = [this] { ApplyProjectInputMapping(); };
    return ctx;
}

void EditorLayer::StartPlay() {
    // Заметка шаблона отвечала на вопрос «почему в сцене пусто»; Play на него
    // и отвечает делом — держать её дальше значит мешать смотреть.
    m_templateNote.clear();
    m_scenePtr = m_scene.get();
    PlayContext ctx = MakePlayContext();
    if (m_play.Start(ctx) < 0) return;
    m_game.RequestFocus(); // «игровое окно» выходит на передний план при запуске
}

void EditorLayer::StopPlay() {
    if (!m_play.Active()) return;
    m_scenePtr = m_scene.get();
    PlayContext ctx = MakePlayContext();
    m_play.Stop(ctx);
    m_viewport.RequestFocus(); // вернулись к редактированию — Viewport вперёд
}

// Мышь и текст панели Game — интерфейсу сцены. Клавиши редактирования берутся
// из ImGui: он уже слушает окно, и второй обработчик на те же клавиши спорил бы
// с ним за автоповтор.
void EditorLayer::UpdatePlayUiInput(float dt) {
    if (!m_scene) return;
    PlayUiInput in;
    in.MouseInside = m_game.MouseInside();
    in.MouseDown = m_game.MouseDown();
    in.Focused = m_game.Focused();
    in.MouseX = m_game.MouseX();
    in.MouseY = m_game.MouseY();
    in.TypedText = m_game.TypedText();
    in.GameWidth = m_renderer.GameWidth();
    in.GameHeight = m_renderer.GameHeight();
    in.DeltaTime = dt;
    if (in.Focused) {
        in.Backspace = ImGui::IsKeyPressed(ImGuiKey_Backspace, true);
        in.Delete = ImGui::IsKeyPressed(ImGuiKey_Delete, true);
        in.Left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
        in.Right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
        in.Home = ImGui::IsKeyPressed(ImGuiKey_Home, true);
        in.End = ImGui::IsKeyPressed(ImGuiKey_End, true);
        in.Enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        in.Escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        in.Tab = ImGui::IsKeyPressed(ImGuiKey_Tab, false);
    }
    m_play.UpdateUiInput(*m_scene, in);
}

// ============================================================================
//  Undo/Redo (снапшот-модель) + dirty-маркер
// ============================================================================

bool EditorLayer::RestoreSceneFromString(const std::string& snapshot) {
    try {
        std::unique_ptr<Scene> restored = SceneSerializer::LoadFromString(snapshot);
        // Запечённый GI переезжает указателем: строковый снапшот не тащит
        // страницы лайтмап, а бейк валиден для той же статичной геометрии
        // (Transplant сверяет отпечаток и при несовпадении не переносит).
        if (m_scene && restored) sage::gi::Transplant(*m_scene, *restored);
        m_scene = std::move(restored);
        // Выбор хранится как id, а сериализатор сохраняет id — выбор переживает
        // откат, если сущность существует в снапшоте (иначе Get() даст invalid).
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Editor") << "Scene restore failed: " << e.what();
        return false;
    }
}
