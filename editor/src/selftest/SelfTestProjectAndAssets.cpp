#include "SelfTestCommon.h"

// --- проект, ассеты и материалы --------------------------------------------
//
// Первая половина того, что редактор обязан уметь до всякой сцены: создать
// пустой проект, сохранить и загрузить, переименовать
// файл вместе с его сайдкаром, назначить материал. Ошибка здесь означает, что
// работать в редакторе нельзя вообще, поэтому этот блок идёт первым.
bool EditorLayer::SelfTestProjectAndAssets() {
    bool ok = true;
    std::error_code ec;
    std::string err;
    (void)ec; (void)err;

    // --- Новый проект: пустой проект и сцена-фикстура ----------------------
    //
    // Проверяется не «функция вернула true», а содержимое стартовой сцены:
    // новый проект обязан быть ПУСТЫМ (одно солнце), а фикстура самопроверки —
    // принести объекты с материалами-файлами и свет без компонента «Меш».
    if (ok) {
        // Painted — объект, у которого проверяется МАТЕРИАЛ.
        struct ProjectCheck { const char* Id; bool Empty; const char* Expect; const char* Painted; };
        const ProjectCheck checks[] = {
            {"empty", true, nullptr, nullptr},
            {"fixture", false, "Green Cube", "Green Cube"},
        };
        for (const ProjectCheck& c : checks) {
            fs::remove_all(std::string("selftest_tpl_") + c.Id, ec);
            std::string tplErr;
            const std::string dir = std::string("selftest_tpl_") + c.Id;
            const bool made = c.Empty ? CreateProject(".", dir, tplErr)
                                      : CreateFixtureProject(".", dir, tplErr);
            if (!made) {
                LOG_ERROR("Editor") << "SELFTEST: проект " << c.Id << " не создался: " << tplErr;
                ok = false;
                break;
            }
            if (c.Empty) {
                // ПУСТОЙ ЗНАЧИТ ПУСТОЙ. Проверяются три разных «ничего», потому
                // что спрятаться содержимое может в каждом по отдельности:
                //   • сущности — объекты сцены;
                //   • элементы интерфейса — они живут компонентами НА сущностях,
                //     то есть считаются вместе с ними, и отдельная проверка
                //     нужна как обещание: интерфейс здесь тоже не появится;
                //   • настройки сцены — они НЕ сущности, и включённый скайбокс
                //     дал бы пустому проекту небо при нулевом счётчике объектов.
                // ПУСТОЙ — ЭТО ПУСТОЙ ПЛЮС СОЛНЦЕ, и это не оговорка. Свет в
                // сцене дают только объекты (см. LightingEnvironment::Sun):
                // движок больше не подсвечивает сцену сам собой, и пустой
                // проект без объекта-солнца открывался бы чёрным вьюпортом.
                // Солнце — обычный объект: видно в иерархии, можно удалить.
                if (m_scene->Count() != 1) {
                    LOG_ERROR("Editor") << "SELFTEST: пустой проект принёс " << m_scene->Count()
                                        << " сущностей (ожидалось одно солнце)";
                    ok = false;
                }
                if (!sage::ecs::SceneHasAnyLight(*m_scene)) {
                    LOG_ERROR("Editor") << "SELFTEST: пустой проект приехал без света вовсе";
                    ok = false;
                }
                const auto uiView = m_scene->Registry().view<sage::ui::Element>();
                if (uiView.begin() != uiView.end()) {
                    LOG_ERROR("Editor") << "SELFTEST: пустой проект принёс элементы интерфейса";
                    ok = false;
                }
                if (m_scene->Lighting.Skybox.Enabled) {
                    LOG_ERROR("Editor") << "SELFTEST: пустой проект включил скайбокс";
                    ok = false;
                }
            }
            if (c.Expect && !m_scene->FindByName(c.Expect).Valid()) {
                LOG_ERROR("Editor") << "SELFTEST: проект " << c.Id << " не принёс " << c.Expect;
                ok = false;
            }

            // У СВЕТА, КАМЕРЫ И ЭЛЕМЕНТА ИНТЕРФЕЙСА НЕТ КОМПОНЕНТА «МЕШ».
            //
            // Scene::CreateObject вешает MeshRenderer всем подряд; свет и камера
            // обязаны заводиться пустыми объектами, иначе у лампы в инспекторе
            // секция «Меш» с моделью и тенями, которых у света нет.
            {
                entt::registry& reg = m_scene->Registry();
                auto complain = [&](entt::entity e, const char* what) {
                    if (!reg.all_of<MeshRendererComponent>(e)) return;
                    const auto* nm = reg.try_get<NameComponent>(e);
                    LOG_ERROR("Editor") << "SELFTEST: у объекта " << (nm ? nm->Name : "?")
                                        << " проекта " << c.Id << " висит компонент «Меш», хотя это "
                                        << what;
                    ok = false;
                };
                for (auto e : reg.view<LightComponent>()) complain(e, "свет");
                for (auto e : reg.view<CameraComponent>()) complain(e, "камера");
                for (auto e : reg.view<sage::ui::Element>()) complain(e, "элемент интерфейса");
            }

            // ФАЙЛ СЦЕНЫ НА ДИСКЕ — у КАЖДОГО нового проекта, пустого в том
            // числе: без него в scenes/ пусто, Ctrl+S спрашивает куда, а
            // собранная игра не находит, с чего начать.
            const fs::path sceneFile = fs::path(std::string("selftest_tpl_") + c.Id) / "assets" / "Scenes" /
                                  "main.sage";
            if (!fs::exists(sceneFile, ec)) {
                LOG_ERROR("Editor") << "SELFTEST: проект " << c.Id << " не создал файл сцены "
                                    << sceneFile.string();
                ok = false;
            } else if (m_scenePath.filename() != "main.sage") {
                // И редактор обязан СЧИТАТЬ её открытой: иначе первое же
                // Ctrl+S спросит путь, будто сцена ничья.
                LOG_ERROR("Editor") << "SELFTEST: проект " << c.Id
                                    << " оставил редактор без пути сцены";
                ok = false;
            }

            // ЦВЕТ — ИЗ МАТЕРИАЛА-ФАЙЛА, А НЕ ИЗ ПОЛЯ ОБЪЕКТА.
            if (c.Painted) {
                GameObject shown = m_scene->FindByName(c.Painted);
                if (!shown.Valid() || !shown.HasRenderer()) {
                    LOG_ERROR("Editor") << "SELFTEST: проект " << c.Id << " не принёс "
                                        << c.Painted;
                    ok = false;
                } else if (shown.Renderer().MaterialPath.empty()) {
                    LOG_ERROR("Editor") << "SELFTEST: у объекта " << c.Painted << " проекта "
                                        << c.Id << " нет материала";
                    ok = false;
                } else if (!fs::exists(fs::path(std::string("selftest_tpl_") + c.Id) /
                                           shown.Renderer().MaterialPath, ec)) {
                    LOG_ERROR("Editor") << "SELFTEST: материал " << shown.Renderer().MaterialPath
                                        << " проекта " << c.Id << " не записан на диск";
                    ok = false;
                }
            }

            fs::remove_all(std::string("selftest_tpl_") + c.Id, ec);
            if (!ok) break;
        }
        // Вернуться в проект самопроверки: дальше идут его шаги.
        if (ok && !OpenProject("selftest_project", err)) {
            LOG_ERROR("Editor") << "SELFTEST: не удалось вернуться в проект: " << err;
            ok = false;
        }
    }

    // --- СМЕНА ПРОЕКТА НЕ ОСТАВЛЯЕТ ПРИЗРАКОВ -----------------------------
    //
    // «Создал частицу в одном проекте, перешёл в другой — и на секунду видно те
    // объекты и ту работу». Держалось прошлое в том, что смена проекта меняла
    // ПУТЬ и не трогала ничего из загруженного: кэш ресурсов (ключ — ссылка
    // проекта, а она в каждом проекте своя и ведёт к другому файлу), живые
    // частицы, обложки и сама сцена.
    //
    // Проверяется СОСТОЯНИЕ ПОСЛЕ ПЕРЕХОДА, а не факт вызова очистки: частицы
    // пускаются по-настоящему, модель по-настоящему кладётся в кэш, и после
    // открытия другого проекта ни того, ни другого остаться не должно.
    if (ok) {
        ParticleSystem& particles = m_renderer.Particles();
        sage::fx::ParticleEffect cfg;
        cfg.StartLifetime = {30.0f, 30.0f};   // заведомо переживёт переход
        particles.Burst(cfg, glm::vec3(0.0f), 64);
        particles.CreateStream("selftest_ghost", cfg, glm::vec3(0.0f));
        particles.SetStreamActive("selftest_ghost", true);
        // Сцена с объектом: его тоже не должно остаться.
        m_scene->CreateObject("ПризракПрошлогоПроекта");

        const size_t aliveBefore = particles.AliveCount();
        const size_t streamsBefore = particles.StreamCount();
        if (aliveBefore == 0 || streamsBefore == 0) {
            LOG_ERROR("Editor") << "SELFTEST: частицы для проверки призраков не завелись";
            ok = false;
        }

        std::string ghostErr;
        if (ok && !CreateProject(".", "selftest_ghost_project", ghostErr)) {
            LOG_ERROR("Editor") << "SELFTEST: не создать проект для проверки призраков: "
                                << ghostErr;
            ok = false;
        }
        if (ok) {
            if (particles.AliveCount() != 0) {
                LOG_ERROR("Editor") << "SELFTEST: после смены проекта осталось живых частиц: "
                                    << particles.AliveCount();
                ok = false;
            }
            if (particles.StreamCount() != 0) {
                LOG_ERROR("Editor") << "SELFTEST: после смены проекта остались эмиттеры: "
                                    << particles.StreamCount();
                ok = false;
            }
            // Объект прошлой сцены. Пустой проект не приносит своих, поэтому
            // любой найденный здесь — чужой.
            bool ghostObject = false;
            m_scene->Registry().view<NameComponent>().each(
                [&](auto, const NameComponent& n) {
                    if (n.Name == "ПризракПрошлогоПроекта") ghostObject = true;
                });
            if (ghostObject) {
                LOG_ERROR("Editor") << "SELFTEST: в новом проекте остался объект прошлой сцены";
                ok = false;
            }
            if (m_selection.Primary() != -1) {
                LOG_ERROR("Editor") << "SELFTEST: после смены проекта осталось выделение";
                ok = false;
            }
        }
        fs::remove_all("selftest_ghost_project", ec);
        if (ok && !OpenProject("selftest_project", err)) {
            LOG_ERROR("Editor") << "SELFTEST: не вернуться в проект самопроверки: " << err;
            ok = false;
        }
    }

    // --- База проектов стартового окна ------------------------------------
    //
    // Проверяется то, ради чего она и заведена: проект попадает в список сам,
    // список читается с диска, поиск и отбор работают по памяти, отсутствующая
    // папка ПОМЕЧАЕТСЯ, а не вычёркивается, «убрать из списка» и «удалить с
    // диска» — разные действия. Каждый из этих пунктов раньше проверялся
    // только глазами: список недавних умел ровно «добавить строку».
    if (ok) {
        using namespace Sage::Launcher;
        const std::string mine = m_project.Dir().string();

        ProjectDatabase db;
        db.Load();
        if (db.IndexOf(mine) < 0) {
            LOG_ERROR("Editor") << "SELFTEST: проект не попал в базу стартового окна";
            ok = false;
        }
        // Порядок «недавние сверху»: только что созданный проект — первый.
        if (ok) {
            const std::vector<int> shown = db.Query(ProjectFilter::All, "", ProjectSort::Recent);
            if (shown.empty() || db.All()[(size_t)shown.front()].Path != db.All()[(size_t)db.IndexOf(mine)].Path) {
                LOG_ERROR("Editor") << "SELFTEST: недавний проект не первый в списке";
                ok = false;
            }
        }
        // Поиск идёт по имени и пути — и ничего не находит по бессмыслице.
        if (ok) {
            const std::string name = db.All()[(size_t)db.IndexOf(mine)].Name;
            if (db.Query(ProjectFilter::All, name, ProjectSort::Recent).empty()) {
                LOG_ERROR("Editor") << "SELFTEST: поиск не нашёл проект по имени";
                ok = false;
            }
            if (!db.Query(ProjectFilter::All, "щщщнетакого", ProjectSort::Recent).empty()) {
                LOG_ERROR("Editor") << "SELFTEST: поиск нашёл несуществующее";
                ok = false;
            }
        }
        // Тип проекта живёт в дескрипторе и меняет вкладку отбора.
        if (ok) {
            std::string metaErr;
            if (!ProjectDatabase::WriteMetadata(m_project.Dir(), ProjectKind::Scene,
                                                "проверка", metaErr)) {
                LOG_ERROR("Editor") << "SELFTEST: тип проекта не записался: " << metaErr;
                ok = false;
            } else {
                db.Refresh(mine);
                const ProjectEntry* e = db.Find(mine);
                const int mineIdx = db.IndexOf(mine);
                if (!e || e->Kind != ProjectKind::Scene || e->Description != "проверка") {
                    LOG_ERROR("Editor") << "SELFTEST: тип/описание проекта не перечитались";
                    ok = false;
                } else {
                    // Ищем ИМЕННО СВОЮ запись по индексу, а не по «пусто ли
                    // вообще» — база стартового окна общая на машину, и в ней
                    // могут лежать другие проекты с тем же именем (например,
                    // от прошлых прогонов самопроверки из другой папки сборки).
                    // Проверка по пустоте результата ловила ИХ тип, а не тип
                    // своего проекта, и падала не из-за отбора, а из-за чужих
                    // записей в общей базе.
                    const std::vector<int> games = db.Query(ProjectFilter::Games, e->Name, ProjectSort::Name);
                    const std::vector<int> scenes = db.Query(ProjectFilter::Scenes, e->Name, ProjectSort::Name);
                    const bool inGames = std::find(games.begin(), games.end(), mineIdx) != games.end();
                    const bool inScenes = std::find(scenes.begin(), scenes.end(), mineIdx) != scenes.end();
                    if (inGames || !inScenes) {
                        LOG_ERROR("Editor") << "SELFTEST: отбор по типу не увидел смену типа";
                        ok = false;
                    }
                }
                // Вернуть тип обратно: дальше этим проектом пользуются другие шаги.
                ProjectDatabase::WriteMetadata(m_project.Dir(), ProjectKind::Game, "", metaErr);
                db.Refresh(mine);
            }
        }
        // Импорт: не проект — понятный отказ; тот же проект второй раз — не
        // дубликат в списке.
        if (ok) {
            const fs::path notProject = fs::path("selftest_not_a_project");
            fs::create_directories(notProject, ec);
            std::string importErr;
            if (db.Import(notProject, importErr) || importErr.empty()) {
                LOG_ERROR("Editor") << "SELFTEST: импорт принял папку без project.sageproj";
                ok = false;
            }
            fs::remove_all(notProject, ec);

            const size_t before = db.All().size();
            std::string dupErr;
            if (!db.Import(m_project.Dir(), dupErr) || db.All().size() != before) {
                LOG_ERROR("Editor") << "SELFTEST: повторный импорт задвоил проект в списке";
                ok = false;
            }
        }
        // Дубликат -> удаление с диска: копия появляется рядом и исчезает
        // насовсем, а оригинал остаётся.
        if (ok) {
            std::string dupErr, copyPath;
            if (!db.Duplicate(mine, "selftest_project_copy", dupErr, &copyPath)) {
                LOG_ERROR("Editor") << "SELFTEST: дубликат проекта не создался: " << dupErr;
                ok = false;
            } else if (!fs::exists(fs::path(copyPath) / "project.sageproj", ec)) {
                LOG_ERROR("Editor") << "SELFTEST: у дубликата нет дескриптора";
                ok = false;
            } else {
                std::string delErr;
                if (!db.DeleteFromDisk(copyPath, delErr)) {
                    LOG_ERROR("Editor") << "SELFTEST: дубликат не удалился: " << delErr;
                    ok = false;
                } else if (fs::exists(copyPath, ec) || db.IndexOf(copyPath) >= 0) {
                    LOG_ERROR("Editor") << "SELFTEST: удалённый проект остался";
                    ok = false;
                }
                // Оригинал НЕ пострадал — иначе «удалить копию» означало бы
                // потерю работы.
                if (ok && !fs::exists(fs::path(mine) / "project.sageproj", ec)) {
                    LOG_ERROR("Editor") << "SELFTEST: удаление копии задело оригинал";
                    ok = false;
                }
            }
        }
        // Пропавшая папка ПОМЕЧАЕТСЯ, а не вычёркивается: внешний диск
        // отключают чаще, чем удаляют проекты.
        if (ok) {
            const fs::path ghost = fs::absolute("selftest_ghost_project", ec);
            fs::create_directories(ghost, ec);
            std::ofstream(ghost / "project.sageproj") << "{\"name\":\"ghost\"}";
            std::string ghostErr;
            if (!db.Import(ghost, ghostErr)) {
                LOG_ERROR("Editor") << "SELFTEST: импорт проекта-призрака отказал: " << ghostErr;
                ok = false;
            } else {
                fs::remove_all(ghost, ec);
                ProjectDatabase reload;
                reload.Load();
                const ProjectEntry* e = reload.Find(ghost.string());
                if (!e || !e->Missing) {
                    LOG_ERROR("Editor") << "SELFTEST: пропавший проект не помечен";
                    ok = false;
                }
                reload.Forget(ghost.string());
                ProjectDatabase after;
                after.Load();
                if (after.Find(ghost.string())) {
                    LOG_ERROR("Editor") << "SELFTEST: запись не убралась из списка";
                    ok = false;
                }
            }
        }
        // Перетаскивание: папка проекта добавляется, чужая — с объяснением.
        if (ok) {
            Sage::Launcher::ProjectLauncher launcher;
            ProjectDatabase drop;
            drop.Load();
            const int added = launcher.AcceptDroppedFiles({mine}, drop);
            if (added != 0 && drop.IndexOf(mine) < 0) {
                LOG_ERROR("Editor") << "SELFTEST: брошенный проект не попал в список";
                ok = false;
            }
            const int junk = launcher.AcceptDroppedFiles({std::string("нет-такой-папки")}, drop);
            if (junk != 0 || launcher.LastError().empty()) {
                LOG_ERROR("Editor") << "SELFTEST: брошенная чужая папка принята молча";
                ok = false;
            }
        }
        // Кэш обложек: путь считается из пути картинки и не меняется от
        // запуска к запуску — иначе кэш не кэш.
        if (ok) {
            const std::string src = "/tmp/пример/preview.png";
            if (ProjectThumbnail::CachePathFor(src) != ProjectThumbnail::CachePathFor(src) ||
                ProjectThumbnail::CachePathFor(src) == ProjectThumbnail::CachePathFor(src + "2")) {
                LOG_ERROR("Editor") << "SELFTEST: путь кэша обложек не постоянен";
                ok = false;
            }
        }
    }

    // --- CameraComponent: сериализация вместе со сценой ---
    if (ok) {
        GameObject camObj = m_scene->FindByName("Main Camera");
        bool camOk = camObj.Valid() &&
                     m_scene->Registry().try_get<CameraComponent>(camObj.Entity()) != nullptr &&
                     HasPrimaryCamera();
        if (!camOk) {
            LOG_ERROR("Editor") << "SELFTEST: Main Camera lost after scene save/load round-trip";
            ok = false;
        }
    }

    // --- LightComponent: демо-лампа (точечный) и прожектор переживают
    // save/load, CollectLighting раскладывает их по типам (Point/Spot) ---
    if (ok) {
        GameObject lamp = m_scene->FindByName("Lamp");
        GameObject spot = m_scene->FindByName("Spotlight");
        const LightComponent* pointLc =
            lamp.Valid() ? m_scene->Registry().try_get<LightComponent>(lamp.Entity()) : nullptr;
        const LightComponent* spotLc =
            spot.Valid() ? m_scene->Registry().try_get<LightComponent>(spot.Entity()) : nullptr;

        LightingEnvironment collected = sage::ecs::CollectLighting(*m_scene);
        size_t expectPoint = m_scene->Lighting.PointLights.size() + 1; // + лампа
        size_t expectSpot = m_scene->Lighting.SpotLights.size() + 1;   // + прожектор
        bool typeOk = spotLc && spotLc->Kind == LightComponent::Type::Spot &&
                      pointLc && pointLc->Kind == LightComponent::Type::Point;
        // Направление прожектора смотрит вниз (поворот -90° по X) — Y-компонента
        // «вперёд» должна быть заметно отрицательной.
        bool dirOk = !collected.SpotLights.empty() && collected.SpotLights.front().Direction.y < -0.9f;
        if (!typeOk || collected.PointLights.size() != expectPoint ||
            collected.SpotLights.size() != expectSpot || !dirOk) {
            LOG_ERROR("Editor") << "SELFTEST: light round-trip failed (types " << typeOk
                                << ", points " << collected.PointLights.size() << "/" << expectPoint
                                << ", spots " << collected.SpotLights.size() << "/" << expectSpot
                                << ", dir " << dirOk << ")";
            ok = false;
        }
    }

    // --- Dirty-маркер: мутация ставит, сохранение снимает ---
    if (ok) {
        PushUndoSnapshot();
        CreateCubeEntity("DirtyProbe");
        if (!m_sceneDirty) {
            LOG_ERROR("Editor") << "SELFTEST: scene not marked dirty after mutation";
            ok = false;
        }
        if (ok && !SaveSceneToFile(m_project.ScenesDir() / "selftest.sage")) ok = false;
        if (ok && m_sceneDirty) {
            LOG_ERROR("Editor") << "SELFTEST: dirty flag not cleared by save";
            ok = false;
        }
        Undo(); // вернуть сцену к сохранённому состоянию
    }

    // --- Undo/Redo: создание сущности откатывается и накатывается обратно ---
    if (ok) {
        size_t n0 = m_scene->Count();
        PushUndoSnapshot();
        CreateCubeEntity("UndoProbe");
        Undo();
        if (m_scene->Count() != n0) {
            LOG_ERROR("Editor") << "SELFTEST: undo failed (count " << m_scene->Count() << ", expected " << n0 << ")";
            ok = false;
        }
        Redo();
        if (ok && m_scene->Count() != n0 + 1) {
            LOG_ERROR("Editor") << "SELFTEST: redo failed (count " << m_scene->Count() << ", expected " << n0 + 1 << ")";
            ok = false;
        }
        Undo(); // вернуть сцену к исходным n0 сущностям
    }

    // --- Создание ассетов: та же логика, что у модалки Create Asset ---
    if (ok) {
        struct { AssetsPanel::CreateKind kind; const char* name; const char* expect; } cases[] = {
            {AssetsPanel::CreateKind::Folder, "selftest_dir", "selftest_dir"},
            {AssetsPanel::CreateKind::SageScript, "selftest_sage", "selftest_sage.slua"},
            {AssetsPanel::CreateKind::Scene, "selftest_new_scene", "selftest_new_scene.sage"},
            {AssetsPanel::CreateKind::Material, "selftest_mat", "selftest_mat.sagemat"},
        };
        for (const auto& c : cases) {
            fs::path created;
            std::string createErr;
            if (!AssetsPanel::CreateAsset(c.kind, c.name, m_assetsCwd, created, createErr) ||
                !fs::exists(m_assetsCwd / c.expect, ec)) {
                LOG_ERROR("Editor") << "SELFTEST: asset create failed for " << c.expect
                                    << " (" << createErr << ")";
                ok = false;
                break;
            }
        }
        // Новая сцена из панели — сразу рабочая: открывается, в ней камера и
        // солнце (без них — чёрный кадр и «нет камеры» в Play).
        if (ok) {
            try {
                auto made = SceneSerializer::Load((m_assetsCwd / "selftest_new_scene.sage").string());
                bool camera = false, sun = false;
                for (auto e : made->Registry().view<CameraComponent>()) { (void)e; camera = true; }
                for (auto e : made->Registry().view<LightComponent>()) { (void)e; sun = true; }
                if (!camera || !sun || made->Name() != "selftest_new_scene") {
                    LOG_ERROR("Editor") << "SELFTEST: новая сцена из панели без камеры или солнца";
                    ok = false;
                }
            } catch (const std::exception& ex) {
                LOG_ERROR("Editor") << "SELFTEST: новая сцена из панели не открывается: " << ex.what();
                ok = false;
            }
        }
        // Заготовка .slua обязана компилироваться как есть: новый скрипт с
        // ошибкой в первой же строке — худшее знакомство с языком.
        if (ok) {
            std::ifstream in(m_assetsCwd / "selftest_sage.slua", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const sage::lang::CompileResult r = sage::lang::Compile("selftest_sage.slua", text);
            if (!r.Ok || r.MainClass != "SelftestSage" || sage::lang::FieldsOf(r).Find("speed") == nullptr) {
                const sage::lang::Diagnostic* d = r.FirstError();
                LOG_ERROR("Editor") << "SELFTEST: заготовка .slua не компилируется: "
                                    << (d ? d->Format() : r.MainClass);
                ok = false;
            }
        }
    }

    // --- Переименование и удаление тащат за собой сайдкары ---
    //
    // Рядом с ассетом лежит .meta — его личность (GUID, по которому сцены его и
    // находят). Панель показывает файл, а не его сайдкар, поэтому отстать
    // сайдкар может только молча: переименованный ассет стал бы для проекта
    // новым (все ссылки на него — битыми), а рядом осталась бы невидимая запись
    // про файл, которого нет.
    if (ok) {
        const fs::path original = m_assetsCwd / "selftest_rename.lua";
        std::ofstream(original) << "-- selftest\n";
        std::ofstream(original.string() + ".meta") << "{\"guid\":\"selftest\"}\n";

        fs::path renamed;
        std::string renameErr;
        if (!AssetsPanel::RenameAsset(original, "selftest_renamed.lua", renamed, renameErr)) {
            LOG_ERROR("Editor") << "SELFTEST: переименование ассета не удалось: " << renameErr;
            ok = false;
        } else if (!fs::exists(renamed.string() + ".meta", ec) ||
                   fs::exists(original.string() + ".meta", ec)) {
            LOG_ERROR("Editor") << "SELFTEST: сайдкар .meta не поехал за переименованным ассетом";
            ok = false;
        }

        // Имя занято и пустое имя — отказ с объяснением, а не молчаливая потеря
        // файла поверх соседа.
        if (ok) {
            fs::path clash;
            std::string clashErr;
            std::ofstream(m_assetsCwd / "selftest_busy.lua") << "-- busy\n";
            if (AssetsPanel::RenameAsset(renamed, "selftest_busy.lua", clash, clashErr) ||
                clashErr.empty()) {
                LOG_ERROR("Editor") << "SELFTEST: переименование поверх существующего файла прошло молча";
                ok = false;
            }
            if (ok && AssetsPanel::RenameAsset(renamed, "", clash, clashErr)) {
                LOG_ERROR("Editor") << "SELFTEST: пустое имя принято";
                ok = false;
            }
            fs::remove(m_assetsCwd / "selftest_busy.lua", ec);
        }

        if (ok) {
            AssetsPanel::DeleteAsset(renamed);
            if (fs::exists(renamed, ec) || fs::exists(renamed.string() + ".meta", ec)) {
                LOG_ERROR("Editor") << "SELFTEST: после удаления ассета остался он сам или его .meta";
                ok = false;
            }
        }
    }

    // --- Материалы: правка разделяемого экземпляра + назначение + сохранение
    // сцены + перезагрузка => путь и albedo восстановлены ---
    if (ok) {
        std::string matPath = (m_assetsCwd / "selftest_mat.sagemat").string();
        auto material = ResourceManager::Instance().GetMaterial(matPath);
        material->Albedo = {0.1f, 0.2f, 0.9f};
        try {
            material->SaveToFile(matPath);
        } catch (const std::exception& e) {
            LOG_ERROR("Editor") << "SELFTEST: material save failed: " << e.what();
            ok = false;
        }
        if (ok) {
            GameObject cube = m_scene->FindByName("Green Cube");
            MeshRendererComponent& mr = cube.Renderer();
            mr.MaterialPath = matPath;
            mr.MaterialPtr = material;

            fs::path scenePath = m_project.ScenesDir() / "selftest_mat.sage";
            if (!SaveSceneToFile(scenePath) || !LoadSceneFromFile(scenePath)) ok = false;
            if (ok) {
                MeshRendererComponent& reloaded = m_scene->FindByName("Green Cube").Renderer();
                bool pathOk = reloaded.MaterialPath == matPath;
                bool albedoOk = reloaded.MaterialPtr &&
                                std::abs(reloaded.MaterialPtr->Albedo.b - 0.9f) < 0.001f;
                if (!pathOk || !albedoOk) {
                    LOG_ERROR("Editor") << "SELFTEST: material round-trip failed (path "
                                        << pathOk << ", albedo " << albedoOk << ")";
                    ok = false;
                }
                // Убираем материал, чтобы дальнейшие проверки шли по прежнему сценарию.
                if (ok) {
                    reloaded.MaterialPath.clear();
                    reloaded.MaterialPtr = nullptr;
                }
            }
        }
    }

    // --- Материал НАЗНАЧАЕТСЯ ТАК ЖЕ, КАК В РЕДАКТОРЕ, и правится из панели ---
    //
    // Ровно этот путь и был сломан. Сущность держит ссылку ОТНОСИТЕЛЬНО
    // ПРОЕКТА (Project::AssetRef — так она уезжает в .sage), а панель ассетов
    // открывает материал НАСТОЯЩИМ путём в файловой системе. Пока ключом кэша
    // был путь «как дали», на один файл заводилось два объекта Material:
    // редактор правил свой, рендер рисовал свой, и правки материала не
    // применялись НИКОГДА — ни к одному полю.
    //
    // Проверка идёт по настоящим операциям редактора: ApplyAssetToEntity — то,
    // что делает бросок материала на объект, а GetMaterial(полный путь) — то,
    // что делает редактор материалов при выборе файла в Assets.
    if (ok) {
        const fs::path matFile = m_assetsCwd / "selftest_assign.sagemat";
        Material authored;
        authored.Albedo = {0.9f, 0.1f, 0.1f};
        authored.Metallic = 0.25f;
        authored.Roughness = 0.75f;
        try {
            authored.SaveToFile(matFile.string());
        } catch (const std::exception& e) {
            LOG_ERROR("Editor") << "SELFTEST: не записался материал: " << e.what();
            ok = false;
        }

        GameObject cube = ok ? m_scene->FindByName("Green Cube") : GameObject{};
        if (ok && !cube.Valid()) {
            LOG_ERROR("Editor") << "SELFTEST: нет объекта для назначения материала";
            ok = false;
        }
        if (ok && !ApplyAssetToEntity(cube.Id(), matFile)) {
            LOG_ERROR("Editor") << "SELFTEST: материал не назначился объекту";
            ok = false;
        }
        if (ok) {
            MeshRendererComponent& mr = cube.Renderer();
            // Ссылка обязана быть относительной: абсолютный путь уехал бы в
            // .sage и не открылся бы ни на другой машине, ни после переноса.
            const bool refRelative = !mr.MaterialPath.empty() &&
                                     !fs::path(mr.MaterialPath).is_absolute();
            // А теперь — тот же файл глазами панели ассетов.
            std::shared_ptr<Material> asPanelSeesIt =
                ResourceManager::Instance().GetMaterial(matFile.string());
            const bool sameObject = asPanelSeesIt && asPanelSeesIt == mr.MaterialPtr;
            if (!refRelative || !sameObject) {
                LOG_ERROR("Editor") << "SELFTEST: материал раздвоился между панелью и сценой "
                                    << "(ссылка '" << mr.MaterialPath << "', относительная "
                                    << refRelative << ", один объект " << sameObject << ")";
                ok = false;
            }

            // Правка «ползунком» в редакторе материалов — и она обязана дойти
            // до того, чем красится объект. EffectiveColor — то же самое, что
            // читает рендер.
            if (ok) {
                asPanelSeesIt->Albedo = {0.1f, 0.2f, 0.95f};
                const glm::vec3 shown = EffectiveColor(mr);
                if (std::abs(shown.b - 0.95f) > 0.001f || shown.r > 0.2f) {
                    LOG_ERROR("Editor") << "SELFTEST: правка материала не дошла до объекта ("
                                        << shown.r << ", " << shown.g << ", " << shown.b << ")";
                    ok = false;
                }
            }

            // И сохранение: записали, выбросили из кэша, прочитали заново.
            if (ok) {
                try {
                    asPanelSeesIt->SaveToFile(matFile.string());
                } catch (const std::exception& e) {
                    LOG_ERROR("Editor") << "SELFTEST: материал не сохранился: " << e.what();
                    ok = false;
                }
            }
            if (ok) {
                const Material reread = Material::LoadFromFile(matFile.string());
                const bool kept = std::abs(reread.Albedo.b - 0.95f) < 0.001f &&
                                  std::abs(reread.Metallic - 0.25f) < 0.001f &&
                                  std::abs(reread.Roughness - 0.75f) < 0.001f;
                if (!kept) {
                    LOG_ERROR("Editor") << "SELFTEST: сохранённый материал прочитался иначе";
                    ok = false;
                }
            }
            // Возвращаем объект как был: дальше идут проверки, рассчитанные на
            // прежнюю сцену.
            mr.MaterialPath.clear();
            mr.MaterialPtr = nullptr;
        }
    }

    // --- Примитивы + атмосфера (скайбокс/туман): переживают save/load ---
    if (ok) {
        GameObject probe = m_scene->FindByName("Red Cube");
        if (probe.Valid()) {
            MeshRendererComponent& mr = probe.Renderer();
            mr.Ref = MeshRef{MeshRef::Type::Sphere, ""};
            mr.MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Sphere);
        }
        m_scene->Lighting.Fog.Enabled = true;
        m_scene->Lighting.Fog.Color = {0.1f, 0.4f, 0.7f};
        m_scene->Lighting.Skybox.TopColor = {0.05f, 0.1f, 0.3f};

        fs::path scenePath = m_project.ScenesDir() / "selftest_env.sage";
        if (!SaveSceneToFile(scenePath) || !LoadSceneFromFile(scenePath)) ok = false;
        if (ok) {
            GameObject reloaded = m_scene->FindByName("Red Cube");
            bool meshOk = reloaded.Valid() && reloaded.Renderer().Ref.type == MeshRef::Type::Sphere &&
                          reloaded.Renderer().MeshPtr != nullptr;
            bool fogOk = m_scene->Lighting.Fog.Enabled &&
                         std::abs(m_scene->Lighting.Fog.Color.b - 0.7f) < 0.001f;
            bool skyOk = m_scene->Lighting.Skybox.Enabled &&
                         std::abs(m_scene->Lighting.Skybox.TopColor.b - 0.3f) < 0.001f;
            if (!meshOk || !fogOk || !skyOk) {
                LOG_ERROR("Editor") << "SELFTEST: primitive/environment round-trip failed (mesh "
                                    << meshOk << ", fog " << fogOk << ", sky " << skyOk << ")";
                ok = false;
            }
        }
    }

    return ok;
}
