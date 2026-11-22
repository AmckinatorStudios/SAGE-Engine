#include "SelfTestCommon.h"
#include "../sprite/SpriteEditorBinding.h"

// ---------------------------------------------------------------------------
// МНОГООКОННОСТЬ В ЖИВОМ КАДРЕ (SAGE_EDITOR_SELFTEST=1).
//
// Окно системы заводит ПЛАТФОРМА: ImGui решает завести панели свой вьюпорт,
// бэкенд создаёт под него окно GLFW, и только тогда окно существует. Ломается
// этот последний шаг молча, поэтому проверяется конец пути.
//
// 1) Профайлер не в раскладке: открытый, он плавает ПОВЕРХ главного окна — и
//    обязан стать своим окном системы, а не прямоугольником внутри редактора
//    (NoAutoMerge). Заголовок у него рисует ImGui (за него окно и возвращают).
// 2) «Вернуть все окна в редактор» заводит его в док главного окна.
// 3) Панель из раскладки (Console) проходит круг «вытащить — окно системы —
//    вернуть» и встаёт на своё место (см. DetachRoundTrip).
// ---------------------------------------------------------------------------
int EditorLayer::DetachRoundTrip(const char* id, std::string& why) {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    const ImGuiID mainId = ImGui::GetMainViewport()->ID;
    ImGuiWindow* w = ImGui::FindWindowByID(ImHashStr(id));
    if (m_rtWait > 0) { --m_rtWait; return 0; }
    switch (m_rtPhase) {
        case 0:
            if (!w || !w->WasActive || w->DockId == 0) { why = std::string(id) + " не в доке до проверки"; return -1; }
            m_rtOrigDock = w->DockId;
            // То же действие, что делает перетаскивание вкладки из узла.
            ImGui::DockContextQueueUndockWindow(ctx, w);
            m_rtPhase = 1;
            m_rtWait = 6;   // окно платформы появляется не в том же кадре
            return 0;
        case 1: {
            if (!w || w->Viewport == nullptr || w->Viewport->ID == mainId || w->DockId != 0) {
                why = std::string(id) + ": вытащенная панель не стала окном системы"; return -1;
            }
            if (w->Viewport->PlatformHandle == nullptr) { why = std::string(id) + ": у вьюпорта нет окна платформы"; return -1; }
            if ((w->Viewport->Flags & ImGuiViewportFlags_NoDecoration) == 0 || (w->Flags & ImGuiWindowFlags_NoTitleBar)) {
                why = std::string(id) + ": у отдельного окна нет заголовка ImGui, за который его возвращают"; return -1;
            }
            if (!m_panelRegistry.AnyFloating(m_workspace)) { why = "реестр не видит отдельного окна"; return -1; }
            m_panelRegistry.ReturnFloating(m_workspace);
            m_rtPhase = 2;
            m_rtWait = 6;
            return 0;
        }
        case 2:
            if (!w || w->Viewport == nullptr || w->Viewport->ID != mainId || w->DockId == 0) {
                why = std::string(id) + ": окно не вернулось в док главного окна"; return -1;
            }
            // Остальным панелям возврат ничего не сделал.
            if (m_panelRegistry.AnyFloating(m_workspace)) { why = "после возврата осталось отдельное окно"; return -1; }
            // Назад — ровно в тот узел, откуда вытащили: проверки дальше (и
            // щелчки по координатам) рассчитаны на привычную раскладку.
            ImGui::DockBuilderDockWindow(id, m_rtOrigDock);
            m_rtPhase = 3;
            m_rtWait = 3;
            return 0;
        default:
            if (!w || w->DockId != m_rtOrigDock) { why = std::string(id) + ": не встала на прежнее место"; return -1; }
            m_rtPhase = 0;
            m_rtWait = 0;
            return 1;
    }
}

void EditorLayer::CheckMultiWindowFrame() {
    if (m_multiWindowChecked) return;
    if (!std::getenv("SAGE_EDITOR_SELFTEST")) return;
    // Не в первых кадрах: раскладка и окна платформы устаканиваются.
    if (m_frameCounter < 3) return;
    if (m_multiWindowWait > 0) { --m_multiWindowWait; return; }

    const ImGuiID mainId = ImGui::GetMainViewport()->ID;
    auto fail = [this](const std::string& why) {
        LOG_ERROR("Editor") << "MULTIWINDOW: FAIL — " << why;
        m_panels[EditorPanel::Profiler] = false;
        m_multiWindowChecked = true;
    };
    switch (m_multiWindowStep) {
        case 0:
            m_panels[EditorPanel::Profiler] = true;
            m_multiWindowStep = 1;
            m_multiWindowWait = 7;
            return;
        case 1: {
            // Профайлер мог остаться в доке с прошлого запуска (его место помнит
            // imgui.ini) — проверять надо плавающий, поэтому вытаскиваем его так же,
            // как это делает перетаскивание вкладки.
            ImGuiWindow* ui0 = ImGui::FindWindowByName("Profiler");
            if (ui0 != nullptr && ui0->DockId != 0) {
                ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), ui0);
                m_multiWindowWait = 7;
                return;
            }
            const ImGuiWindow* ui = ImGui::FindWindowByName("Profiler");
            const ImGuiWindow* docked = ImGui::FindWindowByName("Hierarchy");
            if (ui == nullptr || ui->Viewport == nullptr) return fail("окна панели нет в кадре");
            if (ui->Viewport->ID == mainId)
                return fail("плавающая панель слилась с главным окном (нет NoAutoMerge)");
            if (ui->Viewport->PlatformHandle == nullptr) return fail("у отдельного окна нет окна платформы");
            if ((ui->Viewport->Flags & ImGuiViewportFlags_NoDecoration) == 0 || (ui->Flags & ImGuiWindowFlags_NoTitleBar))
                return fail("заголовок отдельного окна рисует не ImGui: за него не вернуть перетаскиванием");
            if (docked && docked->Viewport && docked->Viewport->ID != mainId)
                return fail("панель дока уехала из главного окна вслед за плавающей");
            // Пока окно живо — ввод и захват в нём.
            CheckDetachedWindowInput(ui->Viewport->PlatformHandle);
            m_panelRegistry.ReturnFloating(m_workspace);
            m_multiWindowStep = 2;
            m_multiWindowWait = 7;
            return;
        }
        case 2: {
            const ImGuiWindow* ui = ImGui::FindWindowByName("Profiler");
            if (ui == nullptr || ui->Viewport == nullptr || ui->Viewport->ID != mainId || ui->DockId == 0)
                return fail("«Вернуть все окна» не завело панель в док главного окна");
            // Убираем за собой: закрыть и вернуть вьюпорт вперёд — иначе
            // вкладка профайлера осталась бы выбранной в центре.
            m_panels[EditorPanel::Profiler] = false;
            if (ImGuiWindow* vp = ImGui::FindWindowByName("Viewport")) ImGui::FocusWindow(vp);
            m_multiWindowStep = 3;
            m_multiWindowWait = 3;
            return;
        }
        default: {
            std::string why;
            const int r = DetachRoundTrip("Console", why);
            if (r == 0) return;
            if (r < 0) return fail(why);
            m_multiWindowChecked = true;
            LOG_INFO("Editor") << "MULTIWINDOW: OK — вытащенная панель живёт своим окном системы "
                               << "(заголовок ImGui, без слияния с главным), возвращается "
                               << "перетаскиванием и «Вернуть все окна»; панели дока не затронуты";
            return;
        }
    }
}


// ---------------------------------------------------------------------------
// ОБЩИЕ ПАНЕЛИ ПЕРЕЖИВАЮТ ПЕРЕХОД МЕЖДУ ПРОСТРАНСТВАМИ.
//
// Ассеты и консоль стоят и в сцене, и в вёрстке, но окно у такой панели ОДНО, а
// док-пространства разные. Раскладка вёрстки, построившись, забирала ассеты
// себе — и вернувшись в сцену, человек находил панель висящей отдельным окном
// посреди экрана. Ни состояние, ни лог об этом не говорили ничего: панель
// открыта, галочка стоит, просто стоит она не там.
//
// Проверяется живым переходом туда и обратно: раскладка дока устаканивается за
// несколько кадров, и вызовом функций этого не воспроизвести.
void EditorLayer::CheckWorkspaceDockFrame() {
    if (m_workspaceDockChecked) return;
    if (!std::getenv("SAGE_EDITOR_SELFTEST")) return;
    if (!m_multiWindowChecked) return;   // та проверка сама двигает панели
    if (m_wsDockWait > 0) { --m_wsDockWait; return; }

    auto dockOf = [](const char* id) -> unsigned int {
        const ImGuiWindow* w = ImGui::FindWindowByID(ImHashStr(id));
        return w ? (unsigned int)w->DockId : 0u;
    };
    auto done = [this](bool ok, const char* why) {
        m_workspaceDockChecked = true;
        if (ok) LOG_INFO("Editor") << "WORKSPACE_DOCK: OK — общие панели остаются в доке "
                                   << "после переходов «сцена — интерфейс — анимация» и обратно";
        else LOG_ERROR("Editor") << "WORKSPACE_DOCK: FAIL — " << why;
    };

    switch (m_wsDockStep) {
        case 0:
            m_wsDockHome = dockOf("Assets");
            if (m_wsDockHome == 0) { done(false, "панель ассетов и до перехода не в доке"); return; }
            SetWorkspace(EditorWorkspace::Interface);
            m_wsDockStep = 1;
            m_wsDockWait = 8;   // раскладка вёрстки строится и устаканивается
            return;
        case 1:
            // В ВЁРСТКЕ — СВОЁ ОКНО ПАНЕЛИ («AssetsUI»), и оно обязано быть в
            // доке вёрстки. Пока окно было одно на оба пространства, эта
            // раскладка забирала его у сцены, и вернуть его туда было уже
            // некуда (см. PanelWindowId.h).
            if (dockOf("AssetsUI") == 0) {
                done(false, "в вёрстке панель ассетов оказалась вне дока");
                return;
            }
            // И ОКНО СЦЕНЫ ПРИ ЭТОМ НЕ ТРОНУТО. Это и есть та проверка, ради
            // которой всё: раньше раскладка вёрстки уносила его с собой.
            if (dockOf("Assets") != m_wsDockHome) {
                done(false, "раскладка вёрстки забрала окно ассетов у сцены");
                return;
            }
            // Вытащить панель из дока вёрстки и вернуть (общая система окон).
            m_wsDockStep = 10;
            return;
        case 10: {
            std::string why;
            const int r = DetachRoundTrip("ConsoleUI", why);
            if (r == 0) return;
            if (r < 0) { done(false, why.c_str()); return; }
            SetWorkspace(EditorWorkspace::Scene);
            m_wsDockStep = 2;
            m_wsDockWait = 8;
            return;
        }
        case 2: {
            const unsigned int back = dockOf("Assets");
            if (back == 0) {
                done(false, "вернувшись в сцену, панель ассетов отцепилась от дока");
            } else if (back != m_wsDockHome) {
                done(false, "вернувшись в сцену, панель ассетов встала в чужой узел дока");
            } else if (dockOf("AssetsUI") == 0) {
                done(false, "окно ассетов вёрстки потеряло свой док при возврате в сцену");
            } else {
                // Третье пространство — анимация.
                SetWorkspace(EditorWorkspace::Animation);
                m_wsDockStep = 3;
                m_wsDockWait = 8;
            }
            return;
        }
        case 3: {
            // НИЗ — ОДИН УЗЕЛ: таймлайн, граф, Assets и Console вкладками;
            // вьюпорт, иерархия и инспектор анимации — свои панели, в доке.
            const unsigned int bottom = dockOf("AssetsAnim");
            if (bottom == 0) { done(false, "в анимации панель ассетов вне дока"); return; }
            for (const char* id : {"ConsoleAnim", "AnimationAnim", "AnimGraph"})
                if (dockOf(id) != bottom) { done(false, (std::string(id) + " не во вкладках нижнего узла анимации").c_str()); return; }
            for (const char* id : {"AnimViewport", "AnimHierarchy", "AnimInspector"})
                if (dockOf(id) == 0 || dockOf(id) == bottom) { done(false, (std::string(id) + " не на своём месте в анимации").c_str()); return; }
            if (dockOf("Assets") != m_wsDockHome) { done(false, "раскладка анимации забрала окно ассетов у сцены"); return; }
            m_wsDockStep = 11;
            return;
        }
        case 11: {
            std::string why;
            const int r = DetachRoundTrip("ConsoleAnim", why);
            if (r == 0) return;
            if (r < 0) { done(false, why.c_str()); return; }
            SetWorkspace(EditorWorkspace::Scene);
            m_wsDockStep = 4;
            m_wsDockWait = 8;
            return;
        }
        default: {
            if (dockOf("Assets") != m_wsDockHome) done(false, "после анимации ассеты сцены не на своём месте");
            else if (dockOf("AssetsAnim") == 0) done(false, "окно ассетов анимации потеряло свой док при возврате");
            else done(true, nullptr);
            return;
        }
    }
}


// ---------------------------------------------------------------------------
// ТЕСТЫ СКРИПТИНГА НА LUA — В НАСТОЯЩЕМ PLAY.
//
// Набор движка (editor/assets/tests/lua, рядом с редактором — assets/tests/lua)
// копируется в проект и запускается ТОЙ ЖЕ командой, что пункт меню «Run Lua
// Tests»: Play, объект на файл, тесты корутинами по кадрам, щелчки по
// интерфейсу и клавиши — настоящим путём. Проверяется весь API скриптинга
// живым кадром редактора, а не вызовами функций в пустой сцене.
void EditorLayer::CheckTweenFrame() {
    if (m_tweenChecked || !std::getenv("SAGE_EDITOR_SELFTEST") || !m_envChecked) return;
    entt::registry& reg = m_scene->Registry();
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "TWEEN: FAIL — " << why;
        m_tween.Reset(*this);
        m_panels[EditorPanel::Tween] = false;
        for (const char* n : {"TweenProbe", "TweenQuick"})
            if (GameObject o = m_scene->FindByName(n); o.Valid()) m_scene->RemoveObject(o.Id());
        m_tweenChecked = true;
    };
    switch (m_tweenSelfTestStep) {
        case 0: {
            // Объект с твином «Slide»: сдвиг, затем масштаб — последовательность.
            GameObject o = m_scene->CreateObject("TweenProbe");
            reg.get<Transform>(o.Entity()).Position = glm::vec3(0.0f);
            sage::anim::TweenClip clip;
            clip.Name = "Slide";
            sage::anim::TweenTrack move;
            move.Property = "object.position";
            move.To = glm::vec4(3.0f, 0.0f, 0.0f, 0.0f);
            move.Duration = 0.3f;
            move.Curve = sage::anim::Ease::Make(sage::anim::EaseShape::Back, sage::anim::EaseMode::Out);
            clip.AppendAfter(move);
            sage::anim::TweenTrack grow;
            grow.Property = "object.scale";
            grow.To = glm::vec4(2.0f, 2.0f, 2.0f, 0.0f);
            grow.Duration = 0.2f;
            grow.Curve.Shape = sage::anim::EaseShape::Curve;
            grow.Curve.Bezier = glm::vec4(0.2f, 0.0f, 0.3f, 1.4f);
            clip.AppendAfter(grow);
            reg.emplace<sage::anim::TweenComponent>(o.Entity()).Tweens.push_back(clip);
            m_selection.SetPrimary(o.Id());
            OpenTween(o.Id(), 0, TweenOpen::Edit);
            m_tweenSelfTestErrors = m_console.ErrorCount();
            m_tweenSelfTestStep = 1;
            return;
        }
        case 1: case 2: case 3: case 4:   // окно и инспектор рисуются несколько кадров
            ++m_tweenSelfTestStep;
            return;
        case 5:
            if (m_tween.OwnerId() != m_scene->FindByName("TweenProbe").Id()) return fail("окно Tween не открылось на объекте");
            m_tween.Play(*this);
            if (!m_tween.Previewing()) return fail("Play в окне Tween не запустил твин");
            m_tweenSelfTestAt = glfwGetTime();
            m_tweenSelfTestStep = 6;
            return;
        case 6: {
            if (glfwGetTime() - m_tweenSelfTestAt < 1.0) return;   // твин 0.5 с — с запасом на медленный кадр
            GameObject o = m_scene->FindByName("TweenProbe");
            const Transform& t = reg.get<Transform>(o.Entity());
            if (std::fabs(t.Position.x - 3.0f) > 1e-3f || std::fabs(t.Scale.x - 2.0f) > 1e-3f)
                return fail("просмотр не довёл объект: x=" + std::to_string(t.Position.x) +
                            " scale=" + std::to_string(t.Scale.x));
            m_tween.Reset(*this);
            if (std::fabs(t.Position.x) > 1e-5f || std::fabs(t.Scale.x - 1.0f) > 1e-5f)
                return fail("Reset не вернул объект на место");
            if (m_console.ErrorCount() != m_tweenSelfTestErrors) return fail("окно Tween дало ошибки в консоли");
            m_scene->RemoveObject(o.Id());

            // ТРИ ДЕЙСТВИЯ: объект → «+ Tween» → свойство — и твин уже рабочий
            // (начало — как есть, конец — ЗАМЕТНО другой, 0.5 с, плавное
            // замедление). Конец, равный началу, был твином, который ничего не
            // делает: Play — и объект стоит на месте.
            namespace anim = sage::anim;
            auto offers = [&](GameObject g, const char* id, const char* section) {
                for (const tweenui::Choice& c : tweenui::Choices(reg, g.Entity()))
                    if (c.Property->Id == id) return std::string(c.Section) == section;
                return false;
            };
            auto quick = [&](GameObject g, const char* id) -> anim::TweenTrack* {
                m_selection.SetPrimary(g.Id());
                OpenTween(g.Id(), 0, TweenOpen::Edit);   // меню свойств здесь — AddProperty напрямую
                if (!m_tween.AddProperty(*this, id)) return nullptr;
                auto* qc = reg.try_get<anim::TweenComponent>(g.Entity());
                if (!qc || qc->Tweens.size() != 1 || qc->Tweens[0].Tracks.size() != 1) return nullptr;
                anim::TweenTrack& t = qc->Tweens[0].Tracks[0];
                const bool defaults = t.Property == id && t.FromCurrent && std::fabs(t.Duration - 0.5f) < 1e-6f &&
                                      t.Curve == anim::Ease::Make(anim::EaseShape::Quad, anim::EaseMode::Out);
                return defaults ? &t : nullptr;
            };
            // Кнопка интерфейса: «Непрозрачность» в разделе интерфейса, → 0.
            GameObject button = CreateUIEntity("Button");
            if (!button.Valid() || !offers(button, "element.opacity", "Interface"))
                return fail("у кнопки в меню нет «Непрозрачности» в разделе интерфейса");
            for (const tweenui::Choice& c : tweenui::Choices(reg, button.Entity()))
                if (c.Property->Id.find("sprite") != std::string::npos)
                    return fail("в меню свойств техническое поле " + c.Property->Id);
            anim::TweenTrack* fade = quick(button, "element.opacity");
            if (!fade || std::fabs(fade->To.x) > 1e-6f)
                return fail("быстрый твин кнопки: конец непрозрачности не 0 (ничего бы не изменилось)");
            fade->To.x = 0.0f;
            m_scene->RemoveObject(button.Id());
            // Позиция объекта — в разделе Transform.
            GameObject cube = m_scene->CreateObject("TweenQuick");
            if (!offers(cube, "object.position", "Transform")) return fail("у объекта в меню нет «Позиции» в Transform");
            // Куб: масштаб 1 → 1.2 за 0.3 с.
            anim::TweenTrack* pop = quick(cube, "object.scale");
            if (!pop || std::fabs(pop->To.x - 1.5f) > 1e-6f)
                return fail("быстрый твин куба: конец масштаба не ×1.5 (ничего бы не изменилось)");
            pop->To = glm::vec4(1.2f, 1.2f, 1.2f, 0.0f);
            pop->Duration = 0.3f;
            m_tween.Play(*this);
            if (!m_tween.Previewing()) return fail("быстрый твин не играет");
            m_tweenSelfTestAt = glfwGetTime();
            m_tweenSelfTestStep = 7;
            return;
        }
        case 7: {
            if (glfwGetTime() - m_tweenSelfTestAt < 0.8) return;
            GameObject cube = m_scene->FindByName("TweenQuick");
            const Transform& t = reg.get<Transform>(cube.Entity());
            if (std::fabs(t.Scale.x - 1.2f) > 1e-3f) return fail("быстрый твин не довёл масштаб: " + std::to_string(t.Scale.x));
            m_tween.Reset(*this);
            if (std::fabs(t.Scale.x - 1.0f) > 1e-5f) return fail("Reset не вернул масштаб куба");
            if (m_console.ErrorCount() != m_tweenSelfTestErrors) return fail("окно Tween дало ошибки в консоли");
            m_scene->RemoveObject(cube.Id());
            m_panels[EditorPanel::Tween] = false;

            // Твин из редактора для прогона Lua-тестов: сам стартует в Play
            // (tween.test.lua проверяет, что он доехал и находится по имени).
            GameObject e = m_scene->CreateObject("LT_EditorTween");
            reg.get<Transform>(e.Entity()).Position = glm::vec3(420.0f, 0.0f, 0.0f);
            sage::anim::TweenClip pulse;
            pulse.Name = "Pulse";
            pulse.PlayOnStart = true;
            sage::anim::TweenTrack up;
            up.Property = "object.position";
            up.To = glm::vec4(420.0f, 3.0f, 0.0f, 0.0f);
            up.Duration = 0.3f;
            pulse.Tracks.push_back(up);
            reg.emplace<sage::anim::TweenComponent>(e.Entity()).Tweens.push_back(pulse);

            LOG_INFO("Editor") << "TWEEN: OK — окно Tween: Play общим проигрывателем сцены довёл объект "
                                  "(сдвиг, затем масштаб по своей кривой), Reset вернул его; в три действия "
                                  "созданы твины масштаба куба и непрозрачности кнопки; ошибок нет";
            m_tweenChecked = true;
            return;
        }
        default: return;
    }
}


void EditorLayer::CheckLuaTestsFrame() {
    if (m_luaSelfTestStep < 0) return;
    if (!std::getenv("SAGE_EDITOR_SELFTEST")) return;
    if (!m_tweenChecked || !m_createDialogChecked || m_aiGraphFrameStep >= 0) return;   // Play — по одному
    namespace fs = std::filesystem;
    if (m_luaSelfTestStep == 0) {
        std::error_code ec;
        const fs::path from = fs::path("assets") / "tests" / "lua";
        const fs::path to = m_project.AssetsDir() / "tests" / "lua";
        fs::create_directories(to, ec);
        fs::copy(from, to, fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
        // Учебные скрипты (simple.test.lua ставит их на объекты) — туда же,
        // где их найдёт человек: assets/scripts/examples проекта.
        if (!ec) {
            const fs::path examples = m_project.AssetsDir() / "scripts" / "examples";
            fs::create_directories(examples, ec);
            fs::copy(fs::path("assets") / "scripts" / "examples", examples,
                     fs::copy_options::overwrite_existing | fs::copy_options::recursive, ec);
        }
        if (ec || !m_project.Loaded()) {
            LOG_ERROR("Editor") << "LUA_TESTS: FAIL — набор тестов не скопирован в проект: " << ec.message();
            m_luaSelfTestStep = -1;
            return;
        }
        if (!RunLuaTests()) {
            LOG_ERROR("Editor") << "LUA_TESTS: FAIL — прогон не запустился";
            m_luaSelfTestStep = -1;
            return;
        }
        m_luaSelfTestStep = 1;
        return;
    }
    if (m_luaTests.Running) return;
    m_luaSelfTestStep = -1;
    const LuaTestRun& r = m_luaTests;
    // Тестов меньше, чем в наборе, — тоже провал: файл, в котором разбор не
    // увидел ни одного test_*, иначе «проходил» бы молча.
    const bool enough = r.Passed + r.Failed >= 70;
    // Твин из редактора своё отыграл — объект больше не нужен.
    if (GameObject e = m_scene->FindByName("LT_EditorTween"); e.Valid()) m_scene->RemoveObject(e.Id());
    // Инспектор видит глобальные переменные простого скрипта: speed и
    // jump_force у Player.lua — поля секции Script, с типом по значению.
    bool inspectorOk = false;
    {
        GameObject probe = m_scene->CreateObject("LuaVarsProbe");
        m_scene->Registry().emplace<ScriptComponent>(probe.Entity(), "assets/scripts/examples/Player.lua");
        MergeScriptVars(probe);
        const sage::vars::Table& fields =
            m_scene->Registry().get<ScriptComponent>(probe.Entity()).Scripts.front().Fields;
        const sage::vars::Var* speed = fields.Find("speed");
        const sage::vars::Var* jump = fields.Find("jump_force");
        inspectorOk = speed && jump && speed->Data.Type() == sage::vars::Kind::Float &&
                      std::fabs(speed->Data.AsFloat() - 5.0f) < 1e-5f && fields.Find("direction") == nullptr;
        m_scene->RemoveObject(probe.Id());
        if (!inspectorOk) LOG_ERROR("Editor") << "LUA_TESTS:   инспектор не показал speed/jump_force у Player.lua";
    }
    if (r.Failed == 0 && enough && inspectorOk && !m_play.Active()) {
        LOG_INFO("Editor") << "LUA_TESTS: OK — тестов на Lua прошло " << r.Passed
                           << " в настоящем Play (объект, жизненный цикл, сигналы, UI, ввод, физика, "
                           << "скрипты, окружение, прежний API, простой вид скрипта на учебных примерах, твины, процедурная генерация)";
        return;
    }
    for (const std::string& f : r.Failures) LOG_ERROR("Editor") << "LUA_TESTS:   " << f;
    LOG_ERROR("Editor") << "LUA_TESTS: FAIL — прошло " << r.Passed << ", провалено " << r.Failed
                        << (enough ? "" : " (тестов меньше, чем в наборе)")
                        << (m_play.Active() ? ", Play не остановился" : "");
}


// ---------------------------------------------------------------------------
// ОКНО ENVIRONMENT ПОКАЗЫВАЕТ ТОЛЬКО НУЖНОЕ.
//
// Проверяется по тому, что панель НАРИСОВАЛА в кадре (LastDrawnKeys), а не по
// схеме: схема может быть верной, а панель — рисовать мимо неё. На каждом шаге
// выставляется состояние (тип неба, цикл, туман), кадр рисуется, и на
// следующем шаге сверяется: нужные строки есть, чужих — нет. В конце —
// сохранение и загрузка сцены с новыми полями и возврат прежнего окружения.
void EditorLayer::CheckEnvironmentFrame() {
    if (m_envChecked) return;
    if (!std::getenv("SAGE_EDITOR_SELFTEST")) return;
    if (!m_uiInspectorChecked) return;
    LightingEnvironment& e = m_scene->Lighting;
    using Src = SkyboxSettings::Source;

    auto has = [this](const char* key) {
        const std::vector<std::string>& d = m_environment.LastDrawnKeys();
        return std::find(d.begin(), d.end(), key) != d.end();
    };
    auto expect = [&](const char* what, std::initializer_list<const char*> yes,
                      std::initializer_list<const char*> no) {
        for (const char* k : yes)
            if (!has(k) && m_envFail.empty()) m_envFail = std::string(what) + ": нет строки " + k;
        for (const char* k : no)
            if (has(k) && m_envFail.empty()) m_envFail = std::string(what) + ": лишняя строка " + k;
    };

    // Панель стоит в доке вкладкой, и за соседней вкладкой её дерево не
    // раскрывается вовсе — рисовать было бы нечего. Выводим вперёд КАЖДЫЙ шаг:
    // игровое окно и другие проверки вправе перетянуть фокус на себя. По
    // идентификатору, а не по имени: имя переводится.
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        if (ImGuiWindow* w = ImGui::FindWindowByID(ImHashStr("Lighting"))) ImGui::FocusWindow(w);

    const int step = m_envStep++;
    switch (step) {
        case 0:
            m_envErrors = m_console.ErrorCount();
            m_envSaved = SceneSerializer::SaveToString(*m_scene);
            m_panels[EditorPanel::Environment] = true;
            e.Skybox.Enabled = true;
            e.Skybox.Kind = Src::Procedural;
            e.Cycle.Enabled = false;
            e.Fog.Enabled = false;
            return;
        case 1: case 2: case 3: case 4:   // окно открывается и выходит вперёд
            return;
        case 5:
            expect("процедурное небо", {"sky.enabled", "sky.type", "sky.zenith", "sky.horizon"},
                   {"sky.image", "sky.folder", "sky.rotation", "sky.color", "cycle.time", "fog.color"});
            e.Skybox.Kind = Src::Solid;
            return;
        case 6:
            expect("небо одним цветом", {"sky.color"},
                   {"sky.zenith", "sky.sun.size", "sky.rotation", "sky.exposure", "sky.clouds.height"});
            e.Skybox.Kind = Src::Image;
            return;
        case 7:
            expect("одна картинка", {"sky.image", "sky.imageLayout", "sky.rotation", "sky.exposure"},
                   {"sky.zenith", "sky.folder", "sky.face.px", "sky.color"});
            e.Skybox.Kind = Src::Cubemap;
            return;
        case 8:
            expect("прежняя папка кубической карты", {"sky.folder", "sky.folder.convert", "sky.rotation",
                                                     "sky.exposure"},
                   {"sky.image", "sky.face.px", "sky.zenith"});
            e.Skybox.Kind = Src::Faces;
            return;
        case 9:
            expect("шесть файлов",
                   {"sky.faces.map", "sky.face.px", "sky.face.nx", "sky.face.py", "sky.face.ny",
                    "sky.face.pz", "sky.face.nz", "sky.rotation", "sky.exposure"},
                   {"sky.image", "sky.folder", "sky.zenith"});
            e.Skybox.Enabled = false;
            return;
        case 10:
            expect("небо выключено", {"sky.enabled"}, {"sky.type", "sky.face.px", "sky.zenith"});
            e.Skybox.Enabled = true;
            e.Skybox.Kind = Src::Procedural;
            // Цикл выключен — видна только галка.
            expect("цикл выключен", {"cycle.enabled"}, {"cycle.time", "cycle.speed"});
            e.Cycle.Enabled = true;
            e.Cycle.Time = 18.0f;   // закат
            return;
        case 11: {
            expect("цикл включён", {"cycle.enabled", "cycle.time", "cycle.speed", "cycle.dayLength"}, {});
            // Панель ставит солнце по циклу сразу: в 18 часов оно на горизонте.
            const glm::vec3 d = sage::ecs::SunDirectionAt(e.Cycle);
            if (std::abs(d.y) > 0.02f && m_envFail.empty()) m_envFail = "цикл: в 18 часов солнце не на горизонте";
            e.Cycle.Enabled = false;
            e.Fog.Enabled = false;
            return;
        }
        case 12:
            expect("туман выключен", {"fog.enabled"}, {"fog.type", "fog.color", "fog.density", "fog.start"});
            e.Fog.Enabled = true;
            e.Fog.Kind = FogSettings::Mode::Linear;
            return;
        case 13:
            expect("линейный туман", {"fog.type", "fog.color", "fog.start", "fog.end"},
                   {"fog.density", "fog.falloff"});
            e.Fog.Kind = FogSettings::Mode::ExponentialHeight;
            return;
        case 14: {
            expect("высотный туман", {"fog.color.height", "fog.density", "fog.falloff", "fog.maxOpacity"},
                   {"fog.end", "fog.start"});
            // СОХРАНЕНИЕ И ЗАГРУЗКА: новые поля (цикл) и прежние (тип неба,
            // туман) переживают запись сцены.
            e.Skybox.Kind = Src::Solid;
            e.Skybox.TopColor = {0.25f, 0.5f, 0.75f};
            e.Cycle.Enabled = true;
            e.Cycle.Time = 7.5f;
            e.Cycle.Speed = 3.0f;
            e.Fog.Density = 0.05f;
            std::unique_ptr<Scene> back = SceneSerializer::LoadFromString(
                SceneSerializer::SaveToString(*m_scene));
            const bool same = back && back->Lighting.Skybox.Kind == Src::Solid &&
                              back->Lighting.Skybox.TopColor == e.Skybox.TopColor &&
                              back->Lighting.Cycle.Enabled && back->Lighting.Cycle.Time == 7.5f &&
                              back->Lighting.Cycle.Speed == 3.0f &&
                              back->Lighting.Fog.Kind == FogSettings::Mode::ExponentialHeight &&
                              back->Lighting.Fog.Density == 0.05f;
            if (!same && m_envFail.empty()) m_envFail = "окружение не пережило сохранение и загрузку";
            return;
        }
        default: break;
    }
    // Вернуть сцене прежнее окружение: проверка не должна менять то, что
    // проверяют следующие.
    if (std::unique_ptr<Scene> orig = SceneSerializer::LoadFromString(m_envSaved))
        m_scene->Lighting = orig->Lighting;
    m_envChecked = true;
    const int added = m_console.ErrorCount() - m_envErrors;
    if (m_envFail.empty() && added == 0)
        LOG_INFO("Editor") << "ENVIRONMENT: OK — четыре типа неба и прежняя папка, развёртка граней, цикл суток, туман и сохранение; "
                           << "показывается только нужное";
    else
        LOG_ERROR("Editor") << "ENVIRONMENT: FAIL — "
                            << (m_envFail.empty() ? "новых ошибок в консоли: " + std::to_string(added)
                                                  : m_envFail);
}


// ---------------------------------------------------------------------------
// ИНСПЕКТОР КАЖДОГО ТИПА И ОКНО ДЕВЯТИНЫ — БЕЗ ПРЕТЕНЗИЙ ImGui.
//
// Окно девятины, суженное до ширины, где его колонки не помещались, писало в
// консоль тысячи строк «Code uses SetCursorPos() to extend window/parent
// boundaries» — по строке на кадр. Инспектор элемента разный у каждого типа
// (виды состояний у кнопки, дорожка и ручка у ползунка, раскладка у
// контейнера), и ошибка в одном из них видна только на нём. Поэтому каждый тип
// создаётся и показывается живым кадром, а окно девятины открывается на
// подложке и сужается до минимума. Консоль после этого обязана остаться без
// новых ошибок.
void EditorLayer::CheckUiInspectorFrame() {
    if (m_uiInspectorChecked) return;
    if (!std::getenv("SAGE_EDITOR_SELFTEST")) return;
    if (!m_previewResChecked || m_probeStep >= 0) return;   // те проверки двигают окна
    if (m_uiInspectorWait > 0) { --m_uiInspectorWait; return; }

    const std::vector<sage::ui::Preset>& presets = sage::ui::Presets();
    auto finish = [this]() {
        m_uiInspectorChecked = true;
        m_panelRegistry.SetVisible("SpriteSheet", false);
        SetWorkspace(EditorWorkspace::Scene);
        const int added = m_console.ErrorCount() - m_uiInspectorErrors;
        if (added == 0)
            LOG_INFO("Editor") << "UI_INSPECTOR: OK — инспектор каждого типа и узкое окно девятины "
                               << "без ошибок ImGui";
        else
            LOG_ERROR("Editor") << "UI_INSPECTOR: FAIL — новых ошибок в консоли: " << added;
    };

    // Инспектор интерфейса — вперёд своей вкладки на всю проверку: панель за
    // другой вкладкой не рисуется, и проверка «каждого типа» проходила, не
    // показав ни одного поля (так в main попало зависание контролов).
    m_focusPanelId = "InterfaceInspector";
    m_focusPanelFrames = 2;
    const int step = m_uiInspectorStep++;
    if (step == 0) {
        m_uiInspectorErrors = m_console.ErrorCount();
        SetWorkspace(EditorWorkspace::Interface);
        m_uiInspectorWait = 4;
        return;
    }
    // Шаги 1..2N — по два на тип: создать (оформление движка) и перевести в
    // своё оформление с видами состояний — у каждого свой инспектор.
    const int index = step - 1;
    if (index < 2 * (int)presets.size()) {
        const sage::ui::Preset& preset = presets[(size_t)(index / 2)];
        if (index % 2 == 0) {
            GameObject e = CreateUIEntity(preset.Name);
            if (e.Valid()) {
                m_selection.SetPrimary(e.Id());
                // Цель девятины — подложка кнопки (у панели фон — форма).
                if (preset.Name == "Button") m_uiInspectorElement = e.Id();
            }
        } else if (GameObject e = m_scene->Get(m_selection.Primary()); e.Valid()) {
            entt::registry& reg = m_scene->Registry();
            if (sage::ui::Element* el = reg.try_get<sage::ui::Element>(e.Entity())) {
                sage::ui::BakeEngineSkin(reg, e.Entity());
                el->Skin = sage::ui::Element::SkinMode::Custom;
            }
            // Состояния тоже: инспектор кнопки с включёнными видами
            // состояний — самый длинный.
            if (auto* act = reg.try_get<sage::ui::Interactable>(e.Entity()))
                act->UseHoverLook = act->UsePressedLook = true;
        }
        m_uiInspectorWait = 2;
        return;
    }
    const int after = index - 2 * (int)presets.size();
    if (after == 0) {
        // Девятина подложки кнопки — в редакторе спрайтов, и сразу узким.
        EditorHost::NineSliceTarget t;
        t.ElementId = m_uiInspectorElement;
        t.PartId = "fill";
        t.BorderKey = "sliceBorder";
        OpenNineSliceEditor(t);
        m_uiInspectorWait = 3;
        return;
    }
    if (after == 1) {
        // Девятина из инспектора — в редакторе спрайтов, привязанной к виду
        // элемента: правка доходит до САМОГО элемента (его свои значения),
        // а не до листа или .sage9.
        EditorHost::NineSliceTarget t;
        t.ElementId = m_uiInspectorElement;
        t.PartId = "fill";
        t.BorderKey = "sliceBorder";
        sage::ui::NineSlice probe;
        probe.SetBorder({3.0f, 4.0f, 5.0f, 6.0f});
        sage::editor::sprite::BoundSlice back;
        const bool bound = m_spriteSheet.BoundToElement();
        const bool wrote = sage::editor::sprite::WriteBound(*this, t, probe);
        const bool read = sage::editor::sprite::ReadBound(*this, t, back);
        if (!bound || !wrote || !read || back.Slice.Border() != probe.Border())
            LOG_ERROR("Editor") << "UI_INSPECTOR: девятина элемента не открылась в редакторе спрайтов (привязка "
                                << bound << ", запись " << wrote << ", чтение " << read << ")";
        ImGui::SetWindowSize("###SpriteSheet", ImVec2(420.0f, 300.0f));
        m_uiInspectorWait = 4;
        return;
    }
    if (after == 2) {
        ImGui::SetWindowSize("###SpriteSheet", ImVec2(1100.0f, 700.0f));
        m_uiInspectorWait = 3;
        return;
    }
    finish();
}


// ---------------------------------------------------------------------------
// РАЗРЕШЕНИЕ ПРЕДПРОСМОТРА ПРАВИТ КАДР, А НЕ ПРОЕКТ.
//
// Раскладка интерфейса считается в пикселях кадра, и весь смысл якорей и
// растяжений в том, что на другом размере экрана она ведёт себя правильно.
// Посмотреть на это можно было единственным способом: сменить разрешение в
// настройках игры, взглянуть и вернуть обратно — то есть править то, что
// уедет в собранную игру, ради одного взгляда.
//
// Проверяется живым кадром: холст обязан пересчитаться в выбранный размер, а
// настройки игры — остаться нетронутыми. Ни то, ни другое не видно нигде,
// кроме как в числах после настоящего кадра: размер кадра выставляет сама
// панель холста, когда рисуется.
void EditorLayer::CheckPreviewResolutionFrame() {
    if (m_previewResChecked) return;
    if (!std::getenv("SAGE_EDITOR_SELFTEST")) return;
    if (!m_workspaceDockChecked) return;   // та проверка сама переключает пространство
    if (m_previewResWait > 0) { --m_previewResWait; return; }

    constexpr int kW = 1080, kH = 1920;   // вертикальный экран: 16:9 наоборот
    auto done = [this](bool ok, const char* why) {
        m_previewResChecked = true;
        m_tools.UI.PreviewSize = glm::ivec2(0);
        SetWorkspace(EditorWorkspace::Scene);
        if (ok) LOG_INFO("Editor") << "PREVIEW_RES: OK — холст считается в выбранном разрешении, "
                                   << "настройки игры не тронуты";
        else LOG_ERROR("Editor") << "PREVIEW_RES: FAIL — " << why;
    };

    sage::EngineConfig& cfg = sage::EngineConfig::Get();
    switch (m_previewResStep) {
        case 0:
            m_previewResGameW = cfg.Width;
            m_previewResGameH = cfg.Height;
            SetWorkspace(EditorWorkspace::Interface);
            m_tools.UI.PreviewSize = glm::ivec2(kW, kH);
            m_previewResStep = 1;
            m_previewResWait = 4;   // холст пишет размер, когда рисуется
            return;
        case 1: {
            if ((int)m_tools.UI.FrameSize.x != kW || (int)m_tools.UI.FrameSize.y != kH) {
                done(false, "холст не пересчитался в разрешение предпросмотра");
                return;
            }
            if (m_renderer.GameWidth() != kW || m_renderer.GameHeight() != kH) {
                done(false, "игровой кадр остался прежнего размера");
                return;
            }
            if (cfg.Width != m_previewResGameW || cfg.Height != m_previewResGameH) {
                done(false, "предпросмотр залез в настройки игры");
                return;
            }
            m_tools.UI.PreviewSize = glm::ivec2(0);
            m_previewResStep = 2;
            m_previewResWait = 4;
            return;
        }
        default:
            // Ноль — «как в игре»: кадр обязан вернуться к размеру из настроек,
            // иначе выбранное однажды разрешение осталось бы навсегда.
            if ((int)m_tools.UI.FrameSize.x != m_previewResGameW ||
                (int)m_tools.UI.FrameSize.y != m_previewResGameH) {
                done(false, "холст не вернулся к разрешению игры");
                return;
            }
            done(true, nullptr);
            return;
    }
}
