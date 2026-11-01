#include "ShaderParamsUi.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"

#include "AssetSlot.h"
#include "CreateAssetDialog.h"
#include "EditorHost.h"
#include "PostChainUi.h"
#include "Project.h"
#include "Localization.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/core/Paths.h"
#include "sage/render/Material.h"
#include "sage/render/Shader.h"
#include "sage/render/ShaderParams.h"
#include "ui/ColorPicker.h"

namespace fs = std::filesystem;
using sage::render::ShaderUniformInfo;

namespace sage::editor::shaderparamsui {
namespace {

// Разобранный текст файла: читается заново, только когда файл поменялся.
struct Parsed {
    fs::file_time_type Stamp{};
    bool Ok = false;
    std::vector<ShaderUniformInfo> Uniforms;
};

const std::vector<ShaderUniformInfo>& UniformsOf(const std::string& path) {
    static std::unordered_map<std::string, Parsed> cache;
    static const std::vector<ShaderUniformInfo> none;
    if (path.empty()) return none;
    const std::string located = sage::AssetDatabase::Instance().LocatePath(path);
    std::error_code ec;
    const fs::file_time_type stamp = fs::last_write_time(sage::PathFromUtf8(located), ec);
    Parsed& p = cache[located];
    if (!ec && (!p.Ok || p.Stamp != stamp)) {
        p.Stamp = stamp;
        p.Ok = true;
        try {
            p.Uniforms = sage::render::ParseShaderUniforms(Shader::LoadText(located));
        } catch (const std::exception&) {
            p.Uniforms.clear();   // файла нет — об этом говорит сам слот шейдера
        }
    }
    return p.Uniforms;
}

int Components(ShaderParam::Type t) {
    switch (t) {
        case ShaderParam::Type::Float: return 1;
        case ShaderParam::Type::Vec2: return 2;
        case ShaderParam::Type::Vec3: return 3;
        default: return 4;
    }
}

} // namespace

int Draw(EditorHost& host, const std::shared_ptr<Material>& material) {
    if (!material || !material->HasCustomShader()) return 0;
    // Оба файла: волна объявлена в вершинном, цвет — в пиксельном. Одно имя в
    // обоих — один параметр (юниформ общий для программы).
    std::vector<const ShaderUniformInfo*> list;
    for (const std::string* path : {&material->VertexShaderPath, &material->FragmentShaderPath})
        for (const ShaderUniformInfo& u : UniformsOf(*path)) {
            bool dup = false;
            for (const ShaderUniformInfo* seen : list) dup = dup || seen->Name == u.Name;
            if (!dup) list.push_back(&u);
        }
    if (list.empty()) {
        ImGui::TextDisabled("%s", T("The shader declares no parameters"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", T("Add a line to the shader:\n"
                                      "uniform float uWaveHeight; // 0.35 [0, 2] Wave height\n"
                                      "uniform vec3 uDeepColor; // color 0.02 0.16 0.28 Deep color"));
        return 0;
    }

    for (const ShaderUniformInfo* u : list) {
        ImGui::PushID(u->Name.c_str());
        auto it = material->Params.find(u->Name);
        // Не задан в материале — показываем умолчание шейдера (его же движок и
        // заливает); первая правка записывает значение в материал.
        ShaderParam value = it != material->Params.end() && it->second.Kind == u->Type
                                ? it->second : ShaderParam{u->Type, u->Default};
        const std::string label = u->Label.empty() ? sage::render::UniformLabel(u->Name) : u->Label;
        float* v = &value.Value.x;
        bool changed = false;
        const int n = Components(u->Type);
        if (n == 1) {
            if (u->HasRange) changed = ImGui::SliderFloat(label.c_str(), v, u->Min, u->Max);
            else changed = ImGui::DragFloat(label.c_str(), v, 0.01f);
        } else if (u->Color && n == 3) {
            changed = Sage::UI::ColorField3(label.c_str(), v);
        } else if (u->Color && n == 4) {
            changed = Sage::UI::ColorField4(label.c_str(), v);
        } else if (n == 2) {
            changed = ImGui::DragFloat2(label.c_str(), v, 0.01f);
        } else if (n == 3) {
            changed = ImGui::DragFloat3(label.c_str(), v, 0.01f);
        } else {
            changed = ImGui::DragFloat4(label.c_str(), v, 0.01f);
        }
        // Отмена — одним шагом на жест (тянули ползунок — одна запись).
        host.TrackLastImGuiItem();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(T("Shader uniform %s"), u->Name.c_str());
        if (changed) material->Params[u->Name] = value;
        ImGui::PopID();
    }
    return (int)list.size();
}

void InstallPostSourceSlot() {
    sage::editor::SetPostSourceSlot([](EditorHost* host, sage::render::PostEffect& effect) {
        if (!host) return false;
        // «+» у пустого слота — сразу шаблон экрана: созданный файл встаёт в слот.
        // Окно создания закрывается в другом кадре, и звено к тому времени
        // могло пропасть (сменили объект). Поэтому путь ждёт, пока это же звено
        // нарисуют снова, а не пишется по сохранённому указателю.
        static std::string pending;
        static sage::render::PostEffect* pendingFor = nullptr;
        bool changed = false;
        if (pendingFor == &effect && !pending.empty()) {
            host->PushUndoSnapshot();
            effect.Source = pending;
            pending.clear();
            pendingFor = nullptr;
            changed = true;
        }
        assetslot::Actions actions;
        actions.Create = [host, &effect] {
            sage::editor::createasset::Request req;
            req.Kind = sage::editor::createasset::What::Shader;
            req.Name = "MyScreenEffect";
            req.Template = "screen";
            req.ScreenOnly = true;
            sage::render::PostEffect* target = &effect;
            req.Done = [host, target](const fs::path& made) {
                std::string rel = sage::PathToUtf8(made);
                if (host->CurrentProject().Loaded()) {
                    std::error_code ec;
                    const fs::path r = fs::relative(made, host->CurrentProject().Dir(), ec);
                    if (!ec && !r.empty()) rel = r.generic_string();
                }
                pending = rel;
                pendingFor = target;
            };
            sage::editor::createasset::Open(req);
        };
        ImGui::TextUnformatted(T("Shader file"));
        const assetslot::Result r = assetslot::Draw(*host, "##post_shader", assetslot::Kind::Shader, effect.Source,
                                                    nullptr, T("Drop a .frag screen shader"), &actions);
        if (r.Changed) {
            host->PushUndoSnapshot();
            effect.Source = r.Path;
            changed = true;
        }
        return changed;
    });
}

} // namespace sage::editor::shaderparamsui
