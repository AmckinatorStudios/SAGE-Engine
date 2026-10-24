#include "SelfTestCommon.h"

// --- наклейки, память, сборка игры и режим Play ----------------------------
//
// Всё, что происходит ВОКРУГ сцены: подгрузка и вытеснение ресурсов, сборка
// игры в запускаемую папку, запуск сцены и возврат её по Stop, поведение
// сломанного скрипта. Общее у них — время: каждая проверка здесь чего-то ждёт
// или что-то откатывает.
bool EditorLayer::SelfTestSceneAndPlay() {
    bool ok = true;
    std::error_code ec;
    std::string err;
    (void)ec; (void)err;

    // --- Наклейки: строятся по сцене и переживают save/load ---
    //
    // Проверяется тот самый путь, которым наклейкой пользуются в редакторе:
    // поставили над полом -> она сама нашла геометрию -> сохранили сцену ->
    // перечитали -> она построилась заново. Геометрия проекции проверена
    // юнит-тестами; здесь важно, что параметры доезжают до файла и обратно, а
    // система собирает наклейку после загрузки без единого клика.
    if (ok) {
        GameObject floorObj = m_scene->CreateObject("Decal Floor");
        MeshRendererComponent& fmr = floorObj.Renderer();
        fmr.Ref = MeshRef{MeshRef::Type::Cube, ""};
        fmr.MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
        floorObj.GetTransform().Position = {20.0f, 0.0f, 0.0f};
        floorObj.GetTransform().Scale = {6.0f, 0.5f, 6.0f};

        GameObject dec = m_scene->CreateObject("Selftest Decal");
        dec.Renderer().Ref = MeshRef{MeshRef::Type::None, ""};
        dec.GetTransform().Position = {20.0f, 0.25f, 0.0f}; // верхняя грань пола
        dec.GetTransform().Rotation = {-90.0f, 0.0f, 0.0f}; // -Z смотрит вниз
        dec.GetTransform().Scale = {2.0f, 2.0f, 2.0f};
        DecalComponent dcInit;
        dcInit.AngleLimitDeg = 70.0f;
        dcInit.Offset = 0.012f;
        m_scene->Registry().emplace<DecalComponent>(dec.Entity(), dcInit);

        sage::ecs::BuildDecals(*m_scene, sage::ecs::MakeDecalMesh);
        const DecalComponent& built = m_scene->Registry().get<DecalComponent>(dec.Entity());
        if (built.Triangles <= 0 || !dec.Renderer().MeshPtr) {
            LOG_ERROR("Editor") << "SELFTEST: decal did not project onto the floor (triangles "
                                << built.Triangles << ")";
            ok = false;
        }

        if (ok) {
            fs::path decalScene = m_project.ScenesDir() / "selftest_decal.sage";
            if (!SaveSceneToFile(decalScene) || !LoadSceneFromFile(decalScene)) ok = false;
            if (ok) {
                GameObject back = m_scene->FindByName("Selftest Decal");
                const DecalComponent* dc =
                    back.Valid() ? m_scene->Registry().try_get<DecalComponent>(back.Entity())
                                 : nullptr;
                if (!dc || std::abs(dc->AngleLimitDeg - 70.0f) > 0.01f ||
                    std::abs(dc->Offset - 0.012f) > 1e-4f) {
                    LOG_ERROR("Editor") << "SELFTEST: decal params lost in scene round-trip";
                    ok = false;
                } else {
                    // Геометрии в файле нет и быть не должно — она пересобирается.
                    sage::ecs::BuildDecals(*m_scene, sage::ecs::MakeDecalMesh);
                    const DecalComponent& rebuilt =
                        m_scene->Registry().get<DecalComponent>(back.Entity());
                    if (rebuilt.Triangles <= 0 || !back.Renderer().MeshPtr) {
                        LOG_ERROR("Editor")
                            << "SELFTEST: decal not rebuilt after load (triangles "
                            << rebuilt.Triangles << ")";
                        ok = false;
                    }
                }
            }
        }
    }

    // --- Управление памятью: асинхронная загрузка + GC + LRU-вытеснение ---
    // Полный GPU-путь (у редактора есть GL-контекст): плейсхолдер приходит
    // мгновенно, фоновый поток декодирует файл, PumpAsyncUploads заливает его в
    // VRAM в тот же объект; неиспользуемые текстуры собираются GC; при
    // превышении бюджета LRU-неиспользуемые вытесняются.
    if (ok) {
        auto& rm = ResourceManager::Instance();

        // Пишем настоящий файл-картинку (несжатый TGA) рядом с проектом.
        auto writeTGA = [](const fs::path& path, int w, int h) -> bool {
            std::vector<unsigned char> buf;
            unsigned char hdr[18] = {0};
            hdr[2] = 2; hdr[12] = (unsigned char)(w & 0xFF); hdr[13] = (unsigned char)((w >> 8) & 0xFF);
            hdr[14] = (unsigned char)(h & 0xFF); hdr[15] = (unsigned char)((h >> 8) & 0xFF); hdr[16] = 24;
            buf.insert(buf.end(), hdr, hdr + 18);
            for (int i = 0; i < w * h; ++i) { buf.push_back(20); buf.push_back(120); buf.push_back(220); }
            std::ofstream f(path, std::ios::binary);
            if (!f) return false;
            f.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
            return (bool)f;
        };

        fs::path texA = m_project.Dir() / "selftest_texA.tga";
        fs::path texB = m_project.Dir() / "selftest_texB.tga";
        if (!writeTGA(texA, 32, 32) || !writeTGA(texB, 32, 32)) {
            LOG_ERROR("Editor") << "SELFTEST: could not write test textures";
            ok = false;
        }

        // 1) Async: мгновенно возвращается плейсхолдер 1x1, реальные пиксели
        //    приезжают после декодирования фоновым потоком + PumpAsyncUploads.
        if (ok) {
            auto tex = rm.GetTextureAsync(texA.string());
            bool placeholderOk = tex && tex->Width() == 1 && tex->Height() == 1;
            bool resident = false;
            for (int i = 0; i < 500 && !resident; ++i) {
                rm.PumpAsyncUploads();
                if (tex && tex->Width() == 32 && tex->Height() == 32) resident = true;
                else std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!placeholderOk || !resident) {
                LOG_ERROR("Editor") << "SELFTEST: async texture load failed (placeholder "
                                    << placeholderOk << ", resident " << resident << ")";
                ok = false;
            }
            // 2) GC: отпускаем ссылку — текстура становится неиспользуемой и
            //    выгружается (use_count()==1 держал только кэш).
            if (ok) {
                tex.reset();
                int freed = rm.GarbageCollectUnused();
                if (freed < 1) {
                    LOG_ERROR("Editor") << "SELFTEST: GarbageCollectUnused freed nothing after ref drop";
                    ok = false;
                }
            }
        }

        // 3) LRU-вытеснение: бюджет вмещает одну текстуру. Грузим A (не держим
        //    ссылку), затем B — загрузка B видит превышение и вытесняет
        //    неиспользуемую A. Резидентный размер не превышает бюджет.
        if (ok) {
            size_t oneTex = Texture::EstimateBytes(32, 32, true);
            rm.SetTextureBudgetBytes(oneTex + oneTex / 2); // < 2 текстур, >= 1
            rm.GetTexture(texA.string()); // ссылку не держим -> кандидат на вытеснение
            size_t beforeEvict = rm.GetStats().Evictions;
            rm.GetTexture(texB.string()); // триггерит EvictToBudget
            ResourceManager::Stats st = rm.GetStats();
            bool evicted = st.Evictions > beforeEvict;
            bool underBudget = st.TextureBytes <= st.TextureBudget;
            if (!evicted || !underBudget) {
                LOG_ERROR("Editor") << "SELFTEST: LRU eviction failed (evicted " << evicted
                                    << ", bytes " << st.TextureBytes << " / budget " << st.TextureBudget << ")";
                ok = false;
            }
            rm.SetTextureBudgetBytes(0);   // снимаем ограничение — не мешать остальным проверкам
            rm.GarbageCollectUnused();     // прибираем тестовые текстуры
        }

        std::error_code rmec;
        fs::remove(texA, rmec);
        fs::remove(texB, rmec);
    }

    // --- У КАЖДОГО ИНТЕРФЕЙСА СВОЙ КОНТЕКСТ ----------------------------------
    //
    // Жалоба: «два объекта с интерфейсом — в режиме вёрстки их элементы
    // смешаны, и править один, не задевая другой, можно только пряча чужие
    // объекты». Проверяем границу с обеих сторон: элементы разных интерфейсов
    // не попадают друг к другу, и редактор умеет показать ровно один.
    if (ok) {
        namespace sui = sage::ui;
        auto build = [&](const char* name, float x) {
            GameObject iface = m_scene->CreateEmptyObject(name);
            m_scene->Registry().emplace<sui::InterfaceComponent>(iface.Entity());
            GameObject root = m_scene->CreateEmptyObject(std::string(name) + "Root");
            sui::Element box;
            box.Anchor = UIAnchor::TopLeft;
            box.Position = {x, 10.0f};
            box.Size = {80.0f, 40.0f};
            m_scene->Registry().emplace<sui::Element>(root.Entity(), box);
            m_scene->Registry().emplace<sui::Fill>(root.Entity());
            m_scene->SetParent(root.Entity(), iface.Entity());
            return iface;
        };
        GameObject first = build("SelftestUiA", 10.0f);
        GameObject second = build("SelftestUiB", 300.0f);

        const std::vector<entt::entity> a = sui::InterfaceRoots(*m_scene, first.Entity());
        const std::vector<entt::entity> b = sui::InterfaceRoots(*m_scene, second.Entity());
        if (a.size() != 1 || b.size() != 1 || a[0] == b[0]) {
            LOG_ERROR("Editor") << "SELFTEST: элементы двух интерфейсов смешались (" << a.size()
                                << " и " << b.size() << ")";
            ok = false;
        }

        // Область редактора показывает ОДИН интерфейс — тот, что верстают.
        const size_t all = sui::SolveSceneRects(*m_scene, 1280, 720, true).size();
        const size_t one =
            sui::SolveSceneRects(*m_scene, 1280, 720, true, sui::UIScope::Only(first.Entity()))
                .size();
        if (all < 2 || one != 1) {
            LOG_ERROR("Editor") << "SELFTEST: область вёрстки не ограничивает интерфейс (всего "
                                << all << ", в области " << one << ")";
            ok = false;
        }

        // И редактор ВЫБИРАЕТ его сам: переключение в вёрстку с выделенным
        // элементом обязано открыть интерфейс ЭТОГО элемента, а не первый
        // попавшийся.
        const EditorWorkspace before = m_workspace;
        const int beforeInterface = m_currentInterface;
        m_selection.SetPrimary(m_scene->Registry().get<IdComponent>(b[0]).Id);
        m_currentInterface = -1;
        m_workspace = EditorWorkspace::Scene;
        SetWorkspace(EditorWorkspace::Interface);
        if (m_currentInterface != second.Id()) {
            LOG_ERROR("Editor") << "SELFTEST: вёрстка открылась не на том интерфейсе ("
                                << m_currentInterface << " вместо " << second.Id() << ")";
            ok = false;
        }
        m_workspace = before;
        m_currentInterface = beforeInterface;
        m_selection.Clear();

        // И КНОПКА «ОТКРЫТЬ В РЕЖИМЕ ИНТЕРФЕЙСА» у компонента ведёт туда же.
        //
        // Порядок шагов здесь неочевиден, и проверяется именно он: переход в
        // вёрстку СБРАСЫВАЕТ выделение, а прежний открытый интерфейс сохраняет,
        // пока тот жив. Значит выбор интерфейса и выделение обязаны идти ПОСЛЕ
        // перехода — иначе кнопка молча открывает не тот экран. Берём худший
        // случай: открыт интерфейс A, а кнопку жмут на элементе интерфейса B.
        m_workspace = EditorWorkspace::Scene;
        m_currentInterface = first.Id();
        sage::editor::OpenInInterfaceMode(*this, m_scene->Get(m_scene->Registry().get<IdComponent>(b[0]).Id));
        if (m_workspace != EditorWorkspace::Interface || m_currentInterface != second.Id() ||
            m_selection.Primary() != m_scene->Registry().get<IdComponent>(b[0]).Id) {
            LOG_ERROR("Editor") << "SELFTEST: «открыть в режиме интерфейса» открыло не то ("
                                << m_currentInterface << " вместо " << second.Id()
                                << ", выделено " << m_selection.Primary() << ")";
            ok = false;
        }
        m_workspace = before;
        m_currentInterface = beforeInterface;
        m_selection.Clear();

        m_scene->RemoveObject(first.Id());
        m_scene->RemoveObject(second.Id());
    }

    // --- ПОСТ-ОБРАБОТКА ПРИНАДЛЕЖИТ КАМЕРЕ -----------------------------------
    //
    // Раньше она была настройкой проекта: одна на все камеры, вне сцены, вне
    // префаба и вне отмены правки. Проверяем оба конца правила: камера без
    // компонента обрабатывается НЕ БУДЕТ вовсе, а камера с компонентом
    // обрабатывается СВОИМ трактом — и порядок звеньев в нём тот, в котором
    // кадр обрабатывается, а не тот, в котором их положили в список.
    if (ok) {
        using namespace sage::render;
        // В ТЕКУЩЕЙ сцене и со своей камерой: подменять сцену посреди прогона
        // нельзя — следующие проверки ждут её объекты.
        GameObject cam = m_scene->CreateEmptyObject("SelftestCamera");
        m_scene->Registry().emplace<CameraComponent>(cam.Entity());

        PostChain chain;
        if (ResolvePostChain(*m_scene, cam.Entity(), chain)) {
            LOG_ERROR("Editor") << "SELFTEST: камера без компонента всё равно обрабатывается";
            ok = false;
        }

        PostProcessComponent ppc;
        // Список НАРОЧНО в неправильном порядке: сглаживание перед
        // тон-маппингом, свечение после него. Раньше это был неверный тракт;
        // теперь порядок задают этапы, и он обязан разложиться сам.
        ppc.Chain.Effects.clear();
        for (const char* id : {"fxaa", "tonemap", "bloom", "vignette", "exposure"})
            ppc.Chain.Effects.push_back(MakePostEffect(id));
        m_scene->Registry().emplace<PostProcessComponent>(cam.Entity(), ppc);

        if (!ResolvePostChain(*m_scene, cam.Entity(), chain)) {
            LOG_ERROR("Editor") << "SELFTEST: компонент есть, а обработки нет";
            ok = false;
        } else {
            const PostChain ordered = chain.Ordered();
            auto at = [&ordered](const char* id) {
                for (size_t i = 0; i < ordered.Effects.size(); ++i)
                    if (ordered.Effects[i].Kind == id) return (int)i;
                return -1;
            };
            if (!(at("exposure") < at("bloom") && at("bloom") < at("tonemap") &&
                  at("tonemap") < at("vignette") && at("vignette") < at("fxaa"))) {
                LOG_ERROR("Editor") << "SELFTEST: порядок обработки не совпадает с этапами";
                ok = false;
            }
            if (!chain.Compile().Ok) {
                LOG_ERROR("Editor") << "SELFTEST: тракт камеры не компилируется";
                ok = false;
            }
        }

        // Выключенный компонент — обработки нет, но настройки целы.
        m_scene->Registry().get<PostProcessComponent>(cam.Entity()).Enabled = false;
        if (ResolvePostChain(*m_scene, cam.Entity(), chain)) {
            LOG_ERROR("Editor") << "SELFTEST: выключенная пост-обработка всё равно работает";
            ok = false;
        }
        if (m_scene->Registry().get<PostProcessComponent>(cam.Entity()).Chain.Effects.empty()) {
            LOG_ERROR("Editor") << "SELFTEST: выключение компонента стёрло его настройки";
            ok = false;
        }
        m_scene->RemoveObject(cam.Id());
    }

    // --- ИГРА БЕЗ СЦЕНЫ НЕ СОБИРАЕТСЯ ---------------------------------------
    //
    // Плеер при запуске открывает сцену проекта; сцен нет — он показывает
    // пустой экран. Сборка при этом проходила УСПЕШНО: пятьдесят мегабайт
    // файлов, зелёная строка «готово» — и нерабочая игра, причину которой
    // человек ищет вечер.
    //
    // Проверяем на ПУСТОЙ папке сцен: отсутствие сцены и пустая scenes/ — одно
    // и то же, и отказ обязан быть в обоих случаях.
    if (ok) {
        std::error_code sceneEc;
        const fs::path scenesDir = m_project.ScenesDir();
        const fs::path stash = m_project.Dir() / "scenes_stash";
        fs::remove_all(stash, sceneEc);
        fs::rename(scenesDir, stash, sceneEc);
        fs::create_directories(scenesDir, sceneEc);

        if (HasAnyScene()) {
            LOG_ERROR("Editor") << "SELFTEST: пустая папка сцен считается сценой";
            ok = false;
        }
        std::string noSceneErr;
        if (BuildGame("selftest_dist_noscene", noSceneErr)) {
            LOG_ERROR("Editor") << "SELFTEST: игра без единой сцены собралась";
            ok = false;
        }

        // Возвращаем сцены на место: следующая проверка собирает игру по-настоящему.
        fs::remove_all(scenesDir, sceneEc);
        fs::rename(stash, scenesDir, sceneEc);
        fs::remove_all(fs::path("selftest_dist_noscene"), sceneEc);
        if (!HasAnyScene()) {
            LOG_ERROR("Editor") << "SELFTEST: сцены не вернулись на место после проверки";
            ok = false;
        }
    }

    // --- Сборка игры: SagePlayer + project/ упакованы в запускаемую папку ---
    // (запуск упакованной игры проверяет smoke-тест 5/5 отдельным процессом)
    if (ok) {
        std::string buildErr;
        if (!BuildGame("selftest_dist", buildErr)) {
            LOG_ERROR("Editor") << "SELFTEST: game build failed: " << buildErr;
            ok = false;
        } else {
            fs::path gameDir = fs::path("selftest_dist") / m_project.Name();
#ifdef _WIN32
            fs::path exe = gameDir / (m_project.Name() + ".exe");
#else
            fs::path exe = gameDir / m_project.Name();
#endif
            // Проект едет ПАКЕТОМ (game.sagepak), а не папкой project/.
            // Проверяем и то, что папки нет: положить рядом и пакет, и россыпь
            // значило бы, что какой из них прочтут — дело случая.
            if (!fs::exists(exe, ec) || !fs::exists(gameDir / "game.sagepak", ec) ||
                fs::exists(gameDir / "project", ec)) {
                LOG_ERROR("Editor") << "SELFTEST: built game layout incomplete in " << gameDir.string();
                ok = false;
            }
            // ВСЁ — В ПАКЕТЕ: и ресурсы движка (шейдеры, шрифт), и оглавление
            // базы ассетов. Россыпь assets/ рядом с exe — это файлы, которые
            // игрок правит блокнотом, и ровно этого пакет и не должен допускать.
            if (fs::exists(gameDir / "assets", ec)) {
                LOG_ERROR("Editor") << "SELFTEST: рядом с игрой лежит россыпь assets/";
                ok = false;
            }
            sage::assets::PackReader built;
            if (!built.Open(gameDir / "game.sagepak") || built.Version() != 2 ||
                !built.Contains("assets/shaders/lit.frag") || !built.Contains("assetdb.json") ||
                !built.Contains("project.sageproj")) {
                LOG_ERROR("Editor") << "SELFTEST: в пакете игры нет шейдеров движка, оглавления "
                                       "базы или манифеста (или пакет не защищён)";
                ok = false;
            }
            // МАНИФЕСТА РЯДОМ С EXE НЕТ — он в пакете (см.
            // EngineConfig::BuildProjectFile). Отдельная копия была лишним
            // файлом: игра уже везёт всё содержимое проекта одним пакетом.
            if (fs::exists(gameDir / "project.sageproj", ec)) {
                LOG_ERROR("Editor") << "SELFTEST: рядом с игрой лежит project.sageproj, "
                                       "хотя сборка по умолчанию его не кладёт";
                ok = false;
            }
        }
    }

    // --- Пипетка за пределами окна: системное чтение экрана живое ---------
    //
    // Путь X11/GDI исполняется только у человека с мышью в руке — без этой
    // проверки он мог бы сломаться (не найти libX11, упасть в XGetImage) и
    // остаться незамеченным. Где системы нет (Wayland), проверять нечего.
    if (ok && sage::editor::screencolor::GlobalAvailable()) {
        float rgb[3] = {-1.0f, -1.0f, -1.0f};
        int sx = -1, sy = -1;
        if (!sage::editor::screencolor::SampleAtCursor(rgb, &sx, &sy) || rgb[0] < 0.0f ||
            rgb[0] > 1.0f || sx < 0 || sy < 0) {
            LOG_ERROR("Editor") << "SELFTEST: пипетка не прочла цвет с экрана";
            ok = false;
        }
    }

    // --- Сборка В ФОНЕ: окно не висит, полоса хода доходит до конца --------
    //
    // Пока сборка шла в главном потоке, редактор на настоящем проекте висел
    // белым окном десятки секунд. Проверяем, что StartBuildGame возвращается
    // СРАЗУ (до конца сборки), а поток доводит её до успеха и 100 %.
    if (ok) {
        std::string startErr;
        if (!StartBuildGame("selftest_dist_async", startErr)) {
            LOG_ERROR("Editor") << "SELFTEST: фоновая сборка не запустилась: " << startErr;
            ok = false;
        } else {
            for (int i = 0; i < 6000 && m_builder.Running(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (!m_builder.Finished() || !m_builder.Succeeded() || m_builder.Progress() < 0.999f) {
                LOG_ERROR("Editor") << "SELFTEST: фоновая сборка не дошла до конца: "
                                    << m_builder.Error();
                ok = false;
            }
            std::error_code rmec;
            fs::remove_all("selftest_dist_async", rmec);
        }
    }

    // --- Тот же параметр, включённый: копия манифеста появляется ------------
    //
    // Выключатель без проверки — это обещание. Ради него он и заведён: игру
    // запускают, перетаскивая project.sageproj на плеер, и если включённый
    // параметр ничего не меняет, способ запуска молча пропадает.
    if (ok) {
        const bool saved = m_settings.BuildProjectFile;
        m_settings.BuildProjectFile = true;
        std::string buildErr;
        if (!BuildGame("selftest_dist_manifest", buildErr)) {
            LOG_ERROR("Editor") << "SELFTEST: сборка с манифестом не прошла: " << buildErr;
            ok = false;
        } else {
            const fs::path dir = fs::path("selftest_dist_manifest") / m_project.Name();
            if (!fs::exists(dir / "project.sageproj", ec)) {
                LOG_ERROR("Editor") << "SELFTEST: BuildProjectFile=true не положил манифест рядом";
                ok = false;
            }
            std::error_code rmec;
            fs::remove_all("selftest_dist_manifest", rmec);  // артефакт, не данные
        }
        m_settings.BuildProjectFile = saved;
    }

    // --- Play: скрипт вращает сущность, Stop откатывает сцену к снапшоту ---
    if (ok) {
        GameObject green = m_scene->FindByName("Green Cube");
        if (!green.Valid()) {
            LOG_ERROR("Editor") << "SELFTEST: Green Cube not found for play test";
            ok = false;
        } else {
            m_scene->Registry().emplace_or_replace<ScriptComponent>(
                green.Entity(), ScriptComponent{"assets/scripts/spin.lua"});
            float rotBefore = green.GetTransform().Rotation.y;

            StartPlay();
            // Тики ЧЕРЕЗ ПЛАНИРОВЩИК, а не вызовом m_play.Scripts()->UpdateAll:
            // self-test выполняется до главного цикла, но проверять он обязан
            // тот же путь, которым идёт настоящий кадр (см. OnUpdate). Прямой
            // вызов проверял только сам ScriptEngine — и потому пропустил
            // ровно ту поломку, ради которой этот тест и написан: StartPlay не
            // регистрировал скрипты в планировщике, Play «работал» в тесте и
            // не делал ничего в редакторе.
            for (int i = 0; i < 5; ++i) m_systems.Run(*m_scene, 0.1f);
            float rotDuring = m_scene->FindByName("Green Cube").GetTransform().Rotation.y;
            StopPlay();
            float rotAfter = m_scene->FindByName("Green Cube").GetTransform().Rotation.y;

            if (std::abs(rotDuring - rotBefore) < 1.0f) {
                LOG_ERROR("Editor") << "SELFTEST: play failed - script did not rotate entity ("
                                    << rotBefore << " -> " << rotDuring << ")";
                ok = false;
            }
            if (ok && std::abs(rotAfter - rotBefore) > 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: stop failed - scene not restored (rot "
                                    << rotAfter << ", expected " << rotBefore << ")";
                ok = false;
            }
        }
    }

    // --- ПЕРЕХОД МЕЖДУ УРОВНЯМИ ПРЯМО В РЕДАКТОРЕ ---------------------------
    //
    // `scene:Load("...")` в Play-режиме раньше не делал НИЧЕГО — редактор писал
    // в лог «проверяйте переходы в собранной игре». То есть самую частую ошибку
    // уровня (не та сцена, не тот спавн, потерянный игрок) нельзя было увидеть
    // там, где её правят.
    //
    // Проверяется путь целиком: две сцены проекта, скрипт-портал, Play,
    // переход — и Stop, который обязан вернуть человеку ЕГО документ, а не тот
    // уровень, на котором игра закончилась.
    if (ok) {
        // Своя сцена на время проверки: следующие шаги self-test'а работают с
        // той, что была открыта, и оставить им чужую значило бы уронить их
        // проверки, не имеющие к переходам никакого отношения.
        const std::string sceneBefore = SceneSerializer::SaveToString(*m_scene);
        std::error_code sceneEc;
        fs::create_directories(m_project.Dir() / "assets" / "scripts", sceneEc);
        {
            std::ofstream f(m_project.Dir() / "assets" / "scripts" / "selftest_portal.lua");
            // Заодно отладочная графика: заказ из скрипта обязан долетать до
            // буфера кадра, а не быть объявленной и молчащей функцией.
            f << "local P = {}\n"
              << "function P:Update(dt)\n"
              << "    Debug:DrawLine(Vector3(0, 0, 0), Vector3(0, 2, 0))\n"
              << "    scene:Load(\"selftest_level2\")\n"
              << "end\n"
              << "return P\n";
        }

        // Сцена-назначение: узнаётся по имени объекта.
        NewScene(ProjectTemplateKind::Empty);
        m_scene->CreateObject("SelfTestLevelTwoMarker");
        const fs::path secondPath = m_project.ScenesDir() / "selftest_level2.sage";
        if (!SaveSceneToFile(secondPath)) {
            LOG_ERROR("Editor") << "SELFTEST: вторая сцена не сохранилась";
            ok = false;
        }

        // Сцена-источник: в ней портал.
        if (ok) {
            NewScene(ProjectTemplateKind::Empty);
            GameObject portal = m_scene->CreateObject("SelfTestPortal");
            m_scene->Registry().emplace<ScriptComponent>(
                portal.Entity(), ScriptComponent{"assets/scripts/selftest_portal.lua"});
            const fs::path firstPath = m_project.ScenesDir() / "selftest_level1.sage";
            if (!SaveSceneToFile(firstPath)) {
                LOG_ERROR("Editor") << "SELFTEST: первая сцена не сохранилась";
                ok = false;
            }
        }

        if (ok) {
            StartPlay();
            // Через планировщик и обработку запросов кадра — тем же путём, каким
            // идёт настоящий кадр редактора (см. OnUpdate).
            bool drewDebug = false;
            for (int i = 0; i < 3 && m_play.Active(); ++i) {
                m_systems.Run(*m_scene, 0.05f);
                if (m_play.Scripting() && !m_play.Scripting()->Debug().Empty()) drewDebug = true;
                ProcessScriptRequests();
            }
            if (!drewDebug) {
                LOG_ERROR("Editor") << "SELFTEST: Debug:DrawLine из скрипта не дошёл до кадра";
                ok = false;
            }
            const bool switched = m_scene->FindByName("SelfTestLevelTwoMarker").Valid();
            if (!switched) {
                LOG_ERROR("Editor") << "SELFTEST: scene:Load в Play не переключил сцену";
                ok = false;
            }
            StopPlay();
            // Stop возвращает ДОКУМЕНТ, а не уровень, на котором игра кончилась.
            if (ok && !m_scene->FindByName("SelfTestPortal").Valid()) {
                LOG_ERROR("Editor") << "SELFTEST: после Stop не вернулась исходная сцена";
                ok = false;
            }
        }

        // Стартовая сцена проекта: назначается и переживает перечитывание.
        if (ok) {
            std::string startErr;
            if (!m_project.SetStartScene("selftest_level1", startErr) ||
                m_project.StartScene() != "selftest_level1") {
                LOG_ERROR("Editor") << "SELFTEST: стартовая сцена не назначилась: " << startErr;
                ok = false;
            }
        }
        // Возвращаем ту сцену, с которой пришли: дальше её ждут другие проверки.
        RestoreSceneFromString(sceneBefore);
        if (ok) LOG_INFO("Editor") << "SELFTEST: scene-switch OK";
    }

    // --- Скрипты глазами РЕДАКТОРА: путь проекта, поломка, повторный Play ---
    //
    // Проверка выше показывает, что Play вообще исполняет скрипт. Здесь — то,
    // на чём Play спотыкается в жизни:
    //
    //   • скрипт лежит В ПРОЕКТЕ и записан относительным путём. Текущая папка
    //     редактора — не папка проекта, и без отдельного разрешения пути такой
    //     скрипт работал бы в собранной игре и молчал в Play;
    //   • один скрипт сломан. Play обязан запуститься и отработать остальные:
    //     иначе одна опечатка в одном объекте выключает игру целиком;
    //   • Play нажимают ДВАЖДЫ. Второй запуск обязан дать тот же результат, а
    //     не удвоенный (два экземпляра скрипта на сущности) и не нулевой.
    if (ok) {
        std::error_code scriptEc;
        fs::create_directories(m_project.Dir() / "assets" / "scripts", scriptEc);
        {
            std::ofstream f(m_project.Dir() / "assets" / "scripts" / "selftest_move.lua");
            f << "function OnUpdate(entity, dt)\n"
              << "    entity.Transform.Position.x = entity.Transform.Position.x + dt\n"
              << "end\n";
        }
        {
            // Синтаксически битый — намеренно.
            std::ofstream f(m_project.Dir() / "assets" / "scripts" / "selftest_broken.lua");
            f << "function OnUpdate( end\n";
        }

        GameObject mover = m_scene->CreateObject("SelftestMover");
        m_scene->Registry().emplace<ScriptComponent>(mover.Entity(),
                                                     ScriptComponent{"assets/scripts/selftest_move.lua"});
        GameObject broken = m_scene->CreateObject("SelftestBroken");
        m_scene->Registry().emplace<ScriptComponent>(broken.Entity(),
                                                     ScriptComponent{"assets/scripts/selftest_broken.lua"});
        GameObject missing = m_scene->CreateObject("SelftestMissing");
        m_scene->Registry().emplace<ScriptComponent>(
            missing.Entity(), ScriptComponent{"assets/scripts/selftest_no_such_file.lua"});

        // ТОЛЬКО номера, не дескрипторы: Stop восстанавливает сцену из снапшота,
        // то есть создаёт НОВЫЙ реестр ECS, а старые GameObject держат указатель
        // на разрушенный — обращение к ним после Stop это чтение освобождённой
        // памяти. Проверено падением: первая версия этой проверки уносила
        // редактор в SIGSEGV на строке уборки объектов.
        const int moverId = mover.Id();
        const int brokenId = broken.Id();
        const int missingId = missing.Id();
        const float xBefore = mover.GetTransform().Position.x;

        float xFirst = xBefore;
        for (int run = 0; run < 2 && ok; ++run) {
            StartPlay();
            for (int i = 0; i < 10; ++i) m_systems.Run(*m_scene, 0.1f);
            const float xDuring = m_scene->Get(moverId).GetTransform().Position.x;
            StopPlay();

            const float moved = xDuring - xBefore;
            // Ожидаем ~1.0 (десять кадров по 0.1). Допуск широкий, но верхняя
            // граница ОБЯЗАТЕЛЬНА: без неё двойная привязка скрипта (два
            // экземпляра на сущности) прошла бы проверку незамеченной.
            if (moved < 0.5f || moved > 1.5f) {
                LOG_ERROR("Editor") << "SELFTEST: script from project moved entity by " << moved
                                    << " (expected ~1.0) on play run " << (run + 1);
                ok = false;
            }
            if (run == 0) xFirst = moved;
            else if (ok && std::abs(moved - xFirst) > 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: second play run differs (" << xFirst << " -> "
                                    << moved << ")";
                ok = false;
            }
        }

        // Снятый компонент — снятое поведение: следующий Play уже не двигает.
        if (ok) {
            m_scene->Registry().remove<ScriptComponent>(m_scene->Get(moverId).Entity());
            const float xIdle = m_scene->Get(moverId).GetTransform().Position.x;
            StartPlay();
            for (int i = 0; i < 5; ++i) m_systems.Run(*m_scene, 0.1f);
            const float xAfterIdle = m_scene->Get(moverId).GetTransform().Position.x;
            StopPlay();
            if (std::abs(xAfterIdle - xIdle) > 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: script kept running after component removal";
                ok = false;
            }
        }

        m_scene->RemoveObject(moverId);
        m_scene->RemoveObject(brokenId);
        m_scene->RemoveObject(missingId);
    }

    // --- Скрипт, падающий КАЖДЫЙ КАДР, не заливает консоль -----------------
    //
    // Проверка живёт здесь, а не в модульных тестах движка, потому что смотрят
    // на это в РЕДАКТОРЕ: ошибка в OnUpdate повторяется шестьдесят раз в
    // секунду, и без ограничения панель Console за минуту получает три с
    // половиной тысячи одинаковых строк — в ней перестаёт быть видно всё
    // остальное. Считаем по самой панели: именно её читает человек.
    if (ok) {
        {
            std::ofstream f(m_project.Dir() / "assets" / "scripts" / "selftest_boom.lua");
            f << "function OnUpdate(entity, dt) error('selftest boom') end\n";
        }
        GameObject boom = m_scene->CreateObject("SelftestBoom");
        m_scene->Registry().emplace<ScriptComponent>(boom.Entity(),
                                                     ScriptComponent{"assets/scripts/selftest_boom.lua"});
        const int boomId = boom.Id();

        const int errorsBefore = m_console.ErrorCount();
        StartPlay();
        for (int i = 0; i < 60; ++i) m_systems.Run(*m_scene, 1.0f / 60.0f); // секунда игры
        StopPlay();
        const int added = m_console.ErrorCount() - errorsBefore;

        // Ожидаем два сообщения: сама ошибка и «скрипт отключён». Порог с
        // запасом (пять), но НАМНОГО меньше шестидесяти — иначе проверка не
        // отличала бы починенное поведение от прежнего.
        if (added > 5) {
            LOG_ERROR("Editor") << "SELFTEST: broken script flooded the console with " << added
                                << " errors in one second of play";
            ok = false;
        }
        if (ok && added == 0) {
            LOG_ERROR("Editor") << "SELFTEST: broken script produced NO error in the console";
            ok = false;
        }
        m_scene->RemoveObject(boomId);
    }

    return ok;
}



// --- ПИПЕТКА СЛОТА ОБЪЕКТА И ПРЕФАБЫ БРОСКОМ ---------------------------------
//
// Пипетка: щелчок по объекту во вьюпорте, пока она включена, обязан уйти в
// поле, а не сменить выбор — иначе инспектор переключился бы и поле пропало.
// Префабы: объект из иерархии в Assets — новый файл; префаб на строку
// объекта — его ребёнок (подсказка обещала это давно, а бросок не делал
// ничего); префаб во вьюпорт мимо объектов — на пол, а не в воздух.
bool EditorLayer::SelfTestPickerAndPrefabs() {
    bool ok = true;
    // В ТЕКУЩЕЙ сцене и далеко от её объектов: следующие проверки (мышь во
    // вьюпорте) работают с демо-сценой, и подменять её пустой нельзя. Всё
    // созданное здесь в конце убирается.
    std::vector<int> made;
    const int selectedBefore = m_selection.Primary();
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
    const glm::vec3 far(3000.0f, 0.0f, 3000.0f);

    // --- пипетка ---
    {
        GameObject cube = CreatePrimitiveEntity("PickMe", MeshRef::Type::Cube);
        GameObject other = CreatePrimitiveEntity("Holder", MeshRef::Type::Cube);
        made.push_back(cube.Id());
        made.push_back(other.Id());
        cube.GetTransform().Position = far;
        other.GetTransform().Position = far + glm::vec3(50.0f, 0.0f, 0.0f);
        m_selection.SetPrimary(other.Id());
        const glm::mat4 view = glm::lookAt(far + glm::vec3(0.0f, 0.0f, 8.0f), far, glm::vec3(0, 1, 0));
        constexpr unsigned int kField = 0x5A6E01u;
        objectslot::StartPick(kField);
        PickAtViewportWith(view, proj, 0.5f, 0.5f, /*additive=*/false);
        int got = 0;
        if (!objectslot::TakeDelivered(kField, got) || got != cube.Id()) {
            LOG_ERROR("Editor") << "SELFTEST: пипетка слота объекта не получила объект из вьюпорта";
            ok = false;
        }
        if (m_selection.Primary() != other.Id()) {
            LOG_ERROR("Editor") << "SELFTEST: пипетка сменила выбор вместо того, чтобы заполнить поле";
            ok = false;
        }
        objectslot::Cancel();
    }

    // --- объект в Assets — префаб; префаб на объект — ребёнок ---
    fs::path prefab;
    std::error_code ec;
    {
        GameObject src = CreatePrimitiveEntity("SelftestLamp", MeshRef::Type::Sphere);
        made.push_back(src.Id());
        std::string err;
        prefab = SaveObjectAsPrefab(src.Id(), m_project.Dir() / "assets", err);
        if (prefab.empty() || !fs::exists(prefab, ec)) {
            LOG_ERROR("Editor") << "SELFTEST: объект, брошенный в Assets, не стал префабом: " << err;
            ok = false;
        }
        // Второй бросок того же объекта не затирает первый файл.
        const fs::path second = SaveObjectAsPrefab(src.Id(), m_project.Dir() / "assets", err);
        if (ok && (second.empty() || second == prefab)) {
            LOG_ERROR("Editor") << "SELFTEST: второй префаб с тем же именем затёр первый";
            ok = false;
        }
        if (!second.empty()) fs::remove(second, ec);
    }
    if (ok) {
        GameObject parent = CreatePrimitiveEntity("SelftestPole", MeshRef::Type::Cube);
        made.push_back(parent.Id());
        parent.GetTransform().Position = far + glm::vec3(0.0f, 0.0f, 40.0f);
        const size_t before = m_scene->Count();
        const bool applied = ApplyAssetToEntity(parent.Id(), prefab);
        GameObject child = m_scene->Get(m_selection.Primary());
        if (child.Valid() && child.Id() != parent.Id()) made.push_back(child.Id());
        if (!applied || m_scene->Count() <= before || !child.Valid() ||
            m_scene->ParentOf(child.Entity()) != parent.Entity()) {
            LOG_ERROR("Editor") << "SELFTEST: префаб, брошенный на объект, не лёг внутрь него";
            ok = false;
        }
    }
    if (ok) {
        // Камера далеко от всех объектов и смотрит вниз-вперёд: луч мимо
        // объектов обязан лечь на пол (y = 0).
        const glm::vec3 eye = far + glm::vec3(900.0f, 10.0f, 900.0f);
        const glm::mat4 view = glm::lookAt(eye, eye + glm::vec3(0.0f, -10.0f, -10.0f), glm::vec3(0, 1, 0));
        if (!DropAssetAtViewport(view, proj, 0.5f, 0.5f, prefab)) {
            LOG_ERROR("Editor") << "SELFTEST: префаб не бросается во вьюпорт";
            ok = false;
        } else {
            GameObject placed = m_scene->Get(m_selection.Primary());
            if (placed.Valid()) made.push_back(placed.Id());
            const float y = placed.Valid() ? placed.GetTransform().Position.y : 99.0f;
            if (std::fabs(y) > 0.05f) {
                LOG_ERROR("Editor") << "SELFTEST: префаб мимо объектов повис в воздухе (y = " << y << ")";
                ok = false;
            }
        }
    }
    if (!prefab.empty()) fs::remove(prefab, ec);
    // Детей убирает удаление родителя; уже удалённые номера пропускаются.
    for (auto it = made.rbegin(); it != made.rend(); ++it)
        if (m_scene->Get(*it).Valid()) m_scene->RemoveObject(*it);
    m_selection.SetPrimary(selectedBefore);
    return ok;
}
