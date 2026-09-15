#include "PostChainUi.h"

#include <algorithm>
#include <string>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorTheme.h"
#include "Localization.h"

namespace sage::editor {

namespace {

// Один параметр звена. Виджет выбирается по ТИПУ из описания — панель не знает,
// что такое «порог свечения», и не должна: она знает, что это число в диапазоне.
void DrawParam(EditorHost* host, sage::render::PostEffect& effect,
               const sage::render::PostParamDesc& desc, bool& changed) {
    using namespace sage::render;
    PostValue* value = effect.Find(desc.Name.c_str());
    if (!value) return;
    const char* label = T(desc.Label);

    // Подпись звена и параметра приходит из ДВИЖКА — строкой во время работы, а
    // не литералом здесь. T() такую строку переводит (ключ — сам английский
    // текст), а если перевода нет — отдаёт как есть, и ключ попадает в
    // MissingKeys(): непереведённая подпись видна, а не молчит.
    switch (desc.Type) {
    case PostParamType::Heading:
        // Подпись-разделитель: значения у неё нет, есть только место в списке.
        ImGui::SeparatorText(label);
        return;
    case PostParamType::Bool: {
        bool on = value->B;
        if (ImGui::Checkbox(label, &on)) {
            if (host) host->PushUndoSnapshot();
            value->B = on;
            value->V[0] = on ? 1.0f : 0.0f;
            changed = true;
        }
        break;
    }
    case PostParamType::Int: {
        int number = (int)value->V[0];
        if (ImGui::DragInt(label, &number, 1.0f, (int)desc.Min, (int)desc.Max)) {
            value->V[0] = (float)number;
            changed = true;
        }
        if (host) host->TrackLastImGuiItem();
        break;
    }
    case PostParamType::Vec2: {
        float pair[2] = {value->V[0], value->V[1]};
        if (ImGui::DragFloat2(label, pair, 0.01f, desc.Min, desc.Max)) {
            value->V[0] = pair[0];
            value->V[1] = pair[1];
            changed = true;
        }
        if (host) host->TrackLastImGuiItem();
        break;
    }
    case PostParamType::Color: {
        float rgba[4] = {value->V[0], value->V[1], value->V[2], value->V[3]};
        if (ImGui::ColorEdit4(label, rgba)) {
            for (int c = 0; c < 4; ++c) value->V[c] = rgba[c];
            changed = true;
        }
        if (host) host->TrackLastImGuiItem();
        break;
    }
    default: {
        float number = value->V[0];
        const float span = desc.Max - desc.Min;
        const float step = span > 0.0f ? span * 0.002f : 0.01f;
        if (ImGui::DragFloat(label, &number, step, desc.Min, desc.Max)) {
            value->V[0] = number;
            changed = true;
        }
        if (host) host->TrackLastImGuiItem();
        break;
    }
    }
    // Подсказка звена — текст ДВИЖКА и по-русски: он не переводится, как не
    // переводятся имена иконок. Подпись переводится, описание — нет.
    if (!desc.Hint.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", desc.Hint.c_str());
}

} // namespace

bool DrawPostChainEditor(EditorHost* host, sage::render::PostChain& chain, const char* idScope) {
    using namespace sage::render;
    bool changed = false;

    ImGui::PushID(idScope ? idScope : "postChain");

    // ИТОГ КОМПИЛЯЦИИ — ДО СПИСКА. Если тракт неверен (сглаживание перед
    // тон-маппингом, два тон-маппинга, звено неизвестного вида), человек должен
    // узнать это ЗДЕСЬ, где он его собирает, а не по странной картинке.
    const PostChainReport report = chain.Compile();
    if (!report.Ok) {
        ImGui::TextDisabled("%s", T("The chain will not run:"));
        ImGui::TextWrapped("%s", report.Error.c_str());
    } else if (chain.Empty()) {
        ImGui::TextDisabled("%s", T("Empty chain: the frame is only tone-mapped"));
    }

    int moveFrom = -1, moveTo = -1, removeAt = -1;
    for (size_t i = 0; i < chain.Effects.size(); ++i) {
        PostEffect& effect = chain.Effects[i];
        const PostEffectKind* kind = PostEffectCatalog::Instance().Find(effect.Kind);
        ImGui::PushID((int)i);

        bool enabled = effect.Enabled;
        if (ImGui::Checkbox("##enabled", &enabled)) {
            if (host) host->PushUndoSnapshot();
            effect.Enabled = enabled;
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Enabled"));

        ImGui::SameLine();
        if (ImGui::TreeNodeEx("##effect", ImGuiTreeNodeFlags_SpanAvailWidth, "%s",
                              kind ? T(kind->Label) : T("Unknown effect"))) {
            if (kind) {
                if (!kind->Hint.empty()) EditorTheme::Hint(kind->Hint.c_str());
                for (const PostParamDesc& param : kind->Params)
                    DrawParam(host, effect, param, changed);
            } else {
                // Звено вида, которого нет в каталоге: из файла другой версии или
                // другой игры. Оно не пропадает молча — его видно, и компилятор о
                // нём уже сказал выше.
                ImGui::TextDisabled("%s", T("Unknown effect"));
            }
            ImGui::TreePop();
        }

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.0f);
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("^")) {
            moveFrom = (int)i;
            moveTo = (int)i - 1;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Move up"));
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 >= chain.Effects.size());
        if (ImGui::SmallButton("v")) {
            moveFrom = (int)i;
            moveTo = (int)i + 1;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Move down"));
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) removeAt = (int)i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Remove"));
        ImGui::PopID();
    }

    if (removeAt >= 0) {
        if (host) host->PushUndoSnapshot();
        chain.Effects.erase(chain.Effects.begin() + removeAt);
        changed = true;
    } else if (moveFrom >= 0 && moveTo >= 0) {
        if (host) host->PushUndoSnapshot();
        std::swap(chain.Effects[(size_t)moveFrom], chain.Effects[(size_t)moveTo]);
        changed = true;
    }

    // Добавление звена. Неповторяемое, которое уже стоит, в списке не
    // предлагается: предлагать заведомо неверный тракт — не забота человека.
    if (ImGui::BeginCombo(T("Add effect"), T("Choose an effect"))) {
        for (const PostEffectKind* kind : PostEffectCatalog::Instance().All()) {
            bool already = false;
            for (const PostEffect& existing : chain.Effects)
                if (existing.Kind == kind->Id) already = true;
            if (already && !kind->Repeatable) continue;
            if (ImGui::Selectable(T(kind->Label))) {
                if (host) host->PushUndoSnapshot();
                // Место выбирает движок: звено, читающее HDR, встанет до
                // тон-маппинга. Иначе «добавил эффект» означало бы «получил
                // отказ компилятора».
                AddPostEffect(chain, kind->Id);
                changed = true;
            }
            if (!kind->Hint.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", kind->Hint.c_str());
        }
        ImGui::EndCombo();
    }

    ImGui::PopID();
    return changed;
}

} // namespace sage::editor
