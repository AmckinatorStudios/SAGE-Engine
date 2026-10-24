#include "SelfTestCommon.h"

// ============================================================================
//  МЫШЬ В ЖИВОМ КАДРЕ: щелчок по вьюпорту проверяется НАСТОЯЩИМ щелчком
// ============================================================================
//
// Зачем так, а не вызовом внутренних функций. Два отказа подряд — «щёлкаю по
// гизмо осей, ничего не происходит» и «щёлкаю по пустому месту, выделение не
// снимается» — оба в коде, который по отдельности выглядит правильным.
// Ломается не он, а ВОРОТА на пути щелчка: захват мыши слотом, ImGuizmo::IsOver,
// гизмо осей, рамка выделения. Проверка, которая зовёт PickAtViewportWith
// напрямую, проходит и при полностью мёртвом вьюпорте: она проверяет не то
// место.
//
// Поэтому события мыши кладутся в очередь ImGui теми же вызовами, что и у
// бэкенда GLFW, и дальше кадр идёт своим ходом. Ставить их надо ПОСЛЕ
// ImGui_ImplGlfw_NewFrame и ДО ImGui::NewFrame: бэкенд в своём NewFrame тоже
// трогает положение курсора, и наши события обязаны прийти последними.
void EditorLayer::TickInputProbe() {
    if (m_probeStep < 0) return;
    // СТОРОЖ. Пока проверка не кончилась, редактор не закрывается (см.
    // TakeAutoScreenshot) — значит она обязана кончаться при любом исходе, в том
    // числе если вьюпорта в кадре так и не появилось. Иначе headless-прогон
    // висел бы до таймаута CI без единой строки о причине.
    if (++m_probeFrames > 900) {
        LOG_ERROR("Editor") << "VIEWPORT_INPUT: FAIL — вьюпорт так и не отозвался на мышь "
                            << "(шаг " << m_probeStep << ")";
        m_probeStep = -1;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    // КУРСОР ДЕРЖИМ КАЖДЫЙ КАДР. Бэкенд GLFW в своём NewFrame кладёт в очередь
    // НАСТОЯЩЕЕ положение курсора (в headless — середина окна), и стоит нам
    // пропустить кадр, как «нажатие» и «отпускание» происходят в разных точках:
    // редактор видит не щелчок, а протяжку через полэкрана.
    if (m_probeStep > 0) io.AddMousePosEvent(m_probePos.x, m_probePos.y);
    // ВЬЮПОРТ ДЕРЖИМ ВПЕРЕДИ КАЖДЫЙ КАДР, пока идёт проверка мыши.
    //
    // Он делит центральный узел дока с игровым окном, и вкладка там ОДНА
    // активная. Кто выходил вперёд последним, тот и получает щелчок: игровое
    // окно выводит себя вперёд при каждом запуске игры (GamePanel::RequestFocus),
    // а прогон запускает её не раз. Просьба «выйди вперёд» на одном кадре этого
    // не решает — к моменту щелчка её уже перебили.
    // ПАНЕЛЬ, ПО КОТОРОЙ СЕЙЧАС ЩЁЛКАЮТ, ДЕРЖИМ ВПЕРЕДИ КАЖДЫЙ КАДР.
    //
    // Панели делят узлы дока вкладками, и активная там ОДНА: вьюпорт делит
    // центр с игровым окном, панель ассетов — низ с консолью. Кто вышел вперёд
    // последним, тот и получает щелчок, а выходят вперёд по своим поводам
    // (игровое окно — при каждом запуске игры, и прогон запускает её не раз).
    // Просьба «выйди вперёд» на одном кадре этого не решает: к моменту щелчка
    // её уже перебили. Человек в этот момент просто смотрит на нужную панель —
    // здесь это выражено явно.
    //
    // Окно ищется ПО ИДЕНТИФИКАТОРУ, а не по имени: имя панели переводится
    // («Assets» / «Ассеты»), и SetWindowFocus по строке в русском редакторе не
    // находил ничего — молча, потому что «окна с таким именем нет» ImGui не
    // считает поводом сказать хоть слово. Идентификатор — часть после «###», и
    // он от языка не зависит (ровно ради этого «###» в именах и стоит).
    //
    // ПОКА ОТКРЫТА МОДАЛКА — НЕ ТРОГАЕМ ФОКУС. ImGui::FocusWindow закрывает
    // всплывающие окна, оказавшиеся поверх того, кого выводят вперёд: просьба
    // «выйди вперёд, панель ассетов» каждый кадр гасила вопрос «удалить файл?»
    // на следующем же кадре после его появления. Человек, увидев вопрос, на
    // другую панель не переключается.
    auto focusPanel = [](const char* id) {
        if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
        if (ImGuiWindow* w = ImGui::FindWindowByID(ImHashStr(id))) ImGui::FocusWindow(w);
    };
    if (m_probeStep >= 0 && m_workspace == EditorWorkspace::Scene) {
        if (m_probeStep <= 8) focusPanel("Viewport");
        else if (m_probeStep >= 21) focusPanel("Assets");
    }
    if (m_probeWait > 0) { --m_probeWait; return; }

    const ImVec2 mn = m_viewport.ViewMin();
    const ImVec2 mx = m_viewport.ViewMax();
    if (mx.x - mn.x < 64.0f || mx.y - mn.y < 64.0f) return;  // вьюпорт ещё не нарисован

    auto fail = [&](const char* what) {
        LOG_ERROR("Editor") << "VIEWPORT_INPUT: FAIL — " << what;
        LOG_ERROR("Editor") << "PROBE_DIAG: шаг " << m_probeStep << " прост " << (int)m_workspace
                            << " фокус " << (ImGui::GetCurrentContext()->NavWindow
                                                 ? ImGui::GetCurrentContext()->NavWindow->Name : "-")
                            << " под курсором " << (ImGui::GetCurrentContext()->HoveredWindow
                                                 ? ImGui::GetCurrentContext()->HoveredWindow->Name : "-")
                            << " курсор " << m_probePos.x << "," << m_probePos.y;
        m_probeFailed = true;
    };
    // Щелчок = положение + нажатие; отпускание — отдельным кадром: редактор
    // различает клик и рамку выделения именно по кадрам.
    auto press = [&](const ImVec2& at) {
        m_probePos = at;
        io.AddMousePosEvent(at.x, at.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    };
    auto release = [&]() { io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); };

    switch (m_probeStep) {
        case 0: {
            // ВЬЮПОРТ — ВПЕРЁД И БЕЗ ЧУЖОГО ОКНА СВЕРХУ.
            //
            // Проверка многооконности отстыковывает панель ассетов на несколько
            // кадров и возвращает её обратно (см. CheckMultiWindowFrame). Пока
            // панель снаружи, нижний узел дока пуст, центральный растянут — то
            // есть ВЬЮПОРТ СТОИТ НЕ ТАМ, где он будет через кадр. Щёлкать по
            // координатам в этот момент значит целиться в место, которое
            // сейчас переедет, и получать три одинаковых «щелчок ничего не
            // сделал» подряд.
            //
            // Поэтому ждём не только окончания проверки, но и того, чтобы
            // раскладка устоялась: возврат панели в док занимает несколько
            // кадров (panelwindows::Settle).
            if (!m_multiWindowChecked) { m_probeWait = 1; return; }
            // И проверки общих панелей: она нарочно переключает пространство
            // туда-обратно, а щелчки по вьюпорту сцены в вёрстке бессмысленны.
            if (!m_workspaceDockChecked) { m_probeWait = 1; return; }
            // И проверки разрешения предпросмотра: она тоже уходит в вёрстку.
            if (!m_previewResChecked) { m_probeWait = 1; return; }
            // МОДАЛЬНОЕ ОКНО СЪЕДАЕТ ВСЕ ЩЕЛЧКИ, и проверка мыши под ним
            // осмысленна не больше, чем нажатие на выключенный экран. Такое
            // окно в headless-прогоне — само по себе отказ (например, после
            // падения остался файл восстановления сцены), и назвать его надо
            // прямо, а не тремя одинаковыми «щелчок ничего не сделал».
            if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal()) {
                fail((std::string("щелчку мешает модальное окно: ") + modal->Name).c_str());
                m_probeStep = -1;
                return;
            }
            // И В ПРОСТРАНСТВЕ СЦЕНЫ. Проверка щёлкает по вьюпорту сцены и по
            // списку её объектов, а шаг «каталог объектов» выше создаёт элемент
            // интерфейса — и уводит редактор в пространство вёрстки, где ни
            // того, ни другого окна попросту нет. Три подряд «щелчок ничего не
            // сделал» означали ровно это.
            SetWorkspace(EditorWorkspace::Scene);
            m_panels[EditorPanel::InterfaceViewport] = false;
            m_viewport.RequestFocus();
            m_probeWait = 12;
            break;
        }
        case 1: {
            // Целимся в ПУСТОЕ место: левый верхний угол вида — там небо.
            // Виджет инструментов прижат к самому верху, поэтому берём ниже.
            GameObject any = m_scene->FindByName("Red Cube");
            if (!any.Valid()) { fail("в сцене нет объекта, на котором проверять"); break; }
            m_selection.SetPrimary(any.Id());
            press(ImVec2(mn.x + (mx.x - mn.x) * 0.06f, mn.y + (mx.y - mn.y) * 0.35f));
            break;
        }
        case 2: release(); break;
        case 3:
            // ЩЕЛЧОК ПО ПУСТОМУ МЕСТУ СНИМАЕТ ВЫДЕЛЕНИЕ. Иначе снять его можно
            // только выбрав что-то другое — то есть никак.
            if (!m_selection.Empty() || m_selection.Primary() != -1) {
                fail("щелчок по пустому месту не снял выделение");
            }
            break;
        case 4:
            // СНАЧАЛА ПОДВЕСТИ КУРСОР, ПОТОМ НАЖАТЬ — отдельными кадрами.
            // Гизмо навигации крутит вид по СМЕЩЕНИЮ курсора, и если прыжок к
            // шарику случится в кадре нажатия, камера поедет от протяжки. Такая
            // проверка зелена и при полностью мёртвом щелчке.
            m_probePos = sage::editor::viewgizmo::AxisBall(m_camera, mn, mx, 0, true);
            io.AddMousePosEvent(m_probePos.x, m_probePos.y);
            break;
        case 5:
            // Щелчок по шарику оси X гизмо навигации: камера обязана поехать.
            m_probeYaw = m_camera.Yaw;
            press(m_probePos);
            break;
        case 6: release(); break;
        case 7:
            m_probeWait = 6;   // переход плавный (четверть секунды) — даём кадры
            break;
        case 8:
            if (std::abs(m_camera.Yaw - m_probeYaw) < 0.01f) {
                fail("щелчок по гизмо навигации не повернул камеру");
            }
            break;
        case 9: {
            // ТО ЖЕ САМОЕ В СПИСКЕ ОБЪЕКТОВ. Выделение снимают там, где на него
            // смотрят, а список — второе такое место после вьюпорта.
            GameObject any = m_scene->FindByName("Red Cube");
            if (!any.Valid()) { fail("в сцене нет объекта, на котором проверять"); break; }
            m_selection.SetPrimary(any.Id());
            const ImGuiWindow* list = ImGui::FindWindowByName("Hierarchy");
            if (!list || list->Size.y < 64.0f) { fail("списка объектов нет в кадре"); break; }
            // Ниже строк: у стартовой сцены их единицы, и низ списка пуст.
            press(ImVec2(list->Pos.x + list->Size.x * 0.5f, list->Pos.y + list->Size.y - 12.0f));
            break;
        }
        case 10: release(); break;
        case 11:
            if (!m_selection.Empty() || m_selection.Primary() != -1) {
                fail("щелчок по пустому месту списка объектов не снял выделение");
            }
            break;
        case 12: {
            // ПОЛОСЫ ДОЛГОЙ РАБОТЫ (см. Progress.h). Заводим обе задачи
            // понарошку: важную (модальное окно) и фоновую (карточка в углу).
            namespace progress = sage::editor::progress;
            progress::Clear();
            progress::Begin(progress::Kind::Background, "ПРОВЕРКА: фон", "1 из 2");
            progress::Begin(progress::Kind::Blocking, "ПРОВЕРКА: важное");
            m_probeWait = 2;   // окно появляется не в том же кадре
            break;
        }
        case 13: {
            // Спрашиваем ОКНА, а не ImGui::IsPopupOpen: проверка живёт МЕЖДУ
            // кадрами (см. начало функции), а IsPopupOpen считает идентификатор
            // от текущего окна — которого в этот момент нет, и обращение уходит
            // по нулевому указателю. WasActive отвечает ровно на нужный вопрос:
            // нарисовалось ли окно в прошлом кадре.
            namespace progress = sage::editor::progress;
            auto drawn = [](const char* name) {
                const ImGuiWindow* w = ImGui::FindWindowByName(name);
                return w != nullptr && w->WasActive;
            };
            if (!progress::Blocked()) { fail("важная работа не считается блокирующей"); break; }
            if (!drawn("##progress_modal")) {
                fail("важная загрузка не показала модального окна");
                break;
            }
            if (!drawn("##progress_card_0")) {
                fail("фоновая загрузка не показала карточки в углу");
            }
            break;
        }
        case 14: {
            namespace progress = sage::editor::progress;
            progress::Clear();
            m_probeWait = 2;
            break;
        }
        case 15: {
            // Работа кончилась — окна не осталось. Модалка, которую некому
            // закрыть, запирает редактор наглухо.
            const ImGuiWindow* modal = ImGui::FindWindowByName("##progress_modal");
            if (modal != nullptr && modal->WasActive) {
                fail("модальное окно осталось после конца работы");
            }
            break;
        }
        case 16: {
            // ВЫБОР ДИАПАЗОНА С SHIFT в списке объектов. Щёлкаем по первой
            // строке обычным щелчком — она станет якорем.
            if (m_hierarchy.RowCount() < 4) { fail("в списке объектов меньше четырёх строк"); break; }
            press(m_hierarchy.RowCenter(0));
            break;
        }
        case 17: release(); break;
        case 18: {
            if (m_selection.All().size() != 1) { fail("щелчок по строке списка выбрал не один объект"); break; }
            // И с зажатым Shift — по четвёртой: выбранными обязаны стать все
            // четыре, а не две (так вело себя прежнее «Shift = то же, что Ctrl»).
            io.AddKeyEvent(ImGuiMod_Shift, true);
            press(m_hierarchy.RowCenter(3));
            break;
        }
        case 19:
            release();
            io.AddKeyEvent(ImGuiMod_Shift, false);
            m_probeWait = 2;   // диапазон применяется в конце кадра отрисовки списка
            break;
        case 20: {
            // ПЕРЕТАСКИВАНИЕ ВЫДЕЛЕННОЙ ГРУППЫ — настоящей мышью. Взялись за
            // одну из четырёх выделенных строк и потащили: набор обязан
            // остаться целым и уехать весь. Раньше выбор делался на нажатии,
            // набор схлопывался до одной строки, и уезжала только она.
            // Подшаги — внутри этого шага (m_groupProbe), номер шага не
            // двигается, пока они не кончились.
            const ImGuiWindow* list = ImGui::FindWindowByName("Hierarchy");
            const int sub = m_groupProbe++;
            bool done = false;
            switch (sub) {
                case 0:
                    if (m_selection.All().size() != 4) {
                        fail("Shift-щелчок не выбрал диапазон строк");
                        done = true;
                        break;
                    }
                    // Порядок строк ДО броска: группа обязана приехать в нём же.
                    m_groupIds.clear();
                    for (int i = 0; i < 4; ++i) m_groupIds.push_back(m_hierarchy.RowId(i));
                    press(m_hierarchy.RowCenter(1));
                    break;
                case 1:
                    if (m_selection.All().size() != 4) {
                        fail("нажатие на выделенную строку схлопнуло набор до одной — тащится одна");
                        done = true;
                        break;
                    }
                    m_probePos = ImVec2(m_probePos.x, m_probePos.y + 30.0f);
                    break;
                case 2: {
                    // Цель — нижняя четверть ПОСЛЕДНЕЙ строки: «встать после
                    // неё». Она корневая и свёрнута, так что группа обязана
                    // оказаться последней в корне.
                    const ImVec2 last = m_hierarchy.RowCenter((int)m_hierarchy.RowCount() - 1);
                    m_probePos = ImVec2(last.x, last.y + ImGui::GetTextLineHeight() * 0.38f);
                    (void)list;
                    break;
                }
                case 3:
                    break;   // кадр наведения — цель броска узнаёт курсор
                case 4:
                    release();
                    m_probeWait = 2;
                    break;
                case 5: {
                    if (m_selection.All().size() != 4) {
                        fail("после перетаскивания группы выделение потерялось");
                        done = true;
                        break;
                    }
                    // Все четыре — в конце корня, в прежнем порядке.
                    const std::vector<entt::entity> roots = m_scene->Roots();
                    bool tail = roots.size() >= 4;
                    for (size_t i = 0; tail && i < 4; ++i)
                        tail = m_scene->Registry().get<IdComponent>(roots[roots.size() - 4 + i]).Id == m_groupIds[i];
                    if (!tail) {
                        fail("перетащенная группа не встала в конец списка целиком");
                        done = true;
                        break;
                    }
                    // Обычный щелчок по выделенной (без перетаскивания) — как
                    // и прежде, выбирает одну.
                    press(m_hierarchy.RowCenter((int)m_hierarchy.RowCount() - 1));
                    break;
                }
                case 6:
                    release();
                    m_probeWait = 2;
                    break;
                default:
                    if (m_selection.All().size() != 1) fail("щелчок по выделенной строке не выбрал одну");
                    done = true;
                    break;
            }
            if (!done) --m_probeStep;   // остаёмся на этом шаге
            break;
        }
        case 21: {
            // Панель ассетов делит место с консолью — выводим её вперёд и ждём
            // кадр, иначе щёлкать не по чему. Заодно кладём в папку проекта
            // файл, на котором проверяем удаление: своих файлов в свежем
            // проекте нет, а проверка не должна зависеть от того, что там
            // накопилось от других шагов.
            // Папку заводим заново: шаги выше создают и УДАЛЯЮТ проекты
            // шаблонов, и текущий проект вполне может указывать на снесённую
            // папку — карточек в ней не будет ни одной по причине, к
            // проверяемому отношения не имеющей.
            m_assetsCwd = m_project.AssetsDir();
            std::error_code mk;
            fs::create_directories(m_assetsCwd, mk);
            std::ofstream(m_assetsCwd / "selftest_delete_me.txt") << "delete me\n";
            // ВЫБРАННЫЙ ОБЪЕКТ СЦЕНЫ — ПРИМАНКА. Delete в панели ассетов
            // спрашивал про файл И ТУТ ЖЕ доходил до общего обработчика,
            // который молча удалял выбранный объект: человек смотрел в дерево
            // файлов, а лишался объекта в сцене. Та же клавиша так же утекала
            // из инструмента анимации, где она убирает ключ.
            {
                GameObject victim = m_scene->CreateObject("SelftestHotkeyVictim");
                m_probeVictimId = victim.Id();
                m_selection.SetPrimary(m_probeVictimId);
            }
            m_assets.RequestFocus();
            m_probeWait = 3;
            break;
        }
        case 22: {
            // DELETE В ПАНЕЛИ АССЕТОВ: щёлкаем по первой карточке.
            if (m_assets.TileCount() < 1) { fail("в панели ассетов нет ни одной карточки"); break; }
            press(m_assets.TileCenter(0));
            break;
        }
        case 23:
            release();
            // ФОКУС ИДЁТ ЗА ЩЕЛЧКОМ НЕ В ТОТ ЖЕ КАДР. Delete в панели ассетов
            // срабатывает, только когда работают именно с ней, и нажатие в
            // кадре отпускания приходило в панель, которая ещё не считалась
            // активной. Даём фокусу дойти.
            m_probeWait = 3;
            break;
        case 24: {
            if (m_assets.SelectionSet().empty()) { fail("щелчок по карточке не выбрал файл"); break; }
            io.AddKeyEvent(ImGuiKey_Delete, true);
            break;
        }
        case 25:
            io.AddKeyEvent(ImGuiKey_Delete, false);
            m_probeWait = 2;
            break;
        case 26:
            // Вопрос об удалении задан — файл при этом ещё на месте: удаляет
            // ответ, а не клавиша.
            if (!m_assets.DeletePending()) {
                fail("Delete в панели ассетов не спросил об удалении");
            }
            // И НЕ УТЁК В СЦЕНУ. Вопрос в прогоне выключен, как у человека,
            // нажавшего «больше не спрашивать», — утёкшая клавиша удалила бы
            // объект молча и сразу.
            if (!m_scene->Get(m_probeVictimId).Valid()) {
                fail("Delete в панели ассетов удалил объект из сцены");
            }
            // Уходим из вопроса так же, как человек: Escape.
            io.AddKeyEvent(ImGuiKey_Escape, true);
            break;
        case 27:
            io.AddKeyEvent(ImGuiKey_Escape, false);
            m_probeWait = 3;
            break;
        case 28: {
            // И вопрос не возвращается сам собой: цели удаления сброшены.
            if (m_assets.DeletePending()) {
                fail("вопрос об удалении не закрылся по Escape");
            }
            std::error_code rc;
            fs::remove(m_assetsCwd / "selftest_delete_me.txt", rc);
            if (m_probeVictimId >= 0) m_scene->RemoveObject(m_probeVictimId);
            m_probeVictimId = -1;
            m_selection.Clear();
            break;
        }
        case 29: {
            // CTRL+D ДАЁТ ОДНУ КОПИЮ, А НЕ ДВЕ.
            //
            // Список элементов интерфейса разбирает Ctrl+D сам, и следом та же
            // клавиша доходила до общего обработчика редактора: копий
            // получалось две, одна поверх другой. Вторую замечали не сразу —
            // она стоит ровно там же, — а потом находили в списке лишний
            // объект неизвестно откуда.
            m_probeVictimId = m_scene->CreateObject("SelftestDupSource").Id();
            m_selection.SetPrimary(m_probeVictimId);
            m_probeObjectsBefore = (int)m_scene->Registry().view<IdComponent>().size();
            m_probeWait = 2;
            break;
        }
        case 30:
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiKey_D, true);
            // КЛАВИШУ ДЕРЖИМ ДОЛЬШЕ ПОРОГА АВТОПОВТОРА. Человек не отпускает
            // Ctrl+D мгновенно, а ImGui по умолчанию повторяет нажатие — и
            // задержавшаяся на четверть секунды клавиша делала работу дважды:
            // две копии, одна поверх другой. Проверка, отпускающая клавишу в
            // следующем же кадре, этого не видела никогда.
            m_probeWait = 25;
            break;
        case 31:
            io.AddKeyEvent(ImGuiKey_D, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            m_probeWait = 3;
            break;
        case 32: {
            const int now = (int)m_scene->Registry().view<IdComponent>().size();
            const int added = now - m_probeObjectsBefore;
            if (added != 1) {
                fail(("Ctrl+D создал копий: " + std::to_string(added) + " вместо одной").c_str());
            }
            // Прибираем за собой: и исходник, и всё, что от него народилось.
            std::vector<int> junk;
            m_scene->Registry().view<IdComponent, NameComponent>().each(
                [&](const IdComponent& id, const NameComponent& n) {
                    if (n.Name.rfind("SelftestDupSource", 0) == 0) junk.push_back(id.Id);
                });
            for (int id : junk) m_scene->RemoveObject(id);
            m_probeVictimId = -1;
            m_selection.Clear();
            break;
        }
        case 33:
            // КНОПКА «СОБРАТЬ» ДОЛЖНА ОТКРЫВАТЬ ОКНО, А НЕ МОЛЧАТЬ.
            //
            // Диалоги открываются ПО ИМЕНИ (см. EditorHost::RequestDialog), а
            // ImGui::OpenPopup для окна, которого никто не начинает, не говорит
            // ничего: кнопка внешне срабатывает и не делает НИЧЕГО. Ровно так
            // уже молчали «Сохранить» и «Новая сцена» в верхней панели.
            RequestDialog("Build Game");
            m_probeWait = 3;
            break;
        case 34: {
            // Спрашиваем САМО ОКНО, а не ImGui::IsPopupOpen(имя): та считает
            // идентификатор от ТЕКУЩЕГО окна, а проверка идёт до NewFrame, где
            // текущего окна нет вовсе — и разыменовывает пустоту. Имя окна
            // после «###» переводу не подлежит (см. PanelWindowId.h), поэтому
            // сравнение по нему переживает смену языка.
            const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
            const bool opened =
                modal && std::string(modal->Name).find("###Build Game") != std::string::npos;
            if (!opened) {
                fail("«Собрать» не открыло окно сборки");
            }
            io.AddKeyEvent(ImGuiKey_Escape, true);
            break;
        }
        case 35:
            io.AddKeyEvent(ImGuiKey_Escape, false);
            m_probeWait = 2;
            break;
        default:
            if (!m_probeFailed) LOG_INFO("Editor") << "VIEWPORT_INPUT: OK — щелчок по пустому "
                                                      "снимает выбор и во вьюпорте, и в списке "
                                                      "объектов, гизмо навигации поворачивает "
                                                      "камеру, полосы долгой работы видны, Shift "
                                                      "выбирает диапазон, Delete спрашивает об "
                                                      "удалении файла и не трогает при этом сцену, "
                                                      "Ctrl+D даёт одну копию, «Собрать» "
                                                      "открывает окно сборки";
            m_probeStep = -1;
            return;
    }
    ++m_probeStep;
    // Кадр «на усвоение»: события ImGui разбирает в NewFrame, а редактор
    // отвечает на них уже при отрисовке панели.
    if (m_probeWait == 0) m_probeWait = 1;
}
