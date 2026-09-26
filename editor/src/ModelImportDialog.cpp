#include "ModelImportDialog.h"

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>

#include "imgui.h"

#include "Localization.h"
#include "ModelMaterialImport.h"
#include "sage/core/Log.h"
#include "sage/render/ResourceManager.h"

namespace fs = std::filesystem;

namespace sage::editor::modelimport {

namespace {

struct State {
    bool Open = false;
    bool NeedsOpen = false;
    std::vector<fs::path> Queue;
    size_t Index = 0;
    ModelLoader::ImportSettings Current;
    bool Overwrite = false;
    bool ApplyToAll = false;
    std::function<void()> Then;
    // Последние подтверждённые настройки — стартовые для следующей модели без
    // своих: набор из магазина импортируют пачкой, и у всех его моделей одни и
    // те же беды (ось, масштаб, листва).
    ModelLoader::ImportSettings LastUsed;
};

State& S() {
    static State s;
    return s;
}

ModelLoader::ImportSettings StartingSettings(const fs::path& model) {
    std::error_code ec;
    if (fs::exists(ModelLoader::ImportSidecarPath(model.string()), ec))
        return ModelLoader::LoadImportSettings(model.string());
    return S().LastUsed;
}

void Confirm(const fs::path& model, const ModelLoader::ImportSettings& settings, bool overwrite,
             bool reload) {
    if (!ModelLoader::SaveImportSettings(model.string(), settings)) {
        LOG_ERROR("Editor") << "Настройки импорта не сохранились: " << model.string();
        return;
    }
    // Пересоздать материалы — удалить прежние .sagemat: существующий файл
    // импорт намеренно не перезаписывает (в нём могли быть ручные правки),
    // поэтому новые вырез и двусторонность иначе не доехали бы до сцены.
    if (overwrite) {
        const int removed = RemoveModelMaterialFiles(model.string());
        if (removed > 0)
            LOG_INFO("Editor") << "Материалы модели пересоздаются: " << model.filename().string()
                               << " (удалено файлов " << removed << ")";
    }
    // Модель уже могла лечь в кэш без этих настроек (стоит в сцене в другом
    // месте) — её ставят прямо сейчас, и новый экземпляр обязан быть с ними.
    if (reload) ResourceManager::Instance().ReloadModel(model.string());
    LOG_INFO("Editor") << "Настройки импорта модели: " << model.filename().string()
                       << " (масштаб " << settings.Scale << ")";
}

void Close(bool runThen) {
    State& s = S();
    std::function<void()> then = std::move(s.Then);
    s.Then = nullptr;
    s.Open = false;
    s.Queue.clear();
    s.Index = 0;
    ImGui::CloseCurrentPopup();
    if (runThen && then) then();
}

} // namespace

bool NeedsSettings(const fs::path& model) {
    if (model.empty() || !ModelLoader::IsSupportedModel(model.string())) return false;
    // Свой формат уже приведён при конвертации — спрашивать в нём нечего.
    if (model.extension() == ".sagemesh") return false;
    std::error_code ec;
    return !fs::exists(ModelLoader::ImportSidecarPath(model.string()), ec);
}

std::vector<fs::path> ModelsNeedingSettings(const fs::path& fileOrDir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (fs::is_directory(fileOrDir, ec)) {
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(
                 fileOrDir, fs::directory_options::skip_permission_denied, ec)) {
            std::error_code fec;
            if (e.is_regular_file(fec) && NeedsSettings(e.path())) out.push_back(e.path());
        }
        std::sort(out.begin(), out.end());
    } else if (NeedsSettings(fileOrDir)) {
        out.push_back(fileOrDir);
    }
    return out;
}

void Ask(std::vector<fs::path> models, std::function<void()> then) {
    if (models.empty()) {
        if (then) then();
        return;
    }
    State& s = S();
    s.Queue = std::move(models);
    s.Index = 0;
    s.Current = StartingSettings(s.Queue.front());
    s.Overwrite = false;
    s.ApplyToAll = false;
    s.Then = std::move(then);
    s.Open = true;
    s.NeedsOpen = true;
}

bool IsOpen() { return S().Open; }

bool DrawSettings(ModelLoader::ImportSettings& s, bool* overwriteMaterials) {
    bool changed = false;
    ImGui::PushItemWidth(220.0f);

    ImGui::SeparatorText(T("Transform"));
    changed |= ImGui::DragFloat(T("Scale"), &s.Scale, 0.01f, 0.0001f, 10000.0f, "%.4g");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("A model from a 3D editor is often 100 times too big or too small."));
    changed |= ImGui::DragFloat3(T("Rotation (degrees)"), &s.Rotation.x, 1.0f, -360.0f, 360.0f, "%.0f");
    // Самая частая беда чужой модели — ось Z вверх (3ds Max, старые экспортёры):
    // модель лежит на боку. Одна кнопка вместо поиска правильного угла.
    if (ImGui::SmallButton(T("Z up -> Y up"))) {
        s.Rotation = glm::vec3(-90.0f, 0.0f, 0.0f);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Reset rotation"))) {
        s.Rotation = glm::vec3(0.0f);
        changed = true;
    }
    changed |= ImGui::DragFloat3(T("Offset (m)"), &s.Offset.x, 0.01f);
    changed |= ImGui::Checkbox(T("Move the centre to the origin"), &s.Recenter);
    changed |= ImGui::Checkbox(T("Fit into one metre"), &s.NormalizeSize);
    changed |= ImGui::Checkbox(T("Centimetres to metres (characters)"), &s.AutoUnits);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("A character taller than 50 m was exported in centimetres:\n"
                                  "it is scaled down 100 times. Static models are not touched."));

    ImGui::SeparatorText(T("Geometry"));
    const char* normals[] = {T("From the file"), T("Recompute smooth"), T("Recompute flat")};
    int n = (int)s.Normals;
    if (ImGui::Combo(T("Normals"), &n, normals, 3)) {
        s.Normals = (ModelLoader::ImportSettings::NormalMode)n;
        changed = true;
    }
    changed |= ImGui::Checkbox(T("Flip the texture vertically"), &s.FlipUV);
    changed |= ImGui::Checkbox(T("Flip faces (the model is inside out)"), &s.FlipWinding);

    ImGui::SeparatorText(T("Materials"));
    changed |= ImGui::Checkbox(T("Create materials from the file"), &s.ImportMaterials);
    ImGui::BeginDisabled(!s.ImportMaterials);
    const char* alpha[] = {T("Automatic"), T("Opaque"), T("Alpha cutout (foliage, grass)")};
    int a = (int)s.Alpha;
    if (ImGui::Combo(T("Transparency"), &a, alpha, 3)) {
        s.Alpha = (ModelLoader::ImportSettings::AlphaMode)a;
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T("Automatic: as the file describes it, and a texture whose alpha\n"
                                  "cuts out leaves is recognised by its pixels."));
    if (s.Alpha == ModelLoader::ImportSettings::AlphaMode::Cutout)
        changed |= ImGui::SliderFloat(T("Cutout threshold"), &s.AlphaCutoff, 0.01f, 0.99f, "%.2f");
    const char* sides[] = {T("Automatic"), T("Both sides"), T("Front side only")};
    int d = (int)s.DoubleSided;
    if (ImGui::Combo(T("Visible sides"), &d, sides, 3)) {
        s.DoubleSided = (ModelLoader::ImportSettings::TwoSided)d;
        changed = true;
    }
    if (overwriteMaterials) {
        ImGui::Checkbox(T("Recreate existing materials"), overwriteMaterials);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Materials already created from this model are kept as they are\n"
                                      "(they may hold your edits). Tick this to build them anew\n"
                                      "with the settings above."));
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText(T("Animation"));
    changed |= ImGui::Checkbox(T("Import skeleton and animations"), &s.ImportAnimation);

    ImGui::PopItemWidth();
    return changed;
}

void Draw() {
    State& s = S();
    if (!s.Open) return;
    if (s.NeedsOpen) {
        ImGui::OpenPopup("###model_import");
        s.NeedsOpen = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const std::string title = std::string(T("Model import settings")) + "###model_import";
    if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Окно закрыли не нашими кнопками (например, сменился проект) —
        // очередь сбрасывается, чтобы не висеть открытой навсегда.
        if (!ImGui::IsPopupOpen("###model_import")) s.Open = false;
        return;
    }
    if (s.Index >= s.Queue.size()) {
        Close(true);
        ImGui::EndPopup();
        return;
    }

    const fs::path& model = s.Queue[s.Index];
    ImGui::TextUnformatted(model.filename().string().c_str());
    if (s.Queue.size() > 1) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu / %zu)", s.Index + 1, s.Queue.size());
    }
    DrawSettings(s.Current, &s.Overwrite);

    const size_t remaining = s.Queue.size() - s.Index;
    if (remaining > 1) {
        ImGui::Spacing();
        const std::string label = std::string(T("Apply to all remaining models")) + " (" +
                                  std::to_string(remaining) + ")";
        ImGui::Checkbox(label.c_str(), &s.ApplyToAll);
    }

    ImGui::Separator();
    const bool reload = static_cast<bool>(s.Then);
    ImGui::SetItemDefaultFocus();
    if (ImGui::Button(T("Import"), ImVec2(140, 0))) {
        s.LastUsed = s.Current;
        const size_t last = s.ApplyToAll ? s.Queue.size() : s.Index + 1;
        for (size_t i = s.Index; i < last; ++i) Confirm(s.Queue[i], s.Current, s.Overwrite, reload);
        s.Index = last;
        if (s.Index >= s.Queue.size()) {
            Close(true);
        } else {
            s.Current = StartingSettings(s.Queue[s.Index]);
            s.Overwrite = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Cancel"), ImVec2(140, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        Close(false);
    }
    ImGui::EndPopup();
}

} // namespace sage::editor::modelimport
