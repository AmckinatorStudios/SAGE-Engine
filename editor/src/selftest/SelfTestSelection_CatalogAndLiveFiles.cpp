#include "SelfTestCommon.h"

bool EditorLayer::SelfTestCatalogAndLiveFiles() {
    bool ok = true;
    std::error_code ec;
    std::string err;
    (void)ec; (void)err;
    // --- КАТАЛОГ ОБЪЕКТОВ: КАЖДЫЙ ПУНКТ ДАЁТ ГОТОВЫЙ ПРЕДМЕТ ----------------
    //
    // Что было: два РАЗНЫХ списка «создать» — в меню «Объект» полтора десятка
    // пунктов плоским перечнем, а под правой кнопкой в иерархии всего два
    // («пустой» и «куб»). Часть пунктов при этом выглядела отладочными
    // кнопками автора («Physics Cube»), а «пустой объект» приходил с
    // MeshRenderer, то есть пустым не был.
    //
    // Проверяется ровно это: список ОДИН и РАЗЛОЖЕН по категориям, каждый его
    // ключ действительно создаёт объект, пустой объект пуст, а шаблоны
    // приезжают настроенными — у ящика есть тело и коллайдер, у прожектора
    // поворот вниз, у дыма конфиг именно дыма.
    if (ok) {
        NewScene(ProjectTemplateKind::Empty);
        namespace catalog = sage::editor::objectcatalog;
        const catalog::Group root = catalog::Catalog();

        // РАЗЛОЖЕН, А НЕ ПЛОСКИЙ. Ради этого всё и делалось: одним списком из
        // сорока пунктов пользоваться нельзя.
        bool hasSubGroups = false;
        for (const catalog::Group& g : root.Groups)
            if (!g.Groups.empty()) hasSubGroups = true;
        if (root.Groups.size() < 6 || !hasSubGroups) {
            LOG_ERROR("Editor") << "SELFTEST: каталог объектов не разложен по категориям: категорий "
                                << root.Groups.size() << ", под-списки " << (hasSubGroups ? "есть" : "нет");
            ok = false;
        }

        // Все ключи каталога — плоским списком, включая вложенные.
        std::vector<std::string> ids;
        std::function<void(const catalog::Group&)> collect = [&](const catalog::Group& g) {
            for (const catalog::Item& it : g.Items) ids.push_back(it.Id);
            for (const catalog::Group& sub : g.Groups) collect(sub);
        };
        collect(root);
        if (ids.size() < 25) {
            LOG_ERROR("Editor") << "SELFTEST: в каталоге объектов всего пунктов: " << ids.size();
            ok = false;
        }

        // КАЖДЫЙ пункт обязан что-то создать. Пункт, который внешне сработал и
        // не сделал ничего, — худший вид поломки: о нём не сообщают.
        for (const std::string& id : ids) {
            const int newId = CreateCatalogObject(id);
            if (newId < 0 || !m_scene->Get(newId).Valid()) {
                LOG_ERROR("Editor") << "SELFTEST: пункт каталога ничего не создал: " << id;
                ok = false;
                break;
            }
        }

        // Пустой — ДЕЙСТВИТЕЛЬНО пустой.
        if (ok) {
            const int emptyId = CreateCatalogObject("empty");
            GameObject empty = m_scene->Get(emptyId);
            if (!empty.Valid() || m_scene->Registry().all_of<MeshRendererComponent>(empty.Entity())) {
                LOG_ERROR("Editor") << "SELFTEST: пустой объект приехал с компонентом Меш";
                ok = false;
            }
        }

        // Шаблон, а не заготовка под ручную настройку: тело + коллайдер.
        if (ok) {
            GameObject crate = m_scene->Get(CreateCatalogObject("physics.box"));
            if (!crate.Valid() ||
                !m_scene->Registry().all_of<RigidBodyComponent>(crate.Entity()) ||
                !m_scene->Registry().all_of<ColliderComponent>(crate.Entity()) ||
                m_scene->Registry().get<RigidBodyComponent>(crate.Entity()).Type !=
                    sage::physics::BodyType::Dynamic) {
                LOG_ERROR("Editor") << "SELFTEST: падающий ящик приехал без тела или коллайдера";
                ok = false;
            }
        }

        // Зона-триггер: пропускает сквозь себя и НЕ рисуется.
        if (ok) {
            GameObject trig = m_scene->Get(CreateCatalogObject("physics.trigger"));
            if (!trig.Valid() ||
                !m_scene->Registry().all_of<ColliderComponent>(trig.Entity()) ||
                !m_scene->Registry().get<RigidBodyComponent>(trig.Entity()).Sensor ||
                m_scene->Registry().all_of<MeshRendererComponent>(trig.Entity())) {
                LOG_ERROR("Editor") << "SELFTEST: зона-триггер не сквозная или видимая";
                ok = false;
            }
        }

        // Прожектор смотрит ВНИЗ: свет «в никуда» выглядит неработающим.
        if (ok) {
            GameObject spot = m_scene->Get(CreateCatalogObject("light.spot"));
            if (!spot.Valid() ||
                !m_scene->Registry().all_of<LightComponent>(spot.Entity()) ||
                m_scene->Registry().get<LightComponent>(spot.Entity()).Kind !=
                    LightComponent::Type::Spot ||
                spot.GetTransform().Rotation.x > -45.0f) {
                LOG_ERROR("Editor") << "SELFTEST: прожектор создан без типа или не смотрит вниз";
                ok = false;
            }
        }

        // Частицы: единственный пункт даёт эмиттер. Старые ключи «fx.particles.*»
        // (избранное, шаблоны прежних версий) ведут туда же, а не в пустоту.
        if (ok) {
            for (const char* key : {"fx.particles", "fx.particles.smoke"}) {
                GameObject p = m_scene->Get(CreateCatalogObject(key));
                if (!p.Valid() || !m_scene->Registry().all_of<ParticleEmitterComponent>(p.Entity())) {
                    LOG_ERROR("Editor") << "SELFTEST: пункт каталога «" << key << "» не дал эмиттер частиц";
                    ok = false;
                }
            }
        }

        // Опечатка в ключе не должна выглядеть как удачное создание.
        if (ok && CreateCatalogObject("shape.nonexistent") >= 0) {
            LOG_ERROR("Editor") << "SELFTEST: неизвестный ключ каталога создал объект";
            ok = false;
        }
    }

    // --- МОДЕЛЬ СО СКЕЛЕТОМ СТАНОВИТСЯ ГОТОВЫМ ПЕРСОНАЖЕМ --------------------
    //
    // Что было: файл со скином честно разбирался (кости, клипы), клипы даже
    // выкладывались файлами .sageanim рядом с моделью — а сущность получала
    // ОДИН Mesh. Ни компонента Animation, ни назначенного клипа, ни поднятого
    // скелета: в кадре стояла статуя в позе привязки, и человек справедливо
    // говорил «модель грузится без костей, анимации нет». Настроить это руками
    // можно было, только зная три неочевидные вещи разом (см.
    // ModelMaterialImport.h).
    //
    // Проверяется ВЕСЬ путь тем же жестом, каким модель приносят: бросок в
    // сцену -> компонент Animation -> клип файлом -> живой скелет с привязанным
    // клипом. И отдельно — что декорации это не касается: модель без костей
    // компонента получать не должна, иначе он висел бы на каждом камне.
    if (ok) {
        std::error_code ec;
        const fs::path rigDir = m_project.AssetsDir() / "selftest_rig";
        fs::remove_all(rigDir, ec);
        std::string rigErr;
        if (!WriteRigFixture(rigDir, "rig", true, rigErr)) {
            LOG_ERROR("Editor") << "SELFTEST: не записать эталон скелетной модели: " << rigErr;
            ok = false;
        } else {
            sage::AssetDatabase::Instance().ScanProject(m_project.Dir().string());
            const size_t before = m_scene->Count();
            if (!AddAssetToScene(rigDir / "rig.gltf") || m_scene->Count() != before + 1) {
                LOG_ERROR("Editor") << "SELFTEST: скелетная модель не встала в сцену";
                ok = false;
            } else {
                entt::registry& reg = m_scene->Registry();
                const entt::entity e = m_scene->Get(m_selection.Primary()).Entity();
                const AnimationComponent* am = reg.try_get<AnimationComponent>(e);
                if (!am) {
                    LOG_ERROR("Editor") << "SELFTEST: у модели со скелетом нет компонента Animation";
                    ok = false;
                } else if (am->ClipPath.empty()) {
                    LOG_ERROR("Editor") << "SELFTEST: клип не назначен (в модели их два)";
                    ok = false;
                } else if (am->ClipPath.find("Idle") == std::string::npos) {
                    // «Jumpscare» в файле ПЕРВЫЙ — значит, взять первый попавшийся
                    // мало: персонаж встречал бы человека прыжком.
                    LOG_ERROR("Editor") << "SELFTEST: клипом по умолчанию взят не Idle, а "
                                        << am->ClipPath;
                    ok = false;
                } else if (!fs::exists(sage::AssetDatabase::Instance().LocatePath(am->ClipPath), ec)) {
                    LOG_ERROR("Editor") << "SELFTEST: файла клипа нет на диске: " << am->ClipPath;
                    ok = false;
                }

                // Скелет обязан ПОДНЯТЬСЯ, а клип — лечь на него. Назначенный
                // файл сам по себе ничего не значит: клип от другого персонажа
                // тоже назначается, и играет при этом половина костей.
                if (ok) {
                    sage::anim::UpdateAnimators(*m_scene, 0.016f);
                    const AnimationComponent& live = reg.get<AnimationComponent>(e);
                    // Три: две кости скина и их предок-узел (он двигает их своим
                    // трансформом и тоже стал костью, см. SkinnedModel.cpp).
                    if (!live.Model || live.Model->GetSkeleton().Count() != 3) {
                        LOG_ERROR("Editor") << "SELFTEST: скелет не поднялся (костей "
                                            << (live.Model ? live.Model->GetSkeleton().Count() : -1)
                                            << ", ожидалось 3)";
                        ok = false;
                    } else if (live.OwnClips.empty() || live.MissingBones != 0) {
                        LOG_ERROR("Editor") << "SELFTEST: клип не лёг на скелет (клипов "
                                            << live.OwnClips.size() << ", костей мимо "
                                            << live.MissingBones << ")";
                        ok = false;
                    }
                }

                // РАЗВЁРТКА. В эталоне их две, и вторая намеренно другая:
                // движок обязан взять TEXCOORD_0. Перепутанная развёртка — это
                // «текстура наехала не туда», и на одной развёртке поймать это
                // нечем.
                if (ok) {
                    const std::shared_ptr<sage::render::SkinnedModel> rig =
                        ResourceManager::Instance().GetSkinnedModel(
                            m_project.AssetRef(rigDir / "rig.gltf"));
                    const std::shared_ptr<Mesh> asStatic =
                        ResourceManager::Instance().GetModel(m_project.AssetRef(rigDir / "rig.gltf"));
                    const std::vector<Vertex>* verts = asStatic ? asStatic->CpuVertices() : nullptr;
                    if (!rig || !verts || verts->size() != 3) {
                        LOG_ERROR("Editor") << "SELFTEST: эталон скелетной модели не читается "
                                            << "статическим загрузчиком";
                        ok = false;
                    } else {
                        // Вершина 1 в TEXCOORD_0 = (1,0), в TEXCOORD_1 = (0.5,0.75).
                        // Статика переводит v в соглашение своих текстур
                        // (перевёрнутых под OpenGL, см. GltfImporter.cpp):
                        // (1,0) glTF — это (1,1) у статического меша.
                        const glm::vec2 uv = (*verts)[1].TexCoords;
                        if (std::abs(uv.x - 1.0f) > 1e-3f || std::abs(uv.y - 1.0f) > 1e-3f) {
                            LOG_ERROR("Editor") << "SELFTEST: взята не та развёртка: uv=("
                                                << uv.x << ", " << uv.y << "), ожидалось (1, 1)";
                            ok = false;
                        }
                        // ТРАНСФОРМ УЗЛА У СКИНА НЕ ПРИМЕНЯЕТСЯ (см.
                        // GltfImporter.cpp). В эталоне у узла меша масштаб 2, и
                        // вершина 1 обязана остаться на x = 1: иначе
                        // статическая копия (контур выделения, габариты,
                        // попадание мышью) вдвое разойдётся с тем, что рисует
                        // скелетный проход.
                        const float x = (*verts)[1].Position.x;
                        if (std::abs(x - 1.0f) > 1e-3f) {
                            LOG_ERROR("Editor") << "SELFTEST: у скина применён трансформ узла: "
                                                << "x=" << x << ", ожидалось 1";
                            ok = false;
                        }
                    }
                }
            }
        }

        // Декорация без костей компонента анимации НЕ получает: иначе Animation
        // висел бы на каждом камне, а инспектор персонажа перестал бы
        // отличаться от инспектора забора.
        if (ok && WriteRigFixture(rigDir, "plain", false, rigErr)) {
            const fs::path plain = rigDir / "plain.gltf";
            sage::AssetDatabase::Instance().ScanProject(m_project.Dir().string());
            if (AddAssetToScene(plain)) {
                const entt::entity e = m_scene->Get(m_selection.Primary()).Entity();
                if (m_scene->Registry().all_of<AnimationComponent>(e)) {
                    LOG_ERROR("Editor") << "SELFTEST: модели БЕЗ костей навесили Animation";
                    ok = false;
                }
                // А вот у обычного узла трансформ применяться ОБЯЗАН: правило
                // выше касается только скина, и «не применять никогда» сломало
                // бы любую сцену, собранную из узлов с поворотами и сдвигами.
                const std::shared_ptr<Mesh> mesh =
                    ResourceManager::Instance().GetModel(m_project.AssetRef(plain));
                const std::vector<Vertex>* pv = mesh ? mesh->CpuVertices() : nullptr;
                if (pv && pv->size() == 3 && std::abs((*pv)[1].Position.x - 2.0f) > 1e-3f) {
                    LOG_ERROR("Editor") << "SELFTEST: у обычного узла потерян трансформ: x="
                                        << (*pv)[1].Position.x << ", ожидалось 2";
                    ok = false;
                }
            }
        }
        fs::remove_all(rigDir, ec);
    }

    // --- Обложки картинок: фоном, уменьшенными, с сохранением пропорций --------
    //
    // Три беды разом, и все три человек видел глазами (см. Thumbnails.h):
    // папка с фотографиями подвешивала диалог на секунды, обложка выходила
    // рябой, а панорама 4:1 показывалась квадратом. Проверяется поэтому не
    // «есть обложка», а именно то, чего не было:
    //
    //   1) первый запрос НЕ ЖДЁТ файл — возвращает «читается» и уходит;
    //   2) вместе с обложкой приезжают размеры ИСХОДНОЙ картинки, иначе вписать
    //      её в площадку с сохранением пропорций нечем;
    //   3) файл не той породы (.png, внутри которого WEBP — так их отдают
    //      сетевые редакторы) даёт ВНЯТНЫЙ отказ с названием формата, а не
    //      «unknown image type», по которому вывод один: редактор сломался.
    if (ok) {
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "sage_selftest_thumbs";
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);

        // Картинка НЕ КВАДРАТНАЯ намеренно: на квадрате ошибка с пропорциями не
        // видна вообще, и проверка бы её пропустила.
        const fs::path wide = root / "wide.png";
        {
            std::vector<unsigned char> px((size_t)64 * 16 * 3, 0);
            for (size_t i = 0; i < px.size(); i += 3) { px[i] = 30; px[i + 1] = 160; px[i + 2] = 220; }
            stbi_write_png(wide.string().c_str(), 64, 16, 3, px.data(), 64 * 3);
        }
        // .png снаружи, WEBP внутри — ровно тот файл, на котором редактор
        // ругался непонятным.
        const fs::path fake = root / "fake.png";
        {
            std::ofstream f(fake, std::ios::binary);
            const char riff[] = {'R', 'I', 'F', 'F', 0x20, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
            f.write(riff, sizeof(riff));
        }

        const thumbs::Thumb first = thumbs::Get(wide);
        if (first.Id != 0 || !first.Loading) {
            LOG_ERROR("Editor") << "SELFTEST: the first cover request did not go to the background";
            ok = false;
        }

        thumbs::Thumb ready;
        for (int i = 0; i < 500 && ready.Id == 0 && !ready.Failed; ++i) {
            thumbs::Pump();
            ready = thumbs::Get(wide);
            if (ready.Id == 0) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (ok && (ready.Id == 0 || ready.W != 64 || ready.H != 16)) {
            LOG_ERROR("Editor") << "SELFTEST: the cover did not arrive with the source size ("
                                << ready.W << "x" << ready.H << ")";
            ok = false;
        }

        // И ОБЛОЖКА РИСУЕТСЯ ПО ЭТИМ ПРОПОРЦИЯМ, а не квадратом. Панель
        // ассетов вписывала картинку в квадрат по меньшей стороне: пропорции
        // сохранялись только у квадратных исходников, а панорама 4096x1024,
        // тайл-лист 1:4 и скриншот 16:9 одинаково выходили квадратами. Узнать
        // по такой обложке свой файл нельзя — а она ровно для этого и нужна.
        if (ok) {
            namespace covers = sage::editor::covers;
            const covers::FitRect fit =
                covers::Fit(ImVec2(0.0f, 0.0f), ImVec2(100.0f, 100.0f), ready.W, ready.H);
            const float fw = fit.B.x - fit.A.x, fh = fit.B.y - fit.A.y;
            if (fh <= 0.0f || std::fabs(fw / fh - 4.0f) > 0.05f || fw > 100.5f) {
                LOG_ERROR("Editor") << "SELFTEST: обложка 64x16 вписана не по пропорциям (" << fw
                                    << "x" << fh << ")";
                ok = false;
            }
        }

        // ПРЕВЬЮ ПОД КУРСОРОМ — ТОЛЬКО У ТОГО, ЧТО ЕСТЬ ПОКАЗАТЬ. Картинка,
        // снимок сцены, обложка материала/модели/префаба — да. Звук, скрипт,
        // шрифт, папка — нет: значок во всю подсказку ничего не добавлял к
        // значку на плитке и только закрывал соседние файлы.
        if (ok) {
            namespace covers = sage::editor::covers;
            using Src = covers::PreviewSource;
            const fs::path scenePath = root / "level.sage";
            { std::ofstream f(scenePath); f << "{}"; }
            struct Case { fs::path Path; Src Want; const char* Why; };
            const Case cases[] = {
                {wide, Src::Image, "картинка — сама себе обложка"},
                {root / "hero.sagemat", Src::Rendered, "материал снимается"},
                {root / "hero.sageprefab", Src::Rendered, "префаб снимается"},
                {scenePath, Src::Icon, "сцена без снимка — значок, а не пустота"},
                {root / "hero.lua", Src::Icon, "скрипт — значок типа"},
                {root / "step.wav", Src::Icon, "звук — значок типа"},
                {root / "pixel.ttf", Src::Icon, "шрифт — значок типа"},
            };
            for (const Case& c : cases) {
                const Src got = covers::SourceOf(c.Path, /*isDir=*/false, /*hasPreview=*/true,
                                                 root);
                if (got != c.Want) {
                    LOG_ERROR("Editor") << "SELFTEST: превью ассета — не тот источник ("
                                        << c.Why << ")";
                    ok = false;
                }
            }
            if (ok && covers::SourceOf(root, /*isDir=*/true, true, root) != Src::Icon) {
                LOG_ERROR("Editor") << "SELFTEST: папка получила не значок";
                ok = false;
            }
            // Подсказка с превью — только у тех, у кого есть картинка.
            for (const Case& c : cases) {
                const bool shows = covers::ShowsHoverPreview(covers::SourceOf(c.Path, false, true, root));
                if (ok && shows != (c.Want != Src::Icon)) {
                    LOG_ERROR("Editor") << "SELFTEST: превью под курсором не там (" << c.Why << ")";
                    ok = false;
                }
            }
            if (ok && covers::ShowsHoverPreview(covers::SourceOf(root, true, true, root))) {
                LOG_ERROR("Editor") << "SELFTEST: у папки всплыло превью под курсором";
                ok = false;
            }
        }

        if (ok) {
            thumbs::Thumb bad = thumbs::Get(fake);
            for (int i = 0; i < 500 && !bad.Failed; ++i) {
                thumbs::Pump();
                bad = thumbs::Get(fake);
                if (!bad.Failed) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!bad.Failed || bad.Error.find("WEBP") == std::string::npos) {
                LOG_ERROR("Editor") << "SELFTEST: a WEBP under a .png name got no clear refusal ("
                                    << bad.Error << ")";
                ok = false;
            }
        }

        thumbs::Clear();
        fs::remove_all(root, ec);
    }

    // --- Чем открывать код: список и подстановка строки ------------------------
    //
    // Список редакторов обязан содержать ТОЛЬКО установленное: пункт «Visual
    // Studio Code» у человека без VS Code — обещание, которое не выполнится, и
    // выглядит это как «кнопка не работает». А подстановка {file}/{line} —
    // единственное, ради чего вообще нужен выбор IDE вместо системной
    // ассоциации: ошибка приходит как «hero.lua:42», и искать строку глазами
    // после открытия файла — та самая работа, которую должен делать редактор.
    if (ok) {
        namespace codeapp = sage::editor::codeapp;
        const fs::path file = "/tmp/sage selftest/hero.lua";   // с пробелом — как у человека

        // {file} обязан приехать В КАВЫЧКАХ: путь с пробелом («SAGE Projects»)
        // иначе разъезжается по аргументам, и редактор открывает «SAGE».
        const std::string withLine = codeapp::ExpandArgs("-g {file}:{line}", file, 42);
        if (withLine.find("42") == std::string::npos ||
            withLine.find("hero.lua") == std::string::npos ||
            withLine.find(' ') == std::string::npos) {
            LOG_ERROR("Editor") << "SELFTEST: code editor arguments expand wrong: " << withLine;
            ok = false;
        }
        // Без строки шаблон её и не упоминает — подставлять нечего и некуда.
        if (ok && codeapp::ExpandArgs("{file}", file, 0).find("42") != std::string::npos) {
            LOG_ERROR("Editor") << "SELFTEST: a line number leaked into a line-less pattern";
            ok = false;
        }
        // В списке — только то, что нашлось: у каждого пункта есть команда, и
        // пустого имени быть не может (по нему человек его узнаёт в меню).
        for (const codeapp::App& app : codeapp::Available()) {
            if (app.Id.empty() || app.Name.empty() || app.Command.empty()) {
                LOG_ERROR("Editor") << "SELFTEST: a code editor entry is incomplete";
                ok = false;
                break;
            }
        }
        // Выбор живёт в настройках РЕДАКТОРА и переживает закрытие меню.
        if (ok) {
            const std::string had = codeapp::CurrentId();
            codeapp::SetCurrent("vscode");
            const bool kept = codeapp::CurrentId() == "vscode";
            codeapp::SetCurrent("");
            const bool cleared = codeapp::CurrentId().empty();
            codeapp::SetCurrent(had);
            if (!kept || !cleared) {
                LOG_ERROR("Editor") << "SELFTEST: the chosen code editor is not remembered";
                ok = false;
            }
        }
    }

    // --- Правка скрипта видна БЕЗ перезапуска игры -----------------------------
    //
    // Скрипты правят во внешнем редакторе, и правка не значила ничего, пока игру
    // не перезапустят: цикл «поправил число — посмотрел» стоил прохождения
    // уровня заново. Здесь проверяется связка целиком — со стороны редактора, а
    // не только двигателя скриптов: правка файла в проекте обязана доехать.
    if (ok) {
        ScriptEngine se;
        se.BindScene(*m_scene);
        GameObject probe = m_scene->CreateObject("SelfTestHotReload");
        const fs::path script = m_project.AssetsDir() / "selftest_hotreload.lua";
        { std::ofstream f(script); f << "function OnStart(e) _G.SELFTEST_HOT = 1 end\n"; }

        se.AttachScript(probe, script.string());
        if (se.Lua()["SELFTEST_HOT"].get<int>() != 1) {
            LOG_ERROR("Editor") << "SELFTEST: script did not run on attach";
            ok = false;
        }
        if (ok) {
            { std::ofstream f(script); f << "function OnStart(e) _G.SELFTEST_HOT = 2 end\n"; }
            // Штамп сдвигается ЯВНО: у времени правки есть зернистость, и
            // полагаться на «прошло достаточно» в проверке нельзя.
            const auto when = fs::last_write_time(script, ec);
            fs::last_write_time(script, when + std::chrono::seconds(5), ec);
            if (se.ReloadChangedScripts() != 1 || se.Lua()["SELFTEST_HOT"].get<int>() != 2) {
                LOG_ERROR("Editor") << "SELFTEST: an edited script was not picked up";
                ok = false;
            }
        }
        m_scene->RemoveObject(probe.Id());
        fs::remove(script, ec);
    }

    // --- ПРАВКА ФАЙЛА СНАРУЖИ ДОЕЗЖАЕТ ДО РЕДАКТОРА ----------------------------
    //
    // «Переэкспортировал модель из 3D-редактора — в редакторе ничего не изменилось».
    // Кэш ресурсов держит загруженное по пути и на файл не смотрит; модель и
    // материал перечитывались, а ТЕКСТУРА и СКЕЛЕТНАЯ модель — нет, то есть
    // ровно то, что правят чаще всего, и оставалось прежним до перезапуска.
    // Проверяется настоящей правкой файла: пишем другой файл на то же место и
    // сдвигаем время правки явно (у него зернистость, «прошло достаточно» в
    // проверке не годится).
    if (ok) {
        ResourceManager& rm = ResourceManager::Instance();

        // ТЕКСТУРА: 2x2 -> 4x4. Размер виден без чтения пикселей с видеокарты.
        const fs::path tex = m_project.AssetsDir() / "selftest_reload.png";
        auto writePng = [&](int side) {
            std::vector<unsigned char> rgba((size_t)side * side * 4, 200);
            stbi_write_png(tex.string().c_str(), side, side, 4, rgba.data(), side * 4);
        };
        writePng(2);
        const std::string texRef = m_project.AssetRef(tex);
        std::shared_ptr<Texture> loaded = rm.GetTexture(texRef);
        if (!loaded || loaded->Width() != 2) {
            LOG_ERROR("Editor") << "SELFTEST: тестовая текстура не загрузилась";
            ok = false;
        } else {
            writePng(4);
            const auto when = fs::last_write_time(tex, ec);
            fs::last_write_time(tex, when + std::chrono::seconds(5), ec);
            const uint64_t genBefore = rm.AssetsGeneration();
            rm.ReloadChangedAssets();
            // ТОТ ЖЕ shared_ptr: на текстуру ссылаются материалы и интерфейс, и
            // подмена записи в кэше оставила бы их со старой картинкой.
            if (loaded->Width() != 4) {
                LOG_ERROR("Editor") << "SELFTEST: перерисованная текстура не перечиталась ("
                                    << loaded->Width() << "px)";
                ok = false;
            }
            if (rm.AssetsGeneration() == genBefore) {
                LOG_ERROR("Editor") << "SELFTEST: поколение ресурсов не сменилось";
                ok = false;
            }
        }
        fs::remove(tex, ec);

        // СКЕЛЕТНАЯ МОДЕЛЬ: тот же файл переписывается собой же со сдвинутым
        // временем правки — кэш обязан перечитать его заново.
        const fs::path rigSrc = fs::path("assets") / "test_rig.glb";
        if (ok && fs::exists(rigSrc, ec)) {
            const fs::path rig = m_project.AssetsDir() / "selftest_reload.glb";
            fs::copy_file(rigSrc, rig, fs::copy_options::overwrite_existing, ec);
            const std::string rigRef = m_project.AssetRef(rig);
            std::shared_ptr<sage::render::SkinnedModel> before = rm.GetSkinnedModel(rigRef);
            if (!before) {
                LOG_ERROR("Editor") << "SELFTEST: тестовая скелетная модель не загрузилась";
                ok = false;
            } else {
                const auto when = fs::last_write_time(rig, ec);
                fs::last_write_time(rig, when + std::chrono::seconds(5), ec);
                rm.ReloadChangedAssets();
                std::shared_ptr<sage::render::SkinnedModel> after = rm.GetSkinnedModel(rigRef);
                // ДРУГОЙ объект: у скелетной модели наружу торчат указатели
                // внутрь (аниматор держит адреса скелета и клипов), и подменять
                // её содержимое под ними нельзя — запись заменяется целиком.
                if (!after || after == before) {
                    LOG_ERROR("Editor") << "SELFTEST: скелетная модель не перечиталась";
                    ok = false;
                }
            }
            fs::remove(rig, ec);
        }
    }

    // --- Звук живёт в режиме ПРАВКИ, а не только в игре ------------------------
    //
    // Кнопка «Послушать» у компонента и проигрыватель звукового файла ставят
    // команду и ждут, что её кто-то выполнит. Выполняет стадия «audio», и она
    // заводилась вместе с Play, а по Stop снималась. То есть до первого запуска
    // игры превью звука не работало вовсе, а после первой остановки переставало
    // работать навсегда — молча, без единой строки в логе, потому что никакой
    // ошибки при этом не происходило.
    if (ok) {
        auto hasAudioStage = [this]() {
            for (const std::string& name : m_systems.Order())
                if (name == "audio") return true;
            return false;
        };
        if (!hasAudioStage()) {
            LOG_ERROR("Editor") << "SELFTEST: audio stage missing in edit mode";
            ok = false;
        }
        if (ok && !m_play.AudioIfCreated()) {
            LOG_ERROR("Editor") << "SELFTEST: no audio device outside Play";
            ok = false;
        }
        if (ok) {
            StartPlay();
            if (!hasAudioStage()) {
                LOG_ERROR("Editor") << "SELFTEST: audio stage missing during Play";
                ok = false;
            }
            StopPlay();
            if (ok && !hasAudioStage()) {
                LOG_ERROR("Editor") << "SELFTEST: audio stage lost after Stop";
                ok = false;
            }
        }
    }

    // --- Замок панели свойств -------------------------------------------------
    //
    // Смысл замка ровно один: пока он заперт, панель показывает ТО ЖЕ САМОЕ,
    // сколько бы человек ни щёлкал по другим объектам и файлам. Без этого
    // перетащить текстуру из Assets в слот материала нельзя вовсе — нажатие,
    // которым начинают перетаскивание, уводит панель на эту же текстуру.
    if (ok) {
        // Номера берём СРАЗУ: GameObject после удаления объекта спрашивать
        // нельзя — Id() у него бросает, и проверка падала бы вместо проверки.
        const int idA = m_scene->CreateObject("LockA").Id();
        const int idB = m_scene->CreateObject("LockB").Id();
        m_selection.SetPrimary(idA);
        SetInspectorLocked(true);

        m_selection.SetPrimary(idB);   // «случайный клик», ради которого всё и затевалось
        if (!InspectedObject().Valid() || InspectedObject().Id() != idA) {
            LOG_ERROR("Editor") << "SELFTEST: inspector lock did not hold the object";
            ok = false;
        }
        // Выбор при этом работает как работал: замок держит ПАНЕЛЬ, а не мышь.
        if (ok && (!SelectedObject().Valid() || SelectedObject().Id() != idB)) {
            LOG_ERROR("Editor") << "SELFTEST: inspector lock froze the selection itself";
            ok = false;
        }
        SetInspectorLocked(false);
        if (ok && (!InspectedObject().Valid() || InspectedObject().Id() != idB)) {
            LOG_ERROR("Editor") << "SELFTEST: inspector did not follow the selection after unlock";
            ok = false;
        }
        // Запертый объект удалили — панель обязана честно опустеть, а не
        // показывать чужие поля по освободившемуся номеру.
        if (ok) {
            m_selection.SetPrimary(idA);
            SetInspectorLocked(true);
            m_scene->RemoveObject(idA);
            if (InspectedObject().Valid()) {
                LOG_ERROR("Editor") << "SELFTEST: inspector lock kept a deleted object";
                ok = false;
            }
            SetInspectorLocked(false);
        }
        m_selection.SetPrimary(-1);
        if (m_scene->Get(idA).Valid()) m_scene->RemoveObject(idA);
        m_scene->RemoveObject(idB);
    }

    return ok;
}
