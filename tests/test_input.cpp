// Ввод: устройства -> действия -> события (sage/input).
//
// Проверяется не «функция вернула число», а то, ради чего система написана:
// короткое нажатие не теряется, WASD и левый стик дают игре ОДНО И ТО ЖЕ,
// щелчок по интерфейсу не стреляет в игре, а переназначенная клавиша работает
// без единой правки игрового кода.
//
// Ни одного окна в файле: система ввода не знает про GLFW, поэтому проверяется
// обычным тестом — события кладёт сам тест.
#include "TestFramework.h"

#include <cmath>

#include "sage/events/Events.h"
#include "sage/events/TypedBus.h"
#include <GLFW/glfw3.h>

#include "sage/input/GlfwBridge.h"
#include "sage/input/InputSystem.h"

using namespace sage::input;

namespace {

constexpr float kFrame = 1.0f / 60.0f;

// Кадр без единого события — «человек ничего не делает». Нужен почти каждому
// тесту: однокадровые признаки живут ровно до следующего Update.
void Idle(InputSystem& in, int frames = 1) {
    for (int i = 0; i < frames; ++i) in.Update(kFrame);
}

void KeyDown(InputSystem& in, Key key, uint8_t mods = ModNone) {
    in.Push(InputEvent::KeyDown(key, mods));
}
void KeyUp(InputSystem& in, Key key, uint8_t mods = ModNone) {
    in.Push(InputEvent::KeyUp(key, mods));
}

} // namespace

// ===========================================================================
//  СЛОВАРЬ: имена клавиш и разбор привязок
// ===========================================================================

TEST(Input_key_names_survive_a_round_trip) {
    CHECK_EQ(std::string(KeyName(Key::Space)), std::string("SPACE"));
    CHECK_TRUE(ParseKey("SPACE") == Key::Space);
    CHECK_TRUE(ParseKey(KeyName(Key::LeftShift)) == Key::LeftShift);
    // Регистр не должен решать судьбу настройки, которую человек правил руками.
    CHECK_TRUE(ParseKey("space") == Key::Space);
    CHECK_TRUE(ParseKey("Left_Shift") == Key::LeftShift);
    // Синонимы из привычки других движков.
    CHECK_TRUE(ParseKey("RETURN") == Key::Enter);
    CHECK_TRUE(ParseKey("ESC") == Key::Escape);
    CHECK_TRUE(ParseKey("нет такой клавиши") == Key::Unknown);
}

// Старые скрипты игр пишут раскладку именами "W", "SPACE", "LEFT_SHIFT",
// "MOUSE_LEFT" — они обязаны продолжать работать.
TEST(Input_binding_understands_the_names_games_already_use) {
    CHECK_TRUE(Binding::Parse("W").has_value());
    CHECK_TRUE(Binding::Parse("SPACE")->Kind == SourceKind::Key);
    CHECK_TRUE(Binding::Parse("MOUSE_LEFT")->Kind == SourceKind::MouseButton);
    CHECK_TRUE(Binding::Parse("LEFT_SHIFT")->AsKey() == Key::LeftShift);
    CHECK_TRUE(Binding::Parse("PAD_A")->Kind == SourceKind::GamepadButton);
    CHECK_TRUE(Binding::Parse("PAD_LEFT_X")->Kind == SourceKind::GamepadAxis);
    CHECK_FALSE(Binding::Parse("ЧТО-ТО").has_value());
}

TEST(Input_binding_reads_modifiers_and_sign) {
    const auto save = Binding::Parse("CTRL+S");
    CHECK_TRUE(save.has_value());
    CHECK_EQ((int)save->Modifiers, (int)ModCtrl);
    CHECK_TRUE(save->AsKey() == Key::S);
    CHECK_EQ(save->ToString(), std::string("CTRL+S"));

    // Ведущий минус — вклад −1: так ось «вперёд-назад» описывается одной
    // строкой на привязку.
    const auto back = Binding::Parse("-S");
    CHECK_NEAR(back->Scale, -1.0f, 1e-5f);
    CHECK_EQ(back->ToString(), std::string("-S"));

    // Клавиша, в имени которой есть плюс, не должна разваливаться разбором.
    const auto plus = Binding::Parse("CTRL+KP_ADD");
    CHECK_TRUE(plus.has_value());
    CHECK_TRUE(plus->AsKey() == Key::KpAdd);
}

// ===========================================================================
//  УСТРОЙСТВА: короткое нажатие
// ===========================================================================

// Ради этого теста система и перешла с опроса на события. Нажатие, начавшееся
// и кончившееся МЕЖДУ двумя опросами, при опросе не существовало вовсе — а
// жмут коротко именно то, что важно: прыжок, выстрел, инвентарь.
TEST(Input_a_press_and_release_inside_one_frame_is_not_lost) {
    InputSystem in;
    in.Register("Jump").Bind("SPACE");

    KeyDown(in, Key::Space);
    KeyUp(in, Key::Space);   // всё это между двумя кадрами
    in.Update(kFrame);

    CHECK_TRUE(in.WasPressed("Jump"));
    CHECK_TRUE(in.IsDown("Jump"));      // ровно один кадр «нажато»
    CHECK_FALSE(in.WasReleased("Jump"));

    Idle(in);
    CHECK_FALSE(in.IsDown("Jump"));
    CHECK_TRUE(in.WasReleased("Jump")); // отпускание доехало кадром позже
}

TEST(Input_digital_action_walks_through_its_phases) {
    InputSystem in;
    in.Register("Fire").Bind("MOUSE_LEFT");

    in.Push(InputEvent::MouseDown(MouseButton::Left));
    in.Update(kFrame);
    CHECK_TRUE(in.WasPressed("Fire"));
    CHECK_TRUE(in.Find("Fire")->CurrentPhase() == Phase::Pressed);

    Idle(in);
    CHECK_FALSE(in.WasPressed("Fire"));
    CHECK_TRUE(in.IsDown("Fire"));
    CHECK_TRUE(in.Find("Fire")->CurrentPhase() == Phase::Held);

    in.Push(InputEvent::MouseUp(MouseButton::Left));
    in.Update(kFrame);
    CHECK_TRUE(in.WasReleased("Fire"));
    CHECK_FALSE(in.IsDown("Fire"));
}

// Неизвестное имя не роняет игру: раскладку объявляют скрипты, и опечатка в
// имени действия не должна прерывать бой исключением.
TEST(Input_unknown_action_answers_no_instead_of_throwing) {
    InputSystem in;
    CHECK_FALSE(in.Has("Такого нет"));
    CHECK_FALSE(in.IsDown("Такого нет"));
    CHECK_NEAR(in.Value("Такого нет"), 0.0f, 1e-6f);
}

// ===========================================================================
//  ОСИ И ВЕКТОРЫ
// ===========================================================================

TEST(Input_axis_adds_up_to_plus_and_minus_one) {
    InputSystem in;
    in.Register("MoveForward", ActionType::Axis).Bind("W");
    in.Find("MoveForward")->Bind("-S");

    KeyDown(in, Key::W);
    in.Update(kFrame);
    CHECK_NEAR(in.Value("MoveForward"), 1.0f, 1e-5f);

    KeyDown(in, Key::S);
    in.Update(kFrame);
    // Зажаты обе — человек не едет в обе стороны сразу.
    CHECK_NEAR(in.Value("MoveForward"), 0.0f, 1e-5f);

    KeyUp(in, Key::W);
    in.Update(kFrame);
    CHECK_NEAR(in.Value("MoveForward"), -1.0f, 1e-5f);
}

// Старейшая ошибка в играх: по диагонали персонаж быстрее, чем прямо.
TEST(Input_diagonal_movement_is_not_faster_than_straight) {
    InputSystem in;
    in.Register("Move", ActionType::Vector).BindVector("W", "S", "A", "D");

    KeyDown(in, Key::W);
    KeyDown(in, Key::D);
    in.Update(kFrame);

    const glm::vec2 v = in.Vector("Move");
    CHECK_NEAR(std::sqrt(v.x * v.x + v.y * v.y), 1.0f, 1e-4f);
    CHECK_TRUE(v.x > 0.0f && v.y > 0.0f);
}

// ГЛАВНОЕ ТРЕБОВАНИЕ независимости от устройства: игровой код спрашивает одно
// и то же, а играть можно чем угодно.
TEST(Input_keyboard_and_gamepad_feed_the_same_action) {
    InputSystem in;
    Action& move = in.Register("Move", ActionType::Vector);
    move.BindVector("W", "S", "A", "D");
    move.Bind(Binding::OfPadAxis(GamepadAxis::LeftX).On(Component::X));
    move.Bind(Binding::OfPadAxis(GamepadAxis::LeftY).On(Component::Y));

    KeyDown(in, Key::W);
    in.Update(kFrame);
    const glm::vec2 fromKeys = in.Vector("Move");

    KeyUp(in, Key::W);
    in.Push(InputEvent::PadConnected(0, true));
    in.Push(InputEvent::PadAxisMoved(GamepadAxis::LeftY, 1.0f, 0));
    in.Update(kFrame);
    const glm::vec2 fromPad = in.Vector("Move");

    CHECK_NEAR(fromKeys.y, 1.0f, 1e-4f);
    CHECK_NEAR(fromPad.y, fromKeys.y, 1e-4f);
    CHECK_NEAR(fromPad.x, fromKeys.x, 1e-4f);
}

// Мёртвая зона: дрожание стика около центра не должно вести персонажа.
TEST(Input_dead_zone_ignores_a_resting_stick) {
    InputSystem in;
    Action& move = in.Register("Move", ActionType::Vector);
    move.Bind(Binding::OfPadAxis(GamepadAxis::LeftX).On(Component::X));
    move.Settings().DeadZone = 0.2f;

    in.Push(InputEvent::PadConnected(0, true));
    in.Push(InputEvent::PadAxisMoved(GamepadAxis::LeftX, 0.1f, 0));
    in.Update(kFrame);
    CHECK_NEAR(in.Vector("Move").x, 0.0f, 1e-5f);

    // За зоной значение растянуто обратно до единицы: иначе на полностью
    // отклонённом стике персонаж не выходит на полную скорость.
    in.Push(InputEvent::PadAxisMoved(GamepadAxis::LeftX, 1.0f, 0));
    in.Update(kFrame);
    CHECK_NEAR(in.Vector("Move").x, 1.0f, 1e-4f);
}

// Мёртвая зона придумана для стика — резать ею клавиатуру нельзя.
TEST(Input_dead_zone_does_not_touch_the_keyboard) {
    InputSystem in;
    Action& move = in.Register("Move", ActionType::Axis);
    move.Bind(Binding::OfKey(Key::D));
    move.Settings().DeadZone = 0.9f;

    KeyDown(in, Key::D);
    in.Update(kFrame);
    CHECK_NEAR(in.Value("Move"), 1.0f, 1e-5f);
}

TEST(Input_smoothing_approaches_the_target_instead_of_jumping) {
    InputSystem in;
    Action& look = in.Register("Look", ActionType::Axis);
    look.Bind(Binding::OfKey(Key::D));
    look.Settings().Smoothing = 0.2f;

    KeyDown(in, Key::D);
    in.Update(kFrame);
    const float first = in.Value("Look");
    CHECK_TRUE(first > 0.0f);
    CHECK_TRUE(first < 1.0f); // сглаженное значение не прыгает сразу в цель

    for (int i = 0; i < 60; ++i) in.Update(kFrame);
    CHECK_NEAR(in.Value("Look"), 1.0f, 0.05f);
}

// ===========================================================================
//  МОДИФИКАТОРЫ, УДЕРЖАНИЕ И КОРОТКОЕ НАЖАТИЕ
// ===========================================================================

TEST(Input_modifier_binding_needs_its_modifier) {
    InputSystem in;
    in.Register("Save").Bind("CTRL+S");

    KeyDown(in, Key::S);
    in.Update(kFrame);
    CHECK_FALSE(in.IsDown("Save")); // просто S — это не «сохранить»

    KeyUp(in, Key::S);
    in.Update(kFrame);
    KeyDown(in, Key::LeftControl);
    KeyDown(in, Key::S);
    in.Update(kFrame);
    CHECK_TRUE(in.WasPressed("Save"));
}

// Бег не должен ломать ходьбу: W с зажатым Shift — по-прежнему W.
TEST(Input_a_plain_binding_still_works_under_a_modifier) {
    InputSystem in;
    in.Register("Forward").Bind("W");

    KeyDown(in, Key::LeftShift);
    KeyDown(in, Key::W);
    in.Update(kFrame);
    CHECK_TRUE(in.IsDown("Forward"));
}

TEST(Input_exact_modifiers_are_available_for_shortcuts) {
    InputSystem in;
    Action& save = in.Register("Save");
    save.Bind(Binding::OfKey(Key::S, ModCtrl));
    save.Settings().ExactModifiers = true;

    KeyDown(in, Key::LeftControl);
    KeyDown(in, Key::LeftShift);
    KeyDown(in, Key::S);
    in.Update(kFrame);
    CHECK_FALSE(in.IsDown("Save")); // CTRL+SHIFT+S — это другое сочетание
}

// Одна клавиша под двумя действиями: коротко — поговорить, удержать — открыть
// расширенное меню.
TEST(Input_tap_and_hold_split_one_key_into_two_actions) {
    InputSystem in;
    Action& tap = in.Register("Interact");
    tap.Bind("E");
    tap.Settings().Trigger = TriggerMode::Tap;
    tap.Settings().TapTime = 0.2f;

    Action& hold = in.Register("InteractAdvanced");
    hold.Bind("E");
    hold.Settings().Trigger = TriggerMode::Hold;
    hold.Settings().HoldTime = 0.4f;

    // Коротко: нажали и отпустили через два кадра.
    KeyDown(in, Key::E);
    in.Update(kFrame);
    in.Update(kFrame);
    KeyUp(in, Key::E);
    in.Update(kFrame);
    CHECK_TRUE(in.Triggered("Interact"));
    CHECK_FALSE(in.Triggered("InteractAdvanced"));

    // Долго: держим полсекунды.
    KeyDown(in, Key::E);
    bool holdFired = false;
    int holdFrames = 0;
    for (int i = 0; i < 40; ++i) {
        in.Update(kFrame);
        if (in.Triggered("InteractAdvanced")) { holdFired = true; ++holdFrames; }
    }
    CHECK_TRUE(holdFired);
    CHECK_EQ(holdFrames, 1); // удержание срабатывает ОДИН раз, а не каждый кадр
    KeyUp(in, Key::E);
    in.Update(kFrame);
    CHECK_FALSE(in.Triggered("Interact")); // долгое нажатие — не короткое
}

// ===========================================================================
//  КОНТЕКСТЫ И ПОТРЕБЛЕНИЕ
// ===========================================================================

// Одна клавиша значит разное в игре и в инвентаре (§8 ТЗ).
TEST(Input_same_key_means_different_things_in_different_contexts) {
    InputSystem in;
    Context& gameplay = in.CreateContext("Gameplay", 10);
    Context& inventory = in.CreateContext("Inventory", 50);
    gameplay.Add("Interact").Bind("E");
    inventory.Add("Equip").Bind("E");
    inventory.SetEnabled(false);

    KeyDown(in, Key::E);
    in.Update(kFrame);
    CHECK_TRUE(in.WasPressed("Interact"));
    CHECK_FALSE(in.WasPressed("Equip"));

    KeyUp(in, Key::E);
    in.Update(kFrame);
    inventory.SetEnabled(true);
    gameplay.SetEnabled(false);

    KeyDown(in, Key::E);
    in.Update(kFrame);
    CHECK_TRUE(in.WasPressed("Equip"));
    CHECK_FALSE(in.WasPressed("Interact"));
}

// Щелчок по кнопке интерфейса не должен ОДНОВРЕМЕННО стрелять в игре (§29).
TEST(Input_ui_consumes_the_click_and_gameplay_does_not_get_it) {
    InputSystem in;
    Context& ui = in.CreateContext("UI", 100);
    ui.SetBlocks(DeviceMouse);
    ui.Add("Click").Bind("MOUSE_LEFT");

    Context& game = in.CreateContext("Gameplay", 10);
    game.Add("Fire").Bind("MOUSE_LEFT");
    game.Add("Reload").Bind("R");

    in.Push(InputEvent::MouseDown(MouseButton::Left));
    KeyDown(in, Key::R);
    in.Update(kFrame);

    CHECK_TRUE(ui.Find("Click")->WasPressed());
    CHECK_FALSE(game.Find("Fire")->IsDown());
    // Забрана МЫШЬ, а не ввод целиком: клавиатура осталась у игры.
    CHECK_TRUE(game.Find("Reload")->IsDown());
}

// Интерфейс, до которого в этом кадре не дотронулись, не должен глушить игру.
TEST(Input_an_idle_ui_context_does_not_block_anything) {
    InputSystem in;
    Context& ui = in.CreateContext("UI", 100);
    ui.SetBlocks(DeviceMouse);
    ui.Add("Click").Bind("MOUSE_RIGHT");

    Context& game = in.CreateContext("Gameplay", 10);
    game.Add("Fire").Bind("MOUSE_LEFT");

    in.Push(InputEvent::MouseDown(MouseButton::Left));
    in.Update(kFrame);
    CHECK_TRUE(game.Find("Fire")->IsDown());
}

// Выключенный контекст ОТПУСКАЕТ свои действия: игрок, открывший меню с
// зажатым «вперёд», не должен вернуться в игру, которая всё это время шла.
TEST(Input_disabling_a_context_releases_its_actions) {
    InputSystem in;
    Context& game = in.CreateContext("Gameplay", 10);
    game.Add("Forward").Bind("W");

    KeyDown(in, Key::W);
    in.Update(kFrame);
    CHECK_TRUE(game.Find("Forward")->IsDown());

    game.SetEnabled(false);
    CHECK_FALSE(game.Find("Forward")->IsDown());
    CHECK_TRUE(game.Find("Forward")->WasReleased()); // отпущено честно
}

// Окно потеряло фокус с зажатой клавишей — игра не должна идти вперёд сама.
TEST(Input_release_all_stops_everything_that_was_held) {
    InputSystem in;
    in.Register("Forward").Bind("W");

    KeyDown(in, Key::W);
    in.Update(kFrame);
    CHECK_TRUE(in.IsDown("Forward"));

    in.ReleaseAll();
    CHECK_FALSE(in.IsDown("Forward"));
    in.Update(kFrame);
    CHECK_FALSE(in.IsDown("Forward"));
}

// Интерфейс, живущий вне контекстов (панели редактора на ImGui), забирает
// устройство на кадр.
TEST(Input_blocking_a_device_lasts_exactly_one_frame) {
    InputSystem in;
    in.Register("Fire").Bind("MOUSE_LEFT");

    in.Push(InputEvent::MouseDown(MouseButton::Left));
    in.BlockDevices(DeviceMouse);
    in.Update(kFrame);
    CHECK_FALSE(in.IsDown("Fire"));

    in.Update(kFrame); // маску никто не возвращал — она снялась сама
    CHECK_TRUE(in.IsDown("Fire"));
}

// ===========================================================================
//  ПЕРЕНАЗНАЧЕНИЕ
// ===========================================================================

// Игрок поменял прыжок с пробела на Q — игровой код не изменился ни на строку.
TEST(Input_rebinding_moves_the_action_and_leaves_the_code_alone) {
    InputSystem in;
    in.Register("Jump").Bind("SPACE");

    CHECK_TRUE(in.Rebind("Jump", "Q"));

    KeyDown(in, Key::Space);
    in.Update(kFrame);
    CHECK_FALSE(in.WasPressed("Jump")); // пробел больше не прыжок

    KeyUp(in, Key::Space);
    KeyDown(in, Key::Q);
    in.Update(kFrame);
    CHECK_TRUE(in.WasPressed("Jump"));

    CHECK_FALSE(in.Rebind("Jump", "не клавиша"));
    CHECK_FALSE(in.Rebind("Нет такого действия", "Q"));
}

// Любой из источников вызывает действие (§21).
TEST(Input_several_bindings_all_lead_to_one_action) {
    InputSystem in;
    Action& interact = in.Register("Interact");
    interact.Bind("E");
    interact.Bind("F");
    interact.Bind(Binding::OfPadButton(GamepadButton::X_));

    KeyDown(in, Key::F);
    in.Update(kFrame);
    CHECK_TRUE(in.WasPressed("Interact"));

    KeyUp(in, Key::F);
    in.Update(kFrame);
    in.Push(InputEvent::PadConnected(0, true));
    in.Push(InputEvent::PadDown(GamepadButton::X_, 0));
    in.Update(kFrame);
    CHECK_TRUE(in.WasPressed("Interact"));
}

// Назначая занятую клавишу, экран настроек обязан узнать, у кого её отбирают.
TEST(Input_finds_which_action_already_owns_a_key) {
    InputSystem in;
    in.Register("Jump").Bind("SPACE");
    Action* owner = in.FindByBinding(*Binding::Parse("SPACE"));
    CHECK_TRUE(owner != nullptr);
    CHECK_EQ(owner->Name(), std::string("Jump"));
    CHECK_TRUE(in.FindByBinding(*Binding::Parse("Q")) == nullptr);
}

// ===========================================================================
//  СОХРАНЕНИЕ РАСКЛАДКИ
// ===========================================================================

TEST(Input_mapping_survives_saving_and_loading) {
    InputSystem saved;
    saved.CreateContext("UI", 100).SetBlocks(DeviceMouse);
    saved.Register("UI", "Click", ActionType::Digital).Bind("MOUSE_LEFT");
    Action& jump = saved.Register("Jump");
    jump.Bind("SPACE");
    jump.Bind("PAD_A");
    jump.Settings().Trigger = TriggerMode::Tap;
    Action& move = saved.Register("Move", ActionType::Vector);
    move.BindVector("W", "S", "A", "D");
    move.Settings().DeadZone = 0.3f;

    const std::string text = saved.SaveMappingToString();

    InputSystem loaded;
    CHECK_TRUE(loaded.LoadMappingFromString(text));
    CHECK_TRUE(loaded.Has("Jump"));
    CHECK_EQ((int)loaded.Find("Jump")->Bindings().size(), 2);
    CHECK_TRUE(loaded.Find("Jump")->Settings().Trigger == TriggerMode::Tap);
    CHECK_NEAR(loaded.Find("Move")->Settings().DeadZone, 0.3f, 1e-5f);
    CHECK_TRUE(loaded.Find("Move")->Type() == ActionType::Vector);
    CHECK_TRUE(loaded.ContextEnabled("UI"));
    CHECK_EQ((int)loaded.FindContext("UI")->Blocks(), (int)DeviceMouse);

    // И главное: загруженная раскладка действительно управляет игрой.
    KeyDown(loaded, Key::W);
    loaded.Update(kFrame);
    CHECK_NEAR(loaded.Vector("Move").y, 1.0f, 1e-4f);
}

// Настройки игрока ЗАМЕЩАЮТ умолчания, а не дописываются к ним: иначе
// переназначенная клавиша продолжает работать и на старом месте.
TEST(Input_loading_a_mapping_replaces_bindings_instead_of_adding) {
    InputSystem in;
    in.Register("Jump").Bind("SPACE");

    const std::string userMapping = R"({
        "version": 1,
        "contexts": [
            {"name": "Gameplay", "priority": 10,
             "actions": [{"name": "Jump", "type": "digital",
                          "bindings": [{"source": "Q"}]}]}
        ]
    })";
    CHECK_TRUE(in.LoadMappingFromString(userMapping));
    CHECK_EQ((int)in.Find("Jump")->Bindings().size(), 1);

    KeyDown(in, Key::Space);
    in.Update(kFrame);
    CHECK_FALSE(in.WasPressed("Jump"));
}

TEST(Input_a_broken_mapping_file_does_not_take_the_game_down) {
    InputSystem in;
    CHECK_FALSE(in.LoadMappingFromString("это не json"));
    CHECK_FALSE(in.LoadMappingFromString("{}"));
}

// ===========================================================================
//  СОБЫТИЯ ДЕЙСТВИЙ
// ===========================================================================

// Игра слышит «Прыжок», а не «нажали пробел» (§11 ТЗ).
TEST(Input_publishes_action_events_instead_of_key_events) {
    InputSystem in;
    in.Register("Jump").Bind("SPACE");

    std::vector<std::string> heard;
    in.OnAction([&](const ActionEvent& e) {
        if (e.Triggered) heard.push_back(e.Action);
    });

    KeyDown(in, Key::Space);
    in.Update(kFrame);
    CHECK_EQ((int)heard.size(), 1);
    CHECK_EQ(heard[0], std::string("Jump"));

    // Непрерывное действие не должно шуметь событием каждый кадр.
    Idle(in, 5);
    CHECK_EQ((int)heard.size(), 1);
}

// Тот же прыжок слышат те, кто говорит данными: Lua и связи в инспекторе.
TEST(Input_action_reaches_the_named_event_bus) {
    InputSystem in;
    sage::events::Bus bus;
    in.SetEventBus(&bus);
    in.Register("Jump").Bind("SPACE");

    int fired = 0;
    bus.On("input.Jump", [&](const sage::events::Event&) { ++fired; });

    KeyDown(in, Key::Space);
    in.Update(kFrame);
    CHECK_EQ(fired, 1);

    KeyUp(in, Key::Space);
    in.Update(kFrame);
    CHECK_EQ(fired, 1); // отпускание — отдельное событие, не повтор нажатия
}

TEST(Input_action_reaches_the_typed_bus) {
    InputSystem in;
    sage::events::TypedBus bus;
    in.SetTypedBus(&bus);
    in.Register("Move", ActionType::Vector).BindVector("W", "S", "A", "D");

    glm::vec2 seen(0.0f);
    bus.Subscribe<ActionEvent>([&](const ActionEvent& e) { seen = e.Vector; });

    KeyDown(in, Key::W);
    in.Update(kFrame);
    CHECK_NEAR(seen.y, 1.0f, 1e-4f);
}

// Тот, кто разбирает сырые события сам (поле ввода), может их потребить.
TEST(Input_raw_subscriber_can_consume_an_event) {
    InputSystem in;
    int seen = 0;
    in.OnRawEvent([&](const InputEvent& e) {
        if (e.Type == InputEventType::KeyPressed) { ++seen; return true; }
        return false;
    });
    int second = 0;
    in.OnRawEvent([&](const InputEvent&) { ++second; return false; }, -10);

    KeyDown(in, Key::A);
    in.Update(kFrame);
    CHECK_EQ(seen, 1);
    CHECK_EQ(second, 0); // потреблённое дальше не идёт

    // Состояние устройств всё равно знает правду о железе — иначе
    // потреблённое отпускание оставило бы клавишу вечно нажатой.
    CHECK_TRUE(in.State().Keys().Down(Key::A));
}

TEST(Input_typed_text_arrives_as_symbols_not_key_codes) {
    InputSystem in;
    in.Push(InputEvent::Text((unsigned int)'ё'));
    in.Push(InputEvent::Text(0x0416)); // Ж
    in.Update(kFrame);
    CHECK_EQ((int)in.TypedText().size(), 2);
    CHECK_EQ((int)in.TypedText()[1], 0x0416);
    Idle(in);
    CHECK_EQ((int)in.TypedText().size(), 0); // текст живёт один кадр
}

// ===========================================================================
//  ГРАНИЦА ПЛАТФОРМЫ
// ===========================================================================

// Мост обязан переводить коды окна в словарь движка — иначе сохранённая
// раскладка означала бы разное в разных сборках.
TEST(Input_glfw_codes_translate_into_engine_keys) {
    CHECK_TRUE(GlfwBridge::FromGlfwKey(GLFW_KEY_W) == Key::W);
    CHECK_TRUE(GlfwBridge::FromGlfwKey(GLFW_KEY_SPACE) == Key::Space);
    CHECK_TRUE(GlfwBridge::FromGlfwKey(GLFW_KEY_KP_ADD) == Key::KpAdd);
    CHECK_TRUE(GlfwBridge::FromGlfwKey(-1) == Key::Unknown);

    bool ok = false;
    CHECK_TRUE(GlfwBridge::FromGlfwMouseButton(GLFW_MOUSE_BUTTON_RIGHT, &ok) == MouseButton::Right);
    CHECK_TRUE(ok);

    CHECK_EQ((int)GlfwBridge::FromGlfwMods(GLFW_MOD_CONTROL | GLFW_MOD_SHIFT),
             (int)(ModCtrl | ModShift));
}
