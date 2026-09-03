#include "sage/ui/UIDemos.h"

#include <algorithm>

#include "sage/scene/Components.h"
#include "sage/scene/Scene.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIPresets.h"

namespace sage::ui {
namespace {

// Сущность-элемент: имя, родитель, обязательный прямоугольник.
GameObject Element(Scene& scene, const std::string& name, GameObject parent, const Transform& xf) {
    GameObject e = scene.CreateObject(name);
    scene.Registry().emplace<Transform>(e.Entity(), xf);
    if (parent.Valid()) scene.SetParent(e.Entity(), parent.Entity());
    return e;
}

// Часть на сущность — короткая запись для того, что ниже повторяется десятки раз.
template <typename T>
T& Add(Scene& scene, GameObject e, const T& value = T{}) {
    return scene.Registry().emplace_or_replace<T>(e.Entity(), value);
}

Fill PanelFill(glm::vec4 color, float rounding, float shadow = 0.0f) {
    Fill f;
    f.Color = color;
    f.Rounding = rounding;
    f.ShadowSize = shadow;
    return f;
}

Label Text(const std::string& text, float scale, glm::vec4 color,
           Label::Align h = Label::Align::Center) {
    Label l;
    l.Text = text;
    l.Scale = scale;
    l.Color = color;
    l.Horizontal = h;
    return l;
}

// --- Главное меню -----------------------------------------------------------
//
// Всё, чего у старой системы не было, здесь работает разом: холст задаёт
// опорное разрешение (меню не сжимается вчетверо на 4K), раскладка сама
// расставляет кнопки (шестую можно дописать, не пересчитывая пять чужих
// отступов), у кнопок есть ИМЯ ДЕЙСТВИЯ, а группа гасит весь экран одним
// числом.
int BuildMenu(Scene& scene) {
    GameObject root = scene.CreateObject("DemoMenu");

    Transform screenXf;
    screenXf.Anchor = UIAnchor::TopLeft;
    screenXf.Offset = {0.0f, 0.0f};
    screenXf.Mode = Transform::Stretch::Both;
    GameObject screen = Element(scene, "MenuScreen", root, screenXf);
    Add(scene, screen, PanelFill({0.04f, 0.05f, 0.08f, 0.88f}, 0.0f));
    // Холст: вёрстка сделана под 1920x1080 и пересчитывается под окно. Без него
    // меню жило бы в пикселях экрана — то, из-за чего кнопки «уезжают» на
    // другом разрешении.
    Canvas canvas;
    canvas.Mode = Canvas::Scale::ScaleWithSize;
    canvas.SortOrder = 10;   // меню поверх худа
    Add(scene, screen, canvas);
    // Группа: показать меню наполовину — одно число, а не проход по детям.
    Add(scene, screen, Group{});

    Transform titleXf;
    titleXf.Anchor = UIAnchor::TopCenter;
    titleXf.Offset = {0.0f, 120.0f};
    titleXf.Size = {600.0f, 72.0f};
    GameObject title = Element(scene, "MenuTitle", screen, titleXf);
    Add(scene, title, Text("SAGE", 6.0f, {1.0f, 0.95f, 0.82f, 1.0f}));

    Transform listXf;
    // Якорь Center уже центрирует элемент — Pivot здесь не нужен (см. Layout.h).
    // А вот отступ обнулить НАДО: по умолчанию он {16,16} (разумно для угла), и
    // с ним «по центру» оказывается на шестнадцать пикселей правее и ниже.
    listXf.Anchor = UIAnchor::Center;
    listXf.Offset = {0.0f, 0.0f};
    listXf.Size = {320.0f, 260.0f};
    GameObject list = Element(scene, "MenuButtons", screen, listXf);
    Layout layout;
    layout.Direction = Layout::Flow::Vertical;
    layout.Justify = Layout::Align::Center;
    layout.Spacing = 12.0f;
    Add(scene, list, layout);

    // Имя действия — то, ради чего кнопка и существует. Игра спрашивает
    // «нажали ли continue», и это переживает пересборку сцены, в отличие от
    // номера сущности.
    struct Item { const char* Name; const char* Text; const char* Action; };
    const Item items[] = {
        {"BtnContinue", "Продолжить", "continue"},
        {"BtnNewGame", "Новая игра", "new_game"},
        {"BtnSettings", "Настройки", "settings"},
        {"BtnQuit", "Выход", "quit"},
    };
    int layer = 0;
    for (const Item& item : items) {
        Transform xf;
        xf.Size = {320.0f, 52.0f};
        xf.Layer = layer++;
        GameObject button = Element(scene, item.Name, list, xf);
        Add(scene, button, PanelFill({0.12f, 0.14f, 0.20f, 1.0f}, 10.0f, 8.0f));
        Add(scene, button, Text(item.Text, 2.4f, {0.92f, 0.94f, 1.0f, 1.0f}));
        Interactable act;
        act.Action = item.Action;
        Add(scene, button, act);
    }
    return root.Id();
}

// --- Худ --------------------------------------------------------------------
int BuildHud(Scene& scene) {
    GameObject root = scene.CreateObject("DemoHud");

    Transform screenXf;
    screenXf.Anchor = UIAnchor::TopLeft;
    screenXf.Offset = {0.0f, 0.0f};
    screenXf.Mode = Transform::Stretch::Both;
    GameObject screen = Element(scene, "HudScreen", root, screenXf);
    Canvas canvas;
    canvas.Mode = Canvas::Scale::ScaleWithSize;
    canvas.SortOrder = 0;    // худ ПОД меню паузы
    Add(scene, screen, canvas);
    // Прозрачной подложки у худа нет: он не должен затемнять игру.

    // Полоса здоровья: подложка, значок и шкала — ТРИ ОБЪЕКТА, вложенных друг в
    // друга, а не три части на одной сущности. Раньше значок сам отодвигал бы
    // шкалу вправо — правилом, зашитым в отрисовку; теперь он занимает свой
    // прямоугольник, а шкала — свой, и любой из них можно подвинуть, убрать
    // или заменить, не трогая соседей.
    Transform hpXf;
    hpXf.Anchor = UIAnchor::TopLeft;
    hpXf.Offset = {24.0f, 24.0f};
    hpXf.Size = {320.0f, 34.0f};
    GameObject hp = Element(scene, "HudHealth", screen, hpXf);
    Add(scene, hp, PanelFill({0.0f, 0.0f, 0.0f, 0.45f}, 8.0f));

    Transform heartXf;
    heartXf.Anchor = UIAnchor::CenterLeft;
    heartXf.Offset = {6.0f, 0.0f};
    heartXf.Size = {26.0f, 26.0f};
    GameObject heartObj = Element(scene, "HudHealthIcon", hp, heartXf);
    Icon heart;
    heart.Name = "heart";
    heart.Color = {0.95f, 0.35f, 0.35f, 1.0f};
    Add(scene, heartObj, heart);

    Transform barXf;
    barXf.Anchor = UIAnchor::TopLeft;
    barXf.Mode = Transform::Stretch::Both;
    barXf.Margin = {38.0f, 6.0f, 8.0f, 6.0f};   // место слева — под значок
    GameObject barObj = Element(scene, "HudHealthBar", hp, barXf);
    Add(scene, barObj, PanelFill({0.12f, 0.05f, 0.05f, 0.7f}, 5.0f));
    Bar bar;
    bar.Value = 0.72f;
    bar.FillColor = {0.85f, 0.25f, 0.25f, 1.0f};
    // Сглаживание: шкала едет к цели за четверть секунды, а не прыгает рывком.
    bar.Smoothing = 3.0f;
    Add(scene, barObj, bar);

    // Патроны: ширина по содержимому — «7 / 30» и «120 / 240» не должны
    // плавать в панели одного размера.
    Transform ammoXf;
    ammoXf.Anchor = UIAnchor::BottomRight;
    ammoXf.Offset = {24.0f, 24.0f};
    ammoXf.Size = {120.0f, 40.0f};
    GameObject ammo = Element(scene, "HudAmmo", screen, ammoXf);
    Add(scene, ammo, PanelFill({0.0f, 0.0f, 0.0f, 0.45f}, 8.0f));
    Label ammoLabel = Text("7 / 30", 2.6f, {1.0f, 0.95f, 0.75f, 1.0f});
    ammoLabel.AutoWidth = true;
    ammoLabel.PadX = 14.0f;
    Add(scene, ammo, ammoLabel);

    // Панель заданий: список с маской — строки, не влезшие в высоту, обрезаются,
    // а не выезжают на игру.
    Transform questsXf;
    questsXf.Anchor = UIAnchor::TopRight;
    questsXf.Offset = {24.0f, 24.0f};
    questsXf.Size = {280.0f, 150.0f};
    GameObject quests = Element(scene, "HudQuests", screen, questsXf);
    Add(scene, quests, PanelFill({0.05f, 0.06f, 0.10f, 0.72f}, 10.0f));
    Add(scene, quests, Mask{});
    Layout questLayout;
    questLayout.Direction = Layout::Flow::Vertical;
    questLayout.Spacing = 6.0f;
    Add(scene, quests, questLayout);

    const char* lines[] = {"Найти ключ", "Открыть ворота", "Вернуться в лагерь",
                           "Поговорить со старостой", "Забрать награду"};
    int layer = 0;
    for (const char* line : lines) {
        Transform xf;
        xf.Size = {248.0f, 26.0f};
        xf.Layer = layer++;
        GameObject row = Element(scene, std::string("Quest") + std::to_string(layer), quests, xf);
        Add(scene, row, Text(line, 1.6f, {0.85f, 0.88f, 0.95f, 1.0f}, Label::Align::Start));
    }
    return root.Id();
}

// --- Настройки --------------------------------------------------------------
int BuildSettings(Scene& scene) {
    GameObject root = scene.CreateObject("DemoSettings");

    Transform panelXf;
    panelXf.Anchor = UIAnchor::Center;
    panelXf.Offset = {0.0f, 0.0f};   // см. меню: у якоря Center отступ обнуляем
    panelXf.Size = {420.0f, 340.0f};
    GameObject panel = Element(scene, "SettingsPanel", root, panelXf);
    Add(scene, panel, PanelFill({0.07f, 0.08f, 0.12f, 0.95f}, 14.0f, 16.0f));
    Canvas canvas;
    canvas.Mode = Canvas::Scale::ScaleWithSize;
    canvas.SortOrder = 20;
    Add(scene, panel, canvas);
    Layout layout;
    layout.Direction = Layout::Flow::Vertical;
    layout.Spacing = 10.0f;
    layout.Padding = {18.0f, 18.0f, 18.0f, 18.0f};
    // Панель обнимает содержимое: высота 340 в Transform — лишь запасная, на
    // случай пустой панели. Добавили строку — панель подросла сама.
    layout.FitContent = true;
    Add(scene, panel, layout);

    int layer = 0;
    auto row = [&](const std::string& name, float height) {
        Transform xf;
        xf.Size = {0.0f, height};
        xf.Layer = layer++;
        return Element(scene, name, panel, xf);
    };

    // Строка настройки: слева подпись, справа сам элемент.
    //
    // Именно ДВУМЯ элементами, а не подписью поверх ползунка: дорожка занимает
    // всю ширину элемента, и надпись на ней тонет под заполнением — это сразу
    // видно на первом же кадре. Раскладка строки расставляет обе части сама.
    auto labelledRow = [&](const std::string& name, const std::string& caption, float height) {
        GameObject line = row(name + "Row", height);
        Layout inner;
        inner.Direction = Layout::Flow::Horizontal;
        inner.Spacing = 10.0f;
        inner.Padding = {0.0f, 0.0f, 0.0f, 0.0f};
        Add(scene, line, inner);

        Transform capXf;
        capXf.Size = {150.0f, height};
        GameObject cap = Element(scene, name + "Caption", line, capXf);
        Add(scene, cap, Text(caption, 1.6f, {0.9f, 0.92f, 1.0f, 1.0f}, Label::Align::Start));

        Transform ctlXf;
        ctlXf.Size = {224.0f, height};
        ctlXf.Layer = 1;
        return Element(scene, name, line, ctlXf);
    };

    GameObject title = row("SettingsTitle", 40.0f);
    Add(scene, title, Text("Настройки", 3.0f, {1.0f, 0.95f, 0.85f, 1.0f}, Label::Align::Start));

    // Громкость: значение В ИГРОВЫХ ЕДИНИЦАХ и шаг. Раньше ползунок хранил долю
    // 0..1, и «громкость 45%» приходилось пересчитывать каждому читателю.
    //
    // Цвет дорожки — У САМОГО ползунка, а не у подложки рядом: подложка
    // закрашивает элемент ЦЕЛИКОМ, а дорожка тонкая и по центру.
    Range volumeRange;
    volumeRange.Min = 0.0f;
    volumeRange.Max = 100.0f;
    volumeRange.Value = 70.0f;
    volumeRange.Step = 5.0f;
    volumeRange.TrackColor = {0.13f, 0.15f, 0.20f, 1.0f};
    GameObject volume = labelledRow("VolumeSlider", "Громкость", 30.0f);
    Add(scene, volume, Interactable{});
    Add(scene, volume, volumeRange);

    Range sensRange;
    sensRange.Min = 0.1f;
    sensRange.Max = 5.0f;
    sensRange.Value = 1.5f;
    sensRange.TrackColor = {0.13f, 0.15f, 0.20f, 1.0f};
    GameObject sens = labelledRow("SensitivitySlider", "Чувствительность", 30.0f);
    Add(scene, sens, Interactable{});
    Add(scene, sens, sensRange);

    // Галка — тот же диапазон с шагом в единицу: отдельного вида элемента для
    // двух значений заводить незачем. А подпись к ней — ОТДЕЛЬНЫЙ ОБЪЕКТ
    // рядом. Раньше текст жил на той же сущности и начинался «за квадратиком»
    // по правилу из отрисовки; теперь его видно в дереве и можно поставить
    // хоть слева от галки, хоть под ней.
    GameObject fullscreen = row("FullscreenToggle", 30.0f);
    Add(scene, fullscreen, Interactable{});
    Range toggle;
    toggle.Toggle = true;
    toggle.Step = 1.0f;
    toggle.Value = 1.0f;
    toggle.TrackColor = {0.13f, 0.15f, 0.20f, 1.0f};
    Add(scene, fullscreen, toggle);

    Transform fsTextXf;
    fsTextXf.Anchor = UIAnchor::TopLeft;
    fsTextXf.Mode = Transform::Stretch::Both;
    fsTextXf.Margin = {38.0f, 0.0f, 0.0f, 0.0f};   // место слева — под квадратик
    GameObject fsText = Element(scene, "FullscreenCaption", fullscreen, fsTextXf);
    Add(scene, fsText, Text("Полный экран", 1.6f, {0.9f, 0.92f, 1.0f, 1.0f},
                            Label::Align::Start));

    GameObject name = labelledRow("PlayerName", "Имя", 34.0f);
    Add(scene, name, PanelFill({0.10f, 0.11f, 0.16f, 1.0f}, 8.0f));
    Add(scene, name, Text("", 1.8f, {1.0f, 1.0f, 1.0f, 1.0f}, Label::Align::Start));
    Add(scene, name, Interactable{});
    TextInput field;
    field.Placeholder = "Имя игрока";
    field.MaxLength = 24;
    Add(scene, name, field);

    GameObject apply = row("SettingsApply", 40.0f);
    Add(scene, apply, PanelFill({0.18f, 0.34f, 0.24f, 1.0f}, 10.0f));
    Add(scene, apply, Text("Применить", 2.0f, {0.95f, 1.0f, 0.95f, 1.0f}));
    Interactable act;
    act.Action = "apply_settings";
    Add(scene, apply, act);

    return root.Id();
}

} // namespace

const std::vector<std::string>& DemoNames() {
    static const std::vector<std::string> names = {"menu", "hud", "settings"};
    return names;
}

int BuildDemo(Scene& scene, const std::string& name) {
    if (name == "menu") return BuildMenu(scene);
    if (name == "hud") return BuildHud(scene);
    if (name == "settings") return BuildSettings(scene);
    return -1;   // молча собрать «что-нибудь» значило бы скрыть опечатку в имени
}

} // namespace sage::ui
