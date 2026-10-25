#include "SelfTestCommon.h"

bool EditorLayer::SelfTestObjectsAndMarkers() {
    bool ok = true;
    std::error_code ec;
    std::string err;
    (void)ec; (void)err;
    // --- ВЫКЛЮЧЕННЫЙ ОБЪЕКТ НЕ ПОПАДАЕТ В КАДР ------------------------------
    //
    // Флаг в сцене — половина дела; вторая половина в том, что его СЛУШАЮТ все,
    // кто собирает кадр. Проверяется именно это: выключенный объект не приходит
    // в обход рисуемых, выключенный свет не приходит в сбор источников, и обоих
    // не выбрать рамкой — в кадре их нет.
    if (ok) {
        NewScene(ProjectTemplateKind::Empty);
        entt::registry& reg = m_scene->Registry();
        GameObject box = CreatePrimitiveEntity("HiddenBox", MeshRef::Type::Cube);
        GameObject lamp = m_scene->Get(CreateCatalogObject("light.point"));

        auto renderableCount = [&]() {
            int n = 0;
            sage::ecs::ForEachRenderable(*m_scene, [&](Transform&, MeshRendererComponent&) { ++n; });
            return n;
        };
        const int before = renderableCount();
        if (before < 1) {
            LOG_ERROR("Editor") << "SELFTEST: в кадре нет ни одного рисуемого — проверять нечего";
            ok = false;
        }

        if (ok) {
            reg.emplace<HiddenComponent>(box.Entity());
            if (renderableCount() != before - 1) {
                LOG_ERROR("Editor") << "SELFTEST: выключенный объект остался в обходе рисуемых";
                ok = false;
            }
        }
        // И рамкой его не выбрать: выбирать то, чего в кадре нет, — значит
        // выбирать вслепую.
        if (ok) {
            const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 8.0f), glm::vec3(0.0f),
                                               glm::vec3(0, 1, 0));
            const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
            m_selection.SetPrimary(-1);
            SelectInViewportRect(view, proj, 0.0f, 0.0f, 1.0f, 1.0f, false);
            if (m_selection.Contains(box.Id())) {
                LOG_ERROR("Editor") << "SELFTEST: рамка выбрала выключенный объект";
                ok = false;
            }
        }
        // Выключенный СВЕТ не светит.
        if (ok && lamp.Valid()) {
            const size_t lit = sage::ecs::CollectLighting(*m_scene).PointLights.size();
            reg.emplace<HiddenComponent>(lamp.Entity());
            const size_t unlit = sage::ecs::CollectLighting(*m_scene).PointLights.size();
            if (lit == 0 || unlit != lit - 1) {
                LOG_ERROR("Editor") << "SELFTEST: выключенный источник остался в сборе света ("
                                    << lit << " -> " << unlit << ")";
                ok = false;
            }
        }
        // Включаем обратно — объект возвращается в кадр.
        if (ok) {
            reg.remove<HiddenComponent>(box.Entity());
            if (renderableCount() != before) {
                LOG_ERROR("Editor") << "SELFTEST: включённый обратно объект не вернулся в кадр";
                ok = false;
            }
        }
    }

    // --- КАПСУЛА И ШАГ ПО КАДРАМ --------------------------------------------
    //
    // Капсула — рабочая форма персонажа: ею его считает физика (ShapeType::
    // Capsule), а НАРИСОВАТЬ её было нечем, и персонаж собирался из цилиндра —
    // видимое тело не совпадало с тем, чем он сталкивается.
    if (ok) {
        NewScene(ProjectTemplateKind::Empty);
        GameObject cap = CreatePrimitiveEntity("CapsuleShape", MeshRef::Type::Capsule);
        const MeshRendererComponent& mr = cap.Renderer();
        if (!mr.MeshPtr || mr.MeshPtr->TriangleCount() <= 0) {
            LOG_ERROR("Editor") << "SELFTEST: капсула не построилась";
            ok = false;
        }
        // ГАБАРИТ СВЕРЯЕТСЯ С КОЛЛАЙДЕРОМ, А НЕ С ЧИСЛОМ В ЭТОМ ТЕСТЕ.
        //
        // Смысл капсулы ровно один: видимое тело обязано совпадать с тем, чем
        // персонаж сталкивается. Поэтому проверка спрашивает не «0.5 x 1», а
        // «то же, что у ColliderComponent по умолчанию»: разъехавшись, эти два
        // размера снова заставят настраивать рост в двух местах на глаз — и
        // именно так и было (меш 0.25 x 1 против коллайдера 0.5 x 2).
        if (ok) {
            const ColliderComponent shape;   // значения по умолчанию: капсула
            const float wantW = shape.Radius * 2.0f;
            const float wantH = (shape.HalfHeight + shape.Radius) * 2.0f;
            const glm::vec3 lo = mr.MeshPtr->BoundsMin(), hi = mr.MeshPtr->BoundsMax();
            const float w = hi.x - lo.x, h = hi.y - lo.y;
            if (std::abs(h - wantH) > 0.01f || std::abs(w - wantW) > 0.01f) {
                LOG_ERROR("Editor") << "SELFTEST: габарит капсулы " << w << " x " << h
                                    << " вместо " << wantW << " x " << wantH
                                    << " (размер коллайдера по умолчанию)";
                ok = false;
            }
            // И цилиндрическая часть ЕСТЬ: у сферы вершины лежат на одном
            // расстоянии от центра, у капсулы — нет. Проверка формы, а не
            // чисел: габарит у вытянутой сферы был бы такой же.
            if (ok) {
                const std::vector<Vertex>* verts = mr.MeshPtr->CpuVertices();
                float rMin = 1e9f, rMax = 0.0f;
                if (verts) {
                    for (const Vertex& v : *verts) {
                        const float r = glm::length(v.Position);
                        rMin = std::min(rMin, r);
                        rMax = std::max(rMax, r);
                    }
                }
                if (!verts || rMax - rMin < 0.05f) {
                    LOG_ERROR("Editor") << "SELFTEST: капсула построена сферой (разброс радиуса "
                                        << (rMax - rMin) << ")";
                    ok = false;
                }
            }
        }
        // Форма переживает сохранение: тип пишется именем, и опечатка в нём
        // превратила бы капсулу в «ничего» при следующем открытии сцены.
        if (ok) {
            const std::string text = SceneSerializer::SaveToString(*m_scene);
            std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(text);
            GameObject back = loaded ? loaded->FindByName("CapsuleShape") : GameObject{};
            if (!back.Valid() || back.Renderer().Ref.type != MeshRef::Type::Capsule) {
                LOG_ERROR("Editor") << "SELFTEST: капсула не пережила сохранение сцены";
                ok = false;
            }
        }
        // И персонаж из каталога собирается ИМЕННО капсулой — ТОГО ЖЕ размера,
        // что и его контроллер. Проверка на «это капсула» пропускала главное:
        // персонаж рисовался вдвое тоньше своей физической капсулы, потому что
        // масштаб считали от капсулы шириной 0.5, а контроллер — от 0.7.
        if (ok) {
            GameObject ch = m_scene->Get(CreateCatalogObject("physics.character"));
            if (!ch.Valid() || ch.Renderer().Ref.type != MeshRef::Type::Capsule) {
                LOG_ERROR("Editor") << "SELFTEST: персонаж собран не капсулой";
                ok = false;
            } else if (const std::shared_ptr<Mesh>& m = ch.Renderer().MeshPtr; m) {
                const CharacterControllerComponent want;
                const glm::vec3 sc = ch.GetTransform().Scale;
                const glm::vec3 lo = m->BoundsMin(), hi = m->BoundsMax();
                const float drawnW = (hi.x - lo.x) * sc.x;
                const float drawnH = (hi.y - lo.y) * sc.y;
                if (std::abs(drawnW - want.Radius * 2.0f) > 0.01f ||
                    std::abs(drawnH - want.Height) > 0.01f) {
                    LOG_ERROR("Editor") << "SELFTEST: персонаж нарисован " << drawnW << " x "
                                        << drawnH << ", а сталкивается " << want.Radius * 2.0f
                                        << " x " << want.Height;
                    ok = false;
                }
            }
        }
    }

    // ШАГ ПО КАДРАМ. На паузе мир стоит — это единственное состояние, в котором
    // его можно разглядеть; но вопросы к нему про движение. Шаг двигает мир
    // ровно на один кадр и снова останавливает.
    if (ok) {
        NewScene(ProjectTemplateKind::Empty);
        GameObject box = CreatePrimitiveEntity("StepBox", MeshRef::Type::Cube);
        box.GetTransform().Position = glm::vec3(0.0f, 20.0f, 0.0f);
        m_scene->Registry().emplace_or_replace<RigidBodyComponent>(box.Entity());
        const int boxId = box.Id();

        StartPlay();
        PausePlay();
        const float yPaused = m_scene->Get(boxId).GetTransform().Position.y;

        StepPlay();
        OnUpdate(0.05f);
        const float yStepped = m_scene->Get(boxId).GetTransform().Position.y;
        if (yStepped >= yPaused - 1e-5f) {
            LOG_ERROR("Editor") << "SELFTEST: шаг не сдвинул кадр (y " << yPaused << " -> "
                                << yStepped << ")";
            ok = false;
        }
        // И ровно ОДИН кадр: следующие OnUpdate без нажатия шага обязаны
        // оставить мир на месте, иначе это не шаг, а снятие с паузы.
        if (ok) {
            for (int i = 0; i < 10; ++i) OnUpdate(0.05f);
            const float yAfter = m_scene->Get(boxId).GetTransform().Position.y;
            if (std::abs(yAfter - yStepped) > 1e-5f) {
                LOG_ERROR("Editor") << "SELFTEST: после шага кадр продолжает идти сам";
                ok = false;
            }
        }
        // Вне паузы шаг не делает ничего: у работающей игры он смысла не имеет.
        if (ok) {
            StopPlay();
            const float yBefore = m_scene->Get(boxId).GetTransform().Position.y;
            StepPlay();
            OnUpdate(0.05f);
            if (std::abs(m_scene->Get(boxId).GetTransform().Position.y - yBefore) > 1e-5f) {
                LOG_ERROR("Editor") << "SELFTEST: шаг сработал вне паузы";
                ok = false;
            }
        } else {
            StopPlay();
        }
    }

    // --- МЕШ СНИМАЕТСЯ, КАК ЛЮБОЙ КОМПОНЕНТ ---------------------------------
    //
    // Меш был единственным компонентом без кнопки «убрать»: добавить его через
    // «Добавить компонент» было можно, а снять — нельзя, и объект, которому он
    // достался по ошибке (брошенной моделью, например), оставался с ним
    // навсегда. Никакой особости у меша нет, и невидимый объект — законное
    // состояние.
    if (ok) {
        NewScene(ProjectTemplateKind::Empty);
        GameObject box = CreatePrimitiveEntity("MeshRemovable", MeshRef::Type::Cube);
        entt::registry& reg = m_scene->Registry();
        const int boxId = box.Id();

        reg.remove<MeshRendererComponent>(box.Entity());
        if (reg.all_of<MeshRendererComponent>(box.Entity())) {
            LOG_ERROR("Editor") << "SELFTEST: меш не снимается с объекта";
            ok = false;
        }
        // Объект без меша обязан пережить сохранение и остаться без меша: иначе
        // «сняли меш» откатывалось бы при первой же перезагрузке сцены.
        if (ok) {
            const std::string text = SceneSerializer::SaveToString(*m_scene);
            std::unique_ptr<Scene> loaded = SceneSerializer::LoadFromString(text);
            GameObject back = loaded ? loaded->Get(boxId) : GameObject{};
            if (!back.Valid() || loaded->Registry().all_of<MeshRendererComponent>(back.Entity())) {
                LOG_ERROR("Editor") << "SELFTEST: снятый меш вернулся после перезагрузки сцены";
                ok = false;
            }
        }
        // И добавляется обратно — тем же списком компонентов.
        if (ok) {
            reg.emplace<MeshRendererComponent>(box.Entity());
            if (!reg.all_of<MeshRendererComponent>(box.Entity())) {
                LOG_ERROR("Editor") << "SELFTEST: меш не добавляется обратно";
                ok = false;
            }
        }
        // Меш есть в списке «Добавить компонент» — иначе снять его было бы
        // можно, а вернуть нечем.
        if (ok) {
            bool offered = false;
            for (const auto& [category, items] : InspectorPanel::AddComponentMenuContents())
                for (const std::string& n : items)
                    if (n == "Mesh") offered = true;
            if (!offered) {
                LOG_ERROR("Editor") << "SELFTEST: меша нет в списке «Добавить компонент»";
                ok = false;
            }
        }
    }

    // --- НЕВИДИМЫЕ ОБЪЕКТЫ ВЫБИРАЮТСЯ ЩЕЛЧКОМ ПО СВОЕМУ ЗНАЧКУ --------------
    //
    // Камера, свет, эмиттер, зонд и источник звука ничего не рисуют. Значок в
    // их точке показывает, что они там есть, — но щелчок по нему проваливался
    // сквозь у всех, кроме камеры и света: значок обманывал.
    if (ok) {
        NewScene(ProjectTemplateKind::Empty);
        struct Probe { const char* Catalog; const char* What; };
        const Probe kProbes[] = {
            {"fx.particles", "эмиттер частиц"},
            {"fx.probe", "зонд отражений"},
            {"audio.source", "источник звука"},
            {"camera.game", "камера"},
        };
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 8.0f), glm::vec3(0.0f),
                                           glm::vec3(0, 1, 0));
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
        for (const Probe& p : kProbes) {
            const int id = CreateCatalogObject(p.Catalog);
            GameObject obj = m_scene->Get(id);
            if (!obj.Valid()) { ok = false; break; }
            // Ставим в начало координат и целимся ровно в него.
            obj.GetTransform().Position = glm::vec3(0.0f);
            m_selection.SetPrimary(-1);
            PickAtViewportWith(view, proj, 0.5f, 0.5f, /*additive=*/false);
            if (m_selection.Primary() != id) {
                LOG_ERROR("Editor") << "SELFTEST: щелчок по значку не выбирает: " << p.What;
                ok = false;
                break;
            }
            m_scene->RemoveObject(id);
        }
    }

    // --- ЗНАЧКИ СПИСКА И ПАПКИ СЦЕНЫ ---------------------------------------
    //
    // ЗНАЧОК — ПРО ТИП ПРЕДМЕТА, А НЕ ПРО ЕГО ФОРМУ. Куб, сфера, цилиндр и
    // конус в списке — одно и то же: геометрия. Разные значки у них означали
    // бы, что глаз обязан различать кубик и шарик размером с букву, ничего за
    // это не получая. Свет — единственное исключение: солнце, точечный и
    // прожектор ведут себя по-разному, и перепутать их дорого.
    if (ok) {
        NewScene(ProjectTemplateKind::Empty);
        entt::registry& reg = m_scene->Registry();
        auto iconOf = [&](GameObject& o) { return std::string(HierarchyPanel::IconFor(reg, o.Entity())); };

        // Все формы — ОДИН значок.
        const MeshRef::Type kShapes[] = {MeshRef::Type::Cube, MeshRef::Type::Sphere,
                                         MeshRef::Type::Plane, MeshRef::Type::Cylinder,
                                         MeshRef::Type::Cone};
        for (MeshRef::Type type : kShapes) {
            GameObject shape = CreatePrimitiveEntity("IconShape", type);
            if (iconOf(shape) != "cube") {
                LOG_ERROR("Editor") << "SELFTEST: у формы значок '" << iconOf(shape)
                                    << "' вместо общего 'cube'";
                ok = false;
                break;
            }
        }

        // Три типа света — ТРИ РАЗНЫХ значка.
        if (ok) {
            std::string sun, point, spot;
            for (const char* id : {"light.sun", "light.point", "light.spot"}) {
                GameObject light = m_scene->Get(CreateCatalogObject(id));
                if (!light.Valid()) { ok = false; break; }
                const std::string icon = iconOf(light);
                if (std::string(id) == "light.sun") sun = icon;
                else if (std::string(id) == "light.point") point = icon;
                else spot = icon;
            }
            if (ok && (sun == point || point == spot || sun == spot)) {
                LOG_ERROR("Editor") << "SELFTEST: типы света неразличимы по значку: солнце '" << sun
                                    << "', точечный '" << point << "', прожектор '" << spot << "'";
                ok = false;
            }
            // И камера — своим значком, а не общим.
            if (ok) {
                GameObject cam = m_scene->Get(CreateCatalogObject("camera.game"));
                if (!cam.Valid() || iconOf(cam) != "camera") {
                    LOG_ERROR("Editor") << "SELFTEST: у камеры не её значок";
                    ok = false;
                } else {
                    // Новая камера смотрит туда же, куда вид редактора: и в
                    // сторону, и вверх-вниз (знак наклона здесь уже путался).
                    const glm::mat4 w = m_scene->WorldMatrix(cam.Entity());
                    const glm::vec3 fwd = glm::normalize(-glm::vec3(w[2]));
                    if (glm::dot(fwd, glm::normalize(m_camera.Front)) < 0.999f) {
                        LOG_ERROR("Editor") << "SELFTEST: новая камера смотрит не туда, куда вид редактора";
                        ok = false;
                    }
                }
            }
        }

        // Значка-заглушки (пустой квадрат с диагональю) быть не должно ни у
        // одного типа: опечатка в имени выглядит как поломка всего набора.
        if (ok) {
            for (auto e : reg.view<IdComponent>()) {
                const char* icon = HierarchyPanel::IconFor(reg, e);
                if (!EditorIcons::HasGlyph(icon)) {
                    LOG_ERROR("Editor") << "SELFTEST: значка нет в наборе — '" << icon << "'";
                    ok = false;
                    break;
                }
            }
        }

        // ПАПКА СПИСКА: свой значок, свой цвет и НИКАКОГО влияния на сцену.
        if (ok) {
            GameObject folder = m_scene->CreateFolder("Декорации");
            GameObject lamp = CreatePrimitiveEntity("FolderLamp", MeshRef::Type::Cube);
            lamp.GetTransform().Position = glm::vec3(4.0f, 1.0f, 0.0f);
            m_scene->SetParent(lamp.Entity(), folder.Entity());
            if (iconOf(folder) != "folder-full") {
                LOG_ERROR("Editor") << "SELFTEST: у непустой папки значок '" << iconOf(folder) << "'";
                ok = false;
            }
            // Двигаем папку — лампа обязана остаться на месте.
            if (ok) {
                folder.GetTransform().Position = glm::vec3(100.0f, 0.0f, 0.0f);
                const glm::mat4 world = m_scene->WorldMatrix(lamp.Entity());
                if (std::abs(world[3][0] - 4.0f) > 0.001f) {
                    LOG_ERROR("Editor") << "SELFTEST: папка сдвинула содержимое (x " << world[3][0]
                                        << " вместо 4)";
                    ok = false;
                }
            }
            // Цвет метки — из компонента, а не общий цвет значков.
            if (ok) {
                reg.get<FolderComponent>(folder.Entity()).Color = glm::vec3(0.9f, 0.35f, 0.35f);
                const std::string text = SceneSerializer::SaveToString(*m_scene);
                if (text.find("\"folder\"") == std::string::npos) {
                    LOG_ERROR("Editor") << "SELFTEST: папка не сохраняется в сцене";
                    ok = false;
                }
            }
        }
    }

    // --- ЦВЕТНЫЕ МЕТКИ ПАПОК ПРОЕКТА ---------------------------------------
    //
    // Метка обязана пережить перезапуск редактора и переезд проекта: цвет,
    // живущий один сеанс, человек назначит второй раз и перестанет верить.
    if (ok) {
        namespace foldercolors = sage::editor::foldercolors;
        std::error_code ec;
        const fs::path dir = m_project.AssetsDir() / "selftest_colored";
        fs::create_directories(dir, ec);
        foldercolors::SetProject(m_project.Dir());
        foldercolors::Set(dir, glm::vec3(0.45f, 0.80f, 0.45f));

        glm::vec3 got(0.0f);
        if (!foldercolors::Get(dir, got) || std::abs(got.g - 0.80f) > 0.001f) {
            LOG_ERROR("Editor") << "SELFTEST: цвет папки не назначился";
            ok = false;
        }
        // Перечитываем с диска, как при следующем запуске.
        if (ok) {
            foldercolors::SetProject(fs::path{});
            foldercolors::SetProject(m_project.Dir());
            glm::vec3 reloaded(0.0f);
            if (!foldercolors::Get(dir, reloaded) || std::abs(reloaded.g - 0.80f) > 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: цвет папки не пережил перезапуск";
                ok = false;
            }
        }
        // Переименование везёт метку с собой.
        if (ok) {
            const fs::path moved = m_project.AssetsDir() / "selftest_renamed";
            fs::rename(dir, moved, ec);
            foldercolors::Rename(dir, moved);
            glm::vec3 after(0.0f);
            if (!foldercolors::Get(moved, after) || std::abs(after.g - 0.80f) > 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: метка не поехала за переименованной папкой";
                ok = false;
            }
            fs::remove_all(moved, ec);
        }
    }

    // --- ПАУЗА ОСТАНАВЛИВАЕТ КАДР, А НЕ ТОЛЬКО ЗНАЧОК -----------------------
    //
    // Что было: «Пауза» меняла поле состояния, и на этом всё кончалось.
    // Планировщик систем звался в OnUpdate каждый кадр независимо от состояния,
    // и в паузе продолжали идти скрипты, физика, анимация и частицы — кнопка
    // меняла свой вид и больше ничего.
    //
    // Проверяется ИМЕННО ЧЕРЕЗ OnUpdate, а не через m_systems.Run: остановка
    // живёт в кадре редактора, и прогон планировщика напрямую (как в проверке
    // физики выше) прошёл бы мимо неё — то есть не заметил бы поломки.
    if (ok) {
        NewScene(ProjectTemplateKind::Empty);
        GameObject box = CreatePrimitiveEntity("PauseBox", MeshRef::Type::Cube);
        box.GetTransform().Position = glm::vec3(0.0f, 20.0f, 0.0f);
        m_scene->Registry().emplace_or_replace<RigidBodyComponent>(box.Entity());
        const int boxId = box.Id();

        StartPlay();
        for (int i = 0; i < 5; ++i) OnUpdate(0.05f);
        const float yPlaying = m_scene->Get(boxId).GetTransform().Position.y;
        if (yPlaying >= 20.0f - 0.001f) {
            LOG_ERROR("Editor") << "SELFTEST: в Play тело не падает — проверять паузу нечем";
            ok = false;
        }

        if (ok) {
            PausePlay();
            const float yPaused = m_scene->Get(boxId).GetTransform().Position.y;
            for (int i = 0; i < 20; ++i) OnUpdate(0.05f);
            const float yAfterPause = m_scene->Get(boxId).GetTransform().Position.y;
            if (std::abs(yAfterPause - yPaused) > 0.0001f) {
                LOG_ERROR("Editor") << "SELFTEST: на паузе кадр продолжает идти (y " << yPaused
                                    << " -> " << yAfterPause << ")";
                ok = false;
            }
        }

        // И продолжение обязано ПРОДОЛЖИТЬ: пауза, после которой ничего не
        // оживает, — это остановка, а не пауза.
        if (ok) {
            const float yBeforeResume = m_scene->Get(boxId).GetTransform().Position.y;
            ResumePlay();
            for (int i = 0; i < 5; ++i) OnUpdate(0.05f);
            const float yResumed = m_scene->Get(boxId).GetTransform().Position.y;
            if (yResumed >= yBeforeResume - 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: после продолжения кадр не пошёл (y "
                                    << yBeforeResume << " -> " << yResumed << ")";
                ok = false;
            }
        }
        StopPlay();
    }

    // --- ВНЕСТИ В ПРОЕКТ: ФАЙЛ, ПАПКА ИЛИ АРХИВ -----------------------------
    //
    // Кнопка «Внести» умела ровно один файл. Скачивают же по-разному: модель
    // одним файлом, набор текстур папкой, ассет с маркетплейса архивом, — и на
    // двух случаях из трёх кнопка отвечала «выберите файл».
    if (ok) {
        std::error_code ec;
        const fs::path outside = fs::temp_directory_path() / "sage_selftest_import";
        fs::remove_all(outside, ec);
        fs::create_directories(outside / "rock" / "tex", ec);
        {
            std::ofstream(outside / "rock" / "rock.txt") << "mesh";
            std::ofstream(outside / "rock" / "tex" / "albedo.txt") << "map";
        }
        const fs::path dest = m_project.AssetsDir() / "selftest_import";
        fs::remove_all(dest, ec);
        fs::create_directories(dest, ec);

        // ПАПКА целиком, вместе с вложенной.
        const AssetsPanel::ImportReport folderReport = AssetsPanel::ImportAsset(outside / "rock", dest);
        if (!folderReport.Ok || !fs::exists(dest / "rock" / "rock.txt") ||
            !fs::exists(dest / "rock" / "tex" / "albedo.txt")) {
            LOG_ERROR("Editor") << "SELFTEST: папка не внеслась в проект целиком";
            ok = false;
        }

        // АРХИВ — в свою папку, а не вперемешку с тем, что уже лежит рядом.
        if (ok) {
            const fs::path zipPath = outside / "pack.zip";
            WriteSelfTestZip(zipPath);
            const AssetsPanel::ImportReport zipReport = AssetsPanel::ImportAsset(zipPath, dest);
            if (!zipReport.Ok || !fs::exists(dest / "pack" / "readme.txt")) {
                LOG_ERROR("Editor") << "SELFTEST: архив не распаковался в проект";
                ok = false;
            }
        }
        fs::remove_all(outside, ec);
    }

    // --- РАМКА ВЫДЕЛЕНИЯ И СПИСОК КОМПОНЕНТОВ -------------------------------
    //
    // Что было: выделить двадцать объектов можно было только Ctrl-кликом по
    // каждому, а список «Добавить компонент» рос плоской простынёй, которая
    // перестала помещаться в окно. Проверяются обе правки.
    //
    // Рамка: три куба в ряд, камера смотрит на них сбоку. Прямоугольник,
    // накрывающий два левых, обязан выбрать ровно их — ни больше (третий
    // рядом), ни меньше. Считается всё теми же матрицами, что у вьюпорта:
    // проверка спрашивает «что выберет эта рамка на экране», а не подглядывает
    // в реализацию.
    if (ok) {
        NewScene(ProjectTemplateKind::Empty);
        GameObject a = CreatePrimitiveEntity("RectA", MeshRef::Type::Cube);
        GameObject b = CreatePrimitiveEntity("RectB", MeshRef::Type::Cube);
        GameObject c = CreatePrimitiveEntity("RectC", MeshRef::Type::Cube);
        a.GetTransform().Position = glm::vec3(-3.0f, 0.0f, 0.0f);
        b.GetTransform().Position = glm::vec3(0.0f, 0.0f, 0.0f);
        c.GetTransform().Position = glm::vec3(3.0f, 0.0f, 0.0f);

        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 12.0f), glm::vec3(0.0f),
                                           glm::vec3(0, 1, 0));
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
        auto project = [&](const glm::vec3& world, float& u, float& v) {
            const glm::vec4 clip = proj * view * glm::vec4(world, 1.0f);
            u = (clip.x / clip.w) * 0.5f + 0.5f;
            v = 0.5f - (clip.y / clip.w) * 0.5f;
        };
        // Рамка от левого края до промежутка между вторым и третьим кубом.
        float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
        project(glm::vec3(-5.0f, 2.0f, 0.0f), u0, v0);
        project(glm::vec3(1.5f, -2.0f, 0.0f), u1, v1);

        m_selection.SetPrimary(-1);
        SelectInViewportRect(view, proj, u0, v0, u1, v1, /*additive=*/false);
        auto selected = [&](GameObject& o) { return m_selection.Contains(o.Id()); };
        if (m_selection.All().size() != 2 || !selected(a) || !selected(b) || selected(c)) {
            LOG_ERROR("Editor") << "SELFTEST: рамка выделила " << m_selection.All().size()
                                << " объектов вместо двух левых";
            ok = false;
        }

        // Ctrl/Shift — ДОБАВИТЬ к набору, а не заменить его: иначе обвести две
        // группы подряд было бы нельзя.
        if (ok) {
            project(glm::vec3(1.5f, 2.0f, 0.0f), u0, v0);
            project(glm::vec3(5.0f, -2.0f, 0.0f), u1, v1);
            SelectInViewportRect(view, proj, u0, v0, u1, v1, /*additive=*/true);
            if (m_selection.All().size() != 3) {
                LOG_ERROR("Editor") << "SELFTEST: рамка с Ctrl заменила набор, а не дополнила: "
                                    << m_selection.All().size();
                ok = false;
            }
        }

        // Рамка ЗА спиной камеры не выбирает ничего. Точка с отрицательной
        // глубиной проецируется зеркально, и без проверки предметы позади
        // попадали бы в рамку перед лицом.
        if (ok) {
            GameObject behind = CreatePrimitiveEntity("RectBehind", MeshRef::Type::Cube);
            behind.GetTransform().Position = glm::vec3(0.0f, 0.0f, 40.0f);
            m_selection.SetPrimary(-1);
            SelectInViewportRect(view, proj, 0.0f, 0.0f, 1.0f, 1.0f, false);
            if (m_selection.Contains(behind.Id())) {
                LOG_ERROR("Editor") << "SELFTEST: рамка выбрала объект за спиной камеры";
                ok = false;
            }
            m_scene->RemoveObject(behind.Id());
        }
    }

    // Геометрия самой рамки: направление ведения не должно ничего менять, а
    // дрожание руки — не должно считаться рамкой (иначе каждый обычный клик
    // снимал бы выделение).
    if (ok) {
        namespace rectselect = sage::editor::rectselect;
        rectselect::State rs;
        rs.Start = ImVec2(100.0f, 100.0f);
        rs.Current = ImVec2(40.0f, 30.0f); // ведут ВВЕРХ-ВЛЕВО
        if (!rectselect::Hits(rs, ImVec2(50.0f, 50.0f), ImVec2(60.0f, 60.0f)) ||
            rectselect::Hits(rs, ImVec2(200.0f, 200.0f), ImVec2(210.0f, 210.0f))) {
            LOG_ERROR("Editor") << "SELFTEST: рамка, ведённая назад, считает попадания неверно";
            ok = false;
        }
        // Касание КРАЕМ засчитывается: обводя список по диагонали, крайние
        // строки задевают уголком, и терять их нельзя.
        if (ok && !rectselect::Hits(rs, ImVec2(95.0f, 95.0f), ImVec2(300.0f, 300.0f))) {
            LOG_ERROR("Editor") << "SELFTEST: рамка не засчитывает касание краем";
            ok = false;
        }
        if (ok) {
            rectselect::State click;
            click.Start = ImVec2(10.0f, 10.0f);
            click.Current = ImVec2(11.0f, 12.0f); // дрогнула рука
            if (rectselect::Meaningful(click) || !rectselect::Meaningful(rs)) {
                LOG_ERROR("Editor") << "SELFTEST: клик и рамка не различаются по порогу";
                ok = false;
            }
        }
    }

    // Список «Добавить компонент»: сворачивается по категориям, и это опирается
    // на то, что записи одной категории идут ПОДРЯД — иначе категория открылась
    // бы вторым заголовком. Заодно проверяется, что наклейки и «GI Static» в
    // списке больше нет: у обеих есть своё место (каталог объектов и панель
    // Environment), а в списке компонентов они предлагались каждому.
    if (ok) {
        const auto menu = InspectorPanel::AddComponentMenuContents();
        if (menu.size() < 6) {
            LOG_ERROR("Editor") << "SELFTEST: меню компонентов не разложено по категориям: "
                                << menu.size();
            ok = false;
        }
        std::set<std::string> seenCategories;
        std::set<std::string> names;
        for (const auto& [category, items] : menu) {
            if (!seenCategories.insert(category).second) {
                LOG_ERROR("Editor") << "SELFTEST: категория компонентов разорвана: " << category;
                ok = false;
            }
            for (const std::string& n : items) names.insert(n);
        }
        if (ok && (names.count("Decal") || names.count("GI Static"))) {
            LOG_ERROR("Editor") << "SELFTEST: наклейка или GI Static остались в списке компонентов";
            ok = false;
        }
        // И то, ради чего список вообще нужен, на месте.
        if (ok && (!names.count("Mesh") || !names.count("Light") || !names.count("Script"))) {
            LOG_ERROR("Editor") << "SELFTEST: из списка компонентов пропало основное";
            ok = false;
        }
    }

    return ok;
}
