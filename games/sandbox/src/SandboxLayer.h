#pragma once
#include <memory>
#include <optional>
#include <string>

#include "sage/core/Layer.h"
#include "sage/render/Shader.h"
#include "sage/render/Camera.h"
#include "sage/render/Mesh.h"
#include "sage/scene/Scene.h"

class ScriptEngine;

// ---------------------------------------------------------------------------
// SandboxLayer — минимальный showcase движка SAGE, НЕ игра с геймплеем.
//
// Назначение (см. README, раздел "games/sandbox"):
//   1. Референс «как сделать игру на движке» — тонкий main.cpp + один Layer,
//      ECS-сцена из entt-сущностей, один шейдер, скриптинг на Lua.
//   2. Единственная запускаемая fixture для headless smoke-тестов в CI
//      (сама библиотека sage_engine не запускается — нужен exe).
//
// Намеренно НЕ включает тени/пост-процесс/аудио/UI — это подсистемы движка,
// которые новая игра подключает по необходимости (см. соответствующие классы
// в engine/src/sage/render/), а не обязательная часть минимального примера.
// ---------------------------------------------------------------------------
class SandboxLayer : public sage::Layer {
public:
    // Конструктор и деструктор объявлены здесь, определены в .cpp: тело любого
    // из них (даже пустое) заставляет компилятор сгенерировать код разбора для
    // unique_ptr<ScriptEngine> (в т.ч. путь исключений), а для этого нужен
    // ПОЛНЫЙ тип ScriptEngine — ScriptEngine.h включается только в .cpp, в
    // заголовке достаточно forward-declaration.
    SandboxLayer();
    ~SandboxLayer() override;

    void OnAttach() override;
    void OnDetach() override;
    void OnUpdate(float dt) override;
    void OnRender() override;

private:
    Scene m_scene;
    Camera m_camera;
    std::optional<Shader> m_shader;
    std::shared_ptr<Mesh> m_cube;
    std::unique_ptr<ScriptEngine> m_scripts;

    // Авто-скриншот для headless-проверки/CI — тот же паттерн, что и в
    // EditorLayer (SAGE_SCREENSHOT_AT_FRAME/SAGE_SCREENSHOT_PATH).
    std::string m_screenshotPath = "screenshot.png";
    int m_autoScreenshotFrame = -1;
    int m_frameCounter = 0;
};
