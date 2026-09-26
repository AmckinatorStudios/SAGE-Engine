#include "PostChainUi.h"

#include <algorithm>
#include <string>
#include <vector>

#include "imgui.h"

#include "EditorHost.h"
#include "EditorTheme.h"
#include "Localization.h"
#include "ui/ColorPicker.h"

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
    // Своя область имён на каждый параметр. Подпись параметра совпадает с
    // подписью эффекта («Экспозиция» у звена и у его ползунка), и в одной
    // области ImGui это ОДИН идентификатор на два виджета: ImGui ругался
    // «conflicting ID», а щелчок по ползунку снимал галку эффекта.
    ImGui::PushID(desc.Name.empty() ? desc.Label.c_str() : desc.Name.c_str());
    struct PopOnExit { ~PopOnExit() { ImGui::PopID(); } } popOnExit;

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
    case PostParamType::Enum: {
        // Числом режим в интерфейсе не показывается: «2» не говорит ничего,
        // «ACES» говорит всё. Подписи вариантов — ключи перевода, как и подпись
        // самого параметра.
        std::vector<const char*> names;
        names.reserve(desc.Options.size());
        for (const std::string& option : desc.Options) names.push_back(T(option));
        int choice = (int)value->V[0];
        if (!names.empty() &&
            ImGui::Combo(label, &choice, names.data(), (int)names.size())) {
            if (host) host->PushUndoSnapshot();
            value->V[0] = (float)choice;
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
        if (Sage::UI::ColorField4(label, rgba)) {
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

    // ИТОГ ПРОВЕРКИ — ДО РАЗДЕЛОВ. Порядок тракта человек больше испортить не
    // может (его задают этапы), но звено неизвестного вида — из сцены другой
    // версии или другой игры — остаётся возможным, и узнать об этом надо здесь,
    // а не по странной картинке.
    const PostChainReport report = chain.Compile();
    if (!report.Ok) {
        ImGui::TextDisabled("%s", T("The chain will not run:"));
        ImGui::TextWrapped("%s", report.Error.c_str());
    }

    const PostEffectCatalog& catalog = PostEffectCatalog::Instance();

    // Порядок разделов — порядок ОБРАБОТКИ кадра. Тот же, в котором звенья
    // исполняются (PostChain::Ordered), и тот же, в котором они перечислены в
    // каталоге движка.
    static const PostStage kStages[] = {PostStage::Exposure, PostStage::Depth,  PostStage::Bloom,
                                        PostStage::Color,    PostStage::Grading, PostStage::Tonemap,
                                        PostStage::Lens,     PostStage::Film,   PostStage::Output};

    int removeAt = -1;
    for (PostStage stage : kStages) {
        // Какие виды живут на этом этапе — спрашиваем каталог, а не помним
        // здесь: эффект, принесённый игрой, обязан появиться сам.
        std::vector<const PostEffectKind*> kinds;
        for (const PostEffectKind* kind : catalog.All())
            if (kind->Stage == stage) kinds.push_back(kind);
        if (kinds.empty()) continue;

        ImGui::SeparatorText(T(PostStageLabel(stage)));
        for (const PostEffectKind* kind : kinds) {
            ImGui::PushID(kind->Id.c_str());

            // Звено этого вида в тракте — или его там нет, и тогда эффект
            // показан выключенным. ЭТО И ЕСТЬ ГЛАВНОЕ РЕШЕНИЕ ЭКРАНА: человек
            // видит ВСЕ эффекты движка и сразу понимает, какие включены, — а не
            // ищет в отдельном списке, что ещё можно добавить.
            int index = -1;
            for (size_t i = 0; i < chain.Effects.size(); ++i)
                if (chain.Effects[i].Kind == kind->Id) { index = (int)i; break; }

            // Включено — если включена ХОТЬ ОДНА копия звена. Старые сцены и
            // скрипты могли положить звено в тракт дважды, а галка правила
            // только первую копию: снимаешь её — эффект остаётся, вторая
            // копия продолжает работать.
            bool on = false;
            for (const PostEffect& e : chain.Effects)
                if (e.Kind == kind->Id && e.Enabled) on = true;
            if (ImGui::Checkbox(T(kind->Label), &on)) {
                if (host) host->PushUndoSnapshot();
                if (index >= 0) {
                    for (PostEffect& e : chain.Effects)
                        if (e.Kind == kind->Id) e.Enabled = on;
                    if (on) {
                        // Правится первая включённая копия — её и показываем.
                        for (size_t i = 0; i < chain.Effects.size(); ++i)
                            if (chain.Effects[i].Kind == kind->Id) { index = (int)i; break; }
                    }
                } else if (on) {
                    // Место в списке роли не играет — когда звено выполнится,
                    // решает его этап.
                    AddPostEffect(chain, kind->Id);
                    index = (int)chain.Effects.size() - 1;
                }
                changed = true;
            }
            if (!kind->Hint.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", kind->Hint.c_str());

            // Настройки — ТОЛЬКО У ВКЛЮЧЁННОГО. Выключенный эффект показывает
            // ровно одну строку: список, где видно два десятка ползунков сразу,
            // не отвечает ни на один вопрос, ради которого его открыли.
            if (on && index >= 0) {
                ImGui::Indent();
                PostEffect& effect = chain.Effects[(size_t)index];
                for (const PostParamDesc& param : kind->Params)
                    DrawParam(host, effect, param, changed);
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
    }

    // Звенья видов, которых в каталоге нет: из сцены другой версии или другой
    // игры. Их не показать нельзя — иначе они пропадут при первом сохранении, —
    // но и настроить нечем: описания параметров у нас нет.
    for (size_t i = 0; i < chain.Effects.size(); ++i) {
        if (catalog.Find(chain.Effects[i].Kind)) continue;
        ImGui::PushID((int)i);
        ImGui::SeparatorText(T("Unknown effect"));
        ImGui::TextDisabled("%s", chain.Effects[i].Kind.c_str());
        ImGui::SameLine();
        if (ImGui::Button(T("Remove"))) removeAt = (int)i;
        ImGui::PopID();
    }
    if (removeAt >= 0) {
        if (host) host->PushUndoSnapshot();
        chain.Effects.erase(chain.Effects.begin() + removeAt);
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

} // namespace sage::editor
