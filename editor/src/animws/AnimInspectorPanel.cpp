#include "AnimPanels.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "imgui.h"

#include "AnimClipNames.h"
#include "AnimStage.h"
#include "../AssetSlot.h"
#include "../EditorHost.h"
#include "../EditorIcons.h"
#include "../EditorSelection.h"
#include "../EditorTheme.h"
#include "../InspectorControls.h"
#include "../InspectorFields.h"
#include "../Localization.h"
#include "../Project.h"
#include "../ui/UI.h"
#include "sage/anim/AnimationComponents.h"
#include "sage/anim/ClipFile.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/assets/ModelData.h"
#include "sage/core/Paths.h"
#include "sage/render/SkinnedModel.h"
#include "sage/scene/Components.h"

namespace ictl = sage::editor::ictl;
namespace fs = std::filesystem;

namespace {

// Объект уровня с анимацией цели: сам объект или первый потомок с
// AnimationComponent (модель часто висит ребёнком у «персонажа»).
AnimationComponent* LevelAnim(EditorHost& host, int targetId, GameObject* outObj = nullptr) {
    Scene& level = host.LevelScene();
    GameObject t = level.Get(targetId);
    if (!t.Valid()) return nullptr;
    std::vector<entt::entity> stack{t.Entity()};
    while (!stack.empty()) {
        const entt::entity e = stack.front();
        stack.erase(stack.begin());
        if (auto* am = level.Registry().try_get<AnimationComponent>(e)) {
            if (outObj) *outObj = GameObject(&level.Registry(), e);
            return am;
        }
        for (entt::entity c : level.ChildrenOf(e)) stack.push_back(c);
    }
    return nullptr;
}

bool Animatable(entt::registry& reg, entt::entity e) {
    return reg.all_of<AnimationComponent>(e) || reg.all_of<PropertyAnimatorComponent>(e);
}

// Файл клипа модели, вынутый рядом с ней (ModelData): есть — путь в проекте.
std::string ModelClipFile(EditorHost& host, const std::string& modelRef, const std::string& clipName) {
    if (modelRef.empty()) return {};
    const fs::path model = sage::PathFromUtf8(sage::AssetDatabase::Instance().LocatePath(modelRef));
    const fs::path file = sage::assets::ModelDataPath(model, sage::assets::ModelData::Animations,
                                                      sage::anim::ClipFileName(model.stem().string(), clipName));
    std::error_code ec;
    return fs::exists(file, ec) ? host.CurrentProject().AssetRef(file) : std::string();
}

} // namespace

// --- Target -------------------------------------------------------------------

void AnimInspectorPanel::DrawTarget(EditorHost& host, AnimStage& stage) {
    if (!ictl::BeginSection("anim.target", T("Target"), true, "anim")) return;
    Scene& level = host.LevelScene();
    entt::registry& reg = level.Registry();
    GameObject cur = level.Get(stage.TargetId());
    const std::string preview = cur.Valid() ? cur.Name() : std::string(T("None"));
    // Список — объекты уровня, которые уже что-то анимируют: модель со
    // скелетом (Animation) или клип свойств (Property Animator).
    if (ictl::BeginDropdown(T("Object"), preview.c_str())) {
        if (ImGui::Selectable(T("None"), !cur.Valid())) stage.SetTarget(host, -1);
        for (auto e : reg.view<IdComponent>()) {
            if (!Animatable(reg, e)) continue;
            GameObject o(&reg, e);
            if (ImGui::Selectable((o.Name() + "##" + std::to_string(o.Id())).c_str(), o.Id() == stage.TargetId()))
                stage.SetTarget(host, o.Id());
        }
        ictl::EndDropdown();
    }
    // Любой объект уровня (в том числе ещё не анимированный) — тем, что
    // выбрано в сцене: клип свойств для него создаётся в таймлайне.
    GameObject sel = level.Get(host.Selection().Primary());
    ImGui::BeginDisabled(!sel.Valid() || sel.Id() == stage.TargetId());
    if (ictl::Button("select", T("Use selected object"), T("Work with the object selected in the scene")))
        stage.SetTarget(host, sel.Id());
    ImGui::EndDisabled();

    if (cur.Valid()) {
        if (const sage::anim::Skeleton* sk = stage.Skeleton()) {
            const AnimationComponent* am = stage.StageAnim();
            ImGui::TextDisabled("%s", T("Skeletal model"));
            ImGui::Text("%s %d   %s %d", T("Bones:"), sk->Count(), T("Clips in model:"),
                        am && am->Model ? (int)am->Model->Clips().size() : 0);
            if (am && !am->ResolvedFrom.empty()) ImGui::TextDisabled("%s", am->ResolvedFrom.c_str());
        } else if (LevelAnim(host, stage.TargetId())) {
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s",
                               T("Animation without a model with a skeleton in Mesh: nothing to animate"));
        } else {
            ImGui::TextDisabled("%s", T("Object: its properties and children are animated by a clip"));
        }
    }
    ictl::EndSection();
}

// --- Animation Source ---------------------------------------------------------

void AnimInspectorPanel::DrawSource(EditorHost& host, AnimStage& stage) {
    if (!ictl::BeginSection("anim.source", T("Animation Source"), true, "play")) return;
    Scene& level = host.LevelScene();
    GameObject animObj;
    AnimationComponent* real = LevelAnim(host, stage.TargetId(), &animObj);
    const AnimationComponent* view = stage.StageAnim();

    if (stage.TargetId() < 0) {
        ImGui::TextDisabled("%s", T("No animation target"));
        ictl::EndSection();
        return;
    }
    if (!real || !view || !view->Model) {
        // Цель без скелета: её клип свойств — в таймлайне.
        GameObject t = level.Get(stage.TargetId());
        const auto* pa = t.Valid() ? t.Registry()->try_get<PropertyAnimatorComponent>(t.Entity()) : nullptr;
        if (pa && !pa->ClipPath.empty()) {
            ImGui::TextUnformatted(pa->ClipPath.c_str());
            if (ictl::Button("open", T("Open in Timeline"))) host.OpenAnimationClip(pa->ClipPath);
        } else {
            ImGui::TextDisabled("%s", T("No clip yet: create it in Animation Timeline"));
        }
        ictl::EndSection();
        return;
    }

    // ИСТОЧНИК ПРЕДПРОСМОТРА: клип или граф. Граф доступен, если он есть у
    // объекта или открыт в окне графа.
    ictl::GroupItem modes[2];
    modes[0].Text = T("Clip");
    modes[0].Active = stage.Mode() == AnimStage::Source::Clip;
    modes[1].Text = T("Graph");
    modes[1].Active = stage.Mode() == AnimStage::Source::Graph;
    modes[1].Enabled = stage.Graph() != nullptr;
    modes[1].Tooltip = T("Play the animation graph in the preview");
    if (const int hit = ictl::ButtonGroup("##anim_src", modes, 2, ictl::ButtonLook::Standard); hit >= 0)
        stage.SetMode(hit == 0 ? AnimStage::Source::Clip : AnimStage::Source::Graph);

    // Клипы модели — по именам; есть вынутый файл — открывается в таймлайне
    // (правится как свой после «Сделать свой»), нет — только просмотр.
    // Имена в списке — отображаемые (короче и различимы, см. AnimClipNames.h);
    // настоящее имя — в подсказке, номера и ссылки клипов не меняются.
    ImGui::TextDisabled("%s", T("Clips"));
    const auto& clips = view->Model->Clips();
    std::vector<std::string> fullNames;
    fullNames.reserve(clips.size());
    for (const auto& c : clips) fullNames.push_back(c.Name);
    std::string prefix;
    const std::vector<std::string> shown = animnames::DisplayNames(fullNames, &prefix);
    if (clips.size() > 6)
        Sage::UI::SearchField("anim_clip_filter", m_clipFilter, sizeof(m_clipFilter), T("Search clips..."), -1.0f);
    if (!prefix.empty()) ImGui::TextDisabled("%s: %s", T("Common prefix"), prefix.c_str());
    // Высота списка ограничена: сотня клипов не должна занимать весь Inspector.
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    const int rows = std::clamp((int)clips.size(), 1, 8);
    ImGui::BeginChild("##anim_clip_list", ImVec2(0.0f, rowH * (float)rows + 8.0f), true);
    int listed = 0;
    for (int i = 0; i < (int)clips.size(); ++i) {
        if (m_clipFilter[0]) {
            // Поиск — по отображаемому и по настоящему имени, без учёта регистра.
            auto lower = [](std::string s) { for (char& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; };
            const std::string needle = lower(m_clipFilter);
            if (lower(shown[(size_t)i]).find(needle) == std::string::npos &&
                lower(fullNames[(size_t)i]).find(needle) == std::string::npos)
                continue;
        }
        ++listed;
        const std::string file = ModelClipFile(host, view->ResolvedFrom, clips[(size_t)i].Name);
        const bool current = file.empty() ? (stage.ModelClip() == i && stage.ClipPath().empty())
                                          : sage::AssetDatabase::Instance().LocatePath(stage.ClipPath()) ==
                                                sage::AssetDatabase::Instance().LocatePath(file);
        const std::string label = shown[(size_t)i] + "##mclip" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), current)) {
            stage.SetMode(AnimStage::Source::Clip);
            stage.ClearPose();
            if (!file.empty()) host.OpenAnimationClip(file);
            else stage.ShowModelClip(i);
        }
        if (current && m_scrollToCurrent) ImGui::SetScrollHereY(0.5f);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(fullNames[(size_t)i].c_str());
            ImGui::TextDisabled("%s", file.empty() ? T("Clip inside the model (not extracted to a file): view only")
                                                   : file.c_str());
            ImGui::EndTooltip();
        }
    }
    if (listed == 0) ImGui::TextDisabled("%s", T("No clips match"));
    ImGui::EndChild();
    m_scrollToCurrent = false;
    // Свои клипы (.sageanim) — слотом: брошенный файл открывается в таймлайне.
    const assetslot::Result r = assetslot::Draw(host, "anim_clip_open", assetslot::Kind::Animation, stage.ClipPath(),
                                                nullptr, T("Drop a .sageanim to view or edit it"));
    if (r.Changed && !r.Path.empty()) {
        stage.SetMode(AnimStage::Source::Clip);
        host.OpenAnimationClip(r.Path);
    }
    if (ictl::Button("plus", T("New Clip"), T("A new editable clip for this skeleton, in assets/animations"))) {
        const std::string ref = AnimStage::CreateEditableClip(host, animObj.Valid() ? animObj.Name() : std::string());
        if (!ref.empty()) {
            stage.SetMode(AnimStage::Source::Clip);
            host.OpenAnimationClip(ref);
        }
    }

    // Клип ОБЪЕКТА — то, что он играет в игре. Назначается явно: просмотр
    // другого клипа в пространстве объект не меняет.
    ImGui::Spacing();
    ImGui::TextDisabled("%s %s", T("Object plays:"),
                        real->ClipPath.empty() ? T("model clip by number") : real->ClipPath.c_str());
    ImGui::BeginDisabled(stage.ClipPath().empty() || stage.ClipPath() == real->ClipPath);
    if (ictl::Button("check", T("Assign to object"), T("The object plays this clip in the game"))) {
        host.PushUndoSnapshot();
        real->ClipPath = stage.ClipPath();
    }
    ImGui::EndDisabled();
    // Повтор и скорость — объекта: предпросмотр играет с ними же.
    sage::editor::ifield::Check(host, T("Loop"), &real->Loop);
    sage::editor::ifield::Slider(host, T("Speed"), &real->Speed, 0.0f, 3.0f);
    stage.SetLoop(real->Loop);
    stage.SetSpeed(real->Speed);
    ictl::EndSection();
}

// --- Animation Graph ----------------------------------------------------------

void AnimInspectorPanel::DrawGraph(EditorHost& host, AnimStage& stage) {
    if (!ictl::BeginSection("anim.graph", T("Animation Graph"), true, "wire")) return;
    AnimationComponent* real = LevelAnim(host, stage.TargetId());
    if (!real || !stage.Skeletal()) {
        ImGui::TextDisabled("%s", T("A graph plays on a model with a skeleton"));
        ictl::EndSection();
        return;
    }
    const assetslot::Result r = assetslot::Draw(host, "anim_graph", assetslot::Kind::AnimGraph, real->GraphPath,
                                                nullptr, T("No graph assigned to the object"));
    if (r.Changed) {
        host.PushUndoSnapshot();
        real->GraphPath = r.Path;
    }
    // Граф предпросмотра: открытый в окне графа (из памяти) или файл объекта.
    if (!real->GraphPath.empty() && (!stage.Graph() || (stage.GraphPath() != real->GraphPath && !m_graphFromPanel))) {
        try {
            stage.SetGraph(std::make_shared<const sage::anim::AnimGraph>(sage::anim::LoadAnimGraph(
                               sage::AssetDatabase::Instance().LocatePath(real->GraphPath))),
                           real->GraphPath);
        } catch (const std::exception& e) {
            ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s", e.what());
        }
    }
    // Открыть можно только назначенный граф (настоящий файл), и открытие идёт
    // общим путём — с целью предпросмотра и защитой несохранённого документа.
    if (!real->GraphPath.empty() && ictl::Button("wire", T("Open Graph Editor"), nullptr))
        host.EditAnimation(stage.TargetId(), real->GraphPath);
    // ПАРАМЕТРЫ ГРАФА — значения ПРЕДПРОСМОТРА: двигаешь — поза меняется сразу,
    // а объект уровня их не получает (в игре их пишут скрипты: SetFloat).
    if (const sage::anim::AnimGraph* g = stage.Graph()) {
        if (g->Params.empty()) {
            ImGui::TextDisabled("%s", T("The graph has no parameters"));
        } else {
            ImGui::TextDisabled("%s", T("Parameters (preview values)"));
            for (const sage::anim::AnimGraphParam& p : g->Params) {
                float v = stage.ParamValue(p.Name);
                const float lo = std::min(p.Min, p.Max), hi = std::max(p.Min, p.Max);
                if (ictl::SliderNumber(host, p.Name.c_str(), &v, lo, hi > lo ? hi : lo + 1.0f, "%.3f", true, p.Default)) {
                    stage.SetParamValue(p.Name, v);
                    stage.SetMode(AnimStage::Source::Graph);
                }
            }
        }
    }
    // Свойства узла, выбранного на холсте окна графа.
    if (host.AnimGraphPreviewActive()) {
        ImGui::Spacing();
        host.DrawToolInspector();
    }
    ictl::EndSection();
}

// --- Animation Layers ---------------------------------------------------------

void AnimInspectorPanel::DrawLayers(EditorHost& host, AnimStage& stage) {
    if (!ictl::BeginSection("anim.layers", T("Animation Layers"), false, "list")) return;
    AnimationComponent* real = LevelAnim(host, stage.TargetId());
    const AnimationComponent* view = stage.StageAnim();
    if (!real || !stage.Skeletal()) {
        ImGui::TextDisabled("%s", T("Layers blend clips on a model with a skeleton"));
        ictl::EndSection();
        return;
    }
    EditorTheme::Hint(T("Each layer plays its clip over the main pose: only its own channels, "
                        "scaled by its weight and limited by its mask"));
    int remove = -1;
    for (size_t i = 0; i < real->Layers.size(); ++i) {
        AnimationLayer& l = real->Layers[i];
        ImGui::PushID((int)i);
        char name[64];
        std::snprintf(name, sizeof(name), "%s", l.Name.c_str());
        const std::string title = (l.Name.empty() ? std::string(T("Layer")) : l.Name) + "###layer";
        if (ictl::SubSection(("anim.layer." + std::to_string(i)).c_str(), title.c_str(), true)) {
            if (ImGui::InputText(T("Name"), name, sizeof(name))) l.Name = name;
            if (ImGui::IsItemActivated()) host.PushUndoSnapshot();
            const assetslot::Result r = assetslot::Draw(host, "layer_clip", assetslot::Kind::Animation, l.ClipPath,
                                                        nullptr, T("Clip of the layer (.sageanim)"));
            if (r.Changed) {
                host.PushUndoSnapshot();
                l.ClipPath = r.Path;
            }
            sage::editor::ifield::Slider(host, T("Weight"), &l.Weight, 0.0f, 1.0f);
            ImGui::TextDisabled("%s %s", T("Mask:"),
                                l.Mask.empty() ? T("whole skeleton") : (std::to_string(l.Mask.size()) + " " + T("bones")).c_str());
            if (view && i < view->LayerStates.size()) {
                const AnimationLayerState& st = view->LayerStates[i];
                if (!l.ClipPath.empty() && !st.HasClip)
                    ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Danger), "%s", T("The clip is not read"));
                if (st.MissingBones > 0)
                    ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), T("Channels without a bone: %d"),
                                       st.MissingBones);
            }
            if (ictl::Button("trash", T("Remove Layer"))) remove = (int)i;
            ictl::EndSubSection();
        }
        ImGui::PopID();
    }
    if (remove >= 0) {
        host.PushUndoSnapshot();
        real->Layers.erase(real->Layers.begin() + remove);
        m_maskLayer = std::min(m_maskLayer, std::max(0, (int)real->Layers.size() - 1));
    }
    if (ictl::Button("plus", T("Add Layer"))) {
        host.PushUndoSnapshot();
        AnimationLayer l;
        l.Name = std::string("Layer ") + std::to_string(real->Layers.size() + 1);
        real->Layers.push_back(l);
        m_maskLayer = (int)real->Layers.size() - 1;
    }
    ictl::EndSection();
}

// --- Animation Masks ----------------------------------------------------------
//
// Маска — кости НАСТОЯЩЕГО скелета цели, галочками по его иерархии. Отмеченная
// кость берёт с собой всех потомков (они показаны отмеченными и выключенными):
// «от плеча» — вся рука. Имя, которого в скелете нет (переэкспорт), видно и
// убирается, а не тихо игнорируется.
void AnimInspectorPanel::DrawMasks(EditorHost& host, AnimStage& stage) {
    if (!ictl::BeginSection("anim.masks", T("Animation Masks"), false, "layout")) return;
    AnimationComponent* real = LevelAnim(host, stage.TargetId());
    const sage::anim::Skeleton* sk = stage.Skeleton();
    if (!real || !sk || real->Layers.empty()) {
        ImGui::TextDisabled("%s", T("A mask limits a layer to chosen bones: add a layer first"));
        ictl::EndSection();
        return;
    }
    m_maskLayer = std::clamp(m_maskLayer, 0, (int)real->Layers.size() - 1);
    std::vector<std::string> names;
    std::vector<const char*> items;
    for (const AnimationLayer& l : real->Layers) names.push_back(l.Name.empty() ? std::string(T("Layer")) : l.Name);
    for (const std::string& n : names) items.push_back(n.c_str());
    ictl::Combo(T("Layer"), &m_maskLayer, items.data(), (int)items.size());
    AnimationLayer& layer = real->Layers[(size_t)m_maskLayer];

    if (layer.Mask.empty()) ImGui::TextDisabled("%s", T("Empty mask: the layer affects the whole skeleton"));
    else if (ictl::Button("trash", T("Clear Mask"))) {
        host.PushUndoSnapshot();
        layer.Mask.clear();
    }
    // Имена, которых в скелете нет.
    for (size_t k = 0; k < layer.Mask.size(); ++k) {
        bool found = false;
        for (const sage::anim::Joint& j : sk->Joints) found = found || j.Name == layer.Mask[k];
        if (found) continue;
        ImGui::TextColored(EditorTheme::Color(EditorTheme::Role::Warn), "%s %s", T("No such bone:"), layer.Mask[k].c_str());
        ImGui::SameLine();
        ImGui::PushID((int)k);
        if (EditorIcons::IconOnlyButton("trash", T("Remove from the mask"))) {
            host.PushUndoSnapshot();
            layer.Mask.erase(layer.Mask.begin() + (long)k);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    // Кости — в порядке иерархии, отступом по глубине.
    const std::vector<float> covered = sage::anim::MaskWeights(*sk, layer.Mask);
    std::vector<int> depth((size_t)sk->Count(), 0);
    std::vector<int> order;
    std::vector<std::vector<int>> kids((size_t)sk->Count());
    for (int i = 0; i < sk->Count(); ++i) {
        const int p = sk->Joints[(size_t)i].Parent;
        if (p >= 0 && p < sk->Count()) kids[(size_t)p].push_back(i);
    }
    std::vector<int> stack;
    for (int i = sk->Count() - 1; i >= 0; --i)
        if (sk->Joints[(size_t)i].Parent < 0) stack.push_back(i);
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        order.push_back(i);
        for (auto it = kids[(size_t)i].rbegin(); it != kids[(size_t)i].rend(); ++it) {
            depth[(size_t)*it] = depth[(size_t)i] + 1;
            stack.push_back(*it);
        }
    }
    ImGui::BeginChild("##mask_bones", ImVec2(0, std::min(320.0f, 24.0f * (float)order.size() + 8.0f)),
                      ImGuiChildFlags_Borders);
    for (int i : order) {
        const std::string& name = sk->Joints[(size_t)i].Name;
        const bool own = std::find(layer.Mask.begin(), layer.Mask.end(), name) != layer.Mask.end();
        const bool inherited = !own && covered[(size_t)i] > 0.0f;
        ImGui::PushID(i);
        ImGui::Indent(12.0f * (float)depth[(size_t)i] + 1.0f);
        bool on = own || inherited;
        ImGui::BeginDisabled(inherited);
        if (ictl::Check(name.c_str(), &on)) {
            host.PushUndoSnapshot();
            if (on) layer.Mask.push_back(name);
            else layer.Mask.erase(std::remove(layer.Mask.begin(), layer.Mask.end(), name), layer.Mask.end());
        }
        ImGui::EndDisabled();
        if (i == stage.SelectedBone()) ImGui::SetItemDefaultFocus();
        ImGui::Unindent(12.0f * (float)depth[(size_t)i] + 1.0f);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ictl::EndSection();
}

void AnimInspectorPanel::Draw(EditorHost& host, bool* open, const std::string& windowId) {
    if (!*open) return;
    const std::string title = std::string(T("Animation Inspector")) + "###" + windowId;
    if (!ImGui::Begin(title.c_str(), open)) {
        ImGui::End();
        return;
    }
    AnimStage* stage = host.AnimationStage();
    if (!stage) {
        ImGui::End();
        return;
    }
    // Цель следует за выбором в сцене, если выбрали то, что анимируется:
    // персонаж, выбранный в Scene, — цель и здесь.
    Scene& level = host.LevelScene();
    const int selId = host.Selection().Primary();
    if (selId != m_lastSelection) {
        m_lastSelection = selId;
        GameObject sel = level.Get(selId);
        if (sel.Valid() && (Animatable(level.Registry(), sel.Entity()) || LevelAnim(host, selId)))
            stage->SetTarget(host, selId);
    }
    m_graphFromPanel = host.AnimGraphPreviewActive();
    DrawTarget(host, *stage);
    DrawSource(host, *stage);
    DrawGraph(host, *stage);
    DrawLayers(host, *stage);
    DrawMasks(host, *stage);
    ImGui::End();
}
