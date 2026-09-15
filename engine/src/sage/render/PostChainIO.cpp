#include "sage/render/PostChainIO.h"

namespace sage::render {

namespace {

// Описание параметра по имени. Нужно, чтобы знать ТИП значения: в JSON у числа,
// вектора и цвета разная форма, а в звене всё лежит в одном PostValue.
const PostParamDesc* ParamByName(const PostEffect& effect, const char* name) {
    const PostEffectKind* kind = PostEffectCatalog::Instance().Find(effect.Kind);
    if (!kind) return nullptr;
    for (const PostParamDesc& desc : kind->Params)
        if (desc.Name == name) return &desc;
    return nullptr;
}

} // namespace

nlohmann::json PostChainToJson(const PostChain& chain) {
    nlohmann::json out;
    nlohmann::json effects = nlohmann::json::array();
    for (const PostEffect& effect : chain.Effects) {
        const PostEffectKind* kind = PostEffectCatalog::Instance().Find(effect.Kind);
        // Звено вида, которого больше нет в каталоге (сцена от другой версии или
        // от другой игры), ВСЁ РАВНО сохраняется: иначе открыть и перезаписать
        // такую сцену значило бы молча потерять чужие звенья. Восстановить его
        // нечем, но и терять не за что.
        nlohmann::json params = nlohmann::json::object();
        if (kind) {
            for (size_t i = 0; i < kind->Params.size() && i < effect.Values.size(); ++i) {
                const PostParamDesc& desc = kind->Params[i];
                const PostValue& value = effect.Values[i];
                switch (desc.Type) {
                case PostParamType::Heading:
                    break; // у подписи-разделителя значения нет
                case PostParamType::Bool:
                    params[desc.Name] = value.B;
                    break;
                case PostParamType::Vec2:
                    params[desc.Name] = {value.V[0], value.V[1]};
                    break;
                case PostParamType::Color:
                    params[desc.Name] = {value.V[0], value.V[1], value.V[2], value.V[3]};
                    break;
                default:
                    params[desc.Name] = value.V[0];
                    break;
                }
            }
        }
        effects.push_back(
            {{"kind", effect.Kind}, {"enabled", effect.Enabled}, {"params", std::move(params)}});
    }
    out["effects"] = std::move(effects);
    return out;
}

PostChain PostChainFromJson(const nlohmann::json& json) {
    PostChain chain;
    if (!json.is_object()) return chain;
    const nlohmann::json effects = json.value("effects", nlohmann::json::array());
    if (!effects.is_array()) return chain;

    for (const nlohmann::json& entry : effects) {
        if (!entry.is_object()) continue;
        PostEffect effect = MakePostEffect(entry.value("kind", std::string()));
        effect.Enabled = entry.value("enabled", true);

        const nlohmann::json params = entry.value("params", nlohmann::json::object());
        if (params.is_object()) {
            for (auto it = params.begin(); it != params.end(); ++it) {
                PostValue* value = effect.Find(it.key().c_str());
                const PostParamDesc* desc = ParamByName(effect, it.key().c_str());
                if (!value || !desc) continue; // параметр исчез из вида — не ошибка файла
                const nlohmann::json& text = it.value();
                if (desc->Type == PostParamType::Bool) {
                    value->B = text.is_boolean()
                                   ? text.get<bool>()
                                   : (text.is_number() && text.get<float>() != 0.0f);
                    value->V[0] = value->B ? 1.0f : 0.0f;
                } else if (text.is_number()) {
                    value->V[0] = text.get<float>();
                } else if (text.is_array()) {
                    for (size_t i = 0; i < text.size() && i < 4; ++i)
                        if (text[i].is_number()) value->V[i] = text[i].get<float>();
                }
            }
        }
        chain.Effects.push_back(std::move(effect));
    }
    return chain;
}

} // namespace sage::render
