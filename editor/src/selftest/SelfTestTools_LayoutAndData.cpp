#include "SelfTestCommon.h"

bool EditorLayer::SelfTestToolsLayoutAndData() {
    bool ok = true;
    std::error_code ec;
    std::string err;
    (void)ec; (void)err;
    // --- Инструменты вёрстки: выравнивание, распределение, якорь -----------
    //
    // Проверяется ПУТЬ ЦЕЛИКОМ, а не только математика (она проверена
    // юнит-тестами в tests/test_ui.cpp): выделение -> операция -> запись в
    // компоненты -> прямоугольник, который увидит игра. Между «формула верна» и
    // «кнопка в панели работает» лежит ровно то, что здесь и ломается: якорь,
    // масштаб холста и обратный перевод пикселей в опорные единицы.
    //
    // Кликать по самим кнопкам панели в CI нечем, но кнопка не делает ничего,
    // кроме вызова uiops::*, — и вызывается он здесь тем же способом.
    if (ok) {
        NewScene(ProjectTemplateKind::Empty);
        entt::registry& reg = m_scene->Registry();

        // Экран-родитель на весь кадр и три элемента разной ширины в нём.
        GameObject screen = m_scene->CreateObject("UIRoot");
        sage::ui::Element rootXf;
        rootXf.Anchor = UIAnchor::TopLeft;
        rootXf.Position = {0.0f, 0.0f};
        rootXf.Mode = sage::ui::Element::Stretch::Both;
        reg.emplace<sage::ui::Element>(screen.Entity(), rootXf);

        auto makeBox = [&](const char* name, glm::vec2 pos, glm::vec2 size) {
            GameObject o = m_scene->CreateObject(name);
            sage::ui::Element xf;
            xf.Anchor = UIAnchor::TopLeft;
            xf.Position = pos;
            xf.Size = size;
            reg.emplace<sage::ui::Element>(o.Entity(), xf);
            reg.emplace<sage::ui::Fill>(o.Entity());
            m_scene->SetParent(o.Entity(), screen.Entity());
            return o;
        };
        GameObject a = makeBox("BoxA", {10.0f, 10.0f}, {40.0f, 20.0f});
        GameObject b = makeBox("BoxB", {200.0f, 60.0f}, {80.0f, 20.0f});
        GameObject c = makeBox("BoxC", {500.0f, 120.0f}, {40.0f, 20.0f});

        // Кадр вёрстки задаётся явно: в headless панель вьюпорта его ещё не
        // сообщала, а от него зависят прямоугольники (см. UIToolSettings).
        m_tools.UI.FrameSize = {1280.0f, 720.0f};

        auto offsetOf = [&](GameObject o) {
            return reg.get<sage::ui::Element>(o.Entity()).Position;
        };

        // Выравнивание по левому краю первичного (последнего кликнутого).
        // Первичный — тот, кого выделили последним, то есть BoxC.
        m_selection.SetPrimary(a.Id());
        m_selection.Toggle(b.Id());
        m_selection.Toggle(c.Id());
        uiops::Align(*this, sage::ui::AlignEdge::Left);
        if (std::abs(offsetOf(a).x - 500.0f) > 0.01f ||
            std::abs(offsetOf(b).x - 500.0f) > 0.01f ||
            std::abs(offsetOf(c).x - 500.0f) > 0.01f) {
            LOG_ERROR("Editor") << "SELFTEST: выравнивание по левому краю не сработало ("
                                << offsetOf(a).x << ", " << offsetOf(b).x << ", "
                                << offsetOf(c).x << ")";
            ok = false;
        }
        // Выравнивание по горизонтали не имеет права двигать по вертикали.
        if (ok && std::abs(offsetOf(a).y - 10.0f) > 0.01f) {
            LOG_ERROR("Editor") << "SELFTEST: выравнивание сдвинуло элемент по чужой оси";
            ok = false;
        }

        // Распределение по вертикали: крайние стоят, средний встаёт посередине.
        if (ok) {
            uiops::Distribute(*this, /*horizontal=*/false, /*byCenters=*/false);
            const float ya = offsetOf(a).y, yb = offsetOf(b).y, yc = offsetOf(c).y;
            // Занято 60 из (140 - 10) + 20 = 130 по высоте: зазоры по 35.
            if (std::abs(ya - 10.0f) > 0.01f || std::abs(yc - 120.0f) > 0.01f ||
                std::abs(yb - 65.0f) > 0.01f) {
                LOG_ERROR("Editor") << "SELFTEST: распределение по вертикали дало " << ya << ", "
                                    << yb << ", " << yc;
                ok = false;
            }
        }

        // Смена якоря обязана ОСТАВИТЬ элемент на месте.
        if (ok) {
            m_selection.SetPrimary(b.Id());
            const std::vector<sage::ui::ElementRect> before2 =
                sage::ui::SolveSceneRects(*m_scene, 1280, 720, true);
            sage::ui::UIRect was{};
            for (const auto& e : before2)
                if (e.Entity == b.Entity()) was = e.Rect;
            uiops::SetAnchorKeepingPlace(*this, UIAnchor::BottomRight);
            const std::vector<sage::ui::ElementRect> after2 =
                sage::ui::SolveSceneRects(*m_scene, 1280, 720, true);
            sage::ui::UIRect now{};
            for (const auto& e : after2)
                if (e.Entity == b.Entity()) now = e.Rect;
            if (reg.get<sage::ui::Element>(b.Entity()).Anchor != UIAnchor::BottomRight ||
                std::abs(was.x - now.x) > 0.01f || std::abs(was.y - now.y) > 0.01f) {
                LOG_ERROR("Editor") << "SELFTEST: смена якоря сдвинула элемент ("
                                    << was.x << "," << was.y << " -> " << now.x << "," << now.y
                                    << ")";
                ok = false;
            }
        }

        // «Растянуть на родителя с полем» — размер и место считаются от
        // родителя, а не от экрана: элемент внутри панели обязан остаться внутри.
        if (ok) {
            m_selection.SetPrimary(c.Id());
            uiops::StretchToParent(*this, 16.0f);
            const sage::ui::Element& t = reg.get<sage::ui::Element>(c.Entity());
            if (std::abs(t.Size.x - (1280.0f - 32.0f)) > 0.01f ||
                std::abs(t.Size.y - (720.0f - 32.0f)) > 0.01f) {
                LOG_ERROR("Editor") << "SELFTEST: растяжение на родителя дало " << t.Size.x << "x"
                                    << t.Size.y;
                ok = false;
            }
        }
        // «Вернуть на экран»: элемент, уехавший за границу, придвигается внутрь.
        // Проверяется именно то, ради чего кнопка и появилась, — что элемент
        // становится ВИДИМЫМ, а не просто меняет числа.
        if (ok) {
            m_selection.SetPrimary(a.Id());
            sage::ui::Element& ta = reg.get<sage::ui::Element>(a.Entity());
            ta.Position = {5000.0f, -300.0f};   // далеко за краем
            uiops::BringIntoView(*this);
            const std::vector<sage::ui::ElementRect> back =
                sage::ui::SolveSceneRects(*m_scene, 1280, 720, true);
            sage::ui::UIRect r{};
            for (const auto& e : back)
                if (e.Entity == a.Entity()) r = e.Rect;
            if (r.x < 0.0f || r.y < 0.0f || r.x + r.w > 1280.0f || r.y + r.h > 720.0f) {
                LOG_ERROR("Editor") << "SELFTEST: «вернуть на экран» оставило элемент снаружи ("
                                    << r.x << "," << r.y << " " << r.w << "x" << r.h << ")";
                ok = false;
            }
        }
        m_selection.SetPrimary(-1);
    }

    // --- Публичные переменные, ссылки и события кнопки ----------------------
    //
    // Проверяется путь ЦЕЛИКОМ и именно в редакторе: скрипт объявил переменные
    // -> инспектор их подмешал -> человек выставил значение и ссылку -> всё это
    // пережило сохранение и загрузку -> кнопка со связью послала событие сама,
    // без единого скрипта-опросчика. Каждый из шагов ломается молча: связь,
    // теряющаяся при сохранении, работает до перезапуска редактора, а
    // объявление, не дошедшее до инспектора, выглядит как «переменных нет».
    if (ok) {
        const fs::path scriptPath = m_project.AssetsDir() / "selftest_vars.lua";
        {
            std::ofstream f(scriptPath);
            f << "Vars = {\n"
              << "    speed = 3.0,\n"
              << "    damage = { 10, min = 0, max = 100, label = \"Урон\" },\n"
              << "    target = { kind = \"entity\" },\n"
              << "}\n"
              << "function OnUpdate(entity, dt) end\n";
        }

        GameObject door = m_scene->CreateObject("SelfTestDoor");
        GameObject key = m_scene->CreateObject("SelfTestKey");
        m_scene->Registry().emplace_or_replace<ScriptComponent>(
            door.Entity(), ScriptComponent{m_project.AssetRef(scriptPath)});

        // Тот же вызов, что делает инспектор перед показом секции.
        MergeScriptVars(door);
        // Публичные переменные живут В КОМПОНЕНТЕ СКРИПТА: они объявлены им и
        // принадлежат паре «объект + скрипт» (см. ScriptComponent.h).
        ScriptComponent* doorScript =
            m_scene->Registry().try_get<ScriptComponent>(door.Entity());
        if (!doorScript || doorScript->Fields.Size() != 3) {
            LOG_ERROR("Editor") << "SELFTEST: объявление скрипта не дошло до переменных объекта";
            ok = false;
        } else {
            if (doorScript->Fields.Find("damage") == nullptr ||
                std::abs(doorScript->Fields.Find("damage")->Max - 100.0f) > 0.01f) {
                LOG_ERROR("Editor") << "SELFTEST: границы объявленной переменной потерялись";
                ok = false;
            }
            // Человек правит значения в инспекторе.
            doorScript->Fields.Set("speed", sage::vars::Value(9.5f));
            doorScript->Fields.Set("target", sage::vars::Value(sage::vars::EntityRef{key.Id()}));
        }

        // Кнопка со связью из инспектора: «On Click → SelfTestDoor.Open()».
        GameObject button = m_scene->CreateObject("SelfTestButton");
        {
            sage::ui::Element t;
            t.Anchor = UIAnchor::TopLeft;
            t.Position = {0.0f, 0.0f};
            t.Size = {200.0f, 100.0f};
            m_scene->Registry().emplace_or_replace<sage::ui::Element>(button.Entity(), t);
            m_scene->Registry().emplace_or_replace<sage::ui::Fill>(button.Entity());
            m_scene->Registry().emplace_or_replace<sage::ui::Interactable>(button.Entity());
            sage::signals::Link link;
            link.Signal = sage::signals::kClicked;
            link.Target = sage::vars::EntityRef{door.Id()};
            link.Method = "Open";
            m_scene->Registry().emplace_or_replace<sage::signals::SignalLinksComponent>(button.Entity())
                .Links.push_back(link);
        }

        // Сохранение и загрузка: связи и переменные — данные, и терять их при
        // записи нельзя.
        const fs::path scenePath = m_project.ScenesDir() / "selftest_vars.sage";
        if (ok && !SaveSceneToFile(scenePath)) {
            LOG_ERROR("Editor") << "SELFTEST: сцена с переменными не сохранилась";
            ok = false;
        }
        if (ok) {
            NewScene(ProjectTemplateKind::Empty);
            if (!LoadSceneFromFile(scenePath)) {
                LOG_ERROR("Editor") << "SELFTEST: сцена с переменными не загрузилась";
                ok = false;
            }
        }
        if (ok) {
            GameObject loadedDoor = m_scene->FindByName("SelfTestDoor");
            GameObject loadedKey = m_scene->FindByName("SelfTestKey");
            GameObject loadedBtn = m_scene->FindByName("SelfTestButton");
            const ScriptComponent* lv =
                loadedDoor.Valid()
                    ? m_scene->Registry().try_get<ScriptComponent>(loadedDoor.Entity())
                    : nullptr;
            if (!lv || std::abs(lv->Fields.Get("speed").AsFloat() - 9.5f) > 0.01f) {
                LOG_ERROR("Editor") << "SELFTEST: значение переменной не пережило сохранение";
                ok = false;
            } else if (lv->Fields.Get("target").AsEntity().Id != loadedKey.Id()) {
                LOG_ERROR("Editor") << "SELFTEST: ссылка на объект не пережила сохранение";
                ok = false;
            }

            // Связь пережила запись — со своей целью и методом.
            const sage::signals::SignalLinksComponent* links =
                loadedBtn.Valid()
                    ? m_scene->Registry().try_get<sage::signals::SignalLinksComponent>(loadedBtn.Entity())
                    : nullptr;
            if (!links || links->Links.size() != 1 || links->Links[0].Signal != "clicked" ||
                links->Links[0].Target.Id != loadedDoor.Id() || links->Links[0].Method != "Open") {
                LOG_ERROR("Editor") << "SELFTEST: связь «On Click» не пережила сохранение";
                ok = false;
            }

            // ЩЕЛЧОК: кнопка обязана послать СВОЙ сигнал clicked сама.
            int heard = 0;
            int sender = 0;
            const int conn = m_scene->Events.Connect(loadedBtn.Id(), sage::signals::kClicked,
                                                     [&](const sage::events::Event& e) {
                                                         ++heard;
                                                         sender = e.Sender;
                                                     });
            sage::ui::UIInputState down;
            down.Mouse = {50.0f, 50.0f};
            down.MouseDown = true;
            down.MousePressed = true;
            sage::ui::UpdateSceneUI(*m_scene, down, 1280, 720);
            sage::ui::UIInputState up;
            up.Mouse = down.Mouse;
            up.MouseReleased = true;
            sage::ui::UpdateSceneUI(*m_scene, up, 1280, 720);

            m_scene->Events.Disconnect(conn);
            if (heard != 1) {
                LOG_ERROR("Editor") << "SELFTEST: кнопка не послала сигнал clicked (получено "
                                    << heard << ")";
                ok = false;
            } else if (loadedBtn.Valid() && sender != loadedBtn.Id()) {
                LOG_ERROR("Editor") << "SELFTEST: в событии не тот отправитель";
                ok = false;
            }
        }
        fs::remove(scriptPath, ec);
    }

    // --- Заготовка со связями внутри неё ------------------------------------
    //
    // Префаб ставят в сцену много раз, а ссылка хранит НОМЕР объекта. Не
    // переписав номера у копии, получаем заготовку, работающую ровно один раз:
    // вторая дверь открывается кнопкой первой, а десятая не открывается вовсе.
    // Молчит это полностью — в инспекторе ссылка заполнена и ведёт на
    // существующий объект.
    if (ok) {
        GameObject door = m_scene->CreateObject("PrefabDoor");
        GameObject knob = m_scene->CreateObject("PrefabKnob");
        GameObject outsider = m_scene->CreateObject("PrefabOutsider");
        m_scene->SetParent(knob.Entity(), door.Entity());

        sage::vars::Table& vars =
            m_scene->Registry().emplace_or_replace<VarsComponent>(knob.Entity()).Values;
        vars.Set("opens", sage::vars::Value(sage::vars::EntityRef{door.Id()}));
        vars.Set("watch", sage::vars::Value(sage::vars::EntityRef{outsider.Id()}));
        sage::signals::Link b;
        b.Signal = sage::signals::kClicked;
        b.Target = sage::vars::EntityRef{door.Id()};
        b.Method = "Open";
        m_scene->Registry().emplace_or_replace<sage::ui::Element>(knob.Entity());
        m_scene->Registry().emplace_or_replace<sage::ui::Interactable>(knob.Entity());
        m_scene->Registry()
            .emplace_or_replace<sage::signals::SignalLinksComponent>(knob.Entity())
            .Links.push_back(b);

        const fs::path pf = m_project.AssetsDir() / "selftest_door.sageprefab";
        std::string perr;
        m_selection.SetPrimary(door.Id());
        if (!SaveSelectedAsPrefab(pf, perr)) {
            LOG_ERROR("Editor") << "SELFTEST: заготовка не сохранилась";
            ok = false;
        }
        if (ok) {
            sage::scene::ClearPrefabCache();
            const int copyId = InstantiatePrefab(pf);
            GameObject copy = m_scene->Get(copyId);
            if (!copy.Valid()) {
                LOG_ERROR("Editor") << "SELFTEST: заготовка не поставилась в сцену";
                ok = false;
            } else {
                const HierarchyComponent* h =
                    m_scene->Registry().try_get<HierarchyComponent>(copy.Entity());
                if (!h || h->Children.size() != 1) {
                    LOG_ERROR("Editor") << "SELFTEST: у копии заготовки нет дочерней кнопки";
                    ok = false;
                } else {
                    const entt::entity copyKnob = h->Children[0];
                    const VarsComponent* cv =
                        m_scene->Registry().try_get<VarsComponent>(copyKnob);
                    const sage::signals::SignalLinksComponent* ca =
                        m_scene->Registry().try_get<sage::signals::SignalLinksComponent>(copyKnob);
                    if (!cv || cv->Values.Get("opens").AsEntity().Id != copy.Id()) {
                        LOG_ERROR("Editor") << "SELFTEST: ссылка внутри копии ведёт не в копию";
                        ok = false;
                    }
                    if (!ca || ca->Links.empty() || ca->Links[0].Target.Id != copy.Id()) {
                        LOG_ERROR("Editor") << "SELFTEST: адресат связи в копии не переписан";
                        ok = false;
                    }
                    // Ссылка наружу заготовки обрывается при сохранении: номер
                    // из исходной сцены в другой сцене попал бы в посторонний
                    // объект, и такая связь неотличима от настоящей.
                    if (cv && cv->Values.Get("watch").AsEntity().Valid()) {
                        LOG_ERROR("Editor") << "SELFTEST: внешняя ссылка уехала в заготовку";
                        ok = false;
                    }
                }
            }
        }
        // Дублирование идёт тем же путём — проверяем и его: Ctrl+D у объекта со
        // связями обязан давать вторую пару, а не вторую кнопку к первой двери.
        if (ok) {
            m_selection.SetPrimary(door.Id());
            DuplicateSelected();
            const std::vector<int>& sel = m_selection.All();
            GameObject dup = sel.empty() ? GameObject{} : m_scene->Get(sel.front());
            const HierarchyComponent* dh =
                dup.Valid() ? m_scene->Registry().try_get<HierarchyComponent>(dup.Entity())
                            : nullptr;
            if (!dh || dh->Children.size() != 1) {
                LOG_ERROR("Editor") << "SELFTEST: дубликат двери потерял кнопку";
                ok = false;
            } else if (const VarsComponent* dv =
                           m_scene->Registry().try_get<VarsComponent>(dh->Children[0])) {
                if (dv->Values.Get("opens").AsEntity().Id != dup.Id()) {
                    LOG_ERROR("Editor") << "SELFTEST: дубликат ссылается на оригинал";
                    ok = false;
                }
                // А ссылка НАРУЖУ у дубликата остаётся: дублируя лампу,
                // светящую на игрока, человек ждёт вторую такую же лампу.
                if (dv->Values.Get("watch").AsEntity().Id != outsider.Id()) {
                    LOG_ERROR("Editor") << "SELFTEST: дубликат потерял внешнюю ссылку";
                    ok = false;
                }
            }
            m_selection.SetPrimary(-1);
        }
        fs::remove(pf, ec);
        sage::scene::ClearPrefabCache();
    }

// --- Заметка «что делать дальше» после создания проекта -----------------
    //
    // Есть шаблоны, чей проект сразу после создания выглядит ПУСТЫМ: мир в них
    // строят скрипты при запуске, а в сцене на диске — пара объектов. Со
    // стороны это неотличимо от «шаблон не работает», и заметка — единственное,
    // что отличает одно от другого. Она обязана дойти до редактора, а не
    // остаться в файле описания.
    //
    // Шаблон для проверки делаем свой: готовых проектов внутри сборки больше
    // нет, и полагаться на то, что у запускающего установлена какая-то
    // конкретная витрина, значит проверять «повезло или нет».
    if (ok) {
        namespace tstore = sage::editor::templates;
        const fs::path notePack = fs::absolute("selftest_note.sagetemplate", ec);
        fs::remove(notePack, ec);
        fs::remove_all("selftest_tpl_notesrc", ec);
        tstore::Uninstall("selftestnote", err);

        std::string noteErr;
        tstore::Manifest info;
        info.Id = "selftestnote";
        info.Name = "Шаблон с заметкой";
        info.Note = "Нажмите «Играть», чтобы увидеть мир этого шаблона.";
        const bool made = CreateProject(".", "selftest_tpl_notesrc", "demo", noteErr) &&
                          SaveSceneToFile(m_project.ScenesDir() / "main.sage") &&
                          tstore::Pack(m_project.Dir(), info, notePack, noteErr) &&
                          tstore::InstallFromFile(notePack, noteErr);
        if (!made) {
            LOG_ERROR("Editor") << "SELFTEST: шаблон с заметкой не подготовился: " << noteErr;
            ok = false;
        } else {
            RefreshProjectTemplates();
            fs::remove_all("selftest_tpl_note", ec);
            if (!CreateProject(".", "selftest_tpl_note", "selftestnote", noteErr)) {
                LOG_ERROR("Editor") << "SELFTEST: проект из шаблона с заметкой не создался: "
                                    << noteErr;
                ok = false;
            } else {
                if (TemplateNote().empty()) {
                    LOG_ERROR("Editor") << "SELFTEST: заметка шаблона не дошла до редактора";
                    ok = false;
                }
                // Play отвечает на вопрос «почему пусто» делом — заметке
                // дальше висеть незачем.
                StartPlay();
                if (!TemplateNote().empty()) {
                    LOG_ERROR("Editor") << "SELFTEST: заметка пережила запуск игры";
                    ok = false;
                }
                StopPlay();
            }
            // Чужая сцена держит текстуры занятыми, а проверка бюджета
            // видеопамяти меряет ОБЩИЙ резидентный размер — убираем за собой
            // сразу, иначе она провалится не из-за вытеснения, а из-за нас.
            NewScene(ProjectTemplateKind::Empty);
            ResourceManager::Instance().GarbageCollectUnused();
        }
        fs::remove_all("selftest_tpl_note", ec);
        fs::remove_all("selftest_tpl_notesrc", ec);
        fs::remove(notePack, ec);
        tstore::Uninstall("selftestnote", err);
        RefreshProjectTemplates();
    }
    // А у шаблона, который показывает себя сам, заметки быть не должно:
    // строка «что делать дальше» там, где и так всё видно, — шум.
    if (ok) {
        fs::remove_all("selftest_tpl_plain", ec);
        std::string plainErr;
        if (CreateProject(".", "selftest_tpl_plain", "demo", plainErr) &&
            !TemplateNote().empty()) {
            LOG_ERROR("Editor") << "SELFTEST: у демо-шаблона появилась заметка";
            ok = false;
        }
        fs::remove_all("selftest_tpl_plain", ec);
    }

    // --- ОФОРМЛЕНИЕ: тема — данные, и её видно ------------------------------
    //
    // Проверяем не «красиво ли», а то, ради чего система вообще заведена:
    // тем НЕСКОЛЬКО, переключение реально меняет вид, тема из файла работает
    // наравне со встроенной, а несуществующая тема не оставляет редактор без
    // оформления. Раньше стиль был одной функцией на сто строк, и ни один из
    // этих вопросов не имел смысла — ответ был «одна, никак, нет, нечему».
    {
        const std::string startId = EditorTheme::CurrentId();

        if (EditorTheme::Themes().size() < 4) {
            LOG_ERROR("Editor") << "SELFTEST: тем оформления меньше четырёх ("
                                << EditorTheme::Themes().size() << ")";
            ok = false;
        }
        // Тема из themes/*.json — рядом со встроенными, без пересборки.
        // Именно это отличает «настраиваемый редактор» от «редактора, у
        // которого автор предусмотрел четыре варианта».
        const bool fromFile = std::any_of(
            EditorTheme::Themes().begin(), EditorTheme::Themes().end(),
            [](const EditorTheme::Theme& t) { return t.Id == "amber"; });
        if (!fromFile) {
            LOG_ERROR("Editor") << "SELFTEST: тема из файла themes/example-amber.json не подхвачена";
            ok = false;
        }

        // Переключение обязано МЕНЯТЬ КАРТИНКУ. Сравниваем сам стиль ImGui, а
        // не поле «текущая тема»: последнее сходится и тогда, когда Apply()
        // забыли позвать, — а человек в этом случае видит прежний редактор.
        if (EditorTheme::SetTheme("modern-dark")) {
            const ImVec4 darkBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            if (!EditorTheme::SetTheme("modern-light")) {
                LOG_ERROR("Editor") << "SELFTEST: светлая тема не применилась";
                ok = false;
            } else {
                const ImVec4 lightBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
                if (lightBg.x <= darkBg.x) {
                    LOG_ERROR("Editor") << "SELFTEST: светлая тема не светлее тёмной ("
                                        << lightBg.x << " vs " << darkBg.x << ")";
                    ok = false;
                }
                // И цвета РОЛЕЙ едут вместе с темой — иначе панели, которые
                // спрашивают Role::Danger, остались бы в цветах прошлой.
                if (EditorTheme::Color(EditorTheme::Role::Danger).x <= 0.0f) {
                    LOG_ERROR("Editor") << "SELFTEST: роль Danger пуста после смены темы";
                    ok = false;
                }
            }
        } else {
            LOG_ERROR("Editor") << "SELFTEST: тёмная тема не применилась";
            ok = false;
        }

        // Несуществующая тема — отказ, а НЕ «оформления больше нет».
        const std::string beforeBad = EditorTheme::CurrentId();
        if (EditorTheme::SetTheme("нет-такой-темы") || EditorTheme::CurrentId() != beforeBad) {
            LOG_ERROR("Editor") << "SELFTEST: неизвестная тема сменила оформление";
            ok = false;
        }

        // Масштаб интерфейса живёт отдельно от темы и переживает её смену:
        // это свойство экрана, а не оформления.
        EditorTheme::SetUiScale(1.25f);
        EditorTheme::SetTheme("modern-dark");
        if (std::abs(EditorTheme::UiScale() - 1.25f) > 0.001f) {
            LOG_ERROR("Editor") << "SELFTEST: масштаб интерфейса потерялся при смене темы";
            ok = false;
        }
        // И он ДОХОДИТ до ImGui, а не просто хранится.
        if (std::abs(ImGui::GetStyle().FontScaleMain - 1.25f) > 0.001f) {
            LOG_ERROR("Editor") << "SELFTEST: масштаб не дошёл до стиля ImGui";
            ok = false;
        }
        EditorTheme::SetUiScale(1.0f);

        // Выгрузка и чтение обратно: то, что редактор записал, он обязан
        // прочитать без потерь — иначе «возьми текущую за основу» не работает.
        const std::string exported = "selftest_theme.json";
        fs::remove(exported, ec);
        if (!EditorTheme::ExportTheme(EditorTheme::Current(), exported)) {
            LOG_ERROR("Editor") << "SELFTEST: тему не выгрузить в файл";
            ok = false;
        } else {
            std::ifstream in(exported);
            nlohmann::json root;
            try {
                in >> root;
            } catch (const std::exception& e) {
                LOG_ERROR("Editor") << "SELFTEST: выгруженная тема не читается: " << e.what();
                ok = false;
            }
            if (ok) {
                const auto colors = root.find("colors");
                if (colors == root.end() || !colors->is_object() ||
                    colors->size() != (size_t)EditorTheme::Role::_Count) {
                    LOG_ERROR("Editor") << "SELFTEST: в выгруженной теме не все роли";
                    ok = false;
                } else if (!root.contains("metrics") || !root["metrics"].contains("framePadding")) {
                    LOG_ERROR("Editor") << "SELFTEST: в выгруженной теме нет метрик";
                    ok = false;
                }
            }
            fs::remove(exported, ec);
        }

        EditorTheme::SetTheme(startId); // за собой убираем: прогон идёт дальше
    }

    // --- Раскладка управления проекта (панель «Управление») -----------------
    //
    // Проверяется не «панель открывается», а то, ради чего она есть: раскладка
    // переживает запись на диск и чтение обратно, а Play-режим играется ИМЕННО
    // ею. Без последнего панель была бы окном, которое красиво выглядит и ни на
    // что не влияет, — и заметили бы это только после сборки игры.
    {
        namespace in = sage::input;
        // Площадку готовим сами: предыдущие проверки этого блока успевают
        // прибрать за собой папку проекта, а раскладке нужно, куда писать.
        std::error_code inputSetupEc;
        fs::create_directories(m_project.Dir(), inputSetupEc);

        in::InputSystem& mapping = ProjectInput();
        mapping.ClearActions();

        in::Action& jump = mapping.Register("Jump");
        jump.Bind("SPACE");
        jump.Bind("PAD_A");
        in::Action& move = mapping.Register("Move", in::ActionType::Vector);
        move.BindVector("W", "S", "A", "D");
        mapping.CreateContext("Inventory", 50).Add("Equip").Bind("E");

        if (!SaveProjectInput()) {
            LOG_ERROR("Editor") << "SELFTEST: раскладка управления не сохранилась в проект: "
                                << ProjectInputFile().string();
            ok = false;
        } else {
            // Перечитываем с нуля: сохранённый файл обязан полностью описывать
            // раскладку, а не опираться на то, что уже в памяти.
            if (!ReloadProjectInput()) {
                LOG_ERROR("Editor") << "SELFTEST: раскладка управления не перечитывается";
                ok = false;
            } else if (!mapping.Has("Jump") || !mapping.Has("Move") || !mapping.Has("Equip")) {
                LOG_ERROR("Editor") << "SELFTEST: после перечитывания раскладки пропали действия";
                ok = false;
            } else if (mapping.Find("Jump")->Bindings().size() != 2) {
                LOG_ERROR("Editor") << "SELFTEST: у действия Jump потерялась привязка";
                ok = false;
            } else if (mapping.Find("Move")->Type() != in::ActionType::Vector) {
                LOG_ERROR("Editor") << "SELFTEST: вид действия Move не пережил сохранение";
                ok = false;
            } else if (!mapping.MoveAction(in::kDefaultContext, "Inventory", "Jump") ||
                       !SaveProjectInput() || !ReloadProjectInput() ||
                       !mapping.FindContext("Inventory") ||
                       !mapping.FindContext("Inventory")->Has("Jump") ||
                       mapping.FindContext("Inventory")->Find("Jump")->Bindings().size() != 2 ||
                       mapping.FindContext(in::kDefaultContext)->Has("Jump")) {
                // ПЕРЕНОС ДЕЙСТВИЯ В ДРУГОЙ КОНТЕКСТ — и он обязан пережить
                // файл. Половина раскладки переезжает в инвентарь уже после
                // того, как её завели, а до этого перенести действие можно было
                // только пересоздав его руками: имя, вид и все клавиши заново.
                LOG_ERROR("Editor") << "SELFTEST: перенос действия между контекстами не пережил "
                                       "сохранение";
                ok = false;
            } else if (!mapping.FindContext("Inventory") ||
                       mapping.FindContext("Inventory")->Priority() != 50) {
                LOG_ERROR("Editor") << "SELFTEST: контекст Inventory не пережил сохранение";
                ok = false;
            }
        }

        // И главное: раскладка ДОХОДИТ до работающего ввода Play. Проверяем
        // тем же вызовом, которым это делает StartPlay.
        if (ok) {
            m_play.Input().ClearActions();
            ApplyProjectInputMapping();
            if (!m_play.Input().Has("Jump") || !m_play.Input().Has("Equip")) {
                LOG_ERROR("Editor") << "SELFTEST: раскладка проекта не доехала до ввода Play";
                ok = false;
            } else {
                // Ввод отвечает на неё по-настоящему: жмём пробел событием и
                // ждём, что сработает действие, а не клавиша.
                m_play.Input().Push(in::InputEvent::KeyDown(in::Key::Space));
                m_play.Input().Update(1.0f / 60.0f);
                if (!m_play.Input().WasPressed("Jump")) {
                    LOG_ERROR("Editor") << "SELFTEST: действие Jump не сработало от назначенной клавиши";
                    ok = false;
                }
                m_play.Input().ReleaseAll();
            }
            m_play.Input().ClearActions();
        }

        // И то же самое ВИДНО СКРИПТУ через его Lua-API, а не только самому
        // InputSystem на C++ — это разные вещи. Раскладка вполне может
        // доехать до InputSystem и остаться невидимой Lua, если BindInput
        // забыли позвать или порядок регистрации в StartPlay разъехался;
        // проверка выше такую поломку не поймала бы вовсе. Действие "Equip"
        // взято НАРОЧНО — оно живёт в СВОЁМ контексте ("Inventory"), а не в
        // контексте по умолчанию, поэтому заодно проверяется, что связь
        // работает не только для действий по умолчанию.
        if (ok) {
            std::error_code scriptEc;
            fs::create_directories(m_project.Dir() / "assets" / "scripts", scriptEc);
            {
                std::ofstream f(m_project.Dir() / "assets" / "scripts" / "selftest_input_ctx.lua");
                f << "function OnUpdate(entity, dt)\n"
                  << "    if IsActionDown('Equip') then entity.Transform.Position.x = 5.0 end\n"
                  << "end\n";
            }
            GameObject probe = m_scene->CreateObject("SelftestInputCtxProbe");
            m_scene->Registry().emplace<ScriptComponent>(
                probe.Entity(), ScriptComponent{"assets/scripts/selftest_input_ctx.lua"});

            StartPlay();
            m_play.Input().Push(in::InputEvent::KeyDown(in::Key::E));
            m_play.Input().Update(1.0f / 60.0f);
            for (int i = 0; i < 3; ++i) m_systems.Run(*m_scene, 1.0f / 60.0f);
            GameObject after = m_scene->FindByName("SelftestInputCtxProbe");
            const bool sawAction = after.Valid() && after.GetTransform().Position.x > 4.0f;
            StopPlay(); // сцена откатится к снапшоту — читать результат нужно ДО этого

            if (!sawAction) {
                LOG_ERROR("Editor") << "SELFTEST: контекст и действие, заведённые в раскладке "
                                       "проекта ('Inventory' / 'Equip'), не видны скрипту через "
                                       "IsActionDown в Play";
                ok = false;
            }
            m_play.Input().ClearActions();
        }

        // За собой убираем: раскладка селф-теста не должна остаться в проекте.
        mapping.ClearActions();
        std::error_code inputEc;
        fs::remove(ProjectInputFile(), inputEc);
        m_projectInputDirty = false;
    }

    return ok;
}
