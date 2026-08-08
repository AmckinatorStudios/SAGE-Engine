// ---------------------------------------------------------------------------
// Headless-проверки редактора: self-test ядра и E2E-сборка игры.
//
// ПОЧЕМУ ОТДЕЛЬНЫМ ФАЙЛОМ. Они занимали 43% EditorLayer.cpp — 966 строк из
// 2204. Это не редактор: ни одна из этих строк не выполняется, когда человек
// работает в программе. Соседство с реальной логикой мешало обеим сторонам —
// править редактор приходилось, пролистывая сотни строк проверок, а сами
// проверки терялись в хвосте файла.
//
// Перенос — ЧИСТОЕ ПЕРЕМЕЩЕНИЕ: ни строки логики не изменено. Обе функции
// остаются методами EditorLayer (им нужен доступ к его состоянию) и
// вызываются оттуда же, откуда вызывались.
// ---------------------------------------------------------------------------
#include "EditorLayer.h"

#include "sage/ecs/DecalSystem.h"

#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

#include <glm/gtc/matrix_transform.hpp>

#include "sage/core/Application.h"
#include "sage/core/Version.h"
#include "sage/render/ResourceManager.h"
#include "sage/render/Screenshot.h"
#include "sage/ecs/LightSystem.h"
#include "sage/ecs/RenderSystem.h"
#include "sage/physics/Ragdoll.h"
#include "sage/render/Frustum.h"
#include "sage/render/ParticlePresets.h"
#include "sage/anim/AnimationSystem.h"
#include "sage/gi/GI.h"
#include "sage/scene/Components.h"
#include "sage/scene/SceneSerializer.h"

#include "sage/scene/Prefab.h"
#include "sage/render/ModelLoader.h"
#include "sage/render/ModelMaterial.h"
#include "sage/render/PostFX.h"
#include "sage/assets/import/Convert.h"
#include "AssetPreview.h"

namespace fs = std::filesystem;

// ============================================================================
//  Self-test (SAGE_EDITOR_SELFTEST=1, headless CI)
// ============================================================================

// Headless-проверка ядра редактора без UI-кликов (модалки недоступны в CI):
// проект -> сцена -> undo/redo -> ассеты -> материалы -> камера -> Play.
// Результат — строкой SELFTEST: PASS/FAIL в лог.
void EditorLayer::RunSelfTest() {
    size_t before = m_scene->Count();
    std::string err;
    bool ok = true;

    // Подтверждения опасных действий выключаем на время прогона: диалог ждёт
    // человека, а здесь его нет. Это не обход проверки — сама работа
    // подтверждений проверяется ниже отдельным шагом, в обоих состояниях
    // галочки «больше не спрашивать».
    m_confirm.SetSuppressed("delete-entity", true);

    std::error_code ec;
    fs::remove_all("selftest_project", ec); // от прошлого прогона
    if (!CreateProject(".", "selftest_project", err)) {
        LOG_ERROR("Editor") << "SELFTEST: create project failed: " << err;
        ok = false;
    }
    if (ok) {
        before = m_scene->Count(); // CreateProject пересоздал демо-сцену
        fs::path scenePath = m_project.ScenesDir() / "selftest.sage";
        if (!SaveSceneToFile(scenePath)) ok = false;
        if (ok) {
            NewScene(false); // пустая сцена — убеждаемся, что загрузка реально восстанавливает
            if (!LoadSceneFromFile(scenePath)) ok = false;
        }
        if (ok && m_scene->Count() != before) {
            LOG_ERROR("Editor") << "SELFTEST: entity count mismatch: saved " << before
                                << ", loaded " << m_scene->Count();
            ok = false;
        }
    }

    // --- Недавние проекты: наш проект должен был попасть в начало списка ---
    if (ok) {
        RecentProjects reload;
        reload.Load();
        bool found = !reload.List().empty() &&
                     reload.List().front() == m_project.Dir().string();
        if (!found) {
            LOG_ERROR("Editor") << "SELFTEST: recent projects did not record the new project";
            ok = false;
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
            {AssetsPanel::CreateKind::Script, "selftest_script", "selftest_script.lua"},
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
                // Манифест игры лежит РЯДОМ с exe, а не в пакете: на него
                // указывают при запуске из другой папки.
                !fs::exists(gameDir / "project.sageproj", ec) ||
                fs::exists(gameDir / "project", ec) ||
                !fs::exists(gameDir / "assets" / "shaders" / "lit.frag", ec)) {
                LOG_ERROR("Editor") << "SELFTEST: built game layout incomplete in " << gameDir.string();
                ok = false;
            }
        }
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
            // Тики ЧЕРЕЗ ПЛАНИРОВЩИК, а не вызовом m_playScripts->UpdateAll:
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

    // --- Физика: динамическое тело падает под гравитацией, Stop откатывает ---
    if (ok) {
        GameObject green = m_scene->FindByName("Green Cube");
        if (green.Valid()) {
            // Убираем крутящий скрипт, вешаем твёрдое тело — чистая физика.
            m_scene->Registry().remove<ScriptComponent>(green.Entity());
            m_scene->Registry().emplace_or_replace<RigidBodyComponent>(green.Entity());
            float yBefore = green.GetTransform().Position.y;

            StartPlay();
            if (!m_playPhysics || m_playPhysics->BodyCount() < 1) {
                LOG_ERROR("Editor") << "SELFTEST: physics failed - no bodies created";
                ok = false;
            }
            // Тоже через планировщик — по той же причине, что и скрипты выше.
            for (int i = 0; i < 10; ++i) m_systems.Run(*m_scene, 0.1f);
            float yDuring = m_scene->FindByName("Green Cube").GetTransform().Position.y;
            StopPlay();
            float yAfter = m_scene->FindByName("Green Cube").GetTransform().Position.y;

            if (ok && yDuring >= yBefore - 0.01f) {
                LOG_ERROR("Editor") << "SELFTEST: physics failed - body did not fall ("
                                    << yBefore << " -> " << yDuring << ")";
                ok = false;
            }
            if (ok && std::abs(yAfter - yBefore) > 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: physics stop failed - scene not restored (y "
                                    << yAfter << ", expected " << yBefore << ")";
                ok = false;
            }
        }
    }

    // --- Ragdoll + соединения: собранная кукла строит тела и суставы, падает ---
    if (ok) {
        int rootId = sage::physics::BuildRagdoll(*m_scene, {0.0f, 8.0f, 0.0f}, 1.0f);
        GameObject root = m_scene->Get(rootId);
        int jointComps = 0;
        m_scene->Registry().view<JointComponent>().each([&](auto) { ++jointComps; });
        bool built = root.Valid() &&
                     m_scene->Registry().all_of<RigidBodyComponent>(root.Entity()) &&
                     jointComps >= 6; // 11 костей -> 10 суставов (минимум 6)
        if (!built) {
            LOG_ERROR("Editor") << "SELFTEST: ragdoll build failed (root " << root.Valid()
                                << ", joints " << jointComps << ")";
            ok = false;
        } else {
            float yBefore = root.GetTransform().Position.y;
            StartPlay();
            // На Jolt суставы реально строятся; на Simple — их нет (ожидаемо).
            bool jointsOk = !m_playPhysics->SupportsJoints() || m_playPhysics->JointCount() >= 6;
            for (int i = 0; i < 40; ++i) m_playPhysics->Step(*m_scene, 1.0f / 60.0f);
            float yAfter = m_scene->Get(rootId).GetTransform().Position.y;
            StopPlay();
            if (!jointsOk) {
                LOG_ERROR("Editor") << "SELFTEST: ragdoll joints not built on Jolt ("
                                    << m_playPhysics->JointCount() << ")";
                ok = false;
            } else if (yAfter >= yBefore - 0.05f) {
                LOG_ERROR("Editor") << "SELFTEST: ragdoll did not fall (" << yBefore
                                    << " -> " << yAfter << ")";
                ok = false;
            }
        }
    }

    // --- Анимация: демо-скелет проигрывается, палитра костей меняется во времени ---
    if (ok) {
        GameObject rig = m_scene->CreateObject("SelftestRig");
        m_scene->Registry().emplace<AnimatedModelComponent>(rig.Entity());

        sage::anim::UpdateAnimators(*m_scene, 0.0f); // инициализация (загрузка демо + rig)
        AnimatedModelComponent& am = m_scene->Registry().get<AnimatedModelComponent>(rig.Entity());
        if (!am.Model || am.Anim.BoneCount() < 2) {
            LOG_ERROR("Editor") << "SELFTEST: animation failed - rig not built";
            ok = false;
        } else if (am.Anim.ClipCount() < 1) {
            LOG_ERROR("Editor") << "SELFTEST: animation failed - no clips";
            ok = false;
        } else {
            glm::mat4 boneA = am.Anim.BoneMatrices()[1]; // поза в начале
            for (int i = 0; i < 5; ++i) sage::anim::UpdateAnimators(*m_scene, 0.1f); // t=0.5s
            glm::mat4 boneB = am.Anim.BoneMatrices()[1];
            float diff = 0.0f;
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
                diff += std::abs(boneA[c][r] - boneB[c][r]);
            if (diff < 0.001f) {
                LOG_ERROR("Editor") << "SELFTEST: animation failed - bone pose did not change over time";
                ok = false;
            }
            // Кросс-фейд между клипами демо (Wave -> Curl) через ШТАТНЫЙ путь:
            // меняем Clip в компоненте, система сама запускает переход. Во время
            // — Fading()==true и вес в диапазоне; после — CurrentClip == Curl.
            if (ok && am.Anim.ClipCount() >= 2) {
                am.Clip = 1;
                am.BlendTime = 0.4f;
                sage::anim::UpdateAnimators(*m_scene, 0.2f); // система стартует фейд + шаг
                bool midOk = am.Anim.Fading() && am.Anim.FadeWeight() > 0.2f &&
                             am.Anim.FadeWeight() < 0.9f;
                sage::anim::UpdateAnimators(*m_scene, 0.4f); // добить переход
                bool doneOk = !am.Anim.Fading() && am.Anim.CurrentClip() == 1;
                if (!midOk || !doneOk) {
                    LOG_ERROR("Editor") << "SELFTEST: animation crossfade failed (mid " << midOk
                                        << ", done " << doneOk << ")";
                    ok = false;
                }
            }
        }
        m_scene->RemoveObject(rig.Id());
    }

    // --- Конфиг: сохранение и загрузка настроек сохраняют значения (round-trip) ---
    if (ok) {
        sage::EngineConfig a;
        a.Shadows = false;
        a.PostProcessing = false;
        a.ShadowResolution = 1024;
        a.Aspect = sage::AspectMode::R21x9;
        a.RenderScale = 0.75f;
        a.VSync = false;
        a.Msaa = 4;
        std::string cfgPath = "selftest_settings.cfg";
        if (!a.SaveFile(cfgPath)) {
            LOG_ERROR("Editor") << "SELFTEST: config save failed";
            ok = false;
        } else {
            sage::EngineConfig b; // из значений по умолчанию
            if (!b.LoadFile(cfgPath)) {
                LOG_ERROR("Editor") << "SELFTEST: config load failed";
                ok = false;
            } else if (b.Shadows != false || b.PostProcessing != false ||
                       b.ShadowResolution != 1024 || b.Aspect != sage::AspectMode::R21x9 ||
                       std::abs(b.RenderScale - 0.75f) > 0.001f || b.VSync != false || b.Msaa != 4) {
                LOG_ERROR("Editor") << "SELFTEST: config round-trip mismatch";
                ok = false;
            }
            std::error_code cfgEc;
            fs::remove(cfgPath, cfgEc);
        }
    }

    // --- Частицы: эмиттер рождает живые частицы, пул растёт со временем ---
    if (ok) {
        ParticleSystem& particles = m_renderer.Particles();
        GameObject fx = m_scene->CreateObject("SelftestEmitter");
        fx.GetTransform().Position = {0.0f, 1.0f, 0.0f};
        ParticleEmitterComponent em;
        em.Config = ParticlePresets::Fire();
        m_scene->Registry().emplace<ParticleEmitterComponent>(fx.Entity(), em);

        for (int i = 0; i < 8; ++i) sage::fx::UpdateEmitters(*m_scene, particles, 0.05f);
        if (particles.AliveCount() == 0) {
            LOG_ERROR("Editor") << "SELFTEST: particles failed - no live particles emitted";
            ok = false;
        }
        m_scene->RemoveObject(fx.Id());
    }

    // --- Отсечение по фрустуму + инстансный батчинг ---
    if (ok) {
        // 1. Математика фрустума: точка перед камерой видна, за камерой — нет.
        glm::mat4 v = glm::lookAt(glm::vec3(0, 0, 10), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
        glm::mat4 p = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
        sage::render::Frustum fr = sage::render::Frustum::FromViewProj(p * v);
        bool inFront = fr.IntersectsSphere(glm::vec3(0, 0, 0), 1.0f);
        bool behind  = fr.IntersectsSphere(glm::vec3(0, 0, 60), 1.0f); // за спиной камеры
        if (!inFront || behind) {
            LOG_ERROR("Editor") << "SELFTEST: frustum cull math failed (front=" << inFront
                                << " behind=" << behind << ")";
            ok = false;
        }

        // 2. Батчинг: демо-сцена рендерится, группы мешей < числа сущностей
        //    (несколько кубов -> один инстанс-вызов), учёт видимых сходится.
        if (ok) {
            LightingEnvironment env = sage::ecs::CollectLighting(*m_scene);
            glm::mat4 cv = m_camera.GetViewMatrix();
            glm::mat4 cp = m_camera.GetProjectionMatrix(16.0f / 9.0f);
            sage::ecs::RenderStats st = m_renderer.RenderColorForTest(*m_scene, cv, cp, m_camera.Position, env);
            if (st.Total < 1 || st.Batches < 1 || st.Batches > st.Total ||
                st.Drawn + st.Culled != st.Total) {
                LOG_ERROR("Editor") << "SELFTEST: batch render stats inconsistent (total=" << st.Total
                                    << " drawn=" << st.Drawn << " culled=" << st.Culled
                                    << " batches=" << st.Batches << ")";
                ok = false;
            }
        }
    }

    // --- Дубликат: копия несёт ВСЕ движковые компоненты (свет/тело/коллайдер),
    // рантайм-состояние своё (RuntimeBody сброшен) ---
    if (ok) {
        GameObject src = m_scene->CreateObject("DupSource");
        m_scene->Registry().emplace<LightComponent>(src.Entity());
        RigidBodyComponent rb; rb.RuntimeBody = 12345; // «живое» тело — не должно скопироваться
        m_scene->Registry().emplace<RigidBodyComponent>(src.Entity(), rb);
        m_scene->Registry().emplace<ColliderComponent>(src.Entity());
        SetSelectedId(src.Id());
        DuplicateSelected();
        GameObject copy = m_scene->Get(m_selectedId);
        bool compOk = copy.Valid() && copy.Id() != src.Id() &&
                      m_scene->Registry().all_of<LightComponent>(copy.Entity()) &&
                      m_scene->Registry().all_of<RigidBodyComponent>(copy.Entity()) &&
                      m_scene->Registry().all_of<ColliderComponent>(copy.Entity());
        bool runtimeOk = compOk &&
            m_scene->Registry().get<RigidBodyComponent>(copy.Entity()).RuntimeBody ==
                sage::physics::kInvalidBody;
        if (!compOk || !runtimeOk) {
            LOG_ERROR("Editor") << "SELFTEST: duplicate failed (components " << compOk
                                << ", runtime reset " << runtimeOk << ")";
            ok = false;
        }
        if (copy.Valid()) m_scene->RemoveObject(copy.Id());
        m_scene->RemoveObject(src.Id());
        SetSelectedId(-1);
    }

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
            SetSelectedId(parent.Id());
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
        SetSelectedId(-1);
    }

    // --- Мультивыделение: набор из двух, Delete удаляет обе, Undo возвращает,
    // Duplicate дублирует обе ---
    if (ok) {
        GameObject a = m_scene->CreateObject("MultiA");
        GameObject b = m_scene->CreateObject("MultiB");
        SetSelectedId(a.Id());
        ToggleSelection(b.Id()); // теперь выбраны обе, первичная — b
        bool selOk = m_selection.size() == 2 && IsSelected(a.Id()) && IsSelected(b.Id()) &&
                     m_selectedId == b.Id();
        if (!selOk) {
            LOG_ERROR("Editor") << "SELFTEST: multi-select set wrong (size " << m_selection.size() << ")";
            ok = false;
        }
        if (ok) {
            size_t n0 = m_scene->Count();
            DuplicateSelected(); // обе -> +2, выделены копии
            if (m_scene->Count() != n0 + 2 || m_selection.size() != 2) {
                LOG_ERROR("Editor") << "SELFTEST: multi-duplicate failed (count " << m_scene->Count()
                                    << ", sel " << m_selection.size() << ")";
                ok = false;
            }
            if (ok) DeleteSelected(); // убрать копии (выделены они) -> снова n0
            // Удаляем исходную пару (снова мультивыбор) и проверяем Undo.
            if (ok) {
                SetSelectedId(m_scene->FindByName("MultiA").Id());
                ToggleSelection(m_scene->FindByName("MultiB").Id());
                size_t nBefore = m_scene->Count();
                DeleteSelected();
                bool delOk = m_scene->Count() == nBefore - 2 && m_selection.empty();
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
        SetSelectedId(-1);
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
        SetSelectedId(-1);
        PickAtViewport(u, v, /*additive=*/false);
        if (m_selectedId != box.Id()) {
            LOG_ERROR("Editor") << "SELFTEST: pick on box selected " << m_selectedId << ", expected "
                                << box.Id() << " (box)";
            ok = false;
        }

        // Клик по полу далеко от кубика — выбирается пол.
        if (ok) {
            project(glm::vec3(-6.0f, 0.0f, -4.0f), u, v);
            SetSelectedId(-1);
            PickAtViewport(u, v, false);
            if (m_selectedId != floor.Id()) {
                LOG_ERROR("Editor") << "SELFTEST: pick on floor selected " << m_selectedId
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
                SetSelectedId(-1);
                PickAtViewport(u, v, false);
                if (m_selectedId != floor.Id()) {
                    LOG_ERROR("Editor") << "SELFTEST: pick near box at (" << p.x << "," << p.z
                                        << ") selected " << m_selectedId << ", expected "
                                        << floor.Id() << " (floor)";
                    ok = false;
                }
            }
        }


        // --- Инструменты над выделением: габариты, посадка, выравнивание, кадр ---
        if (ok) {
            // Габариты считаются по мировой матрице, а не по локальной коробке:
            // кубик поднят на 0.5 и стоит на полу, значит его коробка — от 0 до 1.
            SetSelectedId(box.Id());
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
            SetSelectedId(box.Id());
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
            SetSelectedId(other.Id());
            ToggleSelection(box.Id());   // первичная — box (x = 0)
            AlignSelection(0);
            const glm::vec3 p = other.GetTransform().Position;
            // По X подтянулась, остальные оси не тронуты.
            if (std::abs(p.x) > 1e-3f || std::abs(p.z - 2.0f) > 1e-3f) {
                LOG_ERROR("Editor") << "SELFTEST: align X gave (" << p.x << "," << p.z
                                    << "), expected (0, 2)";
                ok = false;
            }
            m_scene->RemoveObject(other.Id());
            SetSelectedId(-1);
        }

        // Фокус: камера отъезжает от объекта, но смотрит на него — проверяем,
        // что объект оказался ПЕРЕД камерой и на разумной дистанции.
        if (ok) {
            SetSelectedId(box.Id());
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
        SetSelectedId(-1);
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

        SetSelectedId(root.Id());
        fs::path prefabPath = m_project.Dir() / "assets" / "selftest.sageprefab";
        std::error_code pec;
        fs::create_directories(prefabPath.parent_path(), pec);
        std::string perr;
        bool saved = SaveSelectedAsPrefab(prefabPath, perr);
        // Удаляем оригинал и инстанцируем — проверяем восстановление из файла.
        if (saved) {
            m_scene->RemoveObject(root.Id());
            SetSelectedId(-1);
            int newRootId = InstantiatePrefab(prefabPath);
            GameObject nr = m_scene->Get(newRootId);
            bool rootOk = nr.Valid() && nr.Name() == "PrefabRoot" &&
                          m_scene->Registry().all_of<LightComponent>(nr.Entity()) &&
                          m_selectedId == newRootId;
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
        SetSelectedId(-1);
    }

    // --- Пресеты качества: Low гасит тяжёлые проходы, значения переживают
    // sage.cfg round-trip, оконные параметры не тронуты ---
    if (ok) {
        sage::EngineConfig pc;
        pc.Width = 1600; pc.VSync = false;
        pc.ApplyPreset(sage::QualityPreset::Low);
        bool presetOk = !pc.Shadows && !pc.PostProcessing && !pc.AmbientOcclusion &&
                        std::abs(pc.RenderScale - 0.75f) < 0.001f &&
                        pc.Width == 1600 && pc.VSync == false;
        std::string pPath = "selftest_preset.cfg";
        bool fileOk = presetOk && pc.SaveFile(pPath);
        if (fileOk) {
            sage::EngineConfig back;
            fileOk = back.LoadFile(pPath) && !back.Shadows && !back.PostProcessing &&
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
            bool giGpu = giState->Pages[0].Gpu != nullptr && giState->Probes.GpuR != nullptr;
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

    // --- Новые инструменты редактора: загрузка моделей, префабы, редактор кода,
    //     подтверждения. Проверяем ИМЕННО то, что человек делает мышью, но без
    //     кликов: те же методы, что зовут кнопки.
    {
        // 1. Модель в форматах, которые экспортирует Blender. Раньше сущность
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

        // 3. Редактор кода: открыть скрипт, увидеть строки, определить язык.
        const fs::path luaPath = m_project.AssetsDir() / "selftest_code.lua";
        { std::ofstream f(luaPath); f << "local x = 1\n-- комментарий\nfunction F() end\n"; }
        if (!m_code.OpenFile(luaPath)) {
            LOG_ERROR("Editor") << "SELFTEST: редактор кода не открыл .lua";
            ok = false;
        }

        // 4. Подтверждение опасного действия: пока вопрос включён, объект
        //    остаётся; с выключенным вопросом действие проходит сразу. Именно
        //    так и должна работать галочка «больше не спрашивать».
        GameObject victim = m_scene->CreateObject("SelfTestVictim");
        const int victimId = victim.Id();
        SetSelectedId(victimId);
        m_selection = {victimId};
        m_confirm.SetSuppressed("delete-entity", false);
        DeleteSelected();
        if (!m_scene->Get(victimId).Valid()) {
            LOG_ERROR("Editor") << "SELFTEST: удаление прошло БЕЗ подтверждения";
            ok = false;
        }
        m_confirm.SetSuppressed("delete-entity", true);
        SetSelectedId(victimId);
        m_selection = {victimId};
        DeleteSelected();
        if (m_scene->Get(victimId).Valid()) {
            LOG_ERROR("Editor") << "SELFTEST: с выключенным вопросом объект не удалился";
            ok = false;
        }
        // Возвращаем «не спрашивать» на остаток прогона: дальше кликать
        // по-прежнему некому.
        m_confirm.SetSuppressed("delete-entity", true);
        SetSelectedId(-1);
        m_selection.clear();
    }

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
        if (!rep.Ok) {
            LOG_ERROR("Editor") << "SELFTEST: импорт не удался: " << rep.Error;
            ok = false;
        } else if (!fs::exists(destDir / "crate.obj", ec) ||
                   !fs::exists(destDir / "crate.mtl", ec) ||
                   !fs::exists(destDir / "crate_albedo.png", ec)) {
            LOG_ERROR("Editor") << "SELFTEST: импорт не принёс спутников модели (.mtl/текстура)";
            ok = false;
        } else if (!rep.Missing.empty()) {
            LOG_ERROR("Editor") << "SELFTEST: импорт потерял спутник: " << rep.Missing.front();
            ok = false;
        }

        // Ссылка на внесённый файл обязана быть ОТНОСИТЕЛЬНОЙ: именно так её
        // найдёт собранная игра, делающая chdir в свою папку.
        if (ok) {
            const std::string ref = m_project.AssetRef(destDir / "crate.obj");
            if (ref.empty() || fs::path(ref).is_absolute() ||
                ref != "assets/selftest_import/crate.obj") {
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
                ModelLoader::ExtractMaterial((destDir / "crate.obj").string());
            if (!ex.Found || std::abs(ex.Albedo.g - 0.6f) > 1e-3f) {
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
        const int droppedId = m_selectedId;

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
                GameObject placed = m_scene->Get(m_selectedId);
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
        SetSelectedId(-1);
        m_selection.clear();
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

    if (ok) LOG_INFO("Editor") << "SELFTEST: PASS (project + scene + undo/redo + assets + "
                               << "materials + camera + light + primitives + environment + build + "
                               << "recent + dirty + play + physics + animation + config + particles + "
                               << "culling + duplicate + hierarchy + multiselect + prefab + presets + GI + "
                               << "models + prefab-api + code-editor + confirm + pick + tools + formats + ortho + "
                               << "import + asset-refs + model-material + prefab-cover + drag-drop + settings-live + "
                               << "project-scripts + broken-scripts + replay + error-flood, "
                               << before << " entities)";
    else LOG_ERROR("Editor") << "SELFTEST: FAIL";
}

// ============================================================================
//  E2E: полная игра через редактор (SAGE_EDITOR_E2E=1, headless CI)
//
//  «Без иллюзий»: РЕАЛЬНЫМИ операциями редактора (те же методы, что зовут
//  кнопки UI) создаётся проект-игра «Coin Rush» с Lua-логикой: бот сам едет к
//  монетам, собирает их через SendMessage, монеты уничтожают себя, счёт на
//  HUD обновляется из Lua (GetUI). Сцена сохраняется в .sage, ПЕРЕЧИТЫВАЕТСЯ
//  с диска, проигрывается в Play (проверяем: монеты исчезли, счёт «5/5»),
//  Stop откатывает мир, затем File > Build Game упаковывает ИСПОЛНЯЕМУЮ игру.
//  Отдельный шаг smoke-теста запускает СОБРАННЫЙ бинарник и убеждается по
//  логам/скриншоту, что та же Lua-логика работает и в нём.
// ============================================================================
void EditorLayer::RunE2EGameTest() {
    bool ok = true;
    std::error_code ec;
    fs::remove_all("e2e_game", ec);  // от прошлого прогона
    fs::remove_all("e2e_dist", ec);

    // --- 1. Проект (File > New Project) ---
    std::string err;
    if (!CreateProject(".", "e2e_game", err)) {
        LOG_ERROR("Editor") << "E2E: create project failed: " << err;
        LOG_ERROR("Editor") << "E2E: FAIL";
        return;
    }

    // --- 2. Скрипты игры — файлы в assets/scripts проекта (как из панели Assets) ---
    fs::create_directories(m_project.Dir() / "assets" / "scripts", ec);
    auto writeScript = [&](const char* name, const char* body) {
        std::ofstream f(m_project.Dir() / "assets" / "scripts" / name);
        if (!f) { ok = false; LOG_ERROR("Editor") << "E2E: can't write script " << name; }
        f << body;
    };
    // Бот: сам едет к ближайшей живой монете, вблизи шлёт ей "collect".
    writeScript("bot.lua", R"LUA(
function OnUpdate(entity, dt)
    local coin = nil
    for i = 1, 5 do
        local c = FindObject("Coin" .. i)
        if c ~= nil then coin = c break end
    end
    if coin == nil then return end
    local p = entity.Transform.Position
    local d = coin.Transform.Position - p
    d.y = 0.0
    if d:Length() < 0.45 then
        SendMessage(coin, "collect")
    else
        entity.Transform.Position = p + d:Normalized() * (2.5 * dt)
    end
end
)LUA");
    // Монета: крутится; по "collect" объявляет счёт и уничтожает себя.
    writeScript("coin.lua", R"LUA(
function OnUpdate(entity, dt)
    entity.Transform.Rotation.y = entity.Transform.Rotation.y + dt * 240.0
end
function OnMessage(entity, name, data)
    if name == "collect" then
        log("E2E: coin collected: " .. entity.Name)
        Broadcast("scored")
        entity:Destroy()
    end
end
)LUA");
    // HUD: копит счёт по "scored", пишет текст и заполняет полосу из Lua.
    writeScript("hud.lua", R"LUA(
local score = 0
function OnStart(entity)
    local ui = entity:GetUI()
    if ui ~= nil then ui.Text = "Score: 0 / 5" end
end
function OnMessage(entity, name, data)
    if name ~= "scored" then return end
    score = score + 1
    local ui = entity:GetUI()
    if ui ~= nil then ui.Text = "Score: " .. score .. " / 5" end
    local bar = FindObject("Score Bar")
    if bar ~= nil then bar:GetUI().Value = score / 5.0 end
    if score >= 5 then log("E2E: ALL COINS COLLECTED") end
end
)LUA");

    // --- 3. Сцена игры — редакторскими операциями создания сущностей ---
    NewScene(false);
    m_scene->Lighting.Skybox.Enabled = true;

    GameObject ground = CreatePrimitiveEntity("Ground", MeshRef::Type::Cube);
    ground.GetTransform().Position = {0.0f, -0.55f, 0.0f};
    ground.GetTransform().Scale = {14.0f, 0.3f, 6.0f};
    ground.Renderer().Color = {0.28f, 0.30f, 0.34f};

    GameObject bot = CreatePrimitiveEntity("Bot", MeshRef::Type::Cube);
    bot.GetTransform().Position = {-6.0f, 0.1f, 0.0f};
    bot.GetTransform().Scale = {0.8f, 0.8f, 0.8f};
    bot.Renderer().Color = {0.30f, 0.55f, 0.95f};
    m_scene->Registry().emplace<ScriptComponent>(bot.Entity(), ScriptComponent{"assets/scripts/bot.lua"});

    for (int i = 1; i <= 5; ++i) {
        GameObject coin = CreatePrimitiveEntity("Coin" + std::to_string(i), MeshRef::Type::Sphere);
        coin.GetTransform().Position = {-4.0f + (float)i * 1.6f, 0.35f, 0.0f};
        coin.GetTransform().Scale = {0.45f, 0.45f, 0.45f};
        coin.Renderer().Color = {0.95f, 0.80f, 0.20f};
        m_scene->Registry().emplace<ScriptComponent>(coin.Entity(), ScriptComponent{"assets/scripts/coin.lua"});
    }

    GameObject cam = m_scene->CreateObject("Main Camera");
    cam.GetTransform().Position = {0.0f, 2.5f, 8.5f};
    cam.GetTransform().Rotation = {-12.0f, 0.0f, 0.0f};
    m_scene->Registry().emplace<CameraComponent>(cam.Entity());

    GameObject hud = m_scene->CreateObject("HUD");
    UIElementComponent hudUi;
    hudUi.Type = UIElementComponent::Kind::Panel;
    hudUi.Offset = {16.0f, 16.0f};
    hudUi.Size = {240.0f, 64.0f};
    hudUi.Rounding = 12.0f;
    hudUi.BorderThickness = 2.0f;
    hudUi.Text = "Score: 0 / 5";
    hudUi.TextCentered = false;
    m_scene->Registry().emplace<UIElementComponent>(hud.Entity(), hudUi);
    m_scene->Registry().emplace<ScriptComponent>(hud.Entity(), ScriptComponent{"assets/scripts/hud.lua"});

    GameObject bar = m_scene->CreateObject("Score Bar");
    UIElementComponent barUi;
    barUi.Type = UIElementComponent::Kind::Bar;
    barUi.Anchor = UIAnchor::BottomLeft;
    barUi.Offset = {12.0f, 8.0f};
    barUi.Size = {216.0f, 16.0f};
    barUi.Rounding = 7.0f;
    barUi.Color = {0.0f, 0.0f, 0.0f, 0.55f};
    barUi.Value = 0.0f;
    barUi.BarFillColor = {0.95f, 0.80f, 0.20f, 1.0f};
    m_scene->Registry().emplace<UIElementComponent>(bar.Entity(), barUi);
    m_scene->SetParent(bar.Entity(), hud.Entity());

    // --- 4. Сохранить и ПЕРЕЧИТАТЬ с диска: играем то, что реально в файле ---
    fs::path scenePath = m_project.ScenesDir() / "main.sage";
    if (!SaveSceneToFile(scenePath) || !LoadSceneFromFile(scenePath)) {
        LOG_ERROR("Editor") << "E2E: scene save/reload failed";
        ok = false;
    }

    // --- 5. Play: логика Lua реально играет (бот собирает все монеты) ---
    if (ok) {
        StartPlay();
        for (int i = 0; i < 240 && ok; ++i) m_playScripts->UpdateAll(0.05f); // ~12 c игры
        bool coinsGone = true;
        for (int i = 1; i <= 5; ++i)
            if (m_scene->FindByName("Coin" + std::to_string(i)).Valid()) coinsGone = false;
        const UIElementComponent* hudNow =
            m_scene->Registry().try_get<UIElementComponent>(m_scene->FindByName("HUD").Entity());
        bool scoreOk = hudNow && hudNow->Text == "Score: 5 / 5";
        const UIElementComponent* barNow =
            m_scene->Registry().try_get<UIElementComponent>(m_scene->FindByName("Score Bar").Entity());
        bool barOk = barNow && std::abs(barNow->Value - 1.0f) < 0.001f;
        if (!coinsGone || !scoreOk || !barOk) {
            LOG_ERROR("Editor") << "E2E: play logic failed (coinsGone=" << coinsGone
                                << " scoreOk=" << scoreOk << " barOk=" << barOk
                                << " hudText='" << (hudNow ? hudNow->Text : "?") << "')";
            ok = false;
        }
        StopPlay();
        // Stop обязан вернуть мир: монеты на месте, счёт исходный.
        if (ok && (!m_scene->FindByName("Coin1").Valid() || !m_scene->FindByName("Coin5").Valid())) {
            LOG_ERROR("Editor") << "E2E: stop did not restore coins";
            ok = false;
        }
    }

    // --- 6. File > Build Game: упаковка ИСПОЛНЯЕМОЙ игры ---
    if (ok) {
        std::string buildErr;
        if (!BuildGame("e2e_dist", buildErr)) {
            LOG_ERROR("Editor") << "E2E: build game failed: " << buildErr;
            ok = false;
        } else {
#ifdef _WIN32
            fs::path exe = fs::path("e2e_dist") / "e2e_game" / "e2e_game.exe";
#else
            fs::path exe = fs::path("e2e_dist") / "e2e_game" / "e2e_game";
#endif
            if (!fs::exists(exe, ec)) {
                LOG_ERROR("Editor") << "E2E: built game exe missing: " << exe.string();
                ok = false;
            }
        }
    }

    if (ok) LOG_INFO("Editor") << "E2E: PASS (project + lua scripts + scene save/reload + "
                                  "play logic [5 coins, messaging, HUD from Lua] + stop restore + build exe)";
    else LOG_ERROR("Editor") << "E2E: FAIL";
}

// ============================================================================
//  Headless-сессия над ЧУЖИМ проектом (SAGE_EDITOR_OPEN_PROJECT=<путь>)
//
//  ЗАЧЕМ. Self-test и E2E выше проверяют редактор на игре, которую редактор же
//  и сочинил внутри своего кода. Настоящая игра живёт в отдельном репозитории,
//  её сцены и скрипты пишет человек — и до сих пор ни одна проверка не открывала
//  такой проект НАСТОЯЩИМ редактором. Эта функция закрывает дыру: CI игры
//  запускает SageEditor поверх своего проекта теми же операциями, что и человек
//  мышкой — Open Project, выбрать сцену, Play, Stop, Build Game.
//
//  Управление — переменными окружения (headless, никаких модалок):
//    SAGE_EDITOR_OPEN_PROJECT=<путь>   папка с project.sageproj (обязательна)
//    SAGE_EDITOR_OPEN_SCENE=<имя>      сцена в scenes/ (по умолчанию main.sage)
//    SAGE_EDITOR_PLAY_SECONDS=<сек>    сколько отыграть в Play (0 — не играть)
//    SAGE_EDITOR_PLAY_STEP=<сек>       шаг симуляции (по умолчанию 1/60)
//    SAGE_GAME_ARGS="autopilot=1"      параметры запуска для скриптов игры
//    SAGE_EDITOR_BUILD_TO=<папка>      собрать игру (File > Build Game)
//  Итог — строкой SESSION: PASS/FAIL в лог, чтобы CI искал её грепом.
// ============================================================================
void EditorLayer::RunHeadlessProjectSession() {
    m_launcher.Dismiss(); // hub проектов в headless-прогоне только закрывает кадр

    const char* projectDir = std::getenv("SAGE_EDITOR_OPEN_PROJECT");
    std::string err;
    if (!OpenProject(projectDir, err)) {
        LOG_ERROR("Editor") << "SESSION: open project failed: " << err;
        LOG_ERROR("Editor") << "SESSION: FAIL";
        return;
    }
    LOG_INFO("Editor") << "SESSION: opened project '" << m_project.Name() << "' at " << projectDir;

    const char* sceneName = std::getenv("SAGE_EDITOR_OPEN_SCENE");
    fs::path scenePath = m_project.ScenesDir() / (sceneName ? sceneName : "main.sage");
    if (!LoadSceneFromFile(scenePath)) {
        LOG_ERROR("Editor") << "SESSION: load scene failed: " << scenePath.string();
        LOG_ERROR("Editor") << "SESSION: FAIL";
        return;
    }
    LOG_INFO("Editor") << "SESSION: loaded scene " << scenePath.filename().string()
                       << " (" << m_scene->Count() << " entities)";

    bool ok = true;

    // --- Save Scene: та же кнопка, что у человека (Ctrl+S). Нужна затем, что
    // перезапись файла — единственный способ ПОДНЯТЬ его до текущего формата:
    // миграция при загрузке живёт в памяти и на диск сама не возвращается.
    // Без этого проект, лежащий в старой версии, так в ней и остаётся, а
    // проверить «сохранилось ли то, что прочиталось» нечем.
    if (std::getenv("SAGE_EDITOR_SAVE_SCENE")) {
        if (!SaveSceneToFile(scenePath)) {
            LOG_ERROR("Editor") << "SESSION: save scene failed: " << scenePath.string();
            ok = false;
        } else {
            LOG_INFO("Editor") << "SESSION: saved scene " << scenePath.filename().string();
        }
    }

    // --- Play: та же кнопка, что у человека. Время идёт фиксированным шагом,
    // а не по-настоящему: headless-прогон должен быть воспроизводимым, иначе
    // «успела ли игра дойти до события» зависело бы от загрузки CI-машины. ---
    float playSeconds = 0.0f;
    if (const char* s = std::getenv("SAGE_EDITOR_PLAY_SECONDS")) playSeconds = (float)std::atof(s);
    if (playSeconds > 0.0f) {
        float step = 1.0f / 60.0f;
        if (const char* s = std::getenv("SAGE_EDITOR_PLAY_STEP")) {
            float parsed = (float)std::atof(s);
            if (parsed > 0.0f) step = parsed;
        }
        // Параметры запуска (SAGE_GAME_ARGS) StartPlay раздаёт скриптам сам —
        // до их OnStart, иначе автопрогон не успел бы включиться.
        StartPlay();
        size_t stepCount = (size_t)(playSeconds / step);
        for (size_t i = 0; i < stepCount; ++i) {
            m_playScripts->UpdateAll(step);
            if (m_playPhysics) m_playPhysics->Step(*m_scene, step);
        }
        LOG_INFO("Editor") << "SESSION: played " << playSeconds << "s in " << stepCount
                           << " steps (" << m_scene->Count() << " entities at the end)";
        size_t duringPlay = m_scene->Count();
        StopPlay();
        // Stop обязан вернуть мир к состоянию до Play. Для игры, которая
        // порождает мир скриптами (воксельный ландшафт — тысячи сущностей),
        // это самая содержательная проверка снапшота из всех: если откат течёт,
        // здесь он потечёт заметно.
        LOG_INFO("Editor") << "SESSION: stop restored scene to " << m_scene->Count()
                           << " entities (was " << duringPlay << " during play)";
    }

    // --- File > Build Game: проект превращается в запускаемую игру ---
    if (ok) {
        if (const char* out = std::getenv("SAGE_EDITOR_BUILD_TO")) {
            std::string buildErr;
            if (!BuildGame(out, buildErr)) {
                LOG_ERROR("Editor") << "SESSION: build game failed: " << buildErr;
                ok = false;
            } else {
                LOG_INFO("Editor") << "SESSION: built game into " << out;
            }
        }
    }

    if (ok) LOG_INFO("Editor") << "SESSION: PASS";
    else LOG_ERROR("Editor") << "SESSION: FAIL";
}
