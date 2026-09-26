// ---------------------------------------------------------------------------
// Тракт пост-обработки: каталог звеньев, компилятор порядка и сериализация.
//
// Почему это отдельный набор, а не кадровые тесты. Кадровые проверки отвечают на
// вопрос «какая получилась картинка», и на нём же и останавливаются: собрать
// НЕВЕРНЫЙ тракт (сглаживание до тон-маппинга, два тон-маппинга, звено
// неизвестного вида) и получить при этом живую картинку — совершенно обычное
// дело. Ловится это только проверкой самого тракта, без всякой видеокарты.
// ---------------------------------------------------------------------------
#include "TestFramework.h"

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "sage/render/PostProcessComponent.h"
#include "sage/render/PostChainIO.h"
#include "sage/render/PostEffect.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"

using namespace sage::render;

namespace {

// Звено по имени: nullptr — звена в тракте нет.
const PostEffect* EffectOf(const PostChain& chain, const char* kind) {
    for (const PostEffect& e : chain.Effects)
        if (e.Kind == kind) return &e;
    return nullptr;
}

int IndexOf(const PostChain& chain, const char* kind) {
    for (size_t i = 0; i < chain.Effects.size(); ++i)
        if (chain.Effects[i].Kind == kind) return (int)i;
    return -1;
}

PostChain ChainOf(std::initializer_list<const char*> kinds) {
    PostChain chain;
    for (const char* k : kinds) chain.Effects.push_back(MakePostEffect(k));
    return chain;
}

} // namespace

// --- Каталог -----------------------------------------------------------------

TEST(PostEffect_catalog_offers_the_builtins) {
    const PostEffectCatalog& catalog = PostEffectCatalog::Instance();
    for (const char* id : {"exposure", "ao", "dof", "motionblur", "bloom", "color", "grading",
                           "tonemap", "vignette", "chromatic", "grain", "fxaa"}) {
        const PostEffectKind* kind = catalog.Find(id);
        CHECK_TRUE(kind != nullptr);
        if (!kind) continue;
        CHECK_TRUE(!kind->Label.empty());
        CHECK_TRUE((bool)kind->Run);
    }
    // Тон-маппинг в каталоге РОВНО ОДИН: двух тон-маппингов не бывает ни в одной
    // оптике, и каталог, предлагающий второй, приглашал бы собрать неверный тракт.
    int tonemaps = 0;
    for (const PostEffectKind* k : catalog.All())
        if (k->Tonemaps) ++tonemaps;
    CHECK_EQ(tonemaps, 1);
}

TEST(PostEffect_is_made_with_defaults) {
    const PostEffect bloom = MakePostEffect("bloom");
    CHECK_EQ(bloom.Kind, std::string("bloom"));
    CHECK_TRUE(bloom.Enabled);
    CHECK_NEAR(bloom.Float("threshold"), 1.0f, 1e-6);
    CHECK_NEAR(bloom.Float("intensity"), 0.55f, 1e-6);
    // Значений ровно столько, сколько параметров у вида — иначе параметр и
    // значение разъехались бы, и настройка одного правила другое.
    const PostEffectKind* kind = PostEffectCatalog::Instance().Find("bloom");
    CHECK_TRUE(kind != nullptr);
    if (kind) CHECK_EQ(bloom.Values.size(), kind->Params.size());
}

TEST(PostEffect_unknown_param_is_not_silently_defaulted) {
    PostEffect bloom = MakePostEffect("bloom");
    // Опечатка в имени параметра обязана быть видна: молчаливое «значение по
    // умолчанию» — это ровно тот класс ошибок, который ищут днями.
    CHECK_TRUE(bloom.Find("treshold") == nullptr);
    CHECK_NEAR(bloom.Float("treshold", -1.0f), -1.0f, 1e-6);
    CHECK_TRUE(bloom.Find("threshold") != nullptr);
}

// Своё звено игры. Механизм тот же, что у иконок интерфейса
// (sage::ui::RegisterIcon): движок держит каталог, игра его дополняет.
TEST(PostEffect_accepts_an_effect_brought_by_the_game) {
    PostEffectKind kind;
    kind.Id = "unit_test_effect";
    kind.Label = "Unit Test Effect";
    PostParamDesc p;
    p.Name = "value";
    p.Label = "Value";
    p.Type = PostParamType::Float;
    p.Default[0] = 3.0f;
    kind.Params = {p};
    PostEffectCatalog::Instance().Register(std::move(kind));

    const PostEffectKind* found = PostEffectCatalog::Instance().Find("unit_test_effect");
    CHECK_TRUE(found != nullptr);
    CHECK_EQ(MakePostEffect("unit_test_effect").Float("value"), 3.0f);

    // Звено игры живёт в тракте наравне со встроенным.
    PostChain chain = ChainOf({"bloom", "unit_test_effect", "tonemap"});
    CHECK_TRUE(chain.Compile().Ok);
}

// --- Компилятор порядка ------------------------------------------------------

TEST(PostChain_reports_an_unknown_effect) {
    PostChain chain = ChainOf({"bloom", "no_such_effect", "tonemap"});
    const PostChainReport report = chain.Compile();
    CHECK_FALSE(report.Ok);
    // Причина называет виновника: «тракт неверен» без имени бесполезно.
    CHECK_TRUE(report.Error.find("no_such_effect") != std::string::npos);
}

TEST(PostChain_puts_the_effects_in_processing_order) {
    // ПОРЯДОК БОЛЬШЕ НЕ ПРИНАДЛЕЖИТ СПИСКУ. Раньше этот тракт — сглаживание
    // перед тон-маппингом, свечение после него — был ОТКАЗОМ: человек собирал
    // бессмысленную последовательность, и движку оставалось об этом сообщить.
    // Теперь место звена задано его этапом (PostStage), и такой список просто
    // раскладывается в тот порядок, в котором кадр и обрабатывается.
    const PostChain ordered = ChainOf({"fxaa", "tonemap", "bloom", "vignette", "exposure"}).Ordered();
    CHECK_TRUE(IndexOf(ordered, "exposure") < IndexOf(ordered, "bloom"));
    CHECK_TRUE(IndexOf(ordered, "bloom") < IndexOf(ordered, "tonemap"));
    CHECK_TRUE(IndexOf(ordered, "tonemap") < IndexOf(ordered, "vignette"));
    CHECK_TRUE(IndexOf(ordered, "vignette") < IndexOf(ordered, "fxaa"));
    CHECK_TRUE(ordered.Compile().Ok);
    // И сам список тоже компилируется: проверка смотрит на порядок исполнения.
    CHECK_TRUE(ChainOf({"fxaa", "tonemap", "bloom"}).Compile().Ok);
}

TEST(PostChain_order_follows_the_stages_of_the_catalog) {
    // Порядок разделов инспектора берётся из этих же этапов. Разъехаться им
    // негде — но проверить, что они те самые, надо: перепутанный этап у вида
    // виден только по картинке.
    const PostEffectCatalog& catalog = PostEffectCatalog::Instance();
    auto stage = [&catalog](const char* id) {
        const PostEffectKind* k = catalog.Find(id);
        return k ? (int)k->Stage : -1;
    };
    CHECK_TRUE(stage("exposure") < stage("dof"));
    CHECK_TRUE(stage("dof") < stage("bloom"));
    CHECK_TRUE(stage("bloom") < stage("color"));
    CHECK_TRUE(stage("color") < stage("grading"));
    CHECK_TRUE(stage("grading") < stage("tonemap"));
    CHECK_TRUE(stage("tonemap") < stage("vignette"));
    CHECK_TRUE(stage("vignette") < stage("grain"));
    CHECK_TRUE(stage("grain") < stage("fxaa"));
    // Виньетка и аберрация — ОДИН этап оптики: они делают одно и то же с
    // готовым кадром, и разводить их по этапам значило бы придумать порядок.
    CHECK_EQ(stage("vignette"), stage("chromatic"));
}

TEST(PostChain_rejects_two_tonemaps) {
    const PostChainReport report = ChainOf({"tonemap", "tonemap"}).Compile();
    CHECK_FALSE(report.Ok);
    CHECK_TRUE(report.Error.find("два тон-маппинга") != std::string::npos);
}

TEST(PostChain_rejects_a_duplicate_of_a_non_repeatable_effect) {
    // У встроенных звеньев свои рабочие буферы и своя история кадра: второй
    // экземпляр молча затирал бы первый.
    const PostChainReport report = ChainOf({"bloom", "bloom", "tonemap"}).Compile();
    CHECK_FALSE(report.Ok);
    CHECK_TRUE(report.Error.find("дважды") != std::string::npos);
}

TEST(PostChain_ignores_a_disabled_effect) {
    // Выключенное звено не выполняется, значит и мешать ему нечем: галочка
    // «выключить» и кнопка «убрать» — разные вещи, и обе нужны.
    PostChain chain = ChainOf({"fxaa", "tonemap"});
    chain.Effects[0].Enabled = false;
    CHECK_TRUE(chain.Compile().Ok);
}

TEST(PostChain_rejects_a_chain_that_never_becomes_ldr) {
    const PostChainReport report = ChainOf({"bloom"}).Compile();
    CHECK_FALSE(report.Ok);
    CHECK_TRUE(report.Error.find("LDR") != std::string::npos);
}

TEST(PostChain_completes_a_chain_without_a_tonemap) {
    // Человек снял галочку с тон-маппинга — обычное действие: «посмотреть без
    // кривой». Кадр всё равно обязан стать пригодным к показу, иначе на экран
    // ушёл бы HDR-цвет. Дополняется он НЕЙТРАЛЬНЫМ тон-маппингом: просить убрать
    // кривую и получить обратно ACES — не то, о чём просили.
    const PostChain completed = ChainOf({"bloom", "fxaa"}).Completed();
    CHECK_EQ(completed.Effects.size(), (size_t)3);
    const PostEffect* tonemap = EffectOf(completed, "tonemap");
    CHECK_TRUE(tonemap != nullptr);
    if (tonemap) CHECK_EQ(tonemap->Int("mode"), 0); // Clamp — без характера
    CHECK_TRUE(completed.Compile().Ok);
    // И в порядке исполнения он стоит там, где положено: после свечения, до
    // сглаживания.
    const PostChain ordered = completed.Ordered();
    CHECK_TRUE(IndexOf(ordered, "bloom") < IndexOf(ordered, "tonemap"));
    CHECK_TRUE(IndexOf(ordered, "tonemap") < IndexOf(ordered, "fxaa"));
}

TEST(PostChain_default_is_a_working_chain) {
    // Умолчание компонента — то, что человек получает, добавив камере
    // «Пост-обработку». Оно обязано работать сразу и не содержать эффектов,
    // которые нужны не всем.
    const PostChain chain = PostChain::Default();
    CHECK_TRUE(chain.Compile().Ok);
    CHECK_TRUE(EffectOf(chain, "exposure") != nullptr);
    CHECK_TRUE(EffectOf(chain, "autoexposure") != nullptr);   // адаптация глаза, как в UE5
    CHECK_TRUE(EffectOf(chain, "bloom") != nullptr);
    CHECK_TRUE(EffectOf(chain, "color") != nullptr);
    CHECK_TRUE(EffectOf(chain, "tonemap") != nullptr);
    if (const PostEffect* t = EffectOf(chain, "tonemap")) CHECK_EQ(t->Int("mode", -1), 4);   // Unreal
    // А у самого звена — прежняя ACES: нетронутые сохранённые сцены не меняются.
    CHECK_EQ(MakePostEffect("tonemap").Int("mode", -1), 2);
    CHECK_TRUE(EffectOf(chain, "dof") == nullptr);
    CHECK_TRUE(EffectOf(chain, "grain") == nullptr);
}

TEST(PostChain_effects_carry_their_own_settings) {
    // ГЛАВНОЕ СЛЕДСТВИЕ РАЗДЕЛЕНИЯ. Виньетка, аберрация и экспозиция были
    // ПАРАМЕТРАМИ тон-маппинга: убрать тон-маппинг значило убрать и их, а
    // включить виньетку без него было нельзя вовсе. Теперь это отдельные
    // звенья со своими настройками.
    CHECK_TRUE(MakePostEffect("tonemap").Find("vignette") == nullptr);
    CHECK_TRUE(MakePostEffect("tonemap").Find("exposure") == nullptr);
    CHECK_TRUE(MakePostEffect("vignette").Find("intensity") != nullptr);
    CHECK_TRUE(MakePostEffect("exposure").Find("exposure") != nullptr);
    CHECK_TRUE(MakePostEffect("color").Find("saturation") != nullptr);
    // У тон-маппинга остался его собственный вопрос: какой кривой переводить.
    const PostEffectKind* kind = PostEffectCatalog::Instance().Find("tonemap");
    CHECK_TRUE(kind != nullptr);
    if (kind) {
        const PostParamDesc* mode = nullptr;
        for (const PostParamDesc& p : kind->Params)
            if (p.Name == "mode") mode = &p;
        CHECK_TRUE(mode != nullptr);
        if (mode) {
            CHECK_TRUE(mode->Type == PostParamType::Enum);
            // Clamp/Reinhard/ACES/Filmic + Unreal/AgX/Neutral: новые только в конце.
            CHECK_EQ(mode->Options.size(), (size_t)7);
            if (mode->Options.size() == 7) CHECK_EQ(mode->Options[2], std::string("ACES"));
        }
    }
}

// --- Сериализация ------------------------------------------------------------

TEST(PostProcess_component_survives_a_scene_round_trip) {
    Scene scene;
    GameObject cam = scene.CreateObject("Camera");
    scene.Registry().emplace<CameraComponent>(cam.Entity());

    PostProcessComponent component;
    component.Chain = ChainOf({"bloom", "vignette", "tonemap"});
    component.Chain.Effects[0].Enabled = false;
    component.Chain.Effects[0].Find("threshold")->V[0] = 2.5f;
    component.Chain.Effects[1].Find("intensity")->V[0] = 0.1f;
    scene.Registry().emplace<PostProcessComponent>(cam.Entity(), component);

    const std::string text = SceneSerializer::SaveToString(scene);
    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(text);
    CHECK_TRUE(loaded != nullptr);
    if (!loaded) return;

    const auto view = loaded->Registry().view<PostProcessComponent>();
    CHECK_TRUE(view.begin() != view.end());
    if (view.begin() == view.end()) return;
    const PostProcessComponent* back = loaded->Registry().try_get<PostProcessComponent>(*view.begin());
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    CHECK_EQ(back->Chain.Effects.size(), (size_t)3);
    CHECK_TRUE(back->Enabled);
    CHECK_EQ(back->Chain.Effects[0].Kind, std::string("bloom"));
    CHECK_EQ(back->Chain.Effects[0].Enabled, false); // выключенность — тоже данные
    CHECK_NEAR(back->Chain.Effects[0].Float("threshold"), 2.5f, 1e-4);
    CHECK_NEAR(back->Chain.Effects[1].Float("intensity"), 0.1f, 1e-4);
    CHECK_TRUE(back->Chain.Compile().Ok);
}

TEST(PostProcess_component_reads_the_old_key) {
    // Компонент назывался «тракт пост-обработки» и лежал под ключом postChain.
    // Сцена, сохранённая тогда, обязана открыться с теми же эффектами: смена
    // названия компонента не повод потерять настройку кадра.
    Scene scene;
    GameObject cam = scene.CreateObject("Camera");
    scene.Registry().emplace<CameraComponent>(cam.Entity());
    PostProcessComponent component;
    component.Chain = ChainOf({"bloom", "tonemap"});
    scene.Registry().emplace<PostProcessComponent>(cam.Entity(), component);

    nlohmann::json j = nlohmann::json::parse(SceneSerializer::SaveToString(scene));
    j["objects"][0]["postChain"] = j["objects"][0]["postProcess"];
    j["objects"][0].erase("postProcess");

    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(j.dump());
    CHECK_TRUE(loaded != nullptr);
    if (!loaded) return;
    const auto view = loaded->Registry().view<PostProcessComponent>();
    CHECK_TRUE(view.begin() != view.end());
    if (view.begin() == view.end()) return;
    const PostProcessComponent* back = loaded->Registry().try_get<PostProcessComponent>(*view.begin());
    CHECK_TRUE(back != nullptr);
    if (back) CHECK_TRUE(EffectOf(back->Chain, "bloom") != nullptr);
}

TEST(PostChain_file_with_unknown_pieces_does_not_throw) {
    // Файл сцены правят руками. Неизвестный параметр и звено неизвестного вида —
    // не повод не открыть сцену: значение просто не применяется, звено
    // остаётся в тракте и о его виде скажет компилятор.
    Scene scene;
    GameObject cam = scene.CreateObject("Camera");
    scene.Registry().emplace<CameraComponent>(cam.Entity());
    PostProcessComponent component;
    component.Chain = ChainOf({"bloom", "tonemap"});
    scene.Registry().emplace<PostProcessComponent>(cam.Entity(), component);

    std::string text = SceneSerializer::SaveToString(scene);
    nlohmann::json j = nlohmann::json::parse(text);
    j["objects"][0]["postProcess"]["effects"][0]["params"]["threshold"] = "не число";
    j["objects"][0]["postProcess"]["effects"][0]["params"]["выдуманный"] = 42.0;
    j["objects"][0]["postProcess"]["effects"].push_back(
        {{"kind", "no_such_effect"}, {"enabled", true}});

    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(j.dump());
    CHECK_TRUE(loaded != nullptr);
    if (!loaded) return;
    const auto view = loaded->Registry().view<PostProcessComponent>();
    CHECK_TRUE(view.begin() != view.end());
    if (view.begin() == view.end()) return;
    const PostProcessComponent* back = loaded->Registry().try_get<PostProcessComponent>(*view.begin());
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    CHECK_EQ(back->Chain.Effects.size(), (size_t)3);
    // Строка вместо числа не применилась — параметр остался на умолчании вида.
    CHECK_NEAR(back->Chain.Effects[0].Float("threshold"), 1.0f, 1e-4);
    // Звено чужого вида доехало, и компилятор говорит о нём причину.
    const PostChainReport report = back->Chain.Compile();
    CHECK_FALSE(report.Ok);
    CHECK_TRUE(report.Error.find("no_such_effect") != std::string::npos);
}

// --- Чья обработка -----------------------------------------------------------

TEST(PostProcess_belongs_to_the_camera_and_nowhere_else) {
    // ГЛАВНОЕ ПРАВИЛО СИСТЕМЫ. Нет компонента — нет обработки: не «тракт
    // проекта», не «умолчания движка», а кадр как есть. Пока обработка жила
    // настройкой проекта, выключить её у одной камеры было нельзя вовсе, а
    // включить незаметно для остальных — тем более.
    Scene scene;
    GameObject plain = scene.CreateObject("Plain");
    scene.Registry().emplace<CameraComponent>(plain.Entity());
    GameObject tuned = scene.CreateObject("Tuned");
    scene.Registry().emplace<CameraComponent>(tuned.Entity());

    PostProcessComponent component;
    component.Chain = ChainOf({"bloom", "tonemap"});
    scene.Registry().emplace<PostProcessComponent>(tuned.Entity(), component);

    PostChain chain;
    CHECK_TRUE(ResolvePostChain(scene, tuned.Entity(), chain));
    CHECK_TRUE(EffectOf(chain, "bloom") != nullptr);

    // Камера без компонента — обработки нет.
    PostChain none;
    CHECK_FALSE(ResolvePostChain(scene, plain.Entity(), none));

    // Выключенный компонент — тоже нет, но настройки при этом целы.
    scene.Registry().get<PostProcessComponent>(tuned.Entity()).Enabled = false;
    CHECK_FALSE(ResolvePostChain(scene, tuned.Entity(), none));
    CHECK_TRUE(EffectOf(scene.Registry().get<PostProcessComponent>(tuned.Entity()).Chain, "bloom") !=
               nullptr);

    // Камеры нет вовсе — тоже нет обработки.
    CHECK_FALSE(ResolvePostChain(scene, entt::null, none));
}

TEST(PostProcess_two_cameras_keep_their_own_settings) {
    // То, ради чего обработка и переехала на камеру: у каждой свой вид.
    Scene scene;
    GameObject cinematic = scene.CreateObject("Cinematic");
    scene.Registry().emplace<CameraComponent>(cinematic.Entity());
    GameObject map = scene.CreateObject("Map");
    scene.Registry().emplace<CameraComponent>(map.Entity());

    PostProcessComponent rich;
    rich.Chain = ChainOf({"exposure", "bloom", "grain", "tonemap"});
    rich.Chain.Effects[0].Find("exposure")->V[0] = 1.5f;
    scene.Registry().emplace<PostProcessComponent>(cinematic.Entity(), rich);

    PostProcessComponent lean;
    lean.Chain = ChainOf({"tonemap"});
    scene.Registry().emplace<PostProcessComponent>(map.Entity(), lean);

    PostChain a, b;
    CHECK_TRUE(ResolvePostChain(scene, cinematic.Entity(), a));
    CHECK_TRUE(ResolvePostChain(scene, map.Entity(), b));
    CHECK_TRUE(EffectOf(a, "grain") != nullptr);
    CHECK_TRUE(EffectOf(b, "grain") == nullptr);
    CHECK_NEAR(EffectOf(a, "exposure")->Float("exposure"), 1.5f, 1e-4);
}
