#pragma once

// ===========================================================================
//  SAGE Engine — ОДИН заголовок на весь движок.
//
//      #include <sage/Sage.h>
//
//  ЗАЧЕМ. Движок обязан выглядеть для игры как единая платформа, а не как
//  набор библиотек, которые автор игры сам находит и связывает. До сих пор
//  начало игры выглядело так:
//
//      #include "sage/core/GameModule.h"
//      #include "sage/core/Layer.h"
//      #include "sage/core/InputSystem.h"
//      #include "sage/render/Camera.h"
//      #include "sage/render/ShadowMap.h"
//      #include "sage/ecs/RenderBatch.h"
//      #include "sage/scene/Scene.h"
//      #include <GLFW/glfw3.h>     // ради кодов клавиш
//      #include <glm/glm.hpp>      // ради векторов
//      ... и так десяток строк
//
//  Два последних включения — прямая протечка реализации: игра просила у движка
//  ввод и математику, а получала обязанность знать, какие библиотеки лежат у
//  него внутри. Кодов клавиш это больше не касается (см. sage/core/Keys.h), а
//  здесь закрывается остальное.
//
//  ЧТО ЭТО НЕ ОТМЕНЯЕТ. Точечные включения. Они остаются и остаются
//  правильным выбором там, где нужны две системы из шестнадцати: компилятор
//  разбирает меньше кода, и по списку включений видно, чем файл занят. Этот
//  заголовок — для точки входа игры и для тех, кому удобнее один include.
//
//  ЧЕГО ЗДЕСЬ НЕТ. Внутренностей движка: бэкендов RHI, загрузчиков форматов,
//  сторонних заголовков. Публичный API — это то, что нужно ИГРЕ, а не всё, из
//  чего движок собран.
// ===========================================================================

// --- Приложение и жизненный цикл -------------------------------------------
#include "sage/core/Application.h"
#include "sage/core/Config.h"
#include "sage/core/GameModule.h"   // SAGE_MAIN() — точка входа игры
#include "sage/core/Layer.h"
#include "sage/core/Log.h"
#include "sage/core/Paths.h"
#include "sage/core/Window.h"

// --- Ввод -------------------------------------------------------------------
#include "sage/core/InputBinding.h"
#include "sage/core/InputMap.h"
#include "sage/core/InputSystem.h"
#include "sage/core/Keys.h"         // sage::Key, sage::MouseButton — без GLFW

// --- Время, задачи, профилирование -----------------------------------------
#include "sage/core/JobSystem.h"
#include "sage/core/Profiler.h"
#include "sage/core/SystemScheduler.h"
#include "sage/core/Tween.h"

// --- Сцена, сущности, компоненты -------------------------------------------
#include "sage/ecs/CameraLightComponents.h"
#include "sage/ecs/RenderComponents.h"
#include "sage/scene/Components.h"
#include "sage/scene/Prefab.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneManager.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/scene/Transform.h"

// --- Проект: сцены, ассеты, раскладка каталогов ------------------------------
// Тот же класс, которым пользуются редактор и плеер, — поэтому проект,
// созданный кодом, открывается редактором без конвертации.
#include "sage/project/Project.h"
#include "sage/project/ProjectWatcher.h"

// --- Рисование в 3D ---------------------------------------------------------
// Краска на поверхности мира: кисть, мазок, закрашенная область. Нужно там,
// где форму надо ПОКАЗАТЬ, а не построить: следы, разметка, границы владений.
// Край нарисованного — край кисти, а не край многоугольника.
#include "sage/paint/PaintCanvas.h"

// --- Рендер -----------------------------------------------------------------
#include "sage/ecs/RenderBatch.h"
#include "sage/render/Camera.h"
#include "sage/render/Framebuffer.h"
#include "sage/render/Material.h"
#include "sage/render/Mesh.h"
#include "sage/render/Model.h"
#include "sage/render/ParticleSystem.h"
#include "sage/render/PostFX.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/ScenePasses.h"
#include "sage/render/ShadowMap.h"
#include "sage/render/Texture.h"

// --- Интерфейс --------------------------------------------------------------
#include "sage/ui/UI.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/ui/Widgets.h"

// --- Физика, звук, анимация -------------------------------------------------
#include "sage/anim/AnimationSystem.h"
#include "sage/audio/AudioEngine.h"
#include "sage/physics/PhysicsScene.h"

// --- Ассеты, сохранения, события, переменные, скрипты, сеть ------------------
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/Pack.h"
#include "sage/core/SaveGame.h"
#include "sage/events/Events.h"
#include "sage/net/NetHost.h"
#include "sage/scripting/ScriptEngine.h"
#include "sage/vars/Table.h"
#include "sage/vars/VarsComponent.h"

// --- Математика -------------------------------------------------------------
//
// Через движок, а не «подключите glm сами»: вектор и матрица нужны КАЖДОЙ игре
// с первой строки, и требовать ради них отдельной зависимости значит начинать
// знакомство с движком со сборки чужой библиотеки.
#include "sage/core/Math.h"
