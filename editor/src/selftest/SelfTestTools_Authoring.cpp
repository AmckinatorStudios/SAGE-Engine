#include "SelfTestCommon.h"

bool EditorLayer::SelfTestToolsAuthoring() {
    bool ok = true;
    std::error_code ec;
    std::string err;
    (void)ec; (void)err;

    // --- ДЕВЯТИНА: ОТ КАРТИНКИ ДО ЭЛЕМЕНТА --------------------------------
    //
    // Геометрию нарезки проверяет модульный тест (tests/test_nineslice.cpp), а
    // здесь — ВОРОТА редактора: догадка читает настоящий файл с диска, .sage9
    // ложится рядом с ним и читается обратно, нарезка доезжает до компонента
    // выбранного элемента. Каждый из этих шагов по отдельности работает и на
    // сломанном пути — ломается именно стык, и увидеть это можно было только
    // руками.
    {
        // Рамка 16x16 с каймой в 4 пикселя — ровно то, из чего состоят наборы
        // интерфейса. Пишется PNG, а не подсовывается в память: догадка обязана
        // пройти через настоящий разбор файла.
        const fs::path imagePath = m_project.AssetsDir() / "selftest_frame.png";
        constexpr int N = 16, B = 4;
        std::vector<unsigned char> px((size_t)N * N * 4, 0);
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                const bool edge = x < B || y < B || x >= N - B || y >= N - B;
                unsigned char* q = px.data() + ((size_t)y * N + x) * 4;
                q[0] = edge ? 200 : 40; q[1] = edge ? 180 : 40; q[2] = edge ? 60 : 40; q[3] = 255;
            }
        }
        std::error_code iec;
        fs::create_directories(imagePath.parent_path(), iec);
        stbi_write_png(imagePath.string().c_str(), N, N, 4, px.data(), N * 4);

        sage::ui::NineSlice guess;
        if (!sage::ui::GuessBorderFromFile(imagePath.string(), guess)) {
            LOG_ERROR("Editor") << "SELFTEST: девятина не подобралась по настоящему файлу";
            ok = false;
        } else if (guess.Left != (float)B || guess.Top != (float)B ||
                   guess.Right != (float)B || guess.Bottom != (float)B) {
            LOG_ERROR("Editor") << "SELFTEST: девятина подобрала " << guess.Left << ","
                                << guess.Top << "," << guess.Right << "," << guess.Bottom
                                << " вместо " << B;
            ok = false;
        }

        // Описание ложится РЯДОМ с картинкой и читается обратно тем же путём,
        // каким его ищет окно при выборе файла.
        guess.EdgeFill = sage::ui::SliceFill::Tile;
        guess.DrawCenter = false;
        const std::string sidecar = sage::ui::NineSlice::SidecarPath(imagePath.string());
        std::string serr;
        if (!guess.SaveFile(sidecar, serr)) {
            LOG_ERROR("Editor") << "SELFTEST: .sage9 не записался: " << serr;
            ok = false;
        }
        sage::ui::NineSlice back;
        if (!sage::ui::NineSlice::LoadFile(sidecar, back, serr)) {
            LOG_ERROR("Editor") << "SELFTEST: .sage9 не прочитался: " << serr;
            ok = false;
        } else if (back.Left != guess.Left || back.EdgeFill != sage::ui::SliceFill::Tile ||
                   back.DrawCenter) {
            LOG_ERROR("Editor") << "SELFTEST: .sage9 вернулся не тем, чем был записан";
            ok = false;
        }

        // И доезжает до компонента: именно это делает кнопка «Применить к
        // выбранному», и именно это попадает в сцену и в собранную игру.
        // Имя заготовки — ровно то, что знает движок. Проверяем заодно, что
        // НЕИЗВЕСТНОЕ имя не роняет редактор: именно этим оно и кончалось —
        // заготовка не применялась, Transform не появлялся, а его брали
        // через get<>, то есть падение без единого сообщения.
        if (CreateUIEntity("нет такой заготовки").Valid()) {
            LOG_ERROR("Editor") << "SELFTEST: неизвестная заготовка создала элемент";
            ok = false;
        }
        GameObject el = CreateUIEntity("Panel");
        if (!el.Valid()) {
            LOG_ERROR("Editor") << "SELFTEST: элемент интерфейса для девятины не создался";
            ok = false;
        } else {
            sage::ui::Image& img = m_scene->Registry().get_or_emplace<sage::ui::Image>(el.Entity());
            img.Path = imagePath.string();
            img.SetSlice(back);
            // РЕЖИМ — ВМЕСТЕ С НАРЕЗКОЙ. Ровно это делает кнопка «Применить к
            // выбранному»: подобрать нарезку и не включить режим значит
            // применить её в никуда — числа на месте, картинка как была
            // растянутой, так и осталась.
            img.Fit = sage::ui::Image::Mode::NineSlice;
            const sage::ui::NineSlice roundTrip = img.Slice();
            if (roundTrip.Left != back.Left || roundTrip.EdgeFill != back.EdgeFill ||
                roundTrip.DrawCenter != back.DrawCenter) {
                LOG_ERROR("Editor") << "SELFTEST: девятина не доехала до компонента Image";
                ok = false;
            }
            // Нарезка обязана пережить сохранение сцены: поля объявлены в
            // таблице части, и запись с чтением идут по ней — но ровно эту связь
            // и рвут, забыв ключ.
            const std::string snapshot = SceneSerializer::SaveToString(*m_scene);
            std::unique_ptr<Scene> restored = SceneSerializer::LoadFromString(snapshot);
            bool found = false;
            if (restored) {
                auto view = restored->Registry().view<sage::ui::Image>();
                for (auto e : view) {
                    const sage::ui::Image& r = view.get<sage::ui::Image>(e);
                    if (r.Path != imagePath.string()) continue;
                    found = true;
                    if (r.SliceBorder.x != back.Left || r.SliceEdgeFill != back.EdgeFill ||
                        r.SliceDrawCenter != back.DrawCenter) {
                        LOG_ERROR("Editor") << "SELFTEST: девятина потерялась в сцене";
                        ok = false;
                    }
                    // Режим тоже: он объявлен в той же таблице полей, и забыть
                    // ключ там — значит получить сцену, которая открывается с
                    // растянутыми панелями вместо рамок.
                    if (r.Fit != sage::ui::Image::Mode::NineSlice) {
                        LOG_ERROR("Editor") << "SELFTEST: режим картинки потерялся в сцене";
                        ok = false;
                    }
                }
            }
            if (!found) {
                LOG_ERROR("Editor") << "SELFTEST: элемент с девятиной не нашёлся в сцене";
                ok = false;
            }
            m_scene->RemoveObject(el.Id());
        }
    }

    // --- СОХРАНЕНИЕ СЦЕНЫ РАБОТАЕТ КНОПКОЙ И ХОТКЕЕМ -----------------------
    //
    // Кнопка «Сохранить» и Ctrl+S ПРОСИЛИ ДИАЛОГ «Save Scene», а диалога с
    // таким именем нет: ImGui::OpenPopup для окна, которое никто не начинает,
    // молчит. Нажатие не делало ничего — ни файла, ни ошибки, ни строки в
    // логе, — и сцена терялась при первом же закрытии редактора. Проверяем то,
    // что зовут обе кнопки: у сцены с путём файл обязан обновиться на диске.
    {
        const fs::path scenePath = m_project.ScenesDir() / "selftest_save.sage";
        m_scenePath = scenePath;
        GameObject mark = m_scene->CreateObject("SelftestSaveMark");
        mark.GetTransform().Position = {3.0f, 4.0f, 5.0f};
        m_sceneDirty = true;

        std::error_code sec;
        fs::remove(scenePath, sec);
        SaveCurrentScene();
        if (!fs::exists(scenePath, sec)) {
            LOG_ERROR("Editor") << "SELFTEST: «Сохранить сцену» не записала файл "
                                << scenePath.string();
            ok = false;
        } else if (m_sceneDirty) {
            LOG_ERROR("Editor") << "SELFTEST: после записи сцена всё ещё считается изменённой";
            ok = false;
        }

        // Несохранённая сцена обязана СПРОСИТЬ, а не молча уступить место новой:
        // иначе работа исчезает по одному нажатию.
        m_sceneDirty = true;
        bool ran = false;
        AskUnsaved([&ran] { ran = true; });
        if (ran || !m_unsavedPrompt) {
            LOG_ERROR("Editor") << "SELFTEST: несохранённую сцену заменили без вопроса";
            ok = false;
        }
        m_unsavedPrompt = false;
        m_afterUnsaved = nullptr;

        // А сохранённая — не спрашивает: вопрос, который задают всегда,
        // перестают читать.
        m_sceneDirty = false;
        ran = false;
        AskUnsaved([&ran] { ran = true; });
        if (!ran || m_unsavedPrompt) {
            LOG_ERROR("Editor") << "SELFTEST: сохранённая сцена спросила о потере правок";
            ok = false;
        }
        m_unsavedPrompt = false;
        m_afterUnsaved = nullptr;

        // ЗАКРЫТИЕ ОКНА ИДЁТ ЧЕРЕЗ ТОТ ЖЕ ВОПРОС, и проверять это обязательно.
        //
        // Перехват уже был написан — и не работал НИ РАЗУ: он стоял внутри
        // отрисовки кадра, а флаг закрытия GLFW ставит в PollEvents, в конце
        // кадра. Условие главного цикла проверяется раньше, чем слой получит
        // управление, поэтому редактор закрывался молча вместе с
        // несохранённой сценой, а код перехвата выглядел совершенно рабочим.
        // Теперь закрытие спрашивают хуком (Layer::OnCloseRequest), и проверка
        // зовёт ровно его — то же самое, что зовёт главный цикл.
        m_sceneDirty = true;
        m_closeAfterPrompt = false;
        if (OnCloseRequest()) {
            LOG_ERROR("Editor") << "SELFTEST: закрытие с несохранённой сценой не задержано";
            ok = false;
        }
        if (!m_unsavedPrompt || !m_closeAfterPrompt) {
            LOG_ERROR("Editor") << "SELFTEST: закрытие не подняло вопрос о несохранённой сцене";
            ok = false;
        }
        m_unsavedPrompt = false;
        m_closeAfterPrompt = false;
        m_afterUnsaved = nullptr;

        // Сохранённая сцена закрытию не мешает: иначе выйти из редактора было
        // бы нельзя вовсе.
        m_sceneDirty = false;
        if (!OnCloseRequest() || m_unsavedPrompt) {
            LOG_ERROR("Editor") << "SELFTEST: сохранённая сцена держит закрытие";
            ok = false;
        }
        m_unsavedPrompt = false;
        m_afterUnsaved = nullptr;

        m_scene->RemoveObject(mark.Id());
        fs::remove(scenePath, sec);
        m_scenePath.clear();
        m_sceneDirty = false;
    }

    // --- ПАНЕЛЬ АССЕТОВ НЕ ВЫХОДИТ ИЗ assets/ ------------------------------
    //
    // Выход наружу ломается тихо: панель показывает папку проекта, в ней «пусто»
    // (ассетов там и не было), и человек решает, что ассеты потерялись. А путь,
    // взятый снаружи, в сцене не работает — он не переживёт сборку игры.
    {
        const fs::path assets = m_project.AssetsDir();
        const fs::path sub = assets / "selftest_cwd";
        fs::create_directories(sub, ec);

        m_assetsCwd = m_project.Dir();              // выше корня — вернуть в корень
        AssetsPanel::ClampCwd(*this);
        if (fs::weakly_canonical(m_assetsCwd, ec) != fs::weakly_canonical(assets, ec)) {
            LOG_ERROR("Editor") << "SELFTEST: панель ассетов вышла выше assets/: "
                                << m_assetsCwd.string();
            ok = false;
        }

        m_assetsCwd = sub;                          // внутри корня — не трогать
        AssetsPanel::ClampCwd(*this);
        if (fs::weakly_canonical(m_assetsCwd, ec) != fs::weakly_canonical(sub, ec)) {
            LOG_ERROR("Editor") << "SELFTEST: панель ассетов не удержалась во вложенной папке";
            ok = false;
        }

        m_assetsCwd = assets / "нет-такой-папки";   // исчезнувшая папка — в корень
        AssetsPanel::ClampCwd(*this);
        if (fs::weakly_canonical(m_assetsCwd, ec) != fs::weakly_canonical(assets, ec)) {
            LOG_ERROR("Editor") << "SELFTEST: после пропавшей папки панель осталась «нигде»";
            ok = false;
        }
        fs::remove_all(sub, ec);
    }

    // --- ЗАМОК СВЯЗИ ОСЕЙ МАСШТАБА -----------------------------------------
    //
    // Пропорции обязаны сохраняться ТОЧНО: «почти» здесь означает предмет,
    // который на глаз квадратный, а в сцене слегка приплюснут.
    {
        const glm::vec3 out =
            InspectorPanel::LinkScaleAxes(glm::vec3(2.0f, 1.0f, 4.0f), glm::vec3(3.0f, 1.0f, 4.0f));
        // Потянули X с 2 до 3 — это в полтора раза: Y и Z обязаны стать 1.5 и 6.
        if (std::abs(out.x - 3.0f) > 1e-4f || std::abs(out.y - 1.5f) > 1e-4f ||
            std::abs(out.z - 6.0f) > 1e-4f) {
            LOG_ERROR("Editor") << "SELFTEST: связанные оси масштаба не держат пропорцию: "
                                << out.x << ", " << out.y << ", " << out.z;
            ok = false;
        }
        // Ось была нулевой — отношения нет, и всем осям ставится одно значение:
        // иначе предмет остался бы плоским при любом вводе.
        const glm::vec3 zero =
            InspectorPanel::LinkScaleAxes(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 2.0f, 0.0f));
        if (std::abs(zero.x - 2.0f) > 1e-4f || std::abs(zero.z - 2.0f) > 1e-4f) {
            LOG_ERROR("Editor") << "SELFTEST: из нулевого масштаба оси не восстановились";
            ok = false;
        }
        // Ничего не меняли — ничего и не должно поменяться.
        const glm::vec3 same =
            InspectorPanel::LinkScaleAxes(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f, 2.0f, 3.0f));
        if (std::abs(same.y - 2.0f) > 1e-4f || std::abs(same.z - 3.0f) > 1e-4f) {
            LOG_ERROR("Editor") << "SELFTEST: связь осей сработала без правки";
            ok = false;
        }
    }

    // --- СТРОКА ИНСТРУМЕНТОВ ВЬЮПОРТА: значки и цвета гизмо ----------------
    //
    // Проверяется ровно то, что человек видит глазами и что тихо ломается:
    // отсутствующий значок рисуется ЗАГЛУШКОЙ (пустой квадрат), а цвета гизмо
    // молча возвращаются к ImGuizmo'вским по умолчанию — вдвое темнее нужного.
    // Ни то, ни другое не роняет сборку и не валит ни один другой тест.
    {
        // Значки строки — поимённо. Имена «про смысл», и опечатка в любом из
        // них превращает кнопку в пустой квадратик.
        static const char* const kToolIcons[] = {"select", "move",  "rotate", "scale",
                                                 "world",  "cube",  "magnet", "grid",
                                                 "sun",    "dots"};
        for (const char* icon : kToolIcons) {
            if (!EditorIcons::Has(icon) || !EditorIcons::HasGlyph(icon)) {
                LOG_ERROR("Editor") << "SELFTEST: значка строки инструментов нет в наборе — '"
                                    << icon << "'";
                ok = false;
            }
        }

        // «Только выбор» обязан НЕ совпадать ни с одной операцией ImGuizmo:
        // совпади он с любой — и в режиме выбора на объекте висело бы гизмо,
        // перехватывающее клики.
        const int select = EditorHost::kGizmoSelectOnly;
        if (select == (int)ImGuizmo::TRANSLATE || select == (int)ImGuizmo::ROTATE ||
            select == (int)ImGuizmo::SCALE || select == (int)ImGuizmo::UNIVERSAL ||
            select == (int)ImGuizmo::BOUNDS) {
            LOG_ERROR("Editor") << "SELFTEST: режим «только выбор» совпал с операцией гизмо";
            ok = false;
        }

        // Цвета осей манипулятора — ЧИСТЫЕ: у каждой свой канал почти в
        // максимуме, а два других заметно ниже. Приглушённая тройка по
        // умолчанию (0.666 на канал) этой проверки не проходит.
        EditorTheme::ApplyGizmoColors();
        const ImGuizmo::Style& gs = ImGuizmo::GetStyle();
        const int axes[3] = {ImGuizmo::DIRECTION_X, ImGuizmo::DIRECTION_Y, ImGuizmo::DIRECTION_Z};
        const char* names[3] = {"X", "Y", "Z"};
        for (int a = 0; a < 3; ++a) {
            const ImVec4& c = gs.Colors[axes[a]];
            const float ch[3] = {c.x, c.y, c.z};
            const float own = ch[a];
            const float other = std::max(ch[(a + 1) % 3], ch[(a + 2) % 3]);
            if (own < 0.85f || other > 0.55f) {
                LOG_ERROR("Editor") << "SELFTEST: ось " << names[a]
                                    << " гизмо не чистого цвета: свой канал " << own
                                    << ", чужой " << other;
                ok = false;
            }
        }
    }

    // --- Новые инструменты редактора: загрузка моделей, префабы, редактор кода,
    //     подтверждения. Проверяем ИМЕННО то, что человек делает мышью, но без
    //     кликов: те же методы, что зовут кнопки.
    {
        // 1. Модель в форматах, которые экспортирует 3D-редактор. Раньше сущность
        //    умела только .obj — то есть «свою модель» поставить было нельзя.
        const fs::path modelDir = m_project.AssetsDir() / "selftest_models";
        fs::create_directories(modelDir, ec);
        const fs::path objPath = modelDir / "tri.obj";
        {
            std::ofstream f(objPath);
            f << "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n";
        }
        if (!ModelLoader::IsSupportedModel(objPath.string())) {
            LOG_ERROR("Editor") << "SELFTEST: .obj не считается поддерживаемым";
            ok = false;
        }
        if (!ModelLoader::IsSupportedModel("hero.glb") ||
            !ModelLoader::IsSupportedModel("hero.gltf")) {
            LOG_ERROR("Editor") << "SELFTEST: glTF не считается поддерживаемым";
            ok = false;
        }
        GameObject modelObj = m_scene->CreateObject("SelfTestModel");
        MeshRendererComponent& mmr = modelObj.Renderer();
        mmr.Ref.type = MeshRef::Type::Model;
        mmr.Ref.path = objPath.string();
        mmr.MeshPtr = ResourceManager::Instance().GetModel(mmr.Ref.path);
        if (!mmr.MeshPtr) {
            LOG_ERROR("Editor") << "SELFTEST: модель не загрузилась: " << mmr.Ref.path;
            ok = false;
        }

        // 2. Префаб: сохранить сущность как шаблон и поставить копию.
        const fs::path prefabPath = m_project.AssetsDir() / "selftest_tool.sageprefab";
        std::string perr;
        if (!sage::scene::SavePrefab(*m_scene, modelObj.Entity(), prefabPath.string(), perr)) {
            LOG_ERROR("Editor") << "SELFTEST: префаб не сохранился: " << perr;
            ok = false;
        } else {
            const int copyId = sage::scene::InstantiatePrefabAt(*m_scene, prefabPath.string(),
                                                                {7.0f, 1.0f, -2.0f});
            GameObject copy = m_scene->Get(copyId);
            if (!copy.Valid() || copy.Name() != "SelfTestModel") {
                LOG_ERROR("Editor") << "SELFTEST: префаб не поставился в сцену";
                ok = false;
            } else if (std::abs(copy.GetTransform().Position.x - 7.0f) > 1e-3f) {
                LOG_ERROR("Editor") << "SELFTEST: префаб встал не в заданную точку";
                ok = false;
            }
            m_scene->RemoveObject(copyId);
        }
        m_scene->RemoveObject(modelObj.Id());

        // 4. Подтверждение опасного действия: пока вопрос включён, объект
        //    остаётся; с выключенным вопросом действие проходит сразу. Именно
        //    так и должна работать галочка «больше не спрашивать».
        GameObject victim = m_scene->CreateObject("SelfTestVictim");
        const int victimId = victim.Id();
        m_selection.SetPrimary(victimId);
        m_confirm.SetSuppressed("delete-entity", false);
        DeleteSelected();
        if (!m_scene->Get(victimId).Valid()) {
            LOG_ERROR("Editor") << "SELFTEST: удаление прошло БЕЗ подтверждения";
            ok = false;
        }
        m_confirm.SetSuppressed("delete-entity", true);
        m_selection.SetPrimary(victimId);
        DeleteSelected();
        if (m_scene->Get(victimId).Valid()) {
            LOG_ERROR("Editor") << "SELFTEST: с выключенным вопросом объект не удалился";
            ok = false;
        }
        // Возвращаем «не спрашивать» на остаток прогона: дальше кликать
        // по-прежнему некому.
        m_confirm.SetSuppressed("delete-entity", true);
        m_selection.Clear();
    }

    // Настоящая картинка 4x4: слот материала обязан не просто получить путь, а
    // ЗАГРУЗИТЬ по нему карту, иначе проверка пройдёт на битой ссылке.
    auto WriteSelfTestPng = [](const fs::path& file, unsigned char r, unsigned char g,
                               unsigned char b) {
        unsigned char px[4 * 4 * 3];
        for (int i = 0; i < 4 * 4; ++i) { px[i * 3] = r; px[i * 3 + 1] = g; px[i * 3 + 2] = b; }
        stbi_write_png(file.string().c_str(), 4, 4, 3, px, 4 * 3);
    };

    // --- МНОГОМАТЕРИАЛЬНАЯ МОДЕЛЬ ИЗ СКАЧАННОГО НАБОРА -------------------------
    //
    // Ровно то, на что жаловались: набор с сайта кладут в проект, тащат модель в
    // сцену — и каждая часть приезжает с ПУСТЫМ слотом материала, то есть белой.
    // Проверяется весь путь тем же методом, который зовёт перетаскивание
    // (AddAssetToScene), а не по кускам: разбор материалов работал и раньше,
    // ломалось соединение между ним и слотами.
    if (ok) {
        const fs::path pack = m_project.AssetsDir() / "selftest_pack";
        fs::create_directories(pack / "textures", ec);

        // Две части с РАЗНЫМИ материалами и разными картами — как в наборе, где
        // у каждой постройки своя текстура. Одного материала мало: слот мог бы
        // заполниться «первым попавшимся» и проверка этого не заметила бы.
        WriteSelfTestPng(pack / "textures" / "soil.png", 200, 120, 60);
        WriteSelfTestPng(pack / "textures" / "water.png", 40, 90, 200);
        // Карта нормалей и упакованная ORM: без них проверка сказала бы только
        // про albedo, а жалоба была ровно в том, что остальные карты не едут.
        WriteSelfTestPng(pack / "textures" / "soil_n.png", 128, 128, 255);
        WriteSelfTestPng(pack / "textures" / "soil_orm.png", 40, 160, 220);
        {
            std::ofstream f(pack / "geom.bin", std::ios::binary);
            const float verts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
            const unsigned short idx[3] = {0, 1, 2};
            f.write((const char*)verts, sizeof(verts));
            f.write((const char*)idx, sizeof(idx));
        }
        {
            std::ofstream f(pack / "scene.gltf");
            f << R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 1]}],
  "nodes": [
    {"name": "Soil_Soil_texture_0", "mesh": 0},
    {"name": "Water_Water_texture_0", "mesh": 1}
  ],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]},
    {"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 1}]}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
     "min": [0,0,0], "max": [1,1,0]},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 6}
  ],
  "buffers": [{"byteLength": 42, "uri": "geom.bin"}],
  "images": [{"uri": "textures/soil.png"}, {"uri": "textures/water.png"},
             {"uri": "textures/soil_n.png"}, {"uri": "textures/soil_orm.png"}],
  "textures": [{"source": 0}, {"source": 1}, {"source": 2}, {"source": 3}],
  "materials": [
    {"name": "Soil_texture",
     "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1],
                              "baseColorTexture": {"index": 0},
                              "metallicRoughnessTexture": {"index": 3}},
     "normalTexture": {"index": 2},
     "occlusionTexture": {"index": 3}},
    {"name": "Water_texture", "pbrMetallicRoughness": {
        "baseColorFactor": [1, 1, 1, 1], "baseColorTexture": {"index": 1}}}
  ]
})";
        }
        sage::AssetDatabase::Instance().ScanProject(m_project.Dir().string());

        // Тот самый жест: бросить модель в сцену.
        const size_t before = m_scene->Count();
        if (!AddAssetToScene(pack / "scene.gltf") || m_scene->Count() != before + 1) {
            LOG_ERROR("Editor") << "SELFTEST: модель из набора не встала в сцену";
            ok = false;
        } else {
            GameObject placed = m_scene->Get(m_selection.Primary());
            MeshRendererComponent& pmr = placed.Renderer();
            if (!pmr.MeshPtr || pmr.MeshPtr->Submeshes().size() != 2) {
                LOG_ERROR("Editor") << "SELFTEST: у модели набора нет разметки на две части";
                ok = false;
            } else if (pmr.Slots.size() != 2) {
                LOG_ERROR("Editor") << "SELFTEST: слотов материалов нет (частей "
                                    << pmr.MeshPtr->Submeshes().size() << ")";
                ok = false;
            } else {
                for (size_t i = 0; i < pmr.Slots.size(); ++i) {
                    if (pmr.Slots[i].Path.empty() || !pmr.Slots[i].Ptr) {
                        LOG_ERROR("Editor") << "SELFTEST: слот " << i
                                            << " пуст — материал модели не назначен";
                        ok = false;
                        break;
                    }
                    // И это НЕ болванка: у материала из набора есть карта.
                    if (pmr.Slots[i].Ptr->TexturePath.empty() || !pmr.Slots[i].Ptr->AlbedoTex) {
                        LOG_ERROR("Editor") << "SELFTEST: у материала слота " << i
                                            << " нет карты albedo";
                        ok = false;
                        break;
                    }
                }
                // НЕ ТОЛЬКО ALBEDO. У первой части в файле заданы карта
                // нормалей и упакованная ORM — значит в материале обязаны
                // оказаться и нормали, и металличность, и шероховатость, и
                // затенение. Жалоба звучала именно так: «загружается только
                // albedo, остальные не работают».
                if (ok) {
                    const Material& first = *pmr.Slots[0].Ptr;
                    struct Map { const char* Name; const std::string& Path;
                                 const std::shared_ptr<Texture>& Tex; };
                    const Map maps[] = {{"normal", first.NormalMapPath, first.NormalTex},
                                        {"metallic", first.MetallicMapPath, first.MetallicTex},
                                        {"roughness", first.RoughnessMapPath, first.RoughnessTex},
                                        {"AO", first.AOMapPath, first.AOTex}};
                    for (const Map& m : maps) {
                        if (m.Path.empty() || !m.Tex) {
                            LOG_ERROR("Editor") << "SELFTEST: карта " << m.Name
                                                << " не приехала из модели (путь "
                                                << (m.Path.empty() ? "пуст" : "есть")
                                                << ", текстура "
                                                << (m.Tex ? "есть" : "нет") << ")";
                            ok = false;
                        }
                    }
                }
                // Разные части — разные материалы: один на всё это ровно та
                // болячка, ради которой заводились слоты.
                if (ok && pmr.Slots[0].Path == pmr.Slots[1].Path) {
                    LOG_ERROR("Editor") << "SELFTEST: обе части покрашены одним материалом";
                    ok = false;
                }
            }
            m_scene->RemoveObject(placed.Id());
        }

        // ВТОРОЙ ЖЕСТ: бросить ту же модель НА УЖЕ СТОЯЩИЙ объект. Он подменяет
        // меш, а слоты при этом сбрасываются вместе со старой моделью — и
        // единственный из четырёх путей оставался без импорта материалов, то
        // есть объект оставался белым, сколько его ни перетаскивай.
        if (ok) {
            GameObject host = m_scene->CreateObject("SelfTestDropTarget");
            const int hostId = host.Id();
            if (!ApplyAssetToEntity(hostId, pack / "scene.gltf")) {
                LOG_ERROR("Editor") << "SELFTEST: модель не легла на существующий объект";
                ok = false;
            } else {
                MeshRendererComponent& hmr = m_scene->Get(hostId).Renderer();
                const bool filled = hmr.Slots.size() == 2 && !hmr.Slots[0].Path.empty() &&
                                    !hmr.Slots[1].Path.empty();
                if (!filled) {
                    LOG_ERROR("Editor")
                        << "SELFTEST: брошенная на объект модель осталась без материалов (слотов "
                        << hmr.Slots.size() << ")";
                    ok = false;
                }
            }
            m_scene->RemoveObject(hostId);
        }

        // ТРЕТИЙ ЖЕСТ: бросок ВО ВЬЮПОРТ. Он ставит модель в точку под курсором
        // и был ПЯТЫМ путём, который импорт материалов не звал: сначала забыли
        // три из четырёх, потом один из пяти. Поэтому импорт переехал внутрь
        // SetEntityMesh — забыть его больше нельзя, — а проверка держит все
        // жесты разом.
        if (ok) {
            const size_t was = m_scene->Count();
            const glm::mat4 dropView = m_camera.GetViewMatrix();
            const glm::mat4 dropProj =
                glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
            if (!DropAssetAtViewport(dropView, dropProj, 0.5f, 0.5f, pack / "scene.gltf") ||
                m_scene->Count() != was + 1) {
                LOG_ERROR("Editor") << "SELFTEST: модель не встала во вьюпорт";
                ok = false;
            } else {
                MeshRendererComponent& vmr = m_scene->Get(m_selection.Primary()).Renderer();
                if (vmr.Slots.size() != 2 || vmr.Slots[0].Path.empty() ||
                    vmr.Slots[1].Path.empty()) {
                    LOG_ERROR("Editor")
                        << "SELFTEST: брошенная во вьюпорт модель осталась без материалов (слотов "
                        << vmr.Slots.size() << ")";
                    ok = false;
                }
                m_scene->RemoveObject(m_selection.Primary());
            }
        }
        m_selection.Clear();
    }

    return ok;
}
