// Публичные переменные, ссылки и события: sage/vars, sage/events и их связь с
// интерфейсом и скриптами.
//
// Проверяется не «функция вернула значение», а то, ради чего эти три системы
// написаны: настройка объекта переживает правку скрипта, ссылка переживает
// переименование объекта, а кнопка работает БЕЗ скрипта, который опрашивал бы
// её каждый кадр.
#include "TestFramework.h"

#include <memory>
#include <string>

#include "sage/events/Events.h"
#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/scene/SceneSerializer.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISceneSystem.h"
#include "sage/vars/ScriptVars.h"
#include "sage/vars/VarsComponent.h"

using sage::vars::AssetRef;
using sage::vars::EntityRef;
using sage::vars::Kind;
using sage::vars::Table;
using sage::vars::Value;
using sage::vars::Var;

// ===========================================================================
//  ЗНАЧЕНИЕ
// ===========================================================================

TEST(Vars_a_value_reads_back_as_what_it_is) {
    CHECK_TRUE(Value(3.5f).Type() == Kind::Float);
    CHECK_NEAR(Value(3.5f).AsFloat(), 3.5f, 1e-5f);
    CHECK_TRUE(Value(std::string("да")).Type() == Kind::String);
    CHECK_TRUE(Value(EntityRef{7}).Type() == Kind::Entity);
    CHECK_EQ(Value(EntityRef{7}).AsEntity().Id, 7);
    CHECK_TRUE(Value(AssetRef{"assets/hit.wav"}).Type() == Kind::Asset);
}

// Чтение «не тем» видом ПОДСТАВЛЯЕТ, а не падает: значение приходит из файла и
// из инспектора, и «в сцене строка там, где скрипт ждёт число» — обычное дело
// после переименования, а не повод уронить игру.
TEST(Vars_reading_a_value_as_another_kind_substitutes_instead_of_throwing) {
    CHECK_NEAR(Value(std::string("12.5")).AsFloat(), 12.5f, 1e-4f);
    CHECK_EQ(Value(true).AsInt(), 1);
    CHECK_EQ(Value(std::string("не число")).AsInt(42), 42);
    // Цвет из тройки — с НЕПРОЗРАЧНОЙ альфой: прозрачный по умолчанию значил бы
    // «переменная есть, а на экране ничего».
    CHECK_NEAR(Value(glm::vec3(1.0f)).AsVec4().a, 1.0f, 1e-5f);
}

TEST(Vars_changing_the_kind_keeps_what_was_typed) {
    const Value converted = Value::Convert(Value(7), Kind::String);
    CHECK_TRUE(converted.Type() == Kind::String);
    CHECK_EQ(converted.AsString(), std::string("7"));
}

// ===========================================================================
//  ТАБЛИЦА И СЛИЯНИЕ С ОБЪЯВЛЕНИЕМ СКРИПТА
// ===========================================================================

TEST(Vars_a_table_keeps_the_declared_order) {
    Table t;
    t.Set("speed", Value(1.0f));
    t.Set("damage", Value(10));
    t.Set("name", Value(std::string("дверь")));
    CHECK_EQ(t.Size(), (size_t)3);
    CHECK_EQ(t.All()[0].Name, std::string("speed"));
    CHECK_EQ(t.All()[2].Name, std::string("name"));
    CHECK_TRUE(t.Move(2, 0));
    CHECK_EQ(t.All()[0].Name, std::string("name"));
}

// Присваивание НЕ МЕНЯЕТ вид переменной: скрипт, положивший строку в числовую
// настройку, ошибся, и менять из-за этого тип поля в инспекторе — значит
// спрятать ошибку и сделать тип объекта зависящим от порядка кадров.
TEST(Vars_assigning_keeps_the_declared_kind) {
    Table t;
    t.Set("speed", Value(2.0f));
    t.Set("speed", Value(std::string("5")));
    CHECK_TRUE(t.Find("speed")->Data.Type() == Kind::Float);
    CHECK_NEAR(t.Get("speed").AsFloat(), 5.0f, 1e-4f);
}

// Главное свойство слияния: правка скрипта НЕ СБРАСЫВАЕТ настройки, уже
// расставленные по уровню. Без этого добавление одной переменной в скрипт
// обнуляло бы все двери на карте.
TEST(Vars_merging_a_declaration_keeps_the_values_set_on_the_object) {
    Table onObject;
    onObject.Set("speed", Value(9.0f));      // человек выставил в инспекторе
    onObject.Set("legacy", Value(1));        // переменной уже нет в скрипте

    Table declaration;
    Var speed;  speed.Name = "speed";  speed.Data = Value(3.0f); speed.Label = "Скорость";
    speed.Min = 0.0f; speed.Max = 20.0f;
    Var damage; damage.Name = "damage"; damage.Data = Value(10);
    declaration.Put(speed);
    declaration.Put(damage);

    onObject.MergeDeclaration(declaration);

    // Значение объекта уцелело, описание пришло из скрипта.
    CHECK_NEAR(onObject.Get("speed").AsFloat(), 9.0f, 1e-4f);
    CHECK_EQ(onObject.Find("speed")->Label, std::string("Скорость"));
    CHECK_NEAR(onObject.Find("speed")->Max, 20.0f, 1e-4f);
    CHECK_TRUE(onObject.Find("speed")->Declared);

    // Новая переменная скрипта пришла со своим умолчанием.
    CHECK_EQ(onObject.Get("damage").AsInt(), 10);

    // Пропавшая из скрипта НЕ СТЁРТА, но и не выдаётся за объявленную: молча
    // потерять настройку из-за опечатки в имени хуже, чем показать лишнюю
    // строку.
    CHECK_TRUE(onObject.Find("legacy") != nullptr);
    CHECK_FALSE(onObject.Find("legacy")->Declared);

    // Порядок — из объявления: его задал автор скрипта.
    CHECK_EQ(onObject.All()[0].Name, std::string("speed"));
    CHECK_EQ(onObject.All()[1].Name, std::string("damage"));
}

// ===========================================================================
//  ОБЪЯВЛЕНИЕ В СКРИПТЕ
// ===========================================================================

TEST(Vars_a_script_declares_its_public_variables) {
    const std::string source = R"(
-- Дверь уровня
Vars = {
    speed  = 3.5,
    opened = false,
    damage = { 10, min = 0, max = 100, label = "Урон", tooltip = "За удар" },
    title  = "Ворота",
    target = { kind = "entity", label = "Цель" },
    sound  = { kind = "asset" },
}

function OnUpdate(entity, dt) end
)";
    const Table t = sage::vars::ParseDeclaration(source);
    CHECK_EQ(t.Size(), (size_t)6);
    CHECK_NEAR(t.Get("speed").AsFloat(), 3.5f, 1e-4f);
    CHECK_TRUE(t.Get("opened").Type() == Kind::Bool);
    CHECK_EQ(t.Get("damage").AsInt(), 10);
    CHECK_EQ(t.Find("damage")->Label, std::string("Урон"));
    CHECK_NEAR(t.Find("damage")->Max, 100.0f, 1e-4f);
    CHECK_EQ(t.Get("title").AsString(), std::string("Ворота"));
    // Вид, названный явно, сильнее угаданного: без значения это ссылка, а не
    // пустая строка.
    CHECK_TRUE(t.Get("target").Type() == Kind::Entity);
    CHECK_TRUE(t.Get("sound").Type() == Kind::Asset);
    CHECK_EQ(t.Find("target")->Label, std::string("Цель"));
    // Порядок — как в файле: инспектор показывает его же.
    CHECK_EQ(t.All()[0].Name, std::string("speed"));
    CHECK_EQ(t.All()[5].Name, std::string("sound"));
}

// Разбор читает ДАННЫЕ, а не выполняет файл (см. ScriptVars.h). Значит, скрипт
// без объявления и скрипт с выражением вместо литерала не должны ни падать, ни
// съедать соседние переменные.
TEST(Vars_a_declaration_survives_things_it_cannot_parse) {
    CHECK_TRUE(sage::vars::ParseDeclaration("function OnUpdate() end").Empty());
    const Table t = sage::vars::ParseDeclaration(R"(
Vars = {
    good   = 1,
    weird  = SomeCall(2, 3),
    after  = "цел",
}
)");
    // Незнакомая запись не сбила разбор следующей — иначе одна строка молча
    // отнимала бы у объекта половину настроек.
    CHECK_EQ(t.Size(), (size_t)3);
    CHECK_EQ(t.Get("after").AsString(), std::string("цел"));
}

// ===========================================================================
//  СЦЕНА: ПЕРЕМЕННЫЕ И ССЫЛКИ ПЕРЕЖИВАЮТ ЗАПИСЬ
// ===========================================================================

TEST(Vars_public_variables_survive_save_and_load) {
    Scene scene("vars");
    GameObject door = scene.CreateObject("Door");
    GameObject key = scene.CreateObject("Key");

    VarsComponent& vc = scene.Registry().emplace<VarsComponent>(door.Entity());
    vc.Values.Set("speed", Value(4.25f));
    vc.Values.Set("opened", Value(true));
    vc.Values.Set("title", Value(std::string("Ворота")));
    vc.Values.Set("tint", Value(glm::vec4(0.2f, 0.4f, 0.6f, 1.0f)));
    vc.Values.Set("needs", Value(EntityRef{key.Id()}));
    vc.Values.Set("sound", Value(AssetRef{"assets/audio/creak.wav"}));

    std::unique_ptr<Scene> back =
        SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    GameObject loaded = back->FindByName("Door");
    CHECK_TRUE(loaded.Valid());
    const VarsComponent* lc = back->Registry().try_get<VarsComponent>(loaded.Entity());
    CHECK_TRUE(lc != nullptr);
    if (!lc) return;

    CHECK_NEAR(lc->Values.Get("speed").AsFloat(), 4.25f, 1e-4f);
    CHECK_TRUE(lc->Values.Get("opened").AsBool());
    CHECK_EQ(lc->Values.Get("title").AsString(), std::string("Ворота"));
    CHECK_NEAR(lc->Values.Get("tint").AsVec4().b, 0.6f, 1e-4f);
    CHECK_EQ(lc->Values.Get("sound").AsAsset().Path, std::string("assets/audio/creak.wav"));

    // ССЫЛКА ДЕРЖИТСЯ ЗА Id, и это её главное свойство: объект можно
    // переименовать, а связь останется. Поиск по имени этого не умеет — и
    // именно поэтому ссылки заведены отдельным видом значения.
    const int refId = lc->Values.Get("needs").AsEntity().Id;
    CHECK_EQ(refId, key.Id());
    GameObject pointed = back->Get(refId);
    CHECK_TRUE(pointed.Valid());
    CHECK_EQ(pointed.Name(), std::string("Key"));
}

TEST(Vars_a_reference_survives_renaming_the_object_it_points_at) {
    Scene scene("refs");
    GameObject door = scene.CreateObject("Door");
    GameObject key = scene.CreateObject("Key");
    scene.Registry().emplace<VarsComponent>(door.Entity()).Values.Set(
        "needs", Value(EntityRef{key.Id()}));

    key.SetName("Золотой ключ");
    std::unique_ptr<Scene> back =
        SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    const VarsComponent& lc =
        back->Registry().get<VarsComponent>(back->FindByName("Door").Entity());
    CHECK_EQ(back->Get(lc.Values.Get("needs").AsEntity().Id).Name(),
             std::string("Золотой ключ"));
}

// ===========================================================================
//  ШИНА СОБЫТИЙ
// ===========================================================================

TEST(Events_a_handler_hears_only_its_own_event) {
    sage::events::Bus bus;
    int mine = 0, other = 0;
    bus.On("opened", [&](const sage::events::Event&) { ++mine; });
    bus.On("closed", [&](const sage::events::Event&) { ++other; });
    bus.Emit("opened");
    bus.Emit("opened");
    CHECK_EQ(mine, 2);
    CHECK_EQ(other, 0);
    CHECK_EQ(bus.Count("opened"), 1);
}

TEST(Events_a_once_handler_unsubscribes_itself) {
    sage::events::Bus bus;
    int calls = 0;
    bus.Once("hit", [&](const sage::events::Event&) { ++calls; });
    bus.Emit("hit");
    bus.Emit("hit");
    CHECK_EQ(calls, 1);
    CHECK_EQ(bus.Count("hit"), 0);
}

// Подписка и отписка ИЗНУТРИ обработчика — обычное дело («сработало —
// отпишись»), и она меняет вектор, по которому идёт рассылка. Без снимка
// первый же такой обработчик оставил бы висячую ссылку.
TEST(Events_subscribing_from_inside_a_handler_does_not_corrupt_the_dispatch) {
    sage::events::Bus bus;
    int first = 0, added = 0;
    int id = bus.On("tick", [&](const sage::events::Event&) {
        ++first;
        bus.On("tick", [&](const sage::events::Event&) { ++added; });
    });
    bus.Emit("tick");          // новый подписчик заводится, но в этой рассылке не зовётся
    CHECK_EQ(first, 1);
    CHECK_EQ(added, 0);
    bus.Emit("tick");
    CHECK_EQ(first, 2);
    CHECK_EQ(added, 1);
    bus.Off(id);
    bus.Emit("tick");
    CHECK_EQ(first, 2);
}

// Два обработчика, шлющие событие друг другу, обязаны упереться в предел, а не
// уронить движок переполнением стека: ошибка в НАСТРОЙКЕ кнопки не должна
// убивать игру.
TEST(Events_a_loop_between_handlers_stops_instead_of_crashing) {
    sage::events::Bus bus;
    int a = 0, b = 0;
    bus.On("a", [&](const sage::events::Event&) { ++a; bus.Emit("b"); });
    bus.On("b", [&](const sage::events::Event&) { ++b; bus.Emit("a"); });
    bus.Emit("a");
    CHECK_TRUE(a > 0 && a < 100);
    CHECK_TRUE(b > 0 && b < 100);
}

TEST(Events_a_payload_carries_the_sender_and_the_argument) {
    sage::events::Bus bus;
    int sender = 0;
    std::string arg;
    bus.On("say", [&](const sage::events::Event& e) {
        sender = e.Sender;
        arg = e.Arg.AsString();
    });
    bus.Emit("say", Value(std::string("привет")), 42);
    CHECK_EQ(sender, 42);
    CHECK_EQ(arg, std::string("привет"));
}

// ===========================================================================
//  КНОПКА ДЕЛАЕТ ЧТО-ТО САМА
// ===========================================================================

namespace {
// Кнопка на весь экран: попасть по ней мышью можно, не считая координат.
GameObject MakeButton(Scene& scene, const char* name) {
    GameObject e = scene.CreateObject(name);
    sage::ui::Transform t;
    t.Anchor = UIAnchor::TopLeft;
    t.Offset = {0.0f, 0.0f};
    t.Size = {200.0f, 100.0f};
    scene.Registry().emplace<sage::ui::Transform>(e.Entity(), t);
    scene.Registry().emplace<sage::ui::Fill>(e.Entity());
    scene.Registry().emplace<sage::ui::Interactable>(e.Entity());
    return e;
}

// Один щелчок по точке: нажали и отпустили там же.
void ClickAt(Scene& scene, glm::vec2 point) {
    sage::ui::UIInputState down;
    down.Mouse = point;
    down.MouseDown = true;
    down.MousePressed = true;
    sage::ui::UpdateSceneUI(scene, down, 800, 600);

    sage::ui::UIInputState up;
    up.Mouse = point;
    up.MouseReleased = true;
    sage::ui::UpdateSceneUI(scene, up, 800, 600);
}
} // namespace

// ТО, РАДИ ЧЕГО ВСЁ ЭТО. Кнопка шлёт событие САМА — без уровневого скрипта,
// который каждый кадр спрашивал бы «не нажали ли». Опрос терял нажатие на
// длинном кадре и требовал скрипта-спутника у любой кнопки.
TEST(Events_a_button_emits_on_its_own_without_any_script) {
    Scene scene("ui");
    GameObject button = MakeButton(scene, "Play");
    sage::events::Binding b;
    b.Trigger = "click";
    b.Event = "game.start";
    b.Arg = Value(std::string("level1"));
    scene.Registry().get<sage::ui::Interactable>(button.Entity()).Events.push_back(b);

    int heard = 0;
    std::string level;
    int sender = 0;
    scene.Events.On("game.start", [&](const sage::events::Event& e) {
        ++heard;
        level = e.Arg.AsString();
        sender = e.Sender;
    });

    ClickAt(scene, {50.0f, 50.0f});
    CHECK_EQ(heard, 1);
    CHECK_EQ(level, std::string("level1"));
    // Отправитель — сама кнопка: без него обработчик не знает, КАКУЮ нажали, и
    // каждой кнопке пришлось бы придумывать своё имя события.
    CHECK_EQ(sender, button.Id());
}

TEST(Events_a_button_does_not_emit_when_the_click_misses_it) {
    Scene scene("ui");
    GameObject button = MakeButton(scene, "Play");
    sage::events::Binding b;
    b.Trigger = "click";
    b.Event = "game.start";
    scene.Registry().get<sage::ui::Interactable>(button.Entity()).Events.push_back(b);

    int heard = 0;
    scene.Events.On("game.start", [&](const sage::events::Event&) { ++heard; });
    ClickAt(scene, {500.0f, 400.0f});   // мимо
    CHECK_EQ(heard, 0);
}

// Триггеры различаются: связь на наведение не должна срабатывать от щелчка, и
// наоборот. Иначе «звук при наведении» звучал бы и при нажатии.
TEST(Events_triggers_are_told_apart) {
    Scene scene("ui");
    GameObject button = MakeButton(scene, "Play");
    sage::ui::Interactable& act = scene.Registry().get<sage::ui::Interactable>(button.Entity());
    sage::events::Binding hover;
    hover.Trigger = "hoverIn";
    hover.Event = "ui.hover";
    sage::events::Binding click;
    click.Trigger = "click";
    click.Event = "ui.click";
    act.Events.push_back(hover);
    act.Events.push_back(click);

    int hovers = 0, clicks = 0;
    scene.Events.On("ui.hover", [&](const sage::events::Event&) { ++hovers; });
    scene.Events.On("ui.click", [&](const sage::events::Event&) { ++clicks; });

    // Курсор наехал: только наведение.
    sage::ui::UIInputState move;
    move.Mouse = {50.0f, 50.0f};
    sage::ui::UpdateSceneUI(scene, move, 800, 600);
    CHECK_EQ(hovers, 1);
    CHECK_EQ(clicks, 0);

    // Стоит на месте — второй раз наведение не шлётся: подсветка и звук нужны
    // на переходе, а не каждый кадр.
    sage::ui::UpdateSceneUI(scene, move, 800, 600);
    CHECK_EQ(hovers, 1);

    ClickAt(scene, {50.0f, 50.0f});
    CHECK_EQ(clicks, 1);
}

// Ползунок и галка шлют «change»: слушателю всё равно, чем подвинули значение.
TEST(Events_a_checkbox_reports_a_change) {
    Scene scene("ui");
    GameObject box = scene.CreateObject("Sound");
    CHECK_TRUE(sage::ui::ApplyPreset(scene, box.Entity(), "Checkbox"));
    sage::ui::Transform& t = scene.Registry().get<sage::ui::Transform>(box.Entity());
    t.Anchor = UIAnchor::TopLeft;
    t.Offset = {0.0f, 0.0f};
    t.Size = {200.0f, 100.0f};

    sage::events::Binding b;
    b.Trigger = "change";
    b.Event = "settings.sound";
    scene.Registry().get<sage::ui::Interactable>(box.Entity()).Events.push_back(b);

    bool on = false;
    int heard = 0;
    scene.Events.On("settings.sound", [&](const sage::events::Event& e) {
        ++heard;
        on = e.Arg.AsBool();
    });
    ClickAt(scene, {20.0f, 50.0f});
    CHECK_EQ(heard, 1);
}

// Связи — ДАННЫЕ: их настраивают в инспекторе, и они обязаны пережить запись
// сцены. Связь, теряющаяся при сохранении, хуже отсутствующей: кнопка работает
// до перезапуска редактора.
TEST(Events_bindings_survive_save_and_load) {
    Scene scene("ui");
    GameObject button = MakeButton(scene, "Open");
    GameObject door = scene.CreateObject("Door");

    sage::events::Binding b;
    b.Trigger = "click";
    b.Event = "door.open";
    b.Target = EntityRef{door.Id()};
    b.Method = "Open";
    b.Arg = Value(2.5f);
    scene.Registry().get<sage::ui::Interactable>(button.Entity()).Events.push_back(b);

    std::unique_ptr<Scene> back =
        SceneSerializer::LoadFromString(SceneSerializer::SaveToString(scene));
    CHECK_TRUE(back != nullptr);
    if (!back) return;
    GameObject loaded = back->FindByName("Open");
    CHECK_TRUE(loaded.Valid());
    const sage::ui::Interactable& act =
        back->Registry().get<sage::ui::Interactable>(loaded.Entity());
    CHECK_EQ(act.Events.size(), (size_t)1);
    if (act.Events.empty()) return;
    CHECK_EQ(act.Events[0].Trigger, std::string("click"));
    CHECK_EQ(act.Events[0].Event, std::string("door.open"));
    CHECK_EQ(act.Events[0].Target.Id, door.Id());
    CHECK_EQ(act.Events[0].Method, std::string("Open"));
    CHECK_NEAR(act.Events[0].Arg.AsFloat(), 2.5f, 1e-4f);
}

// Выключенная связь не срабатывает: «временно отключить» должно быть галкой, а
// не удалением с последующим набором заново.
TEST(Events_a_disabled_binding_stays_silent) {
    Scene scene("ui");
    GameObject button = MakeButton(scene, "Play");
    sage::events::Binding b;
    b.Trigger = "click";
    b.Event = "game.start";
    b.Enabled = false;
    scene.Registry().get<sage::ui::Interactable>(button.Entity()).Events.push_back(b);

    int heard = 0;
    scene.Events.On("game.start", [&](const sage::events::Event&) { ++heard; });
    ClickAt(scene, {50.0f, 50.0f});
    CHECK_EQ(heard, 0);
}
