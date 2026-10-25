#include "SelfTestCommon.h"

bool EditorLayer::SelfTestToolsAssetsAndPanels() {
    bool ok = true;
    std::error_code ec;
    std::string err;
    (void)ec; (void)err;
    // --- КЛИПЫ АНИМАЦИИ ВЫНИМАЮТСЯ В ФАЙЛЫ ------------------------------------
    //
    // Жалоба была такая: «компонент анимация вообще не работает, там просто
    // тестовая тентакля». Щупальце убрано, а клипы модели теперь вынимаются в
    // .sageanim рядом с ней — и проверяется это на НАСТОЯЩЕЙ модели со скелетом,
    // а не на синтетике: разбор скина и клипов и есть то, что здесь может
    // сломаться.
#ifdef SAGE_TEST_MODEL
    if (ok) {
        const fs::path animDir = m_project.AssetsDir() / "selftest_anim";
        // С чистого листа: клипы прошлого прогона, лежащие по-старому рядом с
        // моделью, были бы взяты как есть, и проверка раскладки ничего бы не проверяла.
        fs::remove_all(animDir, ec);
        fs::create_directories(animDir, ec);
        const fs::path modelCopy = animDir / "hero.glb";
        fs::copy_file(SAGE_TEST_MODEL, modelCopy, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_ERROR("Editor") << "SELFTEST: тестовая модель не скопировалась: " << ec.message();
            ok = false;
        } else {
            sage::AssetDatabase::Instance().ScanProject(m_project.Dir().string());
            const ClipImportResult r = ImportModelClips(m_project, m_project.AssetRef(modelCopy));
            if (r.Found <= 0) {
                LOG_ERROR("Editor") << "SELFTEST: клипы модели не найдены";
                ok = false;
            } else if (r.Created != r.Found) {
                LOG_ERROR("Editor") << "SELFTEST: записано файлов клипов " << r.Created << " из "
                                    << r.Found;
                ok = false;
            } else {
                // Файлы обязаны лежать на диске и читаться обратно: «импорт
                // сказал, что записал» — это не то же самое, что «файл есть».
                for (const std::string& ref : r.Refs) {
                    const std::string full = sage::AssetDatabase::Instance().LocatePath(ref);
                    // Клипы — в общей папке Animations/ рядом с моделью, а не
                    // россыпью возле неё (sage/assets/ModelData.h).
                    if (fs::path(full).parent_path().filename() != "Animations") {
                        LOG_ERROR("Editor") << "SELFTEST: клип лежит не в Animations/: " << full;
                        ok = false;
                        break;
                    }
                    if (!fs::exists(full, ec)) {
                        LOG_ERROR("Editor") << "SELFTEST: файла клипа нет: " << full;
                        ok = false;
                        break;
                    }
                    try {
                        const sage::anim::ClipAsset clip = sage::anim::LoadClip(full);
                        if (clip.Channels.empty()) {
                            LOG_ERROR("Editor") << "SELFTEST: клип без каналов: " << full;
                            ok = false;
                            break;
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("Editor") << "SELFTEST: клип не читается: " << e.what();
                        ok = false;
                        break;
                    }
                }
            }
            // Повторный импорт НЕ перезаписывает: правку клипа руками терять
            // нельзя, а «переимпортировал модель» — обычное действие.
            if (ok) {
                const ClipImportResult again =
                    ImportModelClips(m_project, m_project.AssetRef(modelCopy));
                if (again.Created != 0) {
                    LOG_ERROR("Editor") << "SELFTEST: повторный импорт перезаписал клипы ("
                                        << again.Created << ")";
                    ok = false;
                }
            }
        }
    }
#endif

    // --- Свои ассеты: внесение файла со стороны и переносимость ссылок --------
    //
    // Ровно тот путь, на котором «не грузятся мои ассеты» и заканчивалось:
    // модель лежит НЕ в проекте, в редакторе она открывается, а в собранной
    // игре её нет — потому что в сцену уехал абсолютный путь этой машины.
    if (ok) {
        // Чужая папка: не проект и не рядом с ним.
        const fs::path outside = fs::temp_directory_path() / "sage_selftest_outside";
        fs::remove_all(outside, ec);
        fs::create_directories(outside, ec);

        // Модель со спутниками: .obj -> .mtl -> текстура. Именно этот случай
        // ломался при «просто скопируй файл»: модель без .mtl приезжает без
        // материала, и молча.
        { std::ofstream f(outside / "crate.obj");
          f << "mtllib crate.mtl\nusemtl painted\n"
               "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n"; }
        { std::ofstream f(outside / "crate.mtl");
          f << "newmtl painted\nKd 0.2 0.6 0.9\nmap_Kd crate_albedo.png\n"; }
        { std::ofstream f(outside / "crate_albedo.png", std::ios::binary); f << "png-заглушка"; }

        const fs::path destDir = m_project.AssetsDir() / "selftest_import";
        const AssetsPanel::ImportReport rep = AssetsPanel::ImportAsset(outside / "crate.obj", destDir);
        // Модель приезжает В СВОЮ ПАПКУ (selftest_import/crate/) — со своими
        // спутниками, а позже и с материалами, текстурами и клипами.
        const fs::path modelDir = destDir / "crate";
        if (!rep.Ok) {
            LOG_ERROR("Editor") << "SELFTEST: импорт не удался: " << rep.Error;
            ok = false;
        } else if (!fs::exists(modelDir / "crate.obj", ec) ||
                   !fs::exists(modelDir / "crate.mtl", ec) ||
                   !fs::exists(modelDir / "crate_albedo.png", ec)) {
            LOG_ERROR("Editor") << "SELFTEST: импорт не принёс спутников модели (.mtl/текстура)";
            ok = false;
        } else if (!rep.Missing.empty()) {
            LOG_ERROR("Editor") << "SELFTEST: импорт потерял спутник: " << rep.Missing.front();
            ok = false;
        }

        // Ссылка на внесённый файл обязана быть ОТНОСИТЕЛЬНОЙ: именно так её
        // найдёт собранная игра, делающая chdir в свою папку.
        if (ok) {
            const std::string ref = m_project.AssetRef(modelDir / "crate.obj");
            if (ref.empty() || fs::path(ref).is_absolute() ||
                ref != "assets/selftest_import/crate/crate.obj") {
                LOG_ERROR("Editor") << "SELFTEST: ссылка на ассет не относительна проекту: " << ref;
                ok = false;
            }
            // ...а на файл ВНЕ проекта — остаться как есть: врать про
            // относительный путь там, где его нет, хуже, чем честный абсолютный.
            const std::string outsideRef = m_project.AssetRef(outside / "crate.obj");
            if (!fs::path(outsideRef).is_absolute()) {
                LOG_ERROR("Editor") << "SELFTEST: путь вне проекта подменён: " << outsideRef;
                ok = false;
            }
            // И по относительной ссылке файл обязан НАХОДИТЬСЯ — иначе она
            // переносима, но бесполезна.
            if (ok && !ModelLoader::IsSupportedModel(ref)) {
                LOG_ERROR("Editor") << "SELFTEST: относительная ссылка не опознана как модель";
                ok = false;
            }
            if (ok && !ResourceManager::Instance().GetModel(ref)) {
                LOG_ERROR("Editor") << "SELFTEST: модель не грузится по относительной ссылке " << ref;
                ok = false;
            }
        }

        // Материал модели приезжает ВМЕСТЕ с ней: .sagemat рядом, с albedo из
        // .mtl. Без этого модель в сцене — белая болванка.
        if (ok) {
            const ModelLoader::ExtractedMaterial ex =
                ModelLoader::ExtractMaterial((modelDir / "crate.obj").string());
            // Kd в .mtl — линейный; в материале цвет хранится так, как его
            // показывает инспектор (sRGB, см. ExtractMaterials).
            if (!ex.Found || std::abs(SrgbToLinear(ex.Albedo.g) - 0.6f) > 1e-3f) {
                LOG_ERROR("Editor") << "SELFTEST: материал модели не прочитан из .mtl";
                ok = false;
            }
        }
        fs::remove_all(outside, ec);
    }

    // --- 3D-обложка префаба ---------------------------------------------------
    //
    // У материала обложка была, у префаба — синий прямоугольник с надписью
    // PREFAB, одинаковый у всех. Проверяем, что теперь префаб реально
    // снимается: непустой хендл текстуры — это состоявшийся проход рендера.
    if (ok) {
        GameObject box = m_scene->CreateObject("SelfTestPrefabCover");
        MeshRendererComponent& bmr = box.Renderer();
        bmr.Ref.type = MeshRef::Type::Cube;
        bmr.MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
        const fs::path coverPrefab = m_project.AssetsDir() / "selftest_cover.sageprefab";
        std::string cerr;
        if (!sage::scene::SavePrefab(*m_scene, box.Entity(), coverPrefab.string(), cerr)) {
            LOG_ERROR("Editor") << "SELFTEST: префаб для обложки не сохранился: " << cerr;
            ok = false;
        } else {
            // Своё превью, а не панельное: self-test идёт до первого кадра, и
            // панели ещё ничего не рисовали. Shutdown обязателен здесь же —
            // буфер превью это объект драйвера, а контекст жив только сейчас.
            AssetPreview cover;
            const uint64_t tex = cover.RenderPrefab(coverPrefab.string(), 96);
            cover.Shutdown();
            if (tex == 0) {
                LOG_ERROR("Editor") << "SELFTEST: 3D-обложка префаба не отрисовалась";
                ok = false;
            }
        }
        m_scene->RemoveObject(box.Id());
    }

    // --- Перетаскивание: те же операции, что делает мышь ----------------------
    //
    // Кликов в headless-прогоне нет, но кликают-то они по этим методам — их и
    // проверяем. Перетаскивание раньше упиралось в то, что принимать его было
    // некому: вьюпорт и иерархия целей не имели вовсе, а окно не слышало
    // проводник системы.
    if (ok) {
        const fs::path modelPath = m_project.AssetsDir() / "selftest_import" / "crate.obj";
        const fs::path matPath = m_assetsCwd / "selftest_mat.sagemat";

        // 1. Бросок модели в СПИСОК сцены: появляется новая сущность с этим мешем.
        const int before = m_scene->Count();
        if (!AddAssetToScene(modelPath)) {
            LOG_ERROR("Editor") << "SELFTEST: модель не добавилась в сцену перетаскиванием";
            ok = false;
        } else if (m_scene->Count() != before + 1) {
            LOG_ERROR("Editor") << "SELFTEST: перетаскивание модели не создало сущность";
            ok = false;
        }
        const int droppedId = m_selection.Primary();

        // 2. Материал НА СУЩНОСТЬ: назначается ей, а не выбранной в панели.
        if (ok && fs::exists(matPath, ec)) {
            if (!ApplyAssetToEntity(droppedId, matPath)) {
                LOG_ERROR("Editor") << "SELFTEST: материал не назначился перетаскиванием";
                ok = false;
            } else {
                GameObject o = m_scene->Get(droppedId);
                if (!o.Valid() || o.Renderer().MaterialPath.empty()) {
                    LOG_ERROR("Editor") << "SELFTEST: материал перетаскиванием не записался";
                    ok = false;
                }
            }
        }

        // 3. Скрипт НА СУЩНОСТЬ — тем же путём, что материал и модель.
        if (ok) {
            const fs::path luaPath = m_project.AssetsDir() / "selftest_code.lua";
            if (!ApplyAssetToEntity(droppedId, luaPath)) {
                LOG_ERROR("Editor") << "SELFTEST: скрипт не назначился перетаскиванием";
                ok = false;
            } else if (!m_scene->Registry().all_of<ScriptComponent>(m_scene->Get(droppedId).Entity())) {
                LOG_ERROR("Editor") << "SELFTEST: ScriptComponent не появился";
                ok = false;
            }
        }

        // 4. Бросок ВО ВЬЮПОРТ: объект встаёт туда, куда показывает луч. Целимся
        //    в центр кадра — там пол демо-сцены, и объект обязан оказаться НА
        //    нём, а не в начале координат и не под ним.
        if (ok) {
            const glm::mat4 v = m_camera.GetViewMatrix();
            const glm::mat4 p = m_camera.GetProjectionMatrix(16.0f / 9.0f);
            const int countBefore = m_scene->Count();
            if (!DropAssetAtViewport(v, p, 0.5f, 0.5f, modelPath)) {
                LOG_ERROR("Editor") << "SELFTEST: вьюпорт не принял модель";
                ok = false;
            } else if (m_scene->Count() != countBefore + 1) {
                LOG_ERROR("Editor") << "SELFTEST: бросок во вьюпорт не создал сущность";
                ok = false;
            } else {
                GameObject placed = m_scene->Get(m_selection.Primary());
                // Луч из редакторской камеры в центр кадра упирается в пол демо-
                // сцены: объект обязан оказаться не в начале координат.
                if (!placed.Valid()) {
                    LOG_ERROR("Editor") << "SELFTEST: брошенный объект невалиден";
                    ok = false;
                } else if (glm::length(placed.GetTransform().Position) < 1e-3f) {
                    LOG_ERROR("Editor") << "SELFTEST: брошенный объект встал в начало координат, "
                                        << "а не под курсор";
                    ok = false;
                }
                if (ok) m_scene->RemoveObject(placed.Id());
            }
        }

        // 5. Файл, брошенный В ОКНО из проводника, вносится в проект.
        if (ok) {
            const fs::path outside = fs::temp_directory_path() / "sage_selftest_drop";
            fs::remove_all(outside, ec);
            fs::create_directories(outside, ec);
            { std::ofstream f(outside / "dropped.png", std::ios::binary); f << "png-заглушка"; }
            m_droppedFiles.push_back((outside / "dropped.png").string());
            HandleDroppedFiles();
            if (!fs::exists(m_assetsCwd / "dropped.png", ec)) {
                LOG_ERROR("Editor") << "SELFTEST: брошенный в окно файл не внесён в проект";
                ok = false;
            }
            // Заглушка не картинка, и панель ассетов будет честно ругаться на
            // неё каждый кадр — убираем, проверка уже состоялась.
            fs::remove(m_assetsCwd / "dropped.png", ec);
            fs::remove(m_assetsCwd / "dropped.png.meta", ec);
            fs::remove_all(outside, ec);
        }

        if (ok) m_scene->RemoveObject(droppedId);
        m_selection.Clear();
    }

    // --- Настройки движка действуют НА КАРТИНКУ, а не только на своё окно ----
    //
    // Окно Settings правило собственную копию EngineConfig, а весь рендер читал
    // глобальный: тени, каскады, сглаживание и пост-обработка оставались на
    // умолчаниях, что ни выбирай. Проверяем связь в обе стороны — и что она
    // переживает пересборку зависящих от неё объектов (карта теней).
    if (ok) {
        const sage::EngineConfig saved = m_settings;

        m_settings.ShadowResolution = 1024;
        m_settings.ShadowCascades = 2;
        m_settings.Msaa = 4;
        ApplyEngineSettings();

        const sage::EngineConfig& live = sage::EngineConfig::Get();
        if (live.ShadowResolution != 1024 || live.ShadowCascades != 2 || live.Msaa != 4) {
            LOG_ERROR("Editor") << "SELFTEST: настройки не доехали до движка "
                                << "(тени " << live.ShadowResolution << "/" << live.ShadowCascades
                                << ", msaa " << live.Msaa << ")";
            ok = false;
        }
        // Число сэмплов буфера сцены считается ИЗ конфига одной функцией на
        // редактор и рантайм — иначе «в редакторе гладко, в игре лесенка».
        if (ok && sage::render::SceneSamples(live) != 4) {
            LOG_ERROR("Editor") << "SELFTEST: MSAA из настроек не превратился в сэмплы";
            ok = false;
        }

        m_settings = saved;
        ApplyEngineSettings();
    }

    // --- Каждый компонент переживает сохранение и загрузку сцены ---
    //
    // Проверки выше берут компоненты по одному и там, где они уже стоят в
    // демо-сцене. Компонент, который забыли записать в сериализатор, так не
    // ловится: сцена сохраняется, открывается, всё на месте — кроме того, что
    // никто не проверял. А узнаётся об этом позже всего: человек настраивает
    // сустав или контроллер персонажа, сохраняет сцену, открывает назавтра — и
    // настройки нет. Здесь на одной сущности собраны ВСЕ компоненты сразу, и
    // после круга «сохранить -> новая сцена -> загрузить» каждый обязан
    // вернуться.
    if (ok) {
        GameObject all = m_scene->CreateObject("All Components");
        entt::registry& reg = m_scene->Registry();
        entt::entity e = all.Entity();
        reg.emplace_or_replace<MeshRendererComponent>(e);
        reg.emplace_or_replace<DecalComponent>(e);
        reg.emplace_or_replace<GIStaticComponent>(e);
        reg.emplace_or_replace<CameraComponent>(e);
        reg.emplace_or_replace<LightComponent>(e);
        reg.emplace_or_replace<RigidBodyComponent>(e);
        reg.emplace_or_replace<ColliderComponent>(e);
        reg.emplace_or_replace<JointComponent>(e);
        reg.emplace_or_replace<CharacterControllerComponent>(e);
        reg.emplace_or_replace<AnimationComponent>(e);
        reg.emplace_or_replace<IKComponent>(e);
        reg.emplace_or_replace<ReflectionProbeComponent>(e);
        reg.emplace_or_replace<ScriptComponent>(e, ScriptComponent{"assets/selftest_script.lua"});
        // Со СЦЕНОЙ, а не с реестром: у кнопки есть ребёнок-надпись, и создать
        // объект умеет только сцена (см. UIPresets.h).
        sage::ui::ApplyPreset(*m_scene, e, "Button");
        reg.emplace_or_replace<ParticleEmitterComponent>(e);

        const fs::path allPath = m_project.ScenesDir() / "selftest_all_components.sage";
        if (!SaveSceneToFile(allPath)) {
            LOG_ERROR("Editor") << "SELFTEST: сцена со всеми компонентами не сохранилась";
            ok = false;
        }
        if (ok) {
            NewScene(ProjectTemplateKind::Empty); // пустая сцена — чтобы «сохранилось» не путать с «осталось в памяти»
            if (!LoadSceneFromFile(allPath)) {
                LOG_ERROR("Editor") << "SELFTEST: сцена со всеми компонентами не загрузилась";
                ok = false;
            }
        }
        if (ok) {
            GameObject loaded = m_scene->FindByName("All Components");
            if (!loaded.Valid()) {
                LOG_ERROR("Editor") << "SELFTEST: сущность со всеми компонентами потерялась";
                ok = false;
            } else {
                entt::registry& r2 = m_scene->Registry();
                const entt::entity e2 = loaded.Entity();
                struct Check { const char* Name; bool Present; };
                const Check checks[] = {
                    {"MeshRenderer", r2.all_of<MeshRendererComponent>(e2)},
                    {"Decal", r2.all_of<DecalComponent>(e2)},
                    {"GIStatic", r2.all_of<GIStaticComponent>(e2)},
                    {"Camera", r2.all_of<CameraComponent>(e2)},
                    {"Light", r2.all_of<LightComponent>(e2)},
                    {"RigidBody", r2.all_of<RigidBodyComponent>(e2)},
                    {"Collider", r2.all_of<ColliderComponent>(e2)},
                    {"Joint", r2.all_of<JointComponent>(e2)},
                    {"CharacterController", r2.all_of<CharacterControllerComponent>(e2)},
                    {"Animation", r2.all_of<AnimationComponent>(e2)},
                    {"IK", r2.all_of<IKComponent>(e2)},
                    {"ReflectionProbe", r2.all_of<ReflectionProbeComponent>(e2)},
                    {"Script", r2.all_of<ScriptComponent>(e2)},
                    {"UIElement", r2.all_of<sage::ui::Element>(e2)},
                    {"ParticleEmitter", r2.all_of<ParticleEmitterComponent>(e2)},
                };
                for (const Check& c : checks) {
                    if (!c.Present) {
                        LOG_ERROR("Editor") << "SELFTEST: компонент " << c.Name
                                            << " не пережил сохранение и загрузку сцены";
                        ok = false;
                    }
                }
                // Значения, а не только наличие: заготовка кнопки обязана
                // остаться кнопкой, а не панелью с чужими полями.
                if (ok) {
                    // Кнопка — это набор частей ПЛЮС объект-надпись внутри:
                    // подложка, не ловящая мышь, кнопкой не является, а надпись
                    // у кнопки больше не встроена — она отдельный ребёнок, и
                    // потеряться при сохранении может именно он.
                    bool button = r2.all_of<sage::ui::Interactable>(e2) &&
                                  r2.all_of<sage::ui::Fill>(e2);
                    bool caption = false;
                    if (const auto* h = r2.try_get<HierarchyComponent>(e2)) {
                        for (entt::entity child : h->Children) {
                            const sage::ui::Label* l = r2.try_get<sage::ui::Label>(child);
                            if (l && !l->Text.empty()) caption = true;
                        }
                    }
                    if (!button || !caption) {
                        LOG_ERROR("Editor") << "SELFTEST: элемент интерфейса приехал не кнопкой";
                        ok = false;
                    }
                    const ScriptComponent* sc = r2.try_get<ScriptComponent>(e2);
                    if (!sc || sc->Path != "assets/selftest_script.lua") {
                        LOG_ERROR("Editor") << "SELFTEST: путь скрипта не сохранился";
                        ok = false;
                    }
                }
            }
        }
    }

    // --- Видимость панелей: закрыть можно, но выход обязан быть ---
    //
    // Крестик на вкладке закрывал панель навсегда: в меню Window её не было, а
    // раскладка лежала в ini — то есть закрытое окно не возвращалось и после
    // перезапуска. Закрыв вкладки одну за другой, человек получал пустой
    // прямоугольник и решал, что «свернул весь редактор». Проверяем ровно это:
    // после закрытия ВСЕХ панелей редактор знает, что показывать нечего, а
    // m_panelRegistry.Restore() — то, что делают и «Сбросить раскладку», и кнопка на
    // пустом доке, — возвращает каждую панель.
    if (ok) {
        const bool visibleBefore = m_panelRegistry.AnyVisible();
        // Закрываем ВСЁ одним вызовом, а не перечислением: список панелей
        // растёт, и забытая в нём новая означает, что проверка проверяет не то,
        // что проверяла. Ровно это и случилось, когда вёрстка стала отдельным
        // пространством с четырьмя своими панелями.
        m_panelRegistry.CloseAll();
        const bool visibleAfterClose = m_panelRegistry.AnyVisible();
        m_panelRegistry.Restore();
        const bool restored = m_panels[EditorPanel::Hierarchy] && m_panels[EditorPanel::Inspector] && m_panels[EditorPanel::Environment] &&
                              m_panels[EditorPanel::Viewport] && m_panels[EditorPanel::Game] && m_panels[EditorPanel::Console] && m_panels[EditorPanel::Assets];
        if (!visibleBefore || visibleAfterClose || !restored) {
            LOG_ERROR("Editor") << "SELFTEST: закрытые панели не возвращаются "
                                << "(до " << visibleBefore << ", после закрытия " << visibleAfterClose
                                << ", восстановлены " << restored << ")";
            ok = false;
        }
    }

    // --- Кнопка в верхней панели и галка в меню — ОДНО И ТО ЖЕ поле --------
    //
    // Верхняя панель включает окна через PanelVisible(EditorPanel), меню Window
    // — через него же. Пока это был switch, руками сопоставляющий пункт
    // перечисления с полем, ошибка в одной строке означала бы кнопку, которая
    // открывает не своё окно, и галку, которая отмечает чужое. Проверяется
    // ТОЧНО: сравниваются адреса — ссылка обязана вести ровно в то поле,
    // которое рисует крестик на вкладке, и у двух разных панелей они не имеют
    // права совпасть.
    if (ok) {
        struct Mapping { EditorPanel Panel; bool* Field; const char* Name; };
        const Mapping mapping[] = {
            {EditorPanel::Hierarchy, &m_panels[EditorPanel::Hierarchy], "Hierarchy"},
            {EditorPanel::Inspector, &m_panels[EditorPanel::Inspector], "Inspector"},
            {EditorPanel::Environment, &m_panels[EditorPanel::Environment], "Environment"},
            {EditorPanel::Assets, &m_panels[EditorPanel::Assets], "Assets"},
            {EditorPanel::Console, &m_panels[EditorPanel::Console], "Console"},
            {EditorPanel::Profiler, &m_panels[EditorPanel::Profiler], "Profiler"},
            {EditorPanel::Game, &m_panels[EditorPanel::Game], "Game"},
            {EditorPanel::Viewport, &m_panels[EditorPanel::Viewport], "Viewport"},
            {EditorPanel::InterfaceViewport, &m_panels[EditorPanel::InterfaceViewport], "Interface canvas"},
            {EditorPanel::Settings, &m_panels[EditorPanel::Settings], "Settings"},
        };
        for (const Mapping& m : mapping) {
            if (&PanelVisible(m.Panel) != m.Field) {
                LOG_ERROR("Editor") << "SELFTEST: PanelVisible ведёт не в своё поле: " << m.Name;
                ok = false;
            }
        }
        // И ни одна панель не делит поле с другой: такая пара переключалась бы
        // парой, и одна из двух кнопок выглядела бы сломанной.
        for (size_t i = 0; i < std::size(mapping) && ok; ++i)
            for (size_t j = i + 1; j < std::size(mapping); ++j)
                if (mapping[i].Field == mapping[j].Field) {
                    LOG_ERROR("Editor") << "SELFTEST: панели " << mapping[i].Name << " и "
                                        << mapping[j].Name << " делят один флаг";
                    ok = false;
                }
    }

    // --- Панель ОТДЕЛЬНЫМ ОКНОМ СИСТЕМЫ (multi-window) ---------------------
    //
    // Проверяется не «панель можно вытащить мышью» — это ImGui умел и раньше, —
    // а ровно то, чего не было и из-за чего вытащенная панель не становилась
    // окном (см. PanelWindows.h):
    //
    //   1) ФЛАГ ВЬЮПОРТА. Пока окну не поставлен ImGuiViewportFlags_NoAutoMerge,
    //      ImGui СЛИВАЕТ его с главным, стоит на главное наехать: для системы
    //      такого окна нет — ни отдельной кнопки на панели задач, ни переноса
    //      на второй монитор. Проверяется прямо: после Before у следующего окна
    //      обязан стоять этот флаг — и обязан НЕ стоять у панели, живущей в
    //      доке, иначе в отдельное окно уедут все подряд.
    //   2) ПАМЯТЬ МЕЖДУ ЗАПУСКАМИ: состояние живёт в настройках редактора, иначе
    //      расстановку окон пришлось бы собирать заново каждый запуск.
    //   3) ПУТЬ НАЗАД: «собрать все окна в главное» возвращает ВСЕ, включая
    //      уехавшее на монитор, которого больше нет, — его иначе не достать.
    //
    // ОТСТЫКОВАННЫХ ПО УМОЛЧАНИЮ ПАНЕЛЕЙ БОЛЬШЕ НЕТ, и проверка это учитывает:
    // раньше редактор интерфейса открывался своим окном, потому что во вкладке
    // центрального дока его холсту оставалась треть экрана. Теперь вёрстка —
    // отдельное рабочее пространство со своими панелями, и выталкивать её
    // наружу незачем. Значит проверять надо ВОЗМОЖНОСТЬ, а не умолчание:
    // панель отстыковывается здесь же, явно.
    if (ok) {
        namespace prefs = sage::editor::prefs;
        // Настройки общие с живым редактором, поэтому состояние возвращается
        // как было: прогон не имеет права переставить человеку окна.
        const bool hadInspector = panelwindows::Detached("InterfaceInspector");
        const bool hadViewport = panelwindows::Detached("Viewport");

        panelwindows::ResetToDefaults();
        if (panelwindows::Detached("InterfaceInspector") || panelwindows::Detached("Viewport")) {
            LOG_ERROR("Editor") << "SELFTEST: по умолчанию панели живут в доке (свойства "
                                << panelwindows::Detached("InterfaceInspector") << ", вьюпорт "
                                << panelwindows::Detached("Viewport") << ")";
            ok = false;
        }
        // Флаг вьюпорта — то самое, без чего окно слипается с главным.
        if (ok) {
            ImGuiContext* ctx = ImGui::GetCurrentContext();
            panelwindows::SetDetached("InterfaceInspector", true);
            panelwindows::Before("InterfaceInspector");
            const bool detachedHasFlag =
                (ctx->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasWindowClass) != 0 &&
                (ctx->NextWindowData.WindowClass.ViewportFlagsOverrideSet &
                 ImGuiViewportFlags_NoAutoMerge) != 0;
            ctx->NextWindowData.ClearFlags();
            panelwindows::Before("Viewport");
            const bool dockedHasFlag =
                (ctx->NextWindowData.HasFlags & ImGuiNextWindowDataFlags_HasWindowClass) != 0 &&
                (ctx->NextWindowData.WindowClass.ViewportFlagsOverrideSet &
                 ImGuiViewportFlags_NoAutoMerge) != 0;
            // Заготовку следующего окна за собой убираем: иначе первый же
            // настоящий Begin в кадре получил бы чужой класс и чужой док.
            ctx->NextWindowData.ClearFlags();
            if (!detachedHasFlag || dockedHasFlag) {
                LOG_ERROR("Editor") << "SELFTEST: NoAutoMerge выставлен неверно (отдельное окно "
                                    << detachedHasFlag << ", панель в доке " << dockedHasFlag << ")";
                ok = false;
            }
        }

        // Память между запусками и путь назад.
        if (ok) {
            panelwindows::SetDetached("Hierarchy", true);
            const bool saved = prefs::GetBool("window.detached.Hierarchy", false);
            panelwindows::AttachAll();
            const bool allBack = !panelwindows::Detached("Hierarchy") &&
                                 !panelwindows::Detached("InterfaceInspector");
            const bool cleared = !prefs::GetBool("window.detached.Hierarchy", true);
            if (!saved || !allBack || !cleared) {
                LOG_ERROR("Editor") << "SELFTEST: состояние отдельных окон не сохраняется "
                                    << "(записалось " << saved << ", собрались " << allBack
                                    << ", стёрлось " << cleared << ")";
                ok = false;
            }
        }

        panelwindows::SetDetached("Hierarchy", false);
        panelwindows::SetDetached("InterfaceInspector", hadInspector);
        panelwindows::SetDetached("Viewport", hadViewport);
    }

    // --- Настройки редактора переживают запись и чтение ---------------------
    //
    // На них держится состав строки инструментов во вьюпорте: человек выключил
    // ненужную группу — и она обязана остаться выключенной после перезапуска.
    // Файл общий с языком и списком недавних проектов, поэтому проверяется
    // ИМЕННО чтение-правка-запись: запись состава не имеет права стереть
    // соседнюю настройку. Ключ после прогона возвращается как был.
    if (ok) {
        namespace prefs = sage::editor::prefs;
        const char* key = "selftest.roundtrip";
        const std::string lang = sage::editor::CurrentLanguageCode();
        prefs::SetBool(key, false);
        const bool readBackFalse = prefs::GetBool(key, true);
        prefs::SetBool(key, true);
        const bool readBackTrue = prefs::GetBool(key, false);
        const bool langKept = sage::editor::CurrentLanguageCode() == lang;
        if (readBackFalse || !readBackTrue || !langKept) {
            LOG_ERROR("Editor") << "SELFTEST: настройки редактора не сохраняются (false -> "
                                << readBackFalse << ", true -> " << readBackTrue
                                << ", язык цел " << langKept << ")";
            ok = false;
        }
    }

    return ok;
}
