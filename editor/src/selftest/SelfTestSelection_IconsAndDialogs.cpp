#include "SelfTestCommon.h"

bool EditorLayer::SelfTestIconsAndDialogs() {
    bool ok = true;
    std::error_code ec;
    std::string err;
    (void)ec; (void)err;
    // --- Иконки: шрифт собрался и у каждого имени есть свой глиф ---------------
    //
    // Отказ здесь беззвучный: не собрался шрифт — на всех кнопках пустые
    // квадратики с диагональю, и редактор при этом работает. Человек видит это
    // глазами, а никакой тест — нет, поэтому спрашиваем прямо.
    if (ok) {
        if (!EditorIcons::FontReady()) {
            LOG_ERROR("Editor") << "SELFTEST: icon font did not build";
            ok = false;
        }
        // И каждое имя из набора обязано иметь глиф: имя без глифа рисуется той
        // же заглушкой, то есть опечатка в имени иконки выглядит как «иконки
        // сломались», а не как опечатка.
        for (int i = 0; ok && i < EditorIcons::Count(); ++i) {
            const char* name = EditorIcons::NameAt(i);
            if (!name || !EditorIcons::HasGlyph(name)) {
                LOG_ERROR("Editor") << "SELFTEST: no glyph for icon '" << (name ? name : "?") << "'";
                ok = false;
            }
        }
    }

    // --- Папка: ПУСТАЯ и НЕПУСТАЯ выглядят по-разному -------------------------
    //
    // Значок один на все каталоги означал, что «где мои модели» решается только
    // заходом внутрь. Отказ здесь беззвучный ровно так же, как у иконок: панель
    // продолжает работать, просто перестаёт что-либо сообщать.
    if (ok) {
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "sage_selftest_folder_state";
        fs::remove_all(root, ec);
        const fs::path empty = root / "empty";
        const fs::path full = root / "full";
        const fs::path onlyMeta = root / "only_meta";
        fs::create_directories(empty, ec);
        fs::create_directories(full, ec);
        fs::create_directories(onlyMeta, ec);
        { std::ofstream(full / "thing.sagemat") << "{}"; }
        // Сайдкар .meta без своего файла — служебная запись, а не содержимое:
        // папка с одним только .meta для человека пустая.
        { std::ofstream(onlyMeta / "thing.sagemat.meta") << "{}"; }

        auto icon = [](const fs::path& p) { return std::string(AssetsPanel::FolderIcon(p)); };
        if (icon(empty) != "folder") {
            LOG_ERROR("Editor") << "SELFTEST: empty folder got icon '" << icon(empty) << "'";
            ok = false;
        }
        if (ok && icon(full) != "folder-full") {
            LOG_ERROR("Editor") << "SELFTEST: folder with a file got icon '" << icon(full) << "'";
            ok = false;
        }
        if (ok && icon(onlyMeta) != "folder") {
            LOG_ERROR("Editor") << "SELFTEST: folder with only .meta got icon '"
                                << icon(onlyMeta) << "'";
            ok = false;
        }
        // ПАПКА С ФАЙЛАМИ, НО БЕЗ ПОДПАПОК — отдельно, потому что именно на ней
        // дерево и расходилось с сеткой: заполненность там считалась по одним
        // подпапкам, и папка с полусотней текстур выглядела в дереве пустой.
        // Проверяем то, что рисуют ОБА места, — один и тот же FolderIcon.
        {
            const fs::path filesOnly = root / "files_only";
            fs::create_directories(filesOnly, ec);
            { std::ofstream(filesOnly / "a.png") << "x"; }
            { std::ofstream(filesOnly / "b.png") << "x"; }
            if (ok && icon(filesOnly) != "folder-full") {
                LOG_ERROR("Editor") << "SELFTEST: папка с файлами без подпапок считается пустой";
                ok = false;
            }
        }

        // ЦВЕТНАЯ МЕТКА — и в сетке, и в дереве, из одного источника. Пока
        // назначать её можно было только из сетки, в дереве она оставалась
        // умолчанием; теперь оба места спрашивают foldercolors::Get, и проверка
        // сторожит именно это — что метка НАЗНАЧАЕТСЯ и ЧИТАЕТСЯ.
        if (ok) {
            namespace foldercolors = sage::editor::foldercolors;
            foldercolors::SetProject(root);
            const glm::vec3 want(0.40f, 0.60f, 0.95f);
            foldercolors::Set(full, want);
            glm::vec3 got(0.0f);
            if (!foldercolors::Get(full, got) || std::fabs(got.b - want.b) > 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: цветная метка папки не назначилась";
                ok = false;
            }
            // Метка переживает перезагрузку проекта: она лежит в файле проекта,
            // а не в памяти сеанса. Цвет, живущий один запуск, — это цвет,
            // который человек поставит второй раз и перестанет верить.
            foldercolors::SetProject(fs::path{});
            foldercolors::SetProject(root);
            glm::vec3 reloaded(0.0f);
            if (ok && (!foldercolors::Get(full, reloaded) ||
                       std::fabs(reloaded.b - want.b) > 0.001f)) {
                LOG_ERROR("Editor") << "SELFTEST: цветная метка папки не пережила перечитывание";
                ok = false;
            }
            // Снятая метка не остаётся в файле.
            foldercolors::Clear(full);
            glm::vec3 cleared(0.0f);
            if (ok && foldercolors::Get(full, cleared)) {
                LOG_ERROR("Editor") << "SELFTEST: снятая метка папки осталась";
                ok = false;
            }
            foldercolors::SetProject(m_project.Dir());  // возвращаем проект прогона
        }

        // И оба значка обязаны быть в шрифте: имя без глифа рисуется заглушкой,
        // и разница между пустой и полной папкой пропадает снова.
        if (ok && (!EditorIcons::HasGlyph("folder") || !EditorIcons::HasGlyph("folder-full"))) {
            LOG_ERROR("Editor") << "SELFTEST: folder icons have no glyphs";
            ok = false;
        }
        // А заодно — ВСЕ значки редактора разом. Список имён и шрифт живут в
        // разных местах (EditorIcons.cpp и generated .inl из
        // scripts/gen_icon_font.py), и расходятся они молча: имя, которому не
        // сделали глиф, рисуется пустотой, и кнопка выглядит сломанной. Правка
        // списка без прогона генератора — ровно тот случай, который эта
        // проверка и ловит.
        for (int i = 0; ok && i < EditorIcons::Count(); ++i) {
            const char* name = EditorIcons::NameAt(i);
            if (name && !EditorIcons::HasGlyph(name)) {
                LOG_ERROR("Editor") << "SELFTEST: у значка '" << name
                                    << "' нет глифа — перегенерируйте шрифт "
                                       "(scripts/gen_icon_font.py)";
                ok = false;
            }
        }
        fs::remove_all(root, ec);
    }

    // --- Слот ассета берёт ПАПКУ, а не только файл ----------------------------
    //
    // Папка кубической карты неба — единственный ассет-каталог в редакторе, и
    // до появления типа Folder её путь набирали руками. Слот, который не узнаёт
    // в каталоге каталог, отказал бы от броска молча — ровно так же, как раньше
    // молчало поле ввода.
    if (ok) {
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "sage_selftest_folder_slot";
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        { std::ofstream(root / "px.png") << "x"; }

        if (assetslot::KindOf(root) != assetslot::Kind::Folder) {
            LOG_ERROR("Editor") << "SELFTEST: a directory is not recognised as a folder asset";
            ok = false;
        }
        if (ok && !assetslot::Accepts(assetslot::Kind::Folder, root)) {
            LOG_ERROR("Editor") << "SELFTEST: folder slot refuses a directory";
            ok = false;
        }
        // И наоборот: в слот папки нельзя бросить файл, а в слот текстуры —
        // папку. Слот, берущий всё подряд, не лучше поля ввода.
        if (ok && assetslot::Accepts(assetslot::Kind::Folder, root / "px.png")) {
            LOG_ERROR("Editor") << "SELFTEST: folder slot accepts a file";
            ok = false;
        }
        if (ok && assetslot::Accepts(assetslot::Kind::Texture, root)) {
            LOG_ERROR("Editor") << "SELFTEST: texture slot accepts a directory";
            ok = false;
        }
        if (ok && assetslot::KindOf("shaders/water.frag") != assetslot::Kind::Shader) {
            LOG_ERROR("Editor") << "SELFTEST: .frag is not recognised as a shader asset";
            ok = false;
        }
        fs::remove_all(root, ec);
    }

    // --- Окно настроек импорта модели: кому оно показывается -------------------
    //
    // Окно спрашивает настройки у МОДЕЛИ БЕЗ .sageimport — и только у неё:
    // модель с настройками второй раз вопросов не задаёт, а картинка и
    // материал моделями не являются. Внесённая в проект папка набора даёт
    // список ровно тех моделей, что ещё без настроек.
    if (ok) {
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "sage_selftest_model_import";
        fs::remove_all(root, ec);
        fs::create_directories(root / "OBJ", ec);
        { std::ofstream(root / "OBJ" / "bush.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"; }
        { std::ofstream(root / "OBJ" / "tree.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"; }
        { std::ofstream(root / "leaf.png") << "x"; }
        ModelLoader::ImportSettings configured;
        configured.Scale = 2.0f;
        ModelLoader::SaveImportSettings((root / "OBJ" / "tree.obj").string(), configured);

        namespace modelimport = sage::editor::modelimport;
        const std::vector<fs::path> asked = modelimport::ModelsNeedingSettings(root);
        if (asked.size() != 1 || asked.front().filename() != "bush.obj") {
            LOG_ERROR("Editor") << "SELFTEST: окно импорта спрашивает не те модели (" << asked.size()
                                << ")";
            ok = false;
        }
        if (ok && (modelimport::NeedsSettings(root / "leaf.png") ||
                   modelimport::NeedsSettings(root / "OBJ" / "tree.obj"))) {
            LOG_ERROR("Editor") << "SELFTEST: окно импорта спрашивает картинку или настроенную модель";
            ok = false;
        }
        // Пустой список — действие сразу, без окна: внесение папки без моделей
        // не должно открывать пустое окно.
        bool ran = false;
        modelimport::Ask({}, [&ran]() { ran = true; });
        if (ok && (!ran || modelimport::IsOpen())) {
            LOG_ERROR("Editor") << "SELFTEST: окно импорта открылось без моделей";
            ok = false;
        }
        fs::remove_all(root, ec);
    }

    // --- Файловый диалог: обложки и выбор вида ---------------------------------
    //
    // Диалог показывал строку с именем и размером — и всё. Имена в скачанных
    // наборах не значат ничего («sky_04.png», «T_Rock_02_D.png»), поэтому выбор
    // картинки сводился к открыванию файлов по одному. Проверяется то, из чего
    // обложка берётся: картинка обязана дать обложку БЕЗ превью-рендера (она
    // сама себе обложка), а папка и текст — не дать ничего, иначе диалог рисовал
    // бы мусор на месте картинки.
    if (ok) {
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "sage_selftest_dialog";
        fs::remove_all(root, ec);
        fs::create_directories(root / "sub", ec);
        const fs::path png = root / "cover.png";
        {
            unsigned char px[4 * 4 * 3];
            for (int i = 0; i < 4 * 4; ++i) { px[i * 3] = 200; px[i * 3 + 1] = 40; px[i * 3 + 2] = 40; }
            stbi_write_png(png.string().c_str(), 4, 4, 3, px, 4 * 3);
        }
        { std::ofstream(root / "notes.txt") << "текст"; }

        // Обложка картинки приезжает ФОНОМ (кэш обложек, Thumbnails.h): слот
        // больше не декодирует картинку целиком в кадре. Ждём её, прокачивая
        // очередь так же, как это делает кадр редактора, — но не вечно.
        uint64_t pictureCover = 0;
        for (int attempt = 0; attempt < 400 && pictureCover == 0; ++attempt) {
            pictureCover = assetslot::Cover(nullptr, png, 64);
            if (pictureCover == 0) {
                thumbs::Pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (pictureCover == 0) {
            LOG_ERROR("Editor") << "SELFTEST: picture got no cover in the file dialog";
            ok = false;
        }
        if (ok && assetslot::Cover(nullptr, root / "sub", 64) != 0) {
            LOG_ERROR("Editor") << "SELFTEST: a folder got a cover";
            ok = false;
        }
        if (ok && assetslot::Cover(nullptr, root / "notes.txt", 64) != 0) {
            LOG_ERROR("Editor") << "SELFTEST: a text file got a cover";
            ok = false;
        }

        // Вид (сетка/строки) — привычка человека, и он обязан пережить закрытие
        // диалога. Хранится в настройках редактора, читается при открытии.
        if (ok) {
            const bool had = sage::editor::prefs::GetBool("filebrowser.grid", false);
            FileBrowser::Config cfg;
            cfg.StartDir = root;

            sage::editor::prefs::SetBool("filebrowser.grid", true);
            FileBrowser asGrid;
            asGrid.Open(cfg);
            sage::editor::prefs::SetBool("filebrowser.grid", false);
            FileBrowser asRows;
            asRows.Open(cfg);
            if (!asGrid.GridView() || asRows.GridView()) {
                LOG_ERROR("Editor") << "SELFTEST: file dialog does not remember the chosen view";
                ok = false;
            }
            sage::editor::prefs::SetBool("filebrowser.grid", had);
        }

        // --- ДИАЛОГ ВЫБОРА АССЕТА ЗАПЕРТ В ПРОЕКТЕ ------------------------
        //
        // У диалога две разные работы: ВНЕСТИ в проект (файл лежит в
        // «Загрузках», на флешке — границы быть не может) и ВЫБРАТЬ ассет
        // проекта. Пока границы не было, второе делалось первым: в слот
        // материала клалась картинка снаружи, и ссылка ломалась при сборке
        // игры и переносе проекта — при том, что у человека файл на диске
        // на месте.
        if (ok) {
            const fs::path inside = root / "inside";
            fs::create_directories(inside, ec);

            FileBrowser bound;
            FileBrowser::Config cfg;
            cfg.StartDir = inside;
            cfg.Root = root;
            bound.Open(cfg);
            if (bound.CurrentDir() != inside.lexically_normal()) {
                LOG_ERROR("Editor") << "SELFTEST: диалог открылся не там, где просили";
                ok = false;
            }

            // Начальная папка СНАРУЖИ — открываемся с границы, а не «где-то
            // там»: иначе первый же кадр показал бы место, куда потом нельзя
            // вернуться.
            FileBrowser clamped;
            FileBrowser::Config outsideCfg;
            outsideCfg.StartDir = root.parent_path();
            outsideCfg.Root = root;
            clamped.Open(outsideCfg);
            if (!sage::editor::pathscope::Within(root, clamped.CurrentDir())) {
                LOG_ERROR("Editor") << "SELFTEST: диалог с границей открылся ВНЕ её: "
                                    << clamped.CurrentDir().string();
                ok = false;
            }

            // А без границы — весь диск, иначе вносить в проект было бы нечего.
            FileBrowser free_;
            FileBrowser::Config freeCfg;
            freeCfg.StartDir = root.parent_path();
            free_.Open(freeCfg);
            if (free_.CurrentDir() != root.parent_path().lexically_normal()) {
                LOG_ERROR("Editor") << "SELFTEST: диалог импорта не пустили наружу";
                ok = false;
            }
        }
        fs::remove_all(root, ec);
    }

    // --- СКАЧАННЫЙ НАБОР ТЕКСТУР СТАНОВИТСЯ МАТЕРИАЛОМ ЦЕЛИКОМ --------------
    //
    // Текстуры скачивают наборами: рядом лежат albedo, normal, roughness,
    // metallic, ao, height. Раньше картинку нельзя было даже бросить на объект
    // («этот файл нельзя назначить объекту»), и единственным путём было создать
    // материал руками и разложить пять карт по слотам, зная, какой файл в какой
    // слот. Понятно, до какого слота доходили: до albedo. Отсюда и вывод,
    // который и звучал, — «работает только albedo, остальные карты
    // игнорируются».
    //
    // Проверяется тем же жестом, каким набор и приносят: бросок ОДНОГО файла на
    // объект. И проверяется не «материал появился», а что в нём: все карты
    // набора, нормаль в формате OpenGL (а не DirectX, лежащая рядом), и ничего
    // лишнего — карту высот движок не применяет и в слот класть не должен.
    if (ok) {
        std::error_code ec;
        const fs::path dir = m_project.AssetsDir() / "selftest_set";
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        auto png = [&](const char* name, unsigned char r, unsigned char g, unsigned char b) {
            const fs::path file = dir / name;
            std::vector<unsigned char> px(4 * 4 * 3);
            for (size_t i = 0; i < px.size(); i += 3) { px[i] = r; px[i + 1] = g; px[i + 2] = b; }
            stbi_write_png(file.string().c_str(), 4, 4, 3, px.data(), 4 * 3);
            return file;
        };
        const fs::path albedo = png("wall_albedo.png", 200, 120, 90);
        png("wall_normal-ogl.png", 128, 128, 255);
        png("wall_normal-dx.png", 128, 128, 255);
        png("wall_roughness.png", 180, 180, 180);
        png("wall_metallic.png", 10, 10, 10);
        png("wall_ao.png", 240, 240, 240);
        png("wall_height.png", 60, 60, 60);   // движок её не применяет
        sage::AssetDatabase::Instance().ScanProject(m_project.Dir().string());

        NewScene(ProjectTemplateKind::Empty);
        GameObject wall = m_scene->CreateObject("Wall");
        wall.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
        wall.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);

        if (!ApplyAssetToEntity(wall.Id(), albedo)) {
            LOG_ERROR("Editor") << "SELFTEST: текстуру нельзя бросить на объект";
            ok = false;
        } else {
            const MeshRendererComponent& mr = wall.Renderer();
            const std::shared_ptr<Material> mat = mr.MaterialPtr;
            if (mr.MaterialPath.empty() || !mat) {
                LOG_ERROR("Editor") << "SELFTEST: материал из набора не назначен объекту";
                ok = false;
            } else if (mat->TexturePath.empty() || mat->NormalMapPath.empty() ||
                       mat->RoughnessMapPath.empty() || mat->MetallicMapPath.empty() ||
                       mat->AOMapPath.empty()) {
                LOG_ERROR("Editor") << "SELFTEST: в материале из набора не все карты: albedo '"
                                    << mat->TexturePath << "', нормаль '" << mat->NormalMapPath
                                    << "', шероховатость '" << mat->RoughnessMapPath
                                    << "', металличность '" << mat->MetallicMapPath
                                    << "', затенение '" << mat->AOMapPath << "'";
                ok = false;
            } else if (mat->NormalMapPath.find("dx") != std::string::npos) {
                // Рядом лежат обе нормали; DX-вариант даёт рельеф наизнанку.
                LOG_ERROR("Editor") << "SELFTEST: взята DirectX-нормаль вместо OpenGL: "
                                    << mat->NormalMapPath;
                ok = false;
            } else if (mat->EmissiveMap.find("height") != std::string::npos) {
                LOG_ERROR("Editor") << "SELFTEST: карта высот попала в слот, хотя не применяется";
                ok = false;
            } else if (!mat->NormalTex || !mat->RoughnessTex || !mat->AOTex) {
                // Пути мало: без загруженных текстур карты не доедут до кадра.
                LOG_ERROR("Editor") << "SELFTEST: карты набора не загрузились в материал";
                ok = false;
            }

            // Файл материала лежит РЯДОМ С КАРТАМИ и назван общим корнем набора:
            // искать его потом человек будет именно там.
            if (ok && !fs::exists(dir / "wall.sagemat", ec)) {
                LOG_ERROR("Editor") << "SELFTEST: .sagemat не записан рядом с набором";
                ok = false;
            }

            // ТОТ ЖЕ ЖЕСТ ВО ВЬЮПОРТЕ: текстуру роняют на поверхность, которую
            // хотят покрасить. Раньше бросок во вьюпорт текстуру просто не
            // замечал — как и бросок в список.
            if (ok) {
                GameObject painted = m_scene->CreateObject("WallDrop");
                painted.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
                painted.Renderer().MeshPtr =
                    ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
                painted.GetTransform().Position = glm::vec3(0.0f, 0.0f, 0.0f);
                const glm::mat4 dropView =
                    glm::lookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f), glm::vec3(0, 1, 0));
                const glm::mat4 dropProj =
                    glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
                if (!DropAssetAtViewport(dropView, dropProj, 0.5f, 0.5f, albedo) ||
                    painted.Renderer().MaterialPath.empty()) {
                    LOG_ERROR("Editor") << "SELFTEST: бросок текстуры во вьюпорт не покрасил объект";
                    ok = false;
                }
            }

            // Набор собирается по ЛЮБОМУ своему файлу, а не только по albedo:
            // человек с равным правом бросит на объект карту нормалей.
            if (ok) {
                GameObject other = m_scene->CreateObject("Wall2");
                other.Renderer().Ref = MeshRef{MeshRef::Type::Cube};
                other.Renderer().MeshPtr = ResourceManager::Instance().GetPrimitive(MeshRef::Type::Cube);
                if (!ApplyAssetToEntity(other.Id(), dir / "wall_normal-ogl.png") ||
                    other.Renderer().MaterialPath != mr.MaterialPath) {
                    LOG_ERROR("Editor") << "SELFTEST: бросок карты нормалей дал другой материал: '"
                                        << other.Renderer().MaterialPath << "'";
                    ok = false;
                }
            }
        }
        fs::remove_all(dir, ec);
    }

    // --- ОБЛОЖКИ МОДЕЛИ НЕ ПЕРЕЖИВАЮТ ГРАФИЧЕСКИЙ КОНТЕКСТ ------------------
    //
    // Кэш материалов модели держит ТЕКСТУРЫ, а деструктор текстуры зовёт
    // glDeleteTextures. Пока кэш был статикой внутри функции, он умирал в самом
    // конце — уже после main, когда контекста нет: редактор падал молча, без
    // отчёта о падении, стоило закрыть его с моделью в сцене. Со стороны это и
    // есть «странно работает»: поработал, закрыл — «программа завершилась
    // некорректно».
    //
    // Проверка держится на двух вещах разом: здесь кэш ЗАПОЛНЯЕТСЯ (без этого
    // падать нечему), а ловит падение код возврата процесса — его сверяет
    // scripts/ci_smoke_test.sh. Внутри программы поймать это нельзя: падение
    // случается позже любой проверки.
    if (ok) {
        const std::vector<std::shared_ptr<Material>>& preview =
            AssetPreview::MaterialsForModel(SAGE_TEST_MODEL);
        if (preview.empty()) {
            LOG_ERROR("Editor") << "SELFTEST: материалы модели для обложки не прочитались: "
                                << SAGE_TEST_MODEL;
            ok = false;
        }
    }

    return ok;
}
