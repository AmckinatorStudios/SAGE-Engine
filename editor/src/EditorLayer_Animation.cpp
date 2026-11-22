// Ресурс анимации объекта — в редактор одним действием (кнопки компонентов в
// Inspector). Путь один и тот же для «Edit» и «Create Clip»: пространство
// Animation, объект — цель предпросмотра (AnimStage), ресурс открыт там, где он
// правится. Создание идёт уже существующими путями: свой скелетный клип —
// AnimStage::CreateEditableClip, клип свойств — AnimationPanel::CreateClipFor
// (файл + компонент), второго способа завести клип нет.
#include "EditorLayer.h"

#include <filesystem>
#include <system_error>

#include "imgui.h"
#include "Localization.h"

#include "sage/anim/AnimationComponents.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Paths.h"

namespace {
// Один и тот же файл, как бы ни был записан путь (ссылка проекта, абсолютный,
// разные разделители).
bool SameFile(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    std::error_code ec;
    const std::filesystem::path pa = std::filesystem::weakly_canonical(sage::PathFromUtf8(a), ec);
    const std::filesystem::path pb = std::filesystem::weakly_canonical(sage::PathFromUtf8(b), ec);
    return pa == pb;
}
} // namespace

void EditorLayer::OpenAnimClipNow(const std::string& clipPath) {
    if (m_workspace != EditorWorkspace::Interface) SetWorkspace(EditorWorkspace::Animation);
    m_panels[EditorPanel::Animation] = true;
    m_animation.RequestFocus();
    m_animation.OpenClip(*this, clipPath);
}

void EditorLayer::OpenAnimGraphNow(const std::string& graphPath) {
    SetWorkspace(EditorWorkspace::Animation);
    m_panelRegistry.SetVisible("AnimGraph", true);
    m_animGraph.Open(*this, graphPath);
}

void EditorLayer::OpenAnimationClip(const std::string& clipPath) {
    GuardAnimDocument(clipPath, [this, clipPath] { OpenAnimClipNow(clipPath); });
}

void EditorLayer::OpenAnimGraph(const std::string& graphPath) {
    GuardAnimDocument(graphPath, [this, graphPath] { OpenAnimGraphNow(graphPath); });
}

void EditorLayer::GuardAnimDocument(const std::string& targetFile, std::function<void()> proceed) {
    // Документы пространства — клип в таймлайне и граф в окне графа; у каждого
    // свой флаг правок (m_dirty), второй системы учёта нет. Мешает только
    // несохранённый документ, который НЕ тот, что открывают.
    const bool clipBlocks = m_animation.Dirty() && !SameFile(m_animation.DocumentPath(), targetFile);
    const bool graphBlocks = m_animGraph.Dirty() && !SameFile(m_animGraph.Path(), targetFile);
    if (!clipBlocks && !graphBlocks) {
        proceed();
        return;
    }
    m_animOpenProceed = std::move(proceed);
    m_animOpenPrompt = true;
}

void EditorLayer::DrawAnimOpenPrompt() {
    if (!m_animOpenPrompt) return;
    const char* const kId = T("Unsaved animation" "###AnimOpenUnsaved");
    if (!ImGui::IsPopupOpen(kId)) ImGui::OpenPopup(kId);
    if (!ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (m_animation.Dirty()) {
        const std::string n = m_animation.DocumentPath().empty()
                                  ? std::string(T("New clip"))
                                  : sage::PathToUtf8(std::filesystem::path(sage::PathFromUtf8(m_animation.DocumentPath())).filename());
        ImGui::Text(T("Clip \"%s\" has unsaved changes."), n.c_str());
    }
    if (m_animGraph.Dirty()) {
        const std::string n = sage::PathToUtf8(std::filesystem::path(sage::PathFromUtf8(m_animGraph.Path())).filename());
        ImGui::Text(T("Graph \"%s\" has unsaved changes."), n.c_str());
    }
    ImGui::TextDisabled("%s", T("Save them before opening another document?"));
    ImGui::Spacing();

    if (ImGui::Button(T("Save"), ImVec2(120, 0))) AnswerAnimOpenPrompt(AnimOpenAnswer::Save);
    ImGui::SameLine();
    if (ImGui::Button(T("Discard"), ImVec2(120, 0))) AnswerAnimOpenPrompt(AnimOpenAnswer::Discard);
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(120, 0))) AnswerAnimOpenPrompt(AnimOpenAnswer::Cancel);
    if (!m_animOpenPrompt) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void EditorLayer::AnswerAnimOpenPrompt(AnimOpenAnswer answer) {
    if (!m_animOpenPrompt) return;
    bool proceed = false;
    switch (answer) {
        case AnimOpenAnswer::Save: {
            // Новый документ открывается только после УСПЕШНОЙ записи обоих:
            // иначе «сохранить и перейти» теряло бы то, что не записалось.
            bool ok = true;
            if (m_animation.Dirty()) ok = m_animation.SaveCurrent(*this);
            if (ok && m_animGraph.Dirty()) ok = m_animGraph.Save(*this);
            if (!ok) SetStatusMessage(T("Could not save: staying on the current document"));
            proceed = ok;
            break;
        }
        case AnimOpenAnswer::Discard:
            if (m_animation.Dirty()) m_animation.DiscardChanges(*this);
            if (m_animGraph.Dirty()) m_animGraph.DiscardChanges(*this);
            proceed = true;
            break;
        case AnimOpenAnswer::Cancel: break;
    }
    m_animOpenPrompt = false;
    auto action = std::move(m_animOpenProceed);
    m_animOpenProceed = nullptr;
    if (proceed && action) action();
}

void EditorLayer::EditAnimation(int objectId, const std::string& resource) {
    GameObject obj = m_scene->Get(objectId);
    if (!obj.Valid()) return;
    const std::string full = resource.empty() ? std::string() : sage::AssetDatabase::Instance().LocatePath(resource);
    // Всё — внутри продолжения: отказ в диалоге оставляет пространство, цель
    // предпросмотра и документ ровно как были. Источник воспроизведения в
    // компоненте не трогается вовсе: правка ресурса — не его переключение.
    const auto go = [this, objectId, full] {
        m_selection.SetPrimary(objectId);
        SetWorkspace(EditorWorkspace::Animation);
        m_animStage.SetTarget(*this, objectId);
        if (full.empty()) return;
        if (std::filesystem::path(sage::PathFromUtf8(full)).extension() == ".sageanimgraph") {
            OpenAnimGraphNow(full);
            m_animStage.SetMode(AnimStage::Source::Graph);
        } else {
            m_animStage.SetMode(AnimStage::Source::Clip);
            OpenAnimClipNow(full);
        }
    };
    if (full.empty()) go();
    else GuardAnimDocument(full, go);
}

bool EditorLayer::CreateAnimationClip(int objectId, bool skeletal) {
    GameObject obj = m_scene->Get(objectId);
    if (!obj.Valid()) return false;
    if (skeletal) {
        const std::string ref = AnimStage::CreateEditableClip(*this, obj.Name());
        if (ref.empty()) return false;
        PushUndoSnapshot();
        AnimationComponent& am = m_scene->Registry().get_or_emplace<AnimationComponent>(obj.Entity());
        // Новый клип встаёт в слот Clip и выбирает его источником; Graph и его
        // путь остаются как были.
        am.Source = AnimationComponent::SourceKind::Clip;
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
