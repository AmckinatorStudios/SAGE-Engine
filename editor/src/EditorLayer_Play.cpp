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

void EditorLayer::StartPlay() {
    if (InPlayMode()) return;

    // Снапшот сцены — Stop вернёт всё ровно как было до Play.
    m_playSnapshot = SceneSerializer::SaveToString(*m_scene);

    m_playScripts = std::make_unique<ScriptEngine>();
    m_playScripts->BindScene(*m_scene);
    // Паритет с рантаймом: частицы доступны скриптам уже в OnStart
    // (EmitParticles/CreateParticleStream рисуются в предпросмотре сцены).
    m_playScripts->BindParticles(m_renderer.Particles());

    // Ввод — как в собранной игре: действия объявляют сами скрипты (BindAction),
    // поэтому карту действий начинаем с ЧИСТОГО ЛИСТА на каждый Play (иначе
    // раскладка прошлого запуска пережила бы правку скрипта), а привязываем ДО
    // AttachScript — OnStart скриптов зовёт BindAction прямо оттуда.
    m_playInput = InputSystem();
    m_playInput.Attach(sage::Application::Get().GetWindow());
    m_playRawInput = std::make_unique<EditorPlayInput>(m_playInput, sage::Application::Get().GetWindow());
    m_playScripts->BindInput(m_playInput.Actions());
    m_playScripts->BindRawInput(*m_playRawInput);

    // Звук — как в собранной игре. Устройство может отсутствовать (headless CI):
    // AudioEngine в этом случае работает вхолостую, но вызовы из Lua валидны.
    if (!m_playAudio) m_playAudio = std::make_unique<AudioEngine>();
    m_playScripts->BindAudio(*m_playAudio);

    // Модули Lua (require "voxel") ищутся в скриптовой папке ОТКРЫТОГО ПРОЕКТА —
    // тот же контракт, что в собранной игре, где CWD и есть корень проекта.
    if (m_project.Loaded())
        m_playScripts->AddScriptSearchPath((m_project.Dir() / "assets" / "scripts").string());
    m_playScripts->AddScriptSearchPath("assets/scripts"); // скрипты рядом с редактором

    // Параметры запуска игры (LaunchArg в Lua) — до AttachScript, потому что
    // OnStart скриптов читает их сразу. В редакторе источник один: окружение
    // (headless-прогон CI ставит SAGE_GAME_ARGS="autopilot=1").
    if (const char* args = std::getenv("SAGE_GAME_ARGS")) m_playScripts->SetLaunchArgsFromString(args);

    // Привязываем скрипты всех сущностей со ScriptComponent. Ошибка в одном
    // скрипте (нет файла, синтаксис) не срывает Play — логируется, остальные
    // продолжают работать.
    int attached = 0;
    auto view = m_scene->Registry().view<ScriptComponent, IdComponent>();
    for (auto e : view) {
        const std::string& path = view.get<ScriptComponent>(e).Path;
        if (path.empty()) continue;
        // Пути скриптов в сцене — ОТНОСИТЕЛЬНО ПРОЕКТА ("assets/scripts/x.lua"):
        // так их резолвит собранная игра (SagePlayer делает chdir в проект). CWD
        // редактора — не папка проекта, поэтому здесь резолвим сами: как есть
        // (скрипты редактора, абсолютные пути), иначе — от корня проекта. Без
        // этого скрипты проекта работали бы в собранной игре, но НЕ в Play.
        std::string resolved = path;
        std::error_code scriptEc;
        if (!fs::exists(resolved, scriptEc) && m_project.Loaded()) {
            fs::path inProject = m_project.Dir() / path;
            if (fs::exists(inProject, scriptEc)) resolved = inProject.string();
        }
        try {
            m_playScripts->AttachScript(GameObject(&m_scene->Registry(), e), resolved);
            ++attached;
        } catch (const std::exception& ex) {
            LOG_ERROR("Editor") << "Play: script attach failed: " << ex.what();
        }
    }

    // Физика: строим мир по сущностям с RigidBodyComponent. Бэкенд по умолчанию —
    // Jolt, если собран, иначе встроенный движок (см. PhysicsWorld::DefaultBackend).
    m_playPhysics = std::make_unique<PhysicsScene>(
        sage::physics::PhysicsWorld::DefaultBackend(), *m_scene);

    // Скрипты получают доступ к физике времени выполнения (SetVelocity/GetVelocity/
    // SetGravity) — привязываем ПОСЛЕ построения мира, чтобы RuntimeBody сущностей
    // уже существовали к первому OnUpdate.
    m_playScripts->BindPhysics(*m_playPhysics);

    // Состав кадра на время Play — ТОТ ЖЕ, что у собранной игры (см.
    // PlayerLayer): скрипты, физика, анимация, частицы, звук в порядке,
    // заданном один раз в RegisterCoreSystems.
    //
    // Без этой регистрации Play выглядел запущенным и не был им: AttachScript
    // выше зовёт OnStart (и в консоли честно появляется «spin.lua attached
    // to: …»), но UpdateAll не звал НИКТО — планировщик о скриптах не знал.
    // То есть скрипт «привязывался и ничего не делал», а физика не считала ни
    // одного шага. StopPlay при этом снимал системы "scripts"/"physics",
    // которых никогда не добавляли, — по коду выхода из Play было видно
    // намерение, но входа в него не было.
    {
        sage::CoreSystems core;
        core.Scripts = m_playScripts.get();
        core.Physics = m_playPhysics.get();
        core.Particles = &m_renderer.Particles();
        core.Audio = m_playAudio.get();
        // Анимация уже зарегистрирована набором режима правки (превью) и
        // повторной регистрацией только заменилась бы сама на себя.
        core.Animation = false;
        sage::RegisterCoreSystems(m_systems, core);
    }

    m_playState = EditorPlayState::Playing;
    m_game.RequestFocus(); // «игровое окно» выходит на передний план при запуске
    LOG_INFO("Editor") << "Play started (" << attached << " script(s), "
                       << m_playPhysics->BodyCount() << " physics body(ies) on "
                       << m_playPhysics->BackendName() << ")";
}

// Ввод ИНТЕРФЕЙСУ ИГРЫ в Play-режиме редактора.
//
// Раньше этого не было вовсе, и это была не мелочь, а разница между «игра
// работает» и «игра работает только собранной»: панель Game РИСОВАЛА интерфейс
// сцены (DrawSceneUI), но UpdateSceneUI звал только плеер. Кнопка меню в
// редакторе не нажималась, слот инвентаря не подсвечивался, поле ввода не
// принимало текст — молча, без единой строки в логе. Проверить меню можно было
// только собрав игру, то есть ровно в том месте, где редактор обязан заменять
// сборку.
//
// Координаты курсора приходят уже переведёнными в кадр игры (см. GamePanel):
// панель — единственный, кто знает, где нарисована её картинка.
void EditorLayer::UpdatePlayUiInput(float dt) {
    if (!m_scene) return;
    auto uiView = m_scene->Registry().view<sage::ui::Transform>();
    if (uiView.begin() == uiView.end()) return;

    // Захваченный курсор — режим обзора: экранной точки у мыши нет, и
    // подсвечивать ею элементы нельзя (подсветилось бы то, что под центром).
    const bool captured = m_playRawInput && m_playRawInput->MouseCaptured();
    const bool usable = m_game.MouseInside() && !captured;
    const bool down = usable && m_game.MouseDown();

    sage::ui::UIInputState input;
    input.Mouse = usable ? glm::vec2(m_game.MouseX(), m_game.MouseY()) : glm::vec2(-1.0f);
    input.MouseDown = down;
    input.MousePressed = down && !m_playUiMouseWasDown;
    input.MouseReleased = !down && m_playUiMouseWasDown;
    input.TypedText = m_game.TypedText();
    input.DeltaTime = dt;
    m_playUiMouseWasDown = down;

    // Клавиши редактирования — из ImGui: он уже слушает окно, и второй
    // обработчик на те же клавиши спорил бы с ним за автоповтор.
    if (m_game.Focused()) {
        input.Backspace = ImGui::IsKeyPressed(ImGuiKey_Backspace, true);
        input.Delete = ImGui::IsKeyPressed(ImGuiKey_Delete, true);
        input.Left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
        input.Right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
        input.Home = ImGui::IsKeyPressed(ImGuiKey_Home, true);
        input.End = ImGui::IsKeyPressed(ImGuiKey_End, true);
        input.Enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        input.Escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        input.Tab = ImGui::IsKeyPressed(ImGuiKey_Tab, false);
    }

    sage::ui::UpdateSceneUI(*m_scene, input, m_renderer.GameWidth(), m_renderer.GameHeight());
}

void EditorLayer::StopPlay() {
    if (!InPlayMode()) return;

    // Порядок важен: ScriptEngine держит указатель на текущую сцену — гасим
    // его ДО того, как заменить сцену восстановленным снапшотом.
    // Снимаем ДО разрушения объектов: система держит на них указатель, и
    // оставленная в кадре она обратилась бы к освобождённой памяти.
    // Ровно то, что добавил StartPlay. "particles" и "animation" остаются: это
    // превью режима правки, а не игровые системы.
    // Stop — это для игры «выход»: скрипты обязаны узнать о нём раньше, чем
    // исчезнут. Без этого проверить сохранение при выходе можно было только в
    // собранной игре: в Play-режиме прогресс за последние секунды пропадал, и
    // выглядело это как «сохранение не работает в редакторе».
    if (m_playScripts) m_playScripts->DispatchQuit();
    m_systems.Remove("scripts");
    m_systems.Remove("physics");
    m_systems.Remove("audio");
    m_playScripts.reset();
    m_playPhysics.reset();
    // Курсор возвращается человеку РАНЬШЕ всего остального: игра могла его
    // захватить, и без этого Stop оставил бы редактор без мыши.
    if (m_playRawInput) m_playRawInput->ReleaseCapture();
    m_playRawInput.reset();
    RestoreSceneFromString(m_playSnapshot);
    m_playSnapshot.clear();
    m_playState = EditorPlayState::Editing;
    m_viewport.RequestFocus(); // вернулись к редактированию — Viewport вперёд
    LOG_INFO("Editor") << "Play stopped, scene restored";
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
