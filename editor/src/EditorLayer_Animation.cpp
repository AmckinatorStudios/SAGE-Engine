// Ресурс анимации объекта — в редактор одним действием (кнопки компонентов в
// Inspector). Путь один и тот же для «Edit» и «Create Clip»: пространство
// Animation, объект — цель предпросмотра (AnimStage), ресурс открыт там, где он
// правится. Создание идёт уже существующими путями: свой скелетный клип —
// AnimStage::CreateEditableClip, клип свойств — AnimationPanel::CreateClipFor
// (файл + компонент), второго способа завести клип нет.
#include "EditorLayer.h"

#include <filesystem>

#include "sage/anim/AnimationComponents.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Paths.h"

void EditorLayer::EditAnimation(int objectId, const std::string& resource) {
    GameObject obj = m_scene->Get(objectId);
    if (!obj.Valid()) return;
    m_selection.SetPrimary(objectId);
    SetWorkspace(EditorWorkspace::Animation);
    m_animStage.SetTarget(*this, objectId);
    if (resource.empty()) return;
    const std::string full = sage::AssetDatabase::Instance().LocatePath(resource);
    if (std::filesystem::path(sage::PathFromUtf8(full)).extension() == ".sageanimgraph") {
        OpenAnimGraph(full);
        m_animStage.SetMode(AnimStage::Source::Graph);
    } else {
        m_animStage.SetMode(AnimStage::Source::Clip);
        OpenAnimationClip(full);
    }
}

bool EditorLayer::CreateAnimationClip(int objectId, bool skeletal) {
    GameObject obj = m_scene->Get(objectId);
    if (!obj.Valid()) return false;
    if (skeletal) {
        const std::string ref = AnimStage::CreateEditableClip(*this, obj.Name());
        if (ref.empty()) return false;
        PushUndoSnapshot();
        AnimationComponent& am = m_scene->Registry().get_or_emplace<AnimationComponent>(obj.Entity());
        // Клип и граф — взаимоисключающие источники позы: новый клип снимает граф.
        am.GraphPath.clear();
        am.ClipPath = ref;
        EditAnimation(objectId, ref);
        return true;
    }
    m_selection.SetPrimary(objectId);
    SetWorkspace(EditorWorkspace::Animation);
    m_panels[EditorPanel::Animation] = true;
    m_animation.RequestFocus();
    if (!m_animation.CreateClipFor(*this, obj)) return false;
    m_animStage.SetTarget(*this, objectId);
    return true;
}
