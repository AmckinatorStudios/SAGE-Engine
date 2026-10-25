#include "SelfTestCommon.h"

bool EditorLayer::SelfTestSceneBasics() {
    bool ok = true;
    std::error_code ec;
    std::string err;
    (void)ec; (void)err;

    // --- Иерархия: мировая позиция ребёнка складывается из родителя, удаление
    // родителя сносит поддерево, undo возвращает обоих со связью ---
    if (ok) {
        size_t n0 = m_scene->Count();
        GameObject parent = m_scene->CreateObject("HierParent");
        parent.GetTransform().Position = {10.0f, 0.0f, 0.0f};
        GameObject child = m_scene->CreateObject("HierChild");
        child.GetTransform().Position = {0.0f, 5.0f, 0.0f};
        m_scene->SetParent(child.Entity(), parent.Entity());

        glm::vec3 wp = glm::vec3(m_scene->WorldMatrix(child.Entity())[3]);
        if (std::abs(wp.x - 10.0f) > 0.001f || std::abs(wp.y - 5.0f) > 0.001f) {
            LOG_ERROR("Editor") << "SELFTEST: hierarchy world matrix wrong (" << wp.x << ", " << wp.y << ")";
            ok = false;
        }
        if (ok) {
            m_selection.SetPrimary(parent.Id());
            DeleteSelected(); // PushUndoSnapshot внутри; удаляет родителя С ребёнком
            if (m_scene->Count() != n0) {
                LOG_ERROR("Editor") << "SELFTEST: hierarchy subtree delete failed (count "
                                    << m_scene->Count() << ", expected " << n0 << ")";
                ok = false;
            }
        }
        if (ok) {
            Undo(); // возвращает и родителя, и ребёнка, и связь между ними
            GameObject p2 = m_scene->FindByName("HierParent");
            GameObject c2 = m_scene->FindByName("HierChild");
            bool linkOk = p2.Valid() && c2.Valid() &&
                          m_scene->ParentOf(c2.Entity()) == p2.Entity();
            if (!linkOk) {
                LOG_ERROR("Editor") << "SELFTEST: hierarchy undo did not restore parent link";
                ok = false;
            }
            if (p2.Valid()) m_scene->RemoveObject(p2.Id()); // чистим вместе с поддеревом
        }
        m_selection.SetPrimary(-1);
    }

    // --- Мультивыделение: набор из двух, Delete удаляет обе, Undo возвращает,
    // Duplicate дублирует обе ---
    if (ok) {
        GameObject a = m_scene->CreateObject("MultiA");
        GameObject b = m_scene->CreateObject("MultiB");
        m_selection.SetPrimary(a.Id());
        m_selection.Toggle(b.Id()); // теперь выбраны обе, первичная — b
        bool selOk = m_selection.All().size() == 2 && m_selection.Contains(a.Id()) && m_selection.Contains(b.Id()) &&
                     m_selection.Primary() == b.Id();
        if (!selOk) {
            LOG_ERROR("Editor") << "SELFTEST: multi-select set wrong (size " << m_selection.All().size() << ")";
            ok = false;
        }
        if (ok) {
            size_t n0 = m_scene->Count();
            DuplicateSelected(); // обе -> +2, выделены копии
            if (m_scene->Count() != n0 + 2 || m_selection.All().size() != 2) {
                LOG_ERROR("Editor") << "SELFTEST: multi-duplicate failed (count " << m_scene->Count()
                                    << ", sel " << m_selection.All().size() << ")";
                ok = false;
            }
            if (ok) DeleteSelected(); // убрать копии (выделены они) -> снова n0
            // Удаляем исходную пару (снова мультивыбор) и проверяем Undo.
            if (ok) {
                m_selection.SetPrimary(m_scene->FindByName("MultiA").Id());
                m_selection.Toggle(m_scene->FindByName("MultiB").Id());
                size_t nBefore = m_scene->Count();
                DeleteSelected();
                bool delOk = m_scene->Count() == nBefore - 2 && m_selection.Empty();
                Undo();
                bool undoOk = m_scene->FindByName("MultiA").Valid() &&
                              m_scene->FindByName("MultiB").Valid();
                if (!delOk || !undoOk) {
                    LOG_ERROR("Editor") << "SELFTEST: multi-delete/undo failed (del " << delOk
                                        << ", undo " << undoOk << ")";
                    ok = false;
                }
            }
        }
        // Чистим тестовые сущности.
        for (const char* nm : {"MultiA", "MultiB"}) {
            GameObject g = m_scene->FindByName(nm);
            if (g.Valid()) m_scene->RemoveObject(g.Id());
        }
        m_selection.SetPrimary(-1);
    }

    // --- Пикинг: клик по объекту выбирает ЕГО, а не пол под ним ---
    //
    // Это проверка по конкретной жалобе: «кликаю прямо по объекту, а точка как
    // будто ниже». Причина была не в луче, а в габаритах — вместо коробки меша
    // брался куб со стороной радиуса ограничивающей СФЕРЫ. У пола 20x20 такая
    // коробка становится кубом 28x28x28, накрывающим сцену вместе с камерой, и
    // пол выигрывал любой клик просто потому, что его «верхняя грань» ближе к
    // камере, чем то, на что человек смотрит.
    //
    // Сцена нарочно та же, что у человека: пол и кубик на нём. Точки клика
    // считаются ПРОЕЦИРОВАНИЕМ мировых координат теми же матрицами, что
    // используются для луча, — тест не подглядывает в реализацию, а спрашивает
    // «куда попадёт клик по этому пикселю».
    if (ok) {
        GameObject floor = CreatePrimitiveEntity("PickFloor", MeshRef::Type::Plane);
        floor.GetTransform().Scale = glm::vec3(20.0f, 1.0f, 20.0f);

        GameObject box = CreatePrimitiveEntity("PickBox", MeshRef::Type::Cube);
        box.GetTransform().Position = glm::vec3(0.0f, 0.5f, 0.0f);

        // Камера смотрит на кубик сверху-сбоку — обычный ракурс редактора.
        const glm::vec3 eye(3.0f, 3.0f, 5.0f);
        m_view = glm::lookAt(eye, glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0, 1, 0));
        m_proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);

        // Мировая точка -> экранные доли (u, v), ровно как их считает вьюпорт.
        auto project = [&](const glm::vec3& world, float& u, float& v) {
            glm::vec4 clip = m_proj * m_view * glm::vec4(world, 1.0f);
            u = (clip.x / clip.w) * 0.5f + 0.5f;
            v = 0.5f - (clip.y / clip.w) * 0.5f;
        };

        float u = 0.0f, v = 0.0f;
        project(glm::vec3(0.0f, 0.5f, 0.0f), u, v);   // центр кубика
        m_selection.SetPrimary(-1);
        PickAtViewport(u, v, /*additive=*/false);
        if (m_selection.Primary() != box.Id()) {
            LOG_ERROR("Editor") << "SELFTEST: pick on box selected " << m_selection.Primary() << ", expected "
                                << box.Id() << " (box)";
            ok = false;
        }

        // Клик по полу далеко от кубика — выбирается пол.
        if (ok) {
            project(glm::vec3(-6.0f, 0.0f, -4.0f), u, v);
            m_selection.SetPrimary(-1);
            PickAtViewport(u, v, false);
            if (m_selection.Primary() != floor.Id()) {
                LOG_ERROR("Editor") << "SELFTEST: pick on floor selected " << m_selection.Primary()
                                    << ", expected " << floor.Id() << " (floor)";
                ok = false;
            }
        }

        // РЯДОМ с кубиком, но мимо него — вот здесь старый пикинг и врал.
        //
        // Кубик занимает +-0.5, а его ограничивающая сфера — 0.866: точки пола
        // на расстоянии 0.75 от центра лежат ВНЕ кубика, но ВНУТРИ старой
        // коробки. Человек целится в пол вплотную к объекту, а выделяется
        // объект. Обе точки ниже — на полу; обе обязаны выбрать пол.
        if (ok) {
            const glm::vec3 nearMiss[2] = {{0.75f, 0.0f, 0.75f}, {-0.75f, 0.0f, 0.7f}};
            for (const glm::vec3& p : nearMiss) {
                if (!ok) break;
                project(p, u, v);
                m_selection.SetPrimary(-1);
                PickAtViewport(u, v, false);
                if (m_selection.Primary() != floor.Id()) {
                    LOG_ERROR("Editor") << "SELFTEST: pick near box at (" << p.x << "," << p.z
                                        << ") selected " << m_selection.Primary() << ", expected "
                                        << floor.Id() << " (floor)";
                    ok = false;
                }
            }
        }


        // --- Инструменты над выделением: габариты, посадка, выравнивание, кадр ---
        if (ok) {
            // Габариты считаются по мировой матрице, а не по локальной коробке:
            // кубик поднят на 0.5 и стоит на полу, значит его коробка — от 0 до 1.
            m_selection.SetPrimary(box.Id());
            glm::vec3 lo(0.0f), hi(0.0f);
            bool boundsOk = SelectionBounds(lo, hi) && std::abs(lo.y) < 1e-4f &&
                            std::abs(hi.y - 1.0f) < 1e-4f && std::abs(hi.x - 0.5f) < 1e-4f;
            if (!boundsOk) {
                LOG_ERROR("Editor") << "SELFTEST: selection bounds wrong: y " << lo.y << ".." << hi.y
                                    << ", x max " << hi.x;
                ok = false;
            }
        }

        // Поднимаем кубик в воздух и роняем на пол: он обязан встать НИЗОМ на
        // поверхность, а не центром — иначе половина куба ушла бы под пол.
        if (ok) {
            // Место выбрано в стороне от начала координат: в сцене self-test'а
            // над центром уже стоят объекты предыдущих шагов, и кубик сел бы на
            // них — проверка молча измеряла бы не то.
            box.GetTransform().Position = glm::vec3(-8.0f, 6.0f, -8.0f);
            m_selection.SetPrimary(box.Id());
            DropSelectedToSurface();
            const float y = box.GetTransform().Position.y;
            if (std::abs(y - 0.5f) > 1e-3f) {
                LOG_ERROR("Editor") << "SELFTEST: drop to surface put box at y=" << y
                                    << ", expected 0.5 (bottom on floor)";
                ok = false;
            }
            box.GetTransform().Position = glm::vec3(0.0f, 0.5f, 0.0f);   // вернуть для шагов ниже
        }

        // Выравнивание: вторая сущность подтягивается по оси X к первичной.
        if (ok) {
            GameObject other = CreatePrimitiveEntity("PickBox2", MeshRef::Type::Cube);
            other.GetTransform().Position = glm::vec3(4.0f, 0.5f, 2.0f);
            m_selection.SetPrimary(other.Id());
            m_selection.Toggle(box.Id());   // первичная — box (x = 0)
            AlignSelection(0);
            const glm::vec3 p = other.GetTransform().Position;
            // По X подтянулась, остальные оси не тронуты.
            if (std::abs(p.x) > 1e-3f || std::abs(p.z - 2.0f) > 1e-3f) {
                LOG_ERROR("Editor") << "SELFTEST: align X gave (" << p.x << "," << p.z
                                    << "), expected (0, 2)";
                ok = false;
            }
            m_scene->RemoveObject(other.Id());
            m_selection.SetPrimary(-1);
        }

        // Фокус: камера отъезжает от объекта, но смотрит на него — проверяем,
        // что объект оказался ПЕРЕД камерой и на разумной дистанции.
        if (ok) {
            m_selection.SetPrimary(box.Id());
            m_camera.Position = glm::vec3(50.0f, 50.0f, 50.0f);
            FocusSelected();
            const glm::vec3 toBox = glm::vec3(0.0f, 0.5f, 0.0f) - m_camera.Position;
            const float dist = glm::length(toBox);
            const bool inFront = glm::dot(glm::normalize(toBox), m_camera.Front) > 0.99f;
            if (!inFront || dist > 20.0f || dist < 0.5f) {
                LOG_ERROR("Editor") << "SELFTEST: focus left camera at distance " << dist
                                    << ", in front: " << inFront;
                ok = false;
            }
        }

        for (const char* nm : {"PickFloor", "PickBox"}) {
            GameObject g = m_scene->FindByName(nm);
            if (g.Valid()) m_scene->RemoveObject(g.Id());
        }
        m_selection.SetPrimary(-1);
    }

    // --- Ортогональный вид ГЛАВНОГО слота ---
    //
    // Проверка по конкретной поломке: выпадающий список вида (Top/Front/Side) в
    // одиночной раскладке не делал ничего. Слоты 1..3 получали переопределение
    // матриц, а слот 0 рисовался безусловно перспективой — то есть
    // ортогональные виды работали только там, где рядом и так была перспектива.
    //
    // Проверяем то, что видно снаружи: после запроса ортогонального вида для
    // слота 0 матрица проекции обязана СТАТЬ ортогональной. У ортогональной
    // проекции нижняя строка равна (0,0,0,1), у перспективной там -1 в третьем
    // столбце — это и отличает их без всяких допущений.
    if (ok) {
        ViewRequest req[kMaxViews];
        req[0].Active = true;
        req[0].W = 640;
        req[0].H = 360;
        req[0].Ortho = true;
        req[0].View = glm::lookAt(glm::vec3(0.0f, 20.0f, 0.0f), glm::vec3(0.0f),
                                  glm::vec3(0.0f, 0.0f, -1.0f));   // вид сверху
        req[0].Proj = glm::ortho(-10.0f, 10.0f, -6.0f, 6.0f, 0.1f, 1000.0f);
        req[0].EyePos = glm::vec3(0.0f, 20.0f, 0.0f);
        SetViewRequests(req, 1);

        OnRender();   // кадр целиком: именно он и раскладывает виды

        const bool orthoNow = std::abs(m_proj[2][3]) < 1e-6f && std::abs(m_proj[3][3] - 1.0f) < 1e-6f;
        if (!orthoNow) {
            LOG_ERROR("Editor") << "SELFTEST: главный слот остался перспективным при запросе "
                                   "ортогонального вида (proj[2][3]=" << m_proj[2][3] << ")";
            ok = false;
        }

        // И обратно: снятый запрос возвращает перспективу, иначе вид залипал бы.
        req[0].Ortho = false;
        SetViewRequests(req, 1);
        OnRender();
        if (std::abs(m_proj[2][3] + 1.0f) > 1e-6f) {
            LOG_ERROR("Editor") << "SELFTEST: главный слот не вернулся в перспективу";
            ok = false;
        }
    }

    // --- Свои форматы: чужой файл -> .sagemesh -> сущность в сцене ---
    //
    // Проверка сквозная и именно в редакторе, потому что тут проверяется не
    // формат (его проверяют модульные тесты), а ДОСТИЖИМОСТЬ: конвертер, реестр
    // импортёров и загрузчик моделей должны сойтись так, чтобы сконвертированный
    // файл ставился в сцену обычным путём. Ровно эта связка и рвалась раньше у
    // других возможностей движка — они существовали, но из игры до них было не
    // добраться.
    if (ok) {
        const fs::path dir = m_project.Dir() / "assets";
        std::error_code fec;
        fs::create_directories(dir, fec);

        // Исходник в чужом формате — один блок Blockbench.
        const fs::path src = dir / "selftest_block.bbmodel";
        {
            std::ofstream f(src);
            f << R"({"resolution":{"width":16,"height":16},"elements":[{"type":"cube",)"
              << R"("from":[0,0,0],"to":[16,16,16],"faces":{)"
              << R"("north":{"uv":[0,0,16,16],"texture":0},"south":{"uv":[0,0,16,16],"texture":0},)"
              << R"("west":{"uv":[0,0,16,16],"texture":0},"east":{"uv":[0,0,16,16],"texture":0},)"
              << R"("up":{"uv":[0,0,16,16],"texture":0},"down":{"uv":[0,0,16,16],"texture":0}}}]})";
        }

        sage::assets::ConvertOptions copts;
        copts.Overwrite = true;
        const sage::assets::ConvertResult cr =
            sage::assets::ConvertAnyToNative(src.string(), {}, copts);
        if (!cr.Ok || cr.Vertices != 24 || cr.Triangles != 12) {
            LOG_ERROR("Editor") << "SELFTEST: convert to .sagemesh failed: " << cr.Error
                                << " (вершин " << cr.Vertices << ", треугольников "
                                << cr.Triangles << ")";
            ok = false;
        }

        // Сконвертированный файл грузится ОБЫЧНЫМ загрузчиком моделей — то есть
        // тем же, которым сцена грузит любую модель.
        if (ok) {
            std::shared_ptr<Mesh> mesh = ResourceManager::Instance().GetModel(cr.OutputPath);
            if (!mesh) {
                LOG_ERROR("Editor") << "SELFTEST: .sagemesh не загрузился через GetModel";
                ok = false;
            } else {
                // И ставится в сцену как модель, с правильными габаритами: блок
                // Blockbench — это ровно единица движка, а не шестнадцать.
                GameObject obj = m_scene->CreateObject("SelftestConverted");
                MeshRendererComponent& mr = obj.Renderer();
                mr.Ref = MeshRef{MeshRef::Type::Model, cr.OutputPath};
                mr.MeshPtr = mesh;
                const glm::vec3 size = mesh->BoundsMax() - mesh->BoundsMin();
                if (std::abs(size.x - 1.0f) > 0.01f || std::abs(size.y - 1.0f) > 0.01f) {
                    LOG_ERROR("Editor") << "SELFTEST: converted block size " << size.x << "x"
                                        << size.y << ", expected 1x1";
                    ok = false;
                }
                m_scene->RemoveObject(obj.Id());
            }
        }

        // Незнакомый формат обязан отказать ВНЯТНО, а не тихо ничего не сделать.
        if (ok) {
            const sage::assets::ConvertResult bad =
                sage::assets::ConvertAnyToNative((dir / "nope.fbx").string(), {}, copts);
            if (bad.Ok || bad.Error.empty()) {
                LOG_ERROR("Editor") << "SELFTEST: конвертация .fbx не отказала внятно";
                ok = false;
            }
        }

        fs::remove(src, fec);
        if (!cr.OutputPath.empty()) fs::remove(cr.OutputPath, fec);
    }

    // --- Префабы: сущность с ребёнком -> .sageprefab -> инстанс восстанавливает
    // поддерево (имя/компонент/иерархия), новые id ---
    if (ok) {
        GameObject root = m_scene->CreateObject("PrefabRoot");
        root.GetTransform().Position = {3.0f, 1.0f, 0.0f};
        m_scene->Registry().emplace<LightComponent>(root.Entity());
        GameObject kid = m_scene->CreateObject("PrefabKid");
        kid.GetTransform().Position = {0.0f, 1.0f, 0.0f};
        m_scene->SetParent(kid.Entity(), root.Entity());

        m_selection.SetPrimary(root.Id());
        fs::path prefabPath = m_project.Dir() / "assets" / "selftest.sageprefab";
        std::error_code pec;
        fs::create_directories(prefabPath.parent_path(), pec);
        std::string perr;
        bool saved = SaveSelectedAsPrefab(prefabPath, perr);
        // Удаляем оригинал и инстанцируем — проверяем восстановление из файла.
        if (saved) {
            m_scene->RemoveObject(root.Id());
            m_selection.SetPrimary(-1);
            int newRootId = InstantiatePrefab(prefabPath);
            GameObject nr = m_scene->Get(newRootId);
            bool rootOk = nr.Valid() && nr.Name() == "PrefabRoot" &&
                          m_scene->Registry().all_of<LightComponent>(nr.Entity()) &&
                          m_selection.Primary() == newRootId;
            // Ребёнок восстановлен и прикреплён к новому корню.
            const HierarchyComponent* h = nr.Valid()
                ? m_scene->Registry().try_get<HierarchyComponent>(nr.Entity()) : nullptr;
            bool childOk = h && h->Children.size() == 1;
            if (!rootOk || !childOk) {
                LOG_ERROR("Editor") << "SELFTEST: prefab round-trip failed (root " << rootOk
                                    << ", child " << childOk << ")";
                ok = false;
            }
            if (nr.Valid()) m_scene->RemoveObject(nr.Id());
        } else {
            LOG_ERROR("Editor") << "SELFTEST: prefab save failed: " << perr;
            ok = false;
        }
        fs::remove(prefabPath, pec);
        m_selection.SetPrimary(-1);
    }

    // --- Пресеты качества: Low гасит тяжёлые проходы, значения переживают
    // sage.cfg round-trip, оконные параметры не тронуты ---
    if (ok) {
        sage::EngineConfig pc;
        pc.Width = 1600; pc.VSync = false;
        pc.ApplyPreset(sage::QualityPreset::Low);
        bool presetOk = !pc.Shadows && std::abs(pc.RenderScale - 0.75f) < 0.001f &&
                        pc.Width == 1600 && pc.VSync == false;
        std::string pPath = "selftest_preset.cfg";
        bool fileOk = presetOk && pc.SaveFile(pPath);
        if (fileOk) {
            sage::EngineConfig back;
            fileOk = back.LoadFile(pPath) && !back.Shadows &&
                     std::abs(back.RenderScale - 0.75f) < 0.001f;
            std::error_code pEc;
            fs::remove(pPath, pEc);
        }
        if (!presetOk || !fileOk) {
            LOG_ERROR("Editor") << "SELFTEST: quality preset failed (preset " << presetOk
                                << ", file " << fileOk << ")";
            ok = false;
        }
    }

    // --- GI: бейк лайтмап + объёма проб и рендер с ними на реальном GPU ---
    // Проверяет весь конвейер запечённого GI: пометка статики, CPU-бейк
    // (BVH/path tracing/развёртка), ленивое создание GPU-ресурсов (HDR-атлас,
    // 3D-текстуры проб, меши с UV2) и отрисовку через RenderBatch.
    if (ok) {
        GameObject giFloor = CreatePrimitiveEntity("GI Floor", MeshRef::Type::Plane);
        giFloor.GetTransform().Scale = {10.0f, 1.0f, 10.0f};
        m_scene->Registry().emplace<GIStaticComponent>(giFloor.Entity());
        GameObject giCube = CreatePrimitiveEntity("GI Cube", MeshRef::Type::Cube);
        giCube.GetTransform().Position = {0.0f, 1.0f, 0.0f};
        m_scene->Registry().emplace<GIStaticComponent>(giCube.Entity());

        sage::gi::GISettings giSettings;
        giSettings.TexelsPerUnit = 2;
        giSettings.AtlasSize = 256;
        giSettings.SampleCount = 16;
        giSettings.Bounces = 2;
        giSettings.ProbeCellSize = 3.0f;
        giSettings.MaxProbeAxis = 8;
        sage::gi::BakeInput giInput = sage::gi::CollectBakeInput(*m_scene, giSettings);
        std::shared_ptr<sage::gi::GIState> giState = sage::gi::Bake(giInput);
        bool giBaked = giState->Baked && giState->Entities.size() == 2 &&
                       !giState->Pages.empty() && giState->Probes.Valid();
        if (giBaked) {
            m_scene->GI = giState;
            glm::mat4 view = glm::lookAt(glm::vec3(6, 6, 6), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
            glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
            sage::ecs::RenderStats st = m_renderer.RenderColorForTest(
                *m_scene, view, proj, glm::vec3(6, 6, 6), sage::ecs::CollectLighting(*m_scene));
            // Оба GI-объекта видимы и нарисованы уникальными мешами с UV2;
            // GPU-ресурсы (страница атласа, 3D-объём) создались лениво.
            bool giDrawn = st.Drawn >= 2;
            bool giGpu = giState->Pages[0].Gpu != nullptr && giState->Probes.Gpu != nullptr;
            if (!giDrawn || !giGpu) {
                LOG_ERROR("Editor") << "SELFTEST: GI render failed (drawn " << st.Drawn
                                    << ", gpu " << giGpu << ")";
                ok = false;
            }
        } else {
            LOG_ERROR("Editor") << "SELFTEST: GI bake failed (baked " << giState->Baked
                                << ", entities " << giState->Entities.size()
                                << ", pages " << giState->Pages.size() << ")";
            ok = false;
        }
        // Прибираем: GI-объекты и бейк не должны влиять на остальные шаги.
        m_scene->GI.reset();
        m_scene->RemoveObject(giFloor.Id());
        m_scene->RemoveObject(giCube.Id());
    }

    return ok;
}
