#include "SelfTestCommon.h"

// --- устойчивость кадра вьюпорта -------------------------------------------
//
// САМАЯ ДОРОГАЯ ИЗ ВСЕХ ПОЛОМОК РЕНДЕРА — та, которой нет в первом кадре.
// «Создаёшь пустой проект, картинка нормальная, а потом она без причины
// портится»: каждый отдельный кадр правильный, а кадры складываются друг с
// другом, и через десяток-другой картинка уже не та. Ни одна проверка,
// снимающая ОДИН кадр, этого не видит по построению — включая эталонные кадры
// движка.
//
// Здесь кадр вьюпорта снимается несколько раз ПОЛНЫМ путём редактора (тени,
// отражения, сцена, частицы, гизмо, пост-обработка, кайма выделения) и
// сравнивается сам с собой. Сцена при этом неподвижна, время не идёт —
// значит, любое расхождение означает, что рендер что-то КОПИТ между кадрами.
//
// В сцене намеренно есть эмиттер частиц: именно проходы, поднимающие
// смешивание (частицы, билборды, интерфейс), однажды забыли снять его за
// собой — и цепочка пост-обработки, чьи буферы живут между кадрами, начала
// подмешивать кадр к предыдущему вместо замены. Без частиц в сцене этот путь
// не выполняется вовсе, и проверка прошла бы мимо той самой поломки.
bool EditorLayer::SelfTestRenderStability() {
    bool ok = true;

    // Эмиттер на отдельной сущности — минимальная сцена, поднимающая все
    // проходы кадра.
    GameObject fx = m_scene->CreateObject("SelftestParticles");
    fx.GetTransform().Position = {0.0f, 1.0f, 0.0f};
    ParticleEmitterComponent& emitter =
        m_scene->Registry().emplace<ParticleEmitterComponent>(fx.Entity());
    emitter.Effect.RateOverTime = 60.0f;
    emitter.Effect.StartLifetime = {30.0f, 30.0f};   // частицы не должны умереть посреди прогона
    emitter.Playing = true;

    m_renderer.SetViewportSize(0, 320, 240);
    m_renderer.SetGameSize(320, 240);

    // Частицы рождаем ОДИН раз и дальше время не двигаем: движущиеся частицы
    // меняли бы кадр законно, и проверка потеряла бы смысл.
    sage::fx::UpdateEmitters(*m_scene, m_renderer.Particles(), 0.25f);

    const sage::EngineConfig& cfg = sage::EngineConfig::Get();
    std::vector<unsigned char> first;
    int w = 0, h = 0;
    int worstDiff = 0;
    double worstMean = 0.0;
    sage::rhi::ResourceCounts baseline;
    bool haveBaseline = false;
    std::string growth;

    // Восемь кадров: первый прогревает (снимается карта окружения, создаются
    // ленивые шейдеры), со второго кадр обязан быть один и тот же.
    constexpr int kFrames = 8;
    for (int i = 0; i < kFrames; ++i) {
        // Адаптация глаза меняет кадр ЗАКОННО, пока привыкает (и темп её — от
        // часов, а не от номера кадра). Здесь ищется накопление по ошибке,
        // поэтому каждый кадр считается первым: экспозиция встаёт сразу.
        m_renderer.ResetPostHistory();
        LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
        m_renderer.PrepareReflections(*m_scene, env);
        m_renderer.RenderShadow(*m_scene, env, m_camera);
        glm::mat4 view(1.0f), proj(1.0f);
        m_renderer.RenderViewport(*m_scene, m_camera, env, m_selection.Primary(), m_selection.All(),
                                  m_tools.RenderMode, m_tools.ShowGrid, cfg, view, proj, 0);
        m_renderer.RenderGame(*m_scene, env, cfg);

        std::vector<unsigned char> frame;
        int fw = 0, fh = 0;
        if (!m_renderer.ReadViewportPixels(frame, fw, fh) || frame.empty()) {
            LOG_ERROR("Editor") << "SELFTEST: не удалось прочитать кадр вьюпорта";
            ok = false;
            break;
        }
        if (i == 1) {
            first = frame;
            w = fw;
            h = fh;
            baseline = sage::rhi::ResourceLedger::Snapshot();
            haveBaseline = true;
        } else if (i > 1) {
            if (fw != w || fh != h || frame.size() != first.size()) {
                LOG_ERROR("Editor") << "SELFTEST: размер кадра вьюпорта изменился сам собой";
                ok = false;
                break;
            }
            long long sum = 0;
            for (size_t k = 0; k < frame.size(); ++k) {
                const int d = std::abs((int)frame[k] - (int)first[k]);
                sum += d;
                if (d > worstDiff) worstDiff = d;
            }
            worstMean = std::max(worstMean, (double)sum / (double)frame.size());
        }
    }

    if (ok && haveBaseline) {
        growth = sage::rhi::ResourceLedger::Snapshot().DiffFrom(baseline);
    }

    // Допуск в один уровень из 255 — это последний бит арифметики шейдера, а не
    // «похоже». Накопление даёт десятки уровней и проценты средней разницы.
    if (ok && worstDiff > 1) {
        LOG_ERROR("Editor") << "SELFTEST: кадр вьюпорта уплывает от повторов: худший канал "
                            << worstDiff << ", среднее " << worstMean
                            << " — рендер копит изменения между кадрами";
        ok = false;
    }
    if (ok && !growth.empty()) {
        LOG_ERROR("Editor") << "SELFTEST: за " << kFrames
                            << " кадров прибавилось GPU-объектов: " << growth
                            << " — рендер течёт покадрово";
        ok = false;
    }

    // --- ПРЕВЬЮ ВЫБРАННОЙ КАМЕРЫ: кадр правда рисуется ---------------------
    //
    // Проверяется весь путь целиком — расчёт кадра камеры, проход сцены, отдача
    // текстуры, — потому что ломается он молча: карточка в углу остаётся чёрной
    // или показывает прошлую камеру, а ни сборка, ни остальные тесты этого не
    // замечают.
    if (ok) {
        GameObject cam = m_scene->CreateObject("SelftestPreviewCam");
        cam.GetTransform().Position = {0.0f, 2.0f, 6.0f};
        m_scene->Registry().emplace<CameraComponent>(cam.Entity());

        LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
        m_renderer.SetCameraPreviewSize(160, 90);
        m_renderer.RenderCameraPreview(*m_scene, env, cfg, cam.Entity());
        if (m_renderer.CameraPreviewTexture() == 0) {
            LOG_ERROR("Editor") << "SELFTEST: превью выбранной камеры не дало кадра";
            ok = false;
        }

        // У сущности БЕЗ камеры превью обязано исчезнуть, а не остаться от
        // прошлого показа: старый кадр в карточке — это ложь о том, куда
        // смотрит выбранный объект.
        if (ok) {
            GameObject cube = m_scene->CreateObject("SelftestPreviewCube");
            m_renderer.RenderCameraPreview(*m_scene, env, cfg, cube.Entity());
            if (m_renderer.CameraPreviewTexture() != 0) {
                LOG_ERROR("Editor") << "SELFTEST: превью осталось от прошлой камеры";
                ok = false;
            }
            m_scene->RemoveObject(cube.Id());
        }
        m_scene->RemoveObject(cam.Id());
    }

    // --- ОБЛОЖКА СЦЕНЫ: сохранили — появился снимок ------------------------
    //
    // Проверяется весь путь: сохранение заказывает обложку, конец кадра её
    // снимает, файл ложится в .sage/covers и повторяется обложкой проекта для
    // стартового окна. Ломается это молча — сцена сохранена, всё работает,
    // просто в списке проектов по-прежнему рисунок из имени, — и заметить
    // это можно только открыв стартовое окно и вспомнив, как было.
    if (ok && m_project.Loaded()) {
        const fs::path scenePath = m_project.ScenesDir() / "selftest_cover.sage";
        if (!SaveSceneToFile(scenePath)) {
            LOG_ERROR("Editor") << "SELFTEST: сцена под обложку не сохранилась";
            ok = false;
        } else {
            // Кадр — тот же, что рисует редактор: обложка снимается из
            // готового буфера, а до первого прохода буфера нет.
            LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
            glm::mat4 v(1.0f), p(1.0f);
            m_renderer.RenderViewport(*m_scene, m_camera, env, m_selection.Primary(), m_selection.All(),
                                      m_tools.RenderMode, m_tools.ShowGrid, cfg, v, p, 0);
            m_renderer.RenderGame(*m_scene, env, cfg);
            TakeSceneShot();

            std::error_code cec;
            const fs::path cover = scenecover::For(m_project.Dir(), scenePath);
            const fs::path shot = scenecover::ProjectShot(m_project.Dir());
            if (!fs::exists(cover, cec) || fs::file_size(cover, cec) < 1024) {
                LOG_ERROR("Editor") << "SELFTEST: обложка сцены не снялась: " << cover.string();
                ok = false;
            }
            if (!fs::exists(shot, cec)) {
                LOG_ERROR("Editor") << "SELFTEST: обложка проекта для стартового окна не появилась";
                ok = false;
            }
            std::error_code rmec;
            fs::remove(scenePath, rmec);
        }
    }

    // --- АНИМАЦИЯ ПО СВОЙСТВАМ: ОТ КЛЮЧА ДО ДВИЖЕНИЯ В СЦЕНЕ -------------
    //
    // Счёт кривых проверяет модульный тест (tests/test_propertyclip.cpp), а
    // здесь — ВОРОТА: свойство находится по ключу в реестре, значение читается
    // с настоящего объекта, дорожка ложится в файл, компонент поднимает файл и
    // двигает объект. Каждый шаг по отдельности работает и на разорванном
    // пути — ломается именно стык, и увидеть это можно было бы только глазами
    // на шевелящемся кубике.
    if (ok) {
        GameObject mover = m_scene->CreateObject("SelftestAnimTarget");
        mover.GetTransform().Position = {0.0f, 0.0f, 0.0f};

        // Свойство берётся ИЗ РЕЕСТРА по ключу — ровно так, как его возьмёт
        // собранная игра. Своего списка свойств у редактора нет.
        const sage::anim::PropertyType* prop = sage::anim::FindProperty("object.position");
        if (!prop) {
            LOG_ERROR("Editor") << "SELFTEST: свойства object.position нет в реестре";
            ok = false;
        } else {
            sage::anim::PropertyClip clip;
            clip.Name = "Selftest";
            clip.Duration = 2.0f;
            clip.Loop = false;
            sage::anim::Track track;
            track.Property = "object.position";
            sage::anim::SetKey(track, 0.0f, glm::vec4(0.0f));
            sage::anim::SetKey(track, 2.0f, glm::vec4(10.0f, 0.0f, 0.0f, 0.0f));
            clip.Tracks.push_back(track);

            // Применение в середину: значение обязано быть ровно посередине —
            // это и есть проверка того, что дорожка доехала до компонента,
            // а не осталась числом в файле.
            sage::anim::ApplyClipAt(*m_scene, mover.Entity(), clip, 1.0f);
            const float mid = mover.GetTransform().Position.x;
            if (std::fabs(mid - 5.0f) > 0.01f) {
                LOG_ERROR("Editor") << "SELFTEST: клип не сдвинул объект (x=" << mid
                                    << " вместо 5)";
                ok = false;
            }

            // Файл: клип переживает запись и чтение целиком — иначе «сохранил
            // и открыл» теряет половину работы молча.
            const fs::path clipPath = m_project.AssetsDir() / "selftest_move.sageclip";
            std::string cerr;
            std::error_code cec2;
            fs::create_directories(clipPath.parent_path(), cec2);
            if (!sage::anim::SaveClipFile(clip, clipPath.string(), cerr)) {
                LOG_ERROR("Editor") << "SELFTEST: клип не сохранился: " << cerr;
                ok = false;
            } else {
                sage::anim::PropertyClip back;
                if (!sage::anim::LoadClipFile(clipPath.string(), back, cerr)) {
                    LOG_ERROR("Editor") << "SELFTEST: клип не прочитался: " << cerr;
                    ok = false;
                } else if (back.Tracks.size() != 1 || back.Tracks[0].Keys.size() != 2 ||
                           back.Tracks[0].Property != "object.position") {
                    LOG_ERROR("Editor") << "SELFTEST: клип вернулся не тем, чем был записан";
                    ok = false;
                }

                // И КОМПОНЕНТ ЕГО ИГРАЕТ. Это последний стык: путь в сцене,
                // загрузка файла системой, продвижение времени, запись в
                // свойство. Ровно он и рвётся, когда компонент забывают
                // поставить в расписание систем.
                mover.GetTransform().Position = {0.0f, 0.0f, 0.0f};
                PropertyAnimatorComponent& pa =
                    m_scene->Registry().emplace<PropertyAnimatorComponent>(mover.Entity());
                pa.ClipPath = clipPath.string();
                pa.Loop = false;
                sage::anim::UpdatePropertyAnimators(*m_scene, 1.0f);
                const float played = mover.GetTransform().Position.x;
                if (std::fabs(played - 5.0f) > 0.01f) {
                    LOG_ERROR("Editor") << "SELFTEST: компонент не проиграл клип (x=" << played
                                        << " вместо 5)";
                    ok = false;
                }
                // Компонент снимаем: следующая проверка требует, чтобы клип
                // был НИЧЕЙ, пока его не привязали.
                m_scene->Registry().remove<PropertyAnimatorComponent>(mover.Entity());
            }
            // --- КЛИП НЕ БЕРЁТСЯ ИЗ ВОЗДУХА -------------------------------
            //
            // Инструмент анимации правил безымянный клип, живший только в
            // памяти панели: человек выбирал объект, ставил дорожку и двигал
            // его, но в проекте не появлялось НИ ФАЙЛА, НИ КОМПОНЕНТА, а
            // владельцем молча считался тот, кто выбран прямо сейчас. Здесь
            // проверяются оба конца правила: пока панель не открыла клип, она
            // пуста, а открытый клип принадлежит тому объекту, у которого в
            // компоненте записан ЭТОТ путь, — и никому другому.
            if (m_animation.HasClipOpen()) {
                LOG_ERROR("Editor") << "SELFTEST: панель анимации завела клип сама";
                ok = false;
            }
            m_animation.OpenClip(*this, clipPath.string());
            if (!m_animation.HasClipOpen()) {
                LOG_ERROR("Editor") << "SELFTEST: клип не открылся в панели анимации";
                ok = false;
            }
            // Компонента с этим путём в сцене нет — значит и владельца нет,
            // даже при выбранном объекте. Именно это раньше и «анимировалось».
            m_selection.SetPrimary(mover.Id());
            if (m_animation.ClipOwner(*this) != entt::null) {
                LOG_ERROR("Editor") << "SELFTEST: клип без компонента нашёл владельца";
                ok = false;
            }
            // Ставим компонент — владелец обязан найтись, и именно тот.
            PropertyAnimatorComponent& bound =
                m_scene->Registry().get_or_emplace<PropertyAnimatorComponent>(mover.Entity());
            bound.ClipPath = clipPath.string();
            bound.Playing = false;
            if (m_animation.ClipOwner(*this) != mover.Entity()) {
                LOG_ERROR("Editor") << "SELFTEST: клип не нашёл объект, который его играет";
                ok = false;
            }
            m_scene->Registry().remove<PropertyAnimatorComponent>(mover.Entity());

            std::error_code rmec2;
            fs::remove(clipPath, rmec2);

            // ИМПОРТИРОВАННЫЙ КЛИП — ТОЛЬКО ДЛЯ ЧТЕНИЯ, и признак этот обязан
            // пережить файл: иначе после перезапуска редактора он станет
            // обычным, и правку в нём съест следующий переимпорт модели.
            sage::anim::PropertyClip imported = clip;
            imported.Imported = true;
            sage::anim::PropertyClip importedBack;
            std::string ierr;
            if (!sage::anim::FromJsonString(sage::anim::ToJsonString(imported), importedBack, ierr) ||
                !importedBack.Imported) {
                LOG_ERROR("Editor") << "SELFTEST: признак «из модели» потерялся в файле клипа";
                ok = false;
            }
        }
        m_scene->RemoveObject(mover.Id());
    }

    // --- РЕДАКТОР НЕСЁТ СВОЙ ШРИФТ -----------------------------------------
    //
    // Редактор рисует интерфейс ИГРЫ тем же UIRenderer, что и сама игра, а тот
    // без своего шрифта уходит к системным. На Linux находился DejaVu и всё
    // выглядело в порядке; на Windows очередь доходила до C:/Windows/Fonts, и
    // запекание атласа из чужого файла убивало редактор при запуске —
    // исключением 0xc0000005 внутри растеризатора stb.
    //
    // Рядом с любой игрой шрифт кладёт sage_add_game; у редактора своя
    // add_executable, и его там не оказывалось. Проверяем ФАЙЛ НА ДИСКЕ, а не
    // список кандидатов: «шрифт загрузился» на Linux сходится и без него.
    {
        const std::string fontPath = sage::EngineAssetPath("assets/fonts/sage-default.ttf");
        std::error_code fec;
        if (!fs::exists(fs::path(fontPath), fec)) {
            LOG_ERROR("Editor") << "SELFTEST: шрифта движка нет рядом с редактором ("
                                << fontPath << ") — интерфейс игры уйдёт к системным шрифтам";
            ok = false;
        }
    }

    // --- ПАПКА НАСТРОЕК — ОДНА НА ВСЕХ --------------------------------------
    //
    // Список проектов, язык и привычки редактора лежат в одном каталоге, и
    // собирать его каждый обязан ОДИНАКОВО. Пока каждый собирал его сам и
    // сплющивал в UTF-8-строку, на Windows с кириллицей в имени учётной записи
    // обратный ход давал другой путь: список проектов каждый раз открывался
    // пустым, а только что созданный проект в нём не появлялся.
    {
        const fs::path cfg = sage::ConfigDir();
        if (cfg.empty() || cfg.filename() != "sage") {
            LOG_ERROR("Editor") << "SELFTEST: папка настроек собрана не так: "
                                << sage::PathToUtf8(cfg);
            ok = false;
        }
        if (sage::editor::prefs::Path().parent_path() != cfg ||
            Sage::Launcher::ProjectDatabase::StorageFile().parent_path() != cfg) {
            LOG_ERROR("Editor") << "SELFTEST: настройки и список проектов лежат в разных папках";
            ok = false;
        }
    }

    // --- ТИПЫ ЭЛЕМЕНТОВ ПЕРЕВЕДЕНЫ ------------------------------------------
    //
    // Имена типов («Panel», «Vertical List»), разделы меню создания и пояснения
    // живут в ДВИЖКЕ, а показывает их редактор. Пока они не были помечены
    // SAGE_UI_TEXT, проверка переводов их не видела вовсе: список создания и
    // инспектор стояли по-английски посреди русского редактора, и сказать об
    // этом было некому.
    //
    // Спрашиваем сам каталог: перевод есть, если T() отдаёт НЕ ту же строку.
    if (ok && sage::editor::CurrentLanguageCode() != "en") {
        for (const sage::ui::Preset& p : sage::ui::Presets()) {
            if (std::strcmp(T(p.Name.c_str()), p.Name.c_str()) == 0) {
                LOG_ERROR("Editor") << "SELFTEST: тип элемента без перевода: " << p.Name;
                ok = false;
            }
            if (std::strcmp(T(p.Category.c_str()), p.Category.c_str()) == 0) {
                LOG_ERROR("Editor") << "SELFTEST: раздел меню без перевода: " << p.Category;
                ok = false;
            }
        }
    }

    // --- «ИГРАТЬ» В РЕЖИМЕ ИНТЕРФЕЙСА ПОКАЗЫВАЕТ ИГРУ -----------------------
    //
    // Панель Game живёт в рабочем месте сцены. Пока запуск выводил вперёд
    // только её, «Играть» из режима интерфейса не показывал ВООБЩЕ НИЧЕГО:
    // человек оставался на холсте с гизмо, а игра шла в окне, которого на
    // экране нет. Проверять глазами это нельзя — окно просто не появляется.
    if (ok) {
        const EditorWorkspace before = m_workspace;
        m_workspace = EditorWorkspace::Interface;
        m_panels[EditorPanel::InterfacePreview] = false;
        FocusPlayTarget();
        if (!m_panels[EditorPanel::InterfacePreview]) {
            LOG_ERROR("Editor") << "SELFTEST: запуск в режиме интерфейса не открыл предпросмотр";
            ok = false;
        }
        m_workspace = before;
    }

    // --- ГИЗМО И СЕТКА — ДЕБАГ, А НЕ ЧАСТЬ КАРТИНКИ -------------------------
    //
    // Сетка шла вместе со сценой, то есть ДО пост-обработки, и тон-маппинг с
    // экспозицией красили её в тот цвет, который сейчас у кадра: на тёмной
    // сцене она тонула, на яркой белела, а глубина резкости размывала её у
    // горизонта. Опорная сетка, меняющая вид от настроек красоты, перестаёт
    // быть опорной.
    //
    // МЕРЯЕМ САМУ СЕТКУ, А НЕ ЯРКОСТЬ КАДРА. Кадр рисуется дважды — с сеткой и
    // без неё, — и разница между ними и есть её вклад. Иначе в замер попадают
    // оси и значки, которые и так рисуются после поста: они остались бы
    // яркими, и проверка прошла бы при полностью потухшей сетке.
    //
    // Сравниваются два таких вклада: при выкрученной почти в ноль экспозиции и
    // без пост-обработки вовсе. Сетка, нарисованная после поста, вносит
    // одинаково; нарисованная до — гаснет вместе со сценой.
    if (ok) {
        auto gridEnergy = [this](const sage::EngineConfig& cfg) -> long long {
            auto frame = [&](bool grid, std::vector<unsigned char>& out, int& w, int& h) {
                glm::mat4 v(1.0f), p(1.0f);
                m_renderer.RenderViewport(*m_scene, m_camera, m_scene->Lighting, -1, {},
                                          EditorRenderMode::Shaded, grid, cfg, v, p);
                return m_renderer.ReadViewportPixels(out, w, h) && !out.empty();
            };
            std::vector<unsigned char> withGrid, without;
            int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
            if (!frame(true, withGrid, w1, h1) || !frame(false, without, w2, h2)) return -1;
            if (withGrid.size() != without.size()) return -1;
            // МАКСИМУМ ПО ПИКСЕЛЯМ САМОЙ СЕТКИ, а не сумма разностей: сумма
            // зависит ещё и от того, по какому фону сетка смешалась, а вопрос
            // здесь ровно один — доезжает ли до экрана СОБСТВЕННЫЙ цвет её
            // линий. Линия, нарисованная после поста, остаётся своего цвета
            // при любой экспозиции; нарисованная до — гаснет вместе с кадром.
            long long best = 0;
            for (size_t i = 0; i + 2 < withGrid.size(); i += 4) {
                const int a = (int)withGrid[i] + (int)withGrid[i + 1] + (int)withGrid[i + 2];
                const int b = (int)without[i] + (int)without[i + 1] + (int)without[i + 2];
                if (a > b) best = std::max(best, (long long)a);
            }
            return best;
        };

        // ОБРАБОТКА — У КАМЕРЫ, а не в настройках. Заводим камеру с
        // «Пост-обработкой» и сажаем экспозицию в пол: кадр сцены становится
        // почти чёрным, и если сетка ушла под пост, она чернеет вместе с ним.
        GameObject postCam = m_scene->CreateEmptyObject("SelftestPostCamera");
        m_scene->Registry().emplace<CameraComponent>(postCam.Entity());
        sage::render::PostProcessComponent ppc;
        ppc.Chain = sage::render::PostChain::Default();
        for (sage::render::PostEffect& e : ppc.Chain.Effects)
            if (e.Kind == "exposure")
                if (sage::render::PostValue* v = e.Find("exposure")) v->V[0] = -6.0f;
        sage::render::PostProcessComponent& live =
            m_scene->Registry().emplace<sage::render::PostProcessComponent>(postCam.Entity(), ppc);

        const long long darkGrid = gridEnergy(m_settings);
        live.Enabled = false;   // та же сцена, но кадр без обработки вовсе
        const long long plainGrid = gridEnergy(m_settings);
        m_scene->RemoveObject(postCam.Id());
        if (darkGrid < 0 || plainGrid < 0) {
            LOG_ERROR("Editor") << "SELFTEST: кадр вьюпорта не прочитался";
            ok = false;
        } else if (plainGrid <= 0) {
            LOG_ERROR("Editor") << "SELFTEST: сетки не видно даже без пост-обработки";
            ok = false;
        } else if (darkGrid * 2 < plainGrid) {
            // Половина — щедрый запас: линия рисуется поверх готового кадра и
            // своего цвета от экспозиции не меняет. Сетка, ушедшая под пост,
            // при экспозиции 0.02 гаснет в десятки раз.
            LOG_ERROR("Editor") << "SELFTEST: сетка гаснет вместе с пост-обработкой (вклад "
                                << darkGrid << " против " << plainGrid << ")";
            ok = false;
        }
    }

    // --- ПОДЛОЖКА РЕДАКТОРА ИНТЕРФЕЙСА: ПОД МЕНЮ, А НЕ ПОВЕРХ НЕГО --------
    //
    // Подложка приглушает сцену за интерфейсом. Пока её рисовала панель —
    // полупрозрачным прямоугольником поверх готового кадра, — она ложилась и
    // на сам интерфейс: движок рисует его последним проходом ВНУТРИ кадра.
    // То есть «приглушить фон» приглушало всё, и на единице экран становился
    // ровной заливкой без единого элемента.
    //
    // Проверяется это ТОЛЬКО ПО ПИКСЕЛЯМ: порядок проходов не виден ни в
    // состоянии, ни в логе — видно лишь то, что получилось на картинке.
    if (ok) {
        // Белый элемент во весь левый верхний угол и чёрная подложка: «под» и
        // «поверх» тогда различаются без порогов и подбора.
        GameObject e = m_scene->CreateObject("SelftestBackdrop");
        sage::ui::Element el;
        el.Anchor = UIAnchor::TopLeft;
        el.Position = {0.0f, 0.0f};
        el.Size = {(float)m_renderer.GameWidth() * 0.5f, (float)m_renderer.GameHeight() * 0.5f};
        m_scene->Registry().emplace<sage::ui::Element>(e.Entity(), el);
        sage::ui::Fill fill;
        fill.Color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);   // непрозрачный белый
        fill.Rounding = 0.0f;                              // угол мерим по пикселям
        fill.Gradient = glm::vec4(0.0f);
        m_scene->Registry().emplace<sage::ui::Fill>(e.Entity(), fill);

        m_renderer.SetUIBackdrop(1.0f, glm::vec3(0.0f, 0.0f, 0.0f));
        LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
        m_renderer.RenderGame(*m_scene, env, cfg);

        std::vector<unsigned char> px;
        int fw = 0, fh = 0;
        if (!m_renderer.ReadGamePixels(px, fw, fh) || px.empty()) {
            LOG_ERROR("Editor") << "SELFTEST: игровой кадр с подложкой не прочитался";
            ok = false;
        } else {
            // Средняя яркость двух точек: внутри элемента и заведомо снаружи.
            // Строки RHI отдаёт снизу вверх, поэтому «верхний левый угол» —
            // это верх кадра в координатах UI и низ в координатах чтения.
            auto luma = [&](int x, int y) {
                x = std::clamp(x, 0, fw - 1);
                y = std::clamp(y, 0, fh - 1);
                const size_t k = ((size_t)y * (size_t)fw + (size_t)x) * 3u;
                return (int)((px[k] + px[k + 1] + px[k + 2]) / 3);
            };
            const int inside = luma(fw / 4, fh - fh / 4);
            const int outside = luma(fw - fw / 8, fh / 8);
            if (outside > 24) {
                LOG_ERROR("Editor") << "SELFTEST: подложка не закрыла сцену (яркость вне "
                                    << "элемента " << outside << ") — она не попала в кадр";
                ok = false;
            }
            if (inside < 96) {
                LOG_ERROR("Editor") << "SELFTEST: подложка накрыла сам интерфейс (яркость "
                                    << "элемента " << inside << ") — она легла поверх кадра, "
                                    << "а не под интерфейс";
                ok = false;
            }
        }
        m_renderer.SetUIBackdrop(0.0f, glm::vec3(0.0f));
        m_scene->RemoveObject(e.Id());
    }

    m_scene->RemoveObject(fx.Id());
    return ok;
}
