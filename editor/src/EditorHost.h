#pragma once
#include <filesystem>
#include <string>

#include "sage/scene/Scene.h"
#include "sage/render/Camera.h"
#include "sage/core/Config.h"

class Project;

// Состояние Play-режима редактора (см. EditorLayer): вынесено из класса,
// чтобы панели зависели от контракта EditorHost, а не от EditorLayer.
enum class EditorPlayState { Editing, Playing, Paused };

// Режим отображения сцены во вьюпорте (View > Render Mode / тулбар).
//   Shaded    — полное освещение (по умолчанию);
//   Wireframe — каркас (полигоны линиями), плоский цвет для читаемости;
//   Unlit     — плоский базовый цвет без освещения;
//   Normals   — визуализация нормалей цветом (отладка).
enum class EditorRenderMode { Shaded, Wireframe, Unlit, Normals };

// Пространство манипулятора гизмо: Local — оси объекта, World — оси мира.
enum class EditorGizmoSpace { Local, World };

// ---------------------------------------------------------------------------
// EditorHost — контракт операций редактора, доступных панелям.
//
// Архитектура редактора v3: каждая панель (panels/*.h) — самостоятельный
// класс, владеющий СВОИМ UI-состоянием (поиск, буферы ввода, цели модалок) и
// зависящий только от этого интерфейса. EditorLayer реализует хост и
// оркестрирует панели. Это даёт:
//   • независимую разработку/тестирование панелей (хост подменяем);
//   • у EditorLayer остаётся ядро: сцена/проект/Play/undo/рендер/диалоги;
//   • новая панель = новый файл в panels/ + вызов Draw() в кадре, без
//     врастания в god-object.
// ---------------------------------------------------------------------------
class EditorHost {
public:
    virtual ~EditorHost() = default;

    // --- сцена и выбор ---
    virtual Scene& CurrentScene() = 0;
    virtual int SelectedId() const = 0;
    virtual void SetSelectedId(int id) = 0;
    virtual GameObject SelectedObject() = 0; // invalid, если ничего не выбрано

    // --- проект и файлы сцен ---
    virtual Project& CurrentProject() = 0;
    virtual bool LoadSceneFromFile(const std::filesystem::path& path) = 0;
    virtual bool SaveSceneToFile(const std::filesystem::path& path) = 0;
    virtual bool CreateProject(const std::string& dir, const std::string& name, std::string& err) = 0;
    virtual bool OpenProject(const std::string& path, std::string& err) = 0;
    // Упаковывает открытый проект в готовую к запуску игру (SagePlayer + ассеты).
    // false + err при ошибке. Нужен панели диалогов (Build Game...).
    virtual bool BuildGame(const std::filesystem::path& outputDir, std::string& err) = 0;
    virtual std::filesystem::path& AssetsCwd() = 0; // текущая папка панели Assets

    // Гибкая конфигурация игры (EngineConfig) — редактируется панелью Settings,
    // сохраняется в <проект>/sage.cfg, Build Game кладёт её в собранную игру.
    virtual sage::EngineConfig& Settings() = 0;
    // Короткое сообщение в статус-баре (обратная связь панелей/плагинов).
    virtual void SetStatusMessage(const std::string& message) = 0;

    // --- undo/redo ---
    virtual void PushUndoSnapshot() = 0;
    // Трекинг «размазанных» правок (перетаскивание DragFloat, набор текста):
    // Capture — запомнить состояние «до» (на активации виджета/наведении
    // гизмо), Commit — положить запомненное в undo-стек (на факте изменения).
    virtual void CapturePendingSnapshot() = 0;
    virtual void CommitPendingSnapshot() = 0;
    // Обёртка над Capture/Commit для только что нарисованного ImGui-виджета.
    virtual void TrackLastImGuiItem() = 0;

    // --- сущности ---
    virtual GameObject CreateCubeEntity(const std::string& name) = 0;
    virtual void DuplicateSelected() = 0;
    virtual void DeleteSelected() = 0;

    // --- Play-режим ---
    virtual EditorPlayState GetPlayState() const = 0;
    virtual bool InPlayMode() const = 0;
    virtual void StartPlay() = 0;
    virtual void PausePlay() = 0;
    virtual void ResumePlay() = 0;
    virtual void StopPlay() = 0;

    // --- общее состояние инструментов (делят тулбар и вьюпорт) ---
    virtual int& GizmoOp() = 0;              // значение ImGuizmo::OPERATION
    virtual bool& GizmoSnap() = 0;
    virtual EditorGizmoSpace& GizmoSpace() = 0;
    virtual bool& ShowGrid() = 0;
    virtual EditorRenderMode& RenderMode() = 0;

    // --- вьюпорт/камера ---
    virtual Camera& EditorCamera() = 0;
    virtual const glm::mat4& ViewMatrix() const = 0;
    virtual const glm::mat4& ProjMatrix() const = 0;
    virtual unsigned int SceneTexture() const = 0;     // цвет FBO сцены (для ImGui::Image)
    virtual void SetViewportSize(int w, int h) = 0;    // панель сообщает размер под FBO
    virtual void PickAtViewport(float u, float v) = 0; // u,v в [0..1] — выбор сущности лучом

    // --- панель Game (игровое окно: рендер от Primary-камеры сцены) ---
    virtual unsigned int GameTexture() const = 0;
    virtual void SetGameViewportSize(int w, int h) = 0;
    virtual bool HasPrimaryCamera() = 0;

    // --- выбор в Assets (нужен Inspector'у для назначения материала) ---
    virtual const std::filesystem::path& SelectedAssetPath() const = 0;
};
