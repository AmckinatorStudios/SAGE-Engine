#pragma once

class EditorHost;
struct MeshRendererComponent;
struct CameraComponent;

namespace sage::editor {
// Слой отрисовки объекта (MeshRendererComponent::Layer).
void DrawRenderLayerField(EditorHost& host, MeshRendererComponent& mr);
// Проходы камеры (CameraComponent::Passes).
void DrawCameraPasses(EditorHost& host, CameraComponent& cam);
} // namespace sage::editor
