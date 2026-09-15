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

#include <string>

#include <nlohmann/json.hpp>

#include "sage/render/PostChainComponent.h"
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
    for (const char* id : {"ao", "dof", "motionblur", "bloom", "tonemap", "fxaa"}) {
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

TEST(PostChain_rejects_ldr_reader_before_tonemap) {
    // Сглаживание ищет кромки по ВОСПРИНИМАЕМОЙ яркости, а в линейном HDR его
    // пороги не работают: до тон-маппинга оно бессмысленно, и это обязано быть
    // отказом, а не тихо испорченной картинкой.
    const PostChainReport report = ChainOf({"fxaa", "tonemap"}).Compile();
    CHECK_FALSE(report.Ok);
    CHECK_TRUE(report.Error.find("FXAA") != std::string::npos);
}

TEST(PostChain_rejects_hdr_reader_after_tonemap) {
    // Свечение обязано считаться по HDR-кадру; после тон-маппинга значения
    // обрезаны в [0,1], и свечение превращается в простое осветление.
    const PostChainReport report = ChainOf({"tonemap", "bloom"}).Compile();
    CHECK_FALSE(report.Ok);
    CHECK_TRUE(report.Error.find("Bloom") != std::string::npos);
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

TEST(PostChain_adds_the_tonemap_before_ldr_readers) {
    // Человек убрал тон-маппинг и ещё не добавил новый — обычное состояние в
    // момент правки. Дополнение обязано встать ПЕРЕД сглаживанием, иначе
    // дополненный тракт не скомпилируется, и «просто посмотреть» закончится
    // отказом вместо картинки.
    const PostChain completed = ChainOf({"bloom", "fxaa"}).Completed();
    CHECK_EQ(completed.Effects.size(), (size_t)3);
    CHECK_EQ(completed.Effects[0].Kind, std::string("bloom"));
    CHECK_EQ(completed.Effects[1].Kind, std::string("tonemap"));
    CHECK_EQ(completed.Effects[2].Kind, std::string("fxaa"));
    CHECK_TRUE(completed.Compile().Ok);
}

TEST(PostChain_add_puts_an_effect_where_it_can_work) {
    // Звено, читающее HDR, встаёт ДО тон-маппинга, читающее готовый кадр —
    // ПОСЛЕ. Иначе «добавил глубину резкости» означало бы «получил отказ».
    PostChain chain = ChainOf({"tonemap"});
    AddPostEffect(chain, "bloom");
    CHECK_EQ(IndexOf(chain, "bloom"), 0);
    AddPostEffect(chain, "fxaa");
    CHECK_EQ(IndexOf(chain, "fxaa"), 2);
    CHECK_TRUE(chain.Compile().Ok);
    CHECK_TRUE(RemovePostEffect(chain, "bloom"));
    CHECK_TRUE(EffectOf(chain, "bloom") == nullptr);
    CHECK_TRUE(chain.Compile().Ok);
}

TEST(PostChain_from_config_is_ordered_and_complete) {
    sage::EngineConfig cfg;
    cfg.AmbientOcclusion = true;
    cfg.Bloom = true;
    cfg.DepthOfField = true;
    cfg.MotionBlur = true;
    cfg.Fxaa = true;
    cfg.Msaa = 0;

    const PostChain chain = PostChain::FromConfig(cfg);
    CHECK_TRUE(chain.Compile().Ok);
    // Тон-маппинг есть всегда: без него на экран ушёл бы HDR-цвет.
    CHECK_TRUE(EffectOf(chain, "tonemap") != nullptr);
    // И стоит он ПОСЛЕ всего, что считает в HDR, но ДО сглаживания.
    CHECK_TRUE(IndexOf(chain, "tonemap") < IndexOf(chain, "fxaa"));
    for (const char* hdr : {"ao", "dof", "motionblur", "bloom"})
        CHECK_TRUE(IndexOf(chain, hdr) < IndexOf(chain, "tonemap"));
    // Выключенный в конфиге эффект в тракт не попадает вовсе — а не «попадает
    // выключенным»: тогда его настройки остались бы в файле и ждали своего часа.
    cfg.Bloom = false;
    CHECK_TRUE(EffectOf(PostChain::FromConfig(cfg), "bloom") == nullptr);
}

// --- Сериализация ------------------------------------------------------------

TEST(PostChain_survives_a_scene_round_trip) {
    Scene scene;
    GameObject cam = scene.CreateObject("Camera");
    scene.Registry().emplace<CameraComponent>(cam.Entity());

    PostChainComponent component;
    component.Chain = ChainOf({"bloom", "tonemap"});
    component.Chain.Effects[0].Enabled = false;
    *component.Chain.Effects[0].Find("threshold") = PostValue{};
    component.Chain.Effects[0].Find("threshold")->V[0] = 2.5f;
    component.Chain.Effects[1].Find("vignette")->V[0] = 0.1f;
    scene.Registry().emplace<PostChainComponent>(cam.Entity(), component);

    const std::string text = SceneSerializer::SaveToString(scene);
    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(text);
    CHECK_TRUE(loaded != nullptr);
    if (!loaded) return;

    const PostChainComponent* back =
        loaded->Registry().try_get<PostChainComponent>(loaded->Registry().view<PostChainComponent>().front());
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    CHECK_EQ(back->Chain.Effects.size(), (size_t)2);
    CHECK_EQ(back->UseProjectDefault, false);
    CHECK_EQ(back->Chain.Effects[0].Kind, std::string("bloom"));
    CHECK_EQ(back->Chain.Effects[0].Enabled, false); // выключенность — тоже данные
    CHECK_NEAR(back->Chain.Effects[0].Float("threshold"), 2.5f, 1e-4);
    CHECK_EQ(back->Chain.Effects[1].Kind, std::string("tonemap"));
    CHECK_NEAR(back->Chain.Effects[1].Float("vignette"), 0.1f, 1e-4);
    CHECK_TRUE(back->Chain.Compile().Ok);
}

TEST(PostChain_file_with_unknown_pieces_does_not_throw) {
    // Файл сцены правят руками. Неизвестный параметр и звено неизвестного вида —
    // не повод не открыть сцену: значение просто не применяется, звено
    // остаётся в тракте и о его виде скажет компилятор.
    Scene scene;
    GameObject cam = scene.CreateObject("Camera");
    scene.Registry().emplace<CameraComponent>(cam.Entity());
    PostChainComponent component;
    component.Chain = ChainOf({"bloom", "tonemap"});
    scene.Registry().emplace<PostChainComponent>(cam.Entity(), component);

    std::string text = SceneSerializer::SaveToString(scene);
    nlohmann::json j = nlohmann::json::parse(text);
    j["objects"][0]["postChain"]["effects"][0]["params"]["threshold"] = "не число";
    j["objects"][0]["postChain"]["effects"][0]["params"]["выдуманный"] = 42.0;
    j["objects"][0]["postChain"]["effects"].push_back(
        {{"kind", "no_such_effect"}, {"enabled", true}});

    std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(j.dump());
    CHECK_TRUE(loaded != nullptr);
    if (!loaded) return;
    const auto view = loaded->Registry().view<PostChainComponent>();
    CHECK_TRUE(view.begin() != view.end());
    if (view.begin() == view.end()) return;
    const PostChainComponent* back = loaded->Registry().try_get<PostChainComponent>(*view.begin());
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

// --- Чей тракт показывать ----------------------------------------------------

TEST(PostChain_resolves_from_the_camera_then_the_project) {
    Scene scene;
    GameObject plain = scene.CreateObject("Plain");
    scene.Registry().emplace<CameraComponent>(plain.Entity());
    GameObject tuned = scene.CreateObject("Tuned");
    scene.Registry().emplace<CameraComponent>(tuned.Entity());

    PostChainComponent component;
    component.Chain = ChainOf({"bloom", "tonemap"});
    scene.Registry().emplace<PostChainComponent>(tuned.Entity(), component);

    sage::EngineConfig cfg;
    cfg.Bloom = false;
    cfg.AmbientOcclusion = true;

    // Камера со своим трактом — берётся он.
    const PostChain own = ResolvePostChain(scene, tuned.Entity(), cfg);
    CHECK_TRUE(EffectOf(own, "bloom") != nullptr);

    // Камера без компонента — тракт проекта.
    const PostChain project = ResolvePostChain(scene, plain.Entity(), cfg);
    CHECK_TRUE(EffectOf(project, "bloom") == nullptr);
    CHECK_TRUE(EffectOf(project, "ao") != nullptr);

    // Компонент, просящий умолчание, — тоже тракт проекта: иначе нельзя
    // отличить «камеру ещё не настроили» от «намеренно не настраивали».
    scene.Registry().get<PostChainComponent>(tuned.Entity()).UseProjectDefault = true;
    const PostChain asked = ResolvePostChain(scene, tuned.Entity(), cfg);
    CHECK_TRUE(EffectOf(asked, "bloom") == nullptr);
    CHECK_TRUE(EffectOf(asked, "ao") != nullptr);

    // Камеры нет вовсе — тоже проект, а не пустота.
    CHECK_TRUE(EffectOf(ResolvePostChain(scene, entt::null, cfg), "ao") != nullptr);
}
