#include "sage/render/PostEffect.h"
#include "sage/core/EngineContext.h"

#include <algorithm>

namespace sage::render {

// ============================================================================
//  Каталог видов
// ============================================================================

PostEffectCatalog::PostEffectCatalog() { RegisterBuiltinPostEffects(*this); }

PostEffectCatalog& PostEffectCatalog::Instance() {
    return sage::EngineContext::Current().PostEffects();
}

void PostEffectCatalog::Register(PostEffectKind kind) {
    for (PostEffectKind& existing : m_kinds) {
        if (existing.Id == kind.Id) {
            // Замена НА МЕСТЕ: переопределивший встроенный эффект обязан встать
            // туда же, где стоял исходный, иначе список «что можно добавить» в
            // интерфейсе перестраивался бы от чужой регистрации.
            existing = std::move(kind);
            return;
        }
    }
    m_kinds.push_back(std::move(kind));
}

const PostEffectKind* PostEffectCatalog::Find(const std::string& id) const {
    for (const PostEffectKind& k : m_kinds)
        if (k.Id == id) return &k;
    return nullptr;
}

std::vector<const PostEffectKind*> PostEffectCatalog::All() const {
    std::vector<const PostEffectKind*> out;
    out.reserve(m_kinds.size());
    for (const PostEffectKind& k : m_kinds) out.push_back(&k);
    return out;
}

void PostEffectCatalog::Clear() { m_kinds.clear(); }

PostEffect MakePostEffect(const std::string& kindId) {
    PostEffect e;
    e.Kind = kindId;
    if (const PostEffectKind* k = PostEffectCatalog::Instance().Find(kindId)) {
        e.Values.resize(k->Params.size());
        for (size_t i = 0; i < k->Params.size(); ++i) {
            const PostParamDesc& p = k->Params[i];
            for (int c = 0; c < 4; ++c) e.Values[i].V[c] = p.Default[c];
            e.Values[i].B = p.Default[0] != 0.0f;
        }
    }
    return e;
}

// ============================================================================
//  Значения звена
// ============================================================================

namespace {

// Индекс параметра по имени. Неизвестное имя — -1: молча вернуть «значение по
// умолчанию» значило бы скрыть опечатку в имени параметра, а это ровно тот
// класс ошибок, который потом ищут днями.
int ParamIndex(const PostEffect& e, const char* name) {
    if (!name) return -1;
    const PostEffectKind* k = PostEffectCatalog::Instance().Find(e.Kind);
    if (!k) return -1;
    for (size_t i = 0; i < k->Params.size(); ++i)
        if (k->Params[i].Name == name) return (int)i;
    return -1;
}

} // namespace

PostValue* PostEffect::Find(const char* name) {
    const int i = ParamIndex(*this, name);
    return (i >= 0 && (size_t)i < Values.size()) ? &Values[(size_t)i] : nullptr;
}

const PostValue* PostEffect::Find(const char* name) const {
    const int i = ParamIndex(*this, name);
    return (i >= 0 && (size_t)i < Values.size()) ? &Values[(size_t)i] : nullptr;
}

float PostEffect::Float(const char* name, float fallback) const {
    const PostValue* v = Find(name);
    return v ? v->V[0] : fallback;
}

bool PostEffect::Bool(const char* name, bool fallback) const {
    const PostValue* v = Find(name);
    return v ? v->B : fallback;
}

int PostEffect::Int(const char* name, int fallback) const {
    const PostValue* v = Find(name);
    return v ? (int)v->V[0] : fallback;
}

glm::vec2 PostEffect::Vec2(const char* name, glm::vec2 fallback) const {
    const PostValue* v = Find(name);
    return v ? glm::vec2(v->V[0], v->V[1]) : fallback;
}

// ============================================================================
//  Компиляция тракта
// ============================================================================

const char* PostStageLabel(PostStage stage) {
    switch (stage) {
        case PostStage::Exposure: return "Exposure";
        case PostStage::Depth:    return "Depth Effects";
        case PostStage::Bloom:    return "Bloom";
        case PostStage::Color:    return "Color";
        case PostStage::Grading:  return "Color Grading";
        case PostStage::Tonemap:  return "Tonemapping";
        case PostStage::Lens:     return "Lens / Image Effects";
        case PostStage::Film:     return "Film";
        case PostStage::Output:   return "Output";
    }
    return "Post Processing";
}

PostChain PostChain::Ordered() const {
    PostChain out = *this;
    const PostEffectCatalog& catalog = PostEffectCatalog::Instance();
    auto stageOf = [&catalog](const PostEffect& e) {
        const PostEffectKind* k = catalog.Find(e.Kind);
        // Звено неизвестного вида кладём в самый конец: выполнять его всё равно
        // нечем, а порядок известных от него зависеть не должен.
        return k ? (int)k->Stage : 1000;
    };
    // УСТОЙЧИВАЯ сортировка: внутри этапа порядок списка сохраняется. Это важно
    // для повторяемых звеньев игры — два её эффекта на одном этапе обязаны идти
    // в том порядке, в каком их положил автор.
    std::stable_sort(out.Effects.begin(), out.Effects.end(),
                     [&](const PostEffect& a, const PostEffect& b) {
                         return stageOf(a) < stageOf(b);
                     });
    return out;
}

PostChainReport PostChain::Compile() const {
    PostChainReport report;
    const PostEffectCatalog& catalog = PostEffectCatalog::Instance();
    // ПО ПОРЯДКУ ИСПОЛНЕНИЯ, а не по порядку списка: исполнитель разложит
    // звенья по этапам, и проверять надо то, что реально выполнится.
    const PostChain ordered = Ordered();

    // Идём по тракту слева направо, помня, в каком ПРОСТРАНСТВЕ идёт картинка:
    // до тон-маппинга это HDR (значения за [0,1], свечение и AO складываются в
    // линейном виде), после — LDR (готовое к показу, где работают пороги по
    // воспринимаемой яркости).
    bool ldr = false;
    std::string tonemap;
    std::vector<std::string> placed;

    for (const PostEffect& e : ordered.Effects) {
        const PostEffectKind* kind = catalog.Find(e.Kind);
        if (!kind) {
            report.Ok = false;
            report.Error = "звено «" + e.Kind + "» неизвестно: такого эффекта нет в каталоге";
            return report;
        }
        // Выключенное звено не создаёт ни зависимостей, ни конфликтов: оно не
        // выполняется, и мешать ему нечем. Именно поэтому галочка «выключить» и
        // кнопка «убрать» — разные вещи, и обе нужны.
        if (!e.Enabled) continue;

        if (!kind->Repeatable) {
            if (std::find(placed.begin(), placed.end(), e.Kind) != placed.end()) {
                // ВТОРОЙ ТОН-МАППИНГ ИМЕЕТ СВОЮ ПРИЧИНУ, и она точнее общей.
                //
                // Формально он «неповторяемое звено, поставленное дважды», но
                // сказать это значит отправить человека искать, чем же тон-маппинг
                // отличается от свечения. Он один потому, что кадр переводится в
                // готовый вид РОВНО ОДИН РАЗ, — так и надо сказать.
                if (kind->Tonemaps && !tonemap.empty()) {
                    report.Ok = false;
                    report.Error =
                        "в тракте два тон-маппинга: «" + tonemap + "» и «" + kind->Label + "»";
                    return report;
                }
                report.Ok = false;
                report.Error = "звено «" + kind->Label + "» стоит в тракте дважды, а оно неповторяемое";
                return report;
            }
        }
        placed.push_back(e.Kind);

        const bool needsHdr = Has(kind->Needs, PostNeeds::HdrColor);
        const bool needsLdr = Has(kind->Needs, PostNeeds::LdrColor);

        if (needsHdr && ldr) {
            report.Ok = false;
            report.Error = "звено «" + kind->Label + "» читает HDR-кадр, но кадр уже переведён в LDR звеном «" +
                           tonemap + "» — оно должно стоять до тон-маппинга";
            return report;
        }
        if (needsLdr && !ldr) {
            report.Ok = false;
            report.Error = "звено «" + kind->Label +
                           "» читает готовый (LDR) кадр, а тон-маппинга до него в тракте нет — оно должно "
                           "стоять после тон-маппинга";
            return report;
        }
        if (kind->Tonemaps) {
            tonemap = kind->Label;
            ldr = true;
        }
    }

    if (!ldr) {
        report.Ok = false;
        report.Error = "тракт не переводит кадр в LDR: без тон-маппинга на экран ушёл бы HDR-цвет";
        return report;
    }
    return report;
}

PostChain PostChain::Completed() const {
    PostChain chain = *this;
    const PostEffectCatalog& catalog = PostEffectCatalog::Instance();

    bool haveTonemap = false;
    for (const PostEffect& e : chain.Effects) {
        if (!e.Enabled) continue;
        const PostEffectKind* k = catalog.Find(e.Kind);
        if (k && k->Tonemaps) {
            haveTonemap = true;
            break;
        }
    }
    if (haveTonemap) return chain;

    // КУДА вставить — вопрос снят: место звена решает его этап (PostStage), а
    // не позиция в списке. Дописываем в конец, исполнитель поставит тон-маппинг
    // туда, где он и должен быть — после цвета и до виньетки.
    // НЕЙТРАЛЬНЫЙ, а не «как по умолчанию»: человек, снявший галочку с
    // тон-маппинга, просил убрать кривую света, а не получить обратно ACES.
    // Кадр всё равно обязан стать LDR — иначе на экран ушёл бы HDR-цвет, — но
    // сделать это можно и без характера: обрезкой.
    PostEffect neutral = MakePostEffect("tonemap");
    if (PostValue* mode = neutral.Find("mode")) mode->V[0] = 0.0f;
    chain.Effects.push_back(std::move(neutral));
    return chain;
}

PostEffect& AddPostEffect(PostChain& chain, const std::string& kindId) {
    // Просто в конец списка: КОГДА звено выполнится, решает его этап
    // (PostStage), а не место в списке. Раньше здесь была логика «до
    // тон-маппинга или после» — она и была признанием того, что порядок живёт в
    // двух местах сразу.
    chain.Effects.push_back(MakePostEffect(kindId));
    return chain.Effects.back();
}

bool RemovePostEffect(PostChain& chain, const std::string& kindId) {
    for (size_t i = 0; i < chain.Effects.size(); ++i) {
        if (chain.Effects[i].Kind == kindId) {
            chain.Effects.erase(chain.Effects.begin() + (long)i);
            return true;
        }
    }
    return false;
}

} // namespace sage::render
