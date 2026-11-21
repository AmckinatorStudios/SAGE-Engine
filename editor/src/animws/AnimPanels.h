#pragma once
#include <string>

#include <glm/glm.hpp>

class EditorHost;
class AnimStage;

// ---------------------------------------------------------------------------
// ПАНЕЛИ ПРОСТРАНСТВА ANIMATION — всё про ОДНУ цель и её сцену предпросмотра
// (AnimStage), а не про уровень.
//
//   • Animation Hierarchy — кости скелета цели по их родительским связям
//     (как в файле модели, без своих папок), у цели без скелета — её объекты.
//   • Viewport — сцена предпросмотра: скелет поверх модели, выбор кости
//     щелчком, Move / Rotate / Scale (W / E / R), Local / World.
//   • Animation Inspector — цель, источник анимации, граф с параметрами,
//     слои и маски. Не Inspector сцены: тот правит объекты уровня, этот —
//     анимацию цели.
//
// Выбор кости — общий для всех трёх (AnimStage::SelectedBone): выбрал в
// иерархии — подсветилась во вьюпорте, и наоборот.
// ---------------------------------------------------------------------------

class AnimHierarchyPanel {
public:
    void Draw(EditorHost& host, bool* open, const std::string& windowId);
};

class AnimViewportPanel {
public:
    void Draw(EditorHost& host, bool* open, const std::string& windowId);

private:
    void DrawToolbar(EditorHost& host, AnimStage& stage);
    void DrawBones(AnimStage& stage, const glm::mat4& viewProj, const glm::vec2& origin, const glm::vec2& size);
    int PickBone(AnimStage& stage, const glm::mat4& viewProj, const glm::vec2& origin, const glm::vec2& size,
                 const glm::vec2& mouse) const;
    void Manipulate(EditorHost& host, AnimStage& stage, const glm::vec2& origin, const glm::vec2& size);

    int m_op = 0;            // 0 — Move, 1 — Rotate, 2 — Scale
    bool m_local = true;     // Local / World
    bool m_dragging = false; // орбита/сдвиг камеры идёт
    bool m_gizmoWasUsing = false;
};

class AnimInspectorPanel {
public:
    void Draw(EditorHost& host, bool* open, const std::string& windowId);

private:
    void DrawTarget(EditorHost& host, AnimStage& stage);
    void DrawSource(EditorHost& host, AnimStage& stage);
    void DrawGraph(EditorHost& host, AnimStage& stage);
    void DrawLayers(EditorHost& host, AnimStage& stage);
    void DrawMasks(EditorHost& host, AnimStage& stage);

    int m_maskLayer = 0;    // слой, чья маска правится в разделе Animation Masks
    int m_lastSelection = -2;  // выбор в сцене, за которым следует цель
    bool m_graphFromPanel = false;  // граф предпросмотра ведёт окно графа
};
